#include "systems/bombs.hpp"

#include <algorithm>

#include "bomber/sim/rng.hpp"
#include "grid.hpp"
#include "systems/flames.hpp"
#include "systems/powerups.hpp"

namespace bomber::sim {

void BombSystem::place(Player& p, std::uint8_t owner, int tx, int ty, int fuse_stagger) {
    State& s = s_;
    Bomb b;
    b.active = true;
    b.owner = owner;
    b.x = grid::tile_center_x(tx);
    b.y = grid::tile_center_y(ty);
    // Bomb kind is exclusive and gated (sub_41EB13): jelly sets kind 2, then the
    // trigger branch OVERRIDES to kind 1 only while the player still has trigger
    // allowance (+85 < +86 max_bombs), consuming one (++85). Once the allowance
    // is spent the trigger flag is ignored and this becomes a normal timed bomb
    // (#9 Trigger allowance — the original never blocks placement, it downgrades
    // the bomb). The +85 counter is refilled only by the next Trigger pickup.
    const bool make_trigger = p.trigger && p.trigger_placed < p.max_bombs;
    if (make_trigger) ++p.trigger_placed;
    // The fuse DURATION is computed and stored for EVERY kind (sub_41EB13's
    // v10 -> sub_422EDE word +74), trigger included — a trigger bomb only
    // gates the fuse TICKING; the duration survives for a later downgrade
    // (sub_424C47) or throw restart. The spooge run index staggers the
    // countdown (+k ticks), not the duration (elapsed +68 = -50*k ms).
    b.fuse_init = s.tuning.fuse_frames;
    if (p.sick(Disease::ShortFuse)) b.fuse_init = std::max(1, b.fuse_init / 3);
    b.fuse = make_trigger ? -1 : b.fuse_init + fuse_stagger;
    // Flame reach (sub_41EB13 ordering): short-flame forces 1, then goldflame
    // (+94) OVERRIDES to max(gridW,gridH) — so goldflame beats short-flame. The
    // literal max(cols,rows) replaces our old flame=99 sentinel (#10 Goldflame).
    b.flame = p.sick(Disease::ShortFlame) ? 1 : p.flame;
    if (p.goldflame) b.flame = std::max(kGridWidth, kGridHeight);
    b.jelly = p.jelly && !make_trigger;
    b.trigger = make_trigger;
    // Duds (sub_422EDE): only regular bombs can fizzle, and only while the
    // global gate is open; the gate re-arms base + rand(spread) SECONDS ahead
    // BEFORE the 1-in-N roll (sub_422C13 runs first) — RNG order contract.
    // Units per VALUELST 320/321's own legend ("minimum/additional random
    // number of SECONDS between potential dud bombs"): 180+rand%180 s = 3-6
    // minutes per opportunity, converted to ticks here. The re-arm ADDS to the
    // previous deadline (sub_422C13 `dword_464AF4 += ...`), it does not anchor
    // on "now" — a long-idle gate can bank consecutive openings, exactly like
    // the original. facts.md "Dud bombs" (units corrected 2026-07-09).
    if (!b.trigger && !b.jelly && s.tick >= s.dud_gate) {
        s.dud_gate += (static_cast<std::uint64_t>(s.tuning.dud_gate_base) +
                       random_below(s, static_cast<std::uint32_t>(
                                           std::max<std::int32_t>(1, s.tuning.dud_gate_rand)))) *
                      kTicksPerSecond;
        if (random_below(s, static_cast<std::uint32_t>(
                                std::max<std::int32_t>(1, s.tuning.dud_chance))) == 0)
            b.dud_left = s.tuning.dud_frames;
    }
    ++p.bombs_placed;
    s.bombs.push_back(b);
    // Diarrhea/super players drop with a wet "poops" splat: sub_41F29B's v112
    // branch plays a random SOUNDLST 550-554 instead of the normal drop 100/101.
    // The flag rides in the (unhashed) event data for the SoundDirector.
    const bool poop = p.sick(Disease::Diarrhea) || p.sick(Disease::Super);
    s.events.push_back({Event::Type::BombPlaced, static_cast<std::int8_t>(owner),
                        static_cast<std::int8_t>(tx), static_cast<std::int8_t>(ty),
                        static_cast<std::int8_t>(poop ? 1 : 0)});
}

void BombSystem::drop(Player& p, std::uint8_t owner) {
    int tx = p.tile_x(), ty = p.tile_y();
    if (!grid::tile_open(s_, tx, ty) || grid::bomb_at(s_, tx, ty)) return;
    if (p.bombs_placed >= p.max_bombs) return;
    // No bombs on a WARPHOLE (sub_41F29B drop block ~23354: an actor of type 1
    // under the player short-circuits the placement; sound 40 plays unless the
    // drop was disease-forced). facts.md "Core-feel audit" §3.
    if (grid::in_grid(tx, ty) && s_.actor_type[ty][tx] == ActorType::Warphole) {
        if (!p.sick(Disease::Diarrhea) && !p.sick(Disease::Super))
            s_.events.push_back({Event::Type::DropRefused, static_cast<std::int8_t>(owner),
                                 static_cast<std::int8_t>(tx), static_cast<std::int8_t>(ty), 0});
        return;
    }
    place(p, owner, tx, ty);
}

void BombSystem::spooge_ahead(Player& p, std::uint8_t owner) {
    int cx = p.tile_x(), cy = p.tile_y();
    const int dx = grid::dir_dx(p.facing), dy = grid::dir_dy(p.facing);
    // sub_41F29B spooge loop: stop at a live player (sub_421CB5), a powerup
    // record (sub_42542D), a blocked tile (!sub_41E5C3), or when the supply
    // runs out. The run counter k feeds sub_41EB13 -> a +k-tick fuse stagger,
    // so the laid line detonates as a one-tile-per-tick cascade.
    int k = 0;
    while (p.bombs_placed < p.max_bombs) {
        cx += dx;
        cy += dy;
        if (grid::player_at(s_, cx, cy)) break;
        if (grid::in_grid(cx, cy) && s_.floor[cy][cx] != PowerupType::None) break;
        if (!grid::tile_open(s_, cx, cy)) break;
        if (grid::bomb_at(s_, cx, cy)) break;
        place(p, owner, cx, cy, ++k);
    }
}

void BombSystem::launch(Bomb& b, Direction d, int tiles, std::int32_t arc) {
    b.flying = true;
    b.moving = false;
    b.from_x = b.x;
    b.from_y = b.y;
    b.to_x = b.x + grid::dir_dx(d) * tiles * kTileWF;
    b.to_y = b.y + grid::dir_dy(d) * tiles * kTileHF;
    b.dir = d;
    b.fly_arc = arc;
    Fixed dist = tiles * (grid::dir_dx(d) != 0 ? kTileWF : kTileHF);
    b.fly_total = std::max<std::int32_t>(1, dist / std::max(1, s_.tuning.punched_bomb_speed));
    b.fly_ticks = b.fly_total;
}

void BombSystem::try_punch(Player& p, std::uint8_t who) {
    // sub_424A50 (the +91 punch-glove handler): the glove ALWAYS swings — the
    // caller (sub_41F29B) sets the punch anim state 2 whenever this returns,
    // and it returns unconditionally. Only the bomb launch and the SOUNDLST
    // 150 SFX (sub_427961(150)) live inside `if (bomb ahead)`. So a press with
    // no bomb in front still animates, but is silent and launches nothing.
    // We emit BombPunched every press to drive the swing pose, and flag in the
    // (unhashed) event data whether a bomb was actually hit so the
    // SoundDirector only plays the "kbomb" hit sound in that case.
    int tx = p.tile_x() + grid::dir_dx(p.facing), ty = p.tile_y() + grid::dir_dy(p.facing);
    Bomb* b = grid::bomb_at(s_, tx, ty);
    if (b) launch(*b, p.facing, 3, s_.tuning.punch_arc_first);
    s_.events.push_back({Event::Type::BombPunched, static_cast<std::int8_t>(who),
                         static_cast<std::int8_t>(tx), static_cast<std::int8_t>(ty),
                         static_cast<std::int8_t>(b ? 1 : 0)});
}

bool BombSystem::try_grab(Player& p, int who) {
    // The underfoot probe is sub_422E48: motion states 0 (resting) AND 1
    // (sliding) both qualify — a player can pluck their own bomb mid-slide.
    // Only flying (2) and carried (3) are exempt; bomb_at already excludes
    // those (flying filter / carried bombs are deactivated slots).
    Bomb* b = grid::bomb_at(s_, p.tile_x(), p.tile_y());
    if (!b) return false;
    p.carrying = true;
    p.carried_owner = b->owner;
    // Store the creation-time DURATION, not the frozen remnant: the throw
    // restarts the fuse from scratch (sub_41F29B LABEL_246 zeroes elapsed +68
    // before sub_424987), so the remnant is never consumed by anything.
    p.carried_fuse = b->fuse_init;
    p.carried_flame = b->flame;
    p.carried_jelly = b->jelly;
    p.carried_trigger = b->trigger;
    b->active = false;  // the slot stays reserved (bombs_placed unchanged)
    p.stun = s_.tuning.pickup_pause;
    s_.events.push_back({Event::Type::BombGrabbed, static_cast<std::int8_t>(who),
                         static_cast<std::int8_t>(p.tile_x()),
                         static_cast<std::int8_t>(p.tile_y()), 0});
    return true;
}

void BombSystem::throw_carried(Player& p, int who) {
    Bomb nb;
    nb.active = true;
    nb.owner = p.carried_owner;
    nb.x = grid::tile_center_x(p.tile_x());
    nb.y = grid::tile_center_y(p.tile_y());
    // Fresh full fuse on release (sub_41F29B LABEL_246: `+68 = 0` right before
    // the launch): a thrown bomb always lands with its complete creation-time
    // duration ahead of it, not the remnant frozen at grab time. Trigger bombs
    // stay fuse-less. facts.md "Core-feel audit" §5.
    nb.fuse_init = p.carried_fuse;  // carried_fuse holds fuse_init since grab
    nb.fuse = p.carried_trigger ? -1 : p.carried_fuse;
    nb.flame = p.carried_flame;
    nb.jelly = p.carried_jelly;
    nb.trigger = p.carried_trigger;
    launch(nb, p.facing, 3, s_.tuning.punch_arc_first);
    s_.bombs.push_back(nb);
    p.carrying = false;
    s_.events.push_back({Event::Type::BombThrown, static_cast<std::int8_t>(who),
                         static_cast<std::int8_t>(p.tile_x()),
                         static_cast<std::int8_t>(p.tile_y()), 0});
}

bool BombSystem::detonate_triggered(int owner) {
    // sub_424B41 excludes only motion 3 (carried) and 2 (flying) — a SLIDING
    // trigger bomb is a legal remote-detonation target. Our vector is in
    // creation order, so the first match is the oldest (== the original's
    // min-creation-stamp scan).
    for (std::size_t bi = 0; bi < s_.bombs.size(); ++bi) {
        Bomb& b = s_.bombs[bi];
        if (b.active && b.trigger && b.owner == owner && !b.flying) {
            flames_.explode(bi);
            return true;
        }
    }
    return false;
}

void BombSystem::try_kick(Player& p, Direction d, int who) {
    if (!p.kick) return;
    int tx = p.tile_x() + grid::dir_dx(d), ty = p.tile_y() + grid::dir_dy(d);
    Bomb* b = grid::bomb_at(s_, tx, ty);
    if (!b) return;
    // The mover requires the tile beyond the bomb passable (sub_41E5C3) before
    // dispatching the kick, for resting and sliding targets alike.
    int nx = tx + grid::dir_dx(d), ny = ty + grid::dir_dy(d);
    if (!grid::tile_open(s_, nx, ny) || grid::bomb_at(s_, nx, ny)) return;
    if (b->moving) {
        // sub_42464B on an already-sliding bomb: same direction is a silent
        // no-op (only the speed is re-set — a constant for us); a DIFFERENT
        // direction snaps it to its tile centre and redirects it, with the
        // kick sound (played when `dir != new || !moving`).
        if (b->dir == d) return;
        b->x = grid::tile_center_x(b->tile_x());
        b->y = grid::tile_center_y(b->tile_y());
        b->dir = d;
    } else {
        b->moving = true;
        b->dir = d;
    }
    s_.events.push_back({Event::Type::BombKicked, static_cast<std::int8_t>(who),
                         static_cast<std::int8_t>(tx), static_cast<std::int8_t>(ty), 0});
}

void BombSystem::stop_own_sliding(int owner) {
    // sub_4247C5: every active bomb of this owner that is SLIDING (motion 1)
    // and not jelly (kind 2) gets the stop flag; slide() halts it on the next
    // tile centre. No sound here — the stop itself plays 130 when it lands.
    for (auto& b : s_.bombs)
        if (b.active && b.owner == owner && b.moving && !b.flying && !b.jelly)
            b.stop_pending = true;
}

void BombSystem::fly(Bomb& b) {
    State& s = s_;
    if (--b.fly_ticks > 0) {
        std::int32_t done = b.fly_total - b.fly_ticks;
        b.x = b.from_x + (b.to_x - b.from_x) * done / b.fly_total;
        b.y = b.from_y + (b.to_y - b.from_y) * done / b.fly_total;
        return;
    }
    constexpr Fixed kFW = kGridWidth * kTileWF;
    constexpr Fixed kFH = kGridHeight * kTileHF;
    Fixed lx = ((b.to_x % kFW) + kFW) % kFW;
    Fixed ly = ((b.to_y % kFH) + kFH) % kFH;
    int tx = static_cast<int>(lx / kTileWF), ty = static_cast<int>(ly / kTileHF);
    b.x = grid::tile_center_x(tx);
    b.y = grid::tile_center_y(ty);

    // Jelly veer (sub_42331C flight block): at each tile boundary from the
    // third tile on, a flying jelly bomb rolls a 1-in-N ±90° turn (VALUELST
    // 667) BEFORE the landing checks — the turn only shows when it has to
    // hop onward. Rolled only while the unwrapped position is in-bounds; the
    // roll order is part of the RNG contract.
    if (b.jelly && b.to_x >= 0 && b.to_x < kFW && b.to_y >= 0 && b.to_y < kFH) {
        auto n = static_cast<std::uint32_t>(
            std::max<std::int32_t>(1, s.tuning.jelly_turn_chance));
        if (random_below(s, n) == 0) {
            int side = static_cast<int>(random_below(s, 2));  // 0 left, 1 right
            b.dir = grid::from_godir(grid::to_godir(b.dir) - 1 + 2 * side);
        }
    }

    // Landing sound (sub_42331C case 2, ~25441): the flight block calls
    // sub_427961(160) ("bmdrop3") at EVERY tile boundary once the bomb has
    // travelled >= 3 tiles — UNCONDITIONALLY, before the head-hit/settle/re-hop
    // branch. So a thrown or punched bomb plays 160 on its final landing too,
    // not only when it has to hop onward. (#8 gap 3: the throw arc was missing
    // this settle sound — the "more sounds" the user remembered.) Emitted here,
    // once per boundary, matching the single call site; BombBounced maps to 160.
    s.events.push_back({Event::Type::BombBounced, -1, static_cast<std::int8_t>(tx),
                        static_cast<std::int8_t>(ty), 0});

    // Landing-tile verdict (sub_42331C ~25443): `!sub_425FB9 && !sub_422E48 &&
    // !sub_42542D` — wall/brick, a grounded bomb, AND a floor powerup (hidden
    // OR visible; sub_42542D returns the record regardless of state) are ALL
    // treated as an occupied landing tile. This is facts.md's flagged gap:
    // our previous port never consulted `floor` here, so a flying bomb would
    // land on (and coexist with) a powerup instead of hopping past it like it
    // does past a wall or another bomb. Unlike the sliding-bomb cell-entry
    // probe (sub_4230A5), which squashes a visible powerup as a side effect,
    // the flight path does NOT touch the powerup at all — it just can't land
    // there.
    //
    // The player check (sub_421CB5, head-hit) is nested INSIDE that clear
    // verdict in the original — it never runs when the tile is otherwise
    // occupied. A player can't normally coexist with an unclaimed powerup on
    // the same tile (walking onto one picks it up same-tick), so this nesting
    // is mostly unobservable, but it IS the literal control flow: preserved
    // here rather than checking the player unconditionally.
    bool clear = grid::tile_open(s, tx, ty) && grid::bomb_at(s, tx, ty) == nullptr &&
                 (!grid::in_grid(tx, ty) || s.floor[ty][tx] == PowerupType::None);

    int victim = -1;
    if (clear) {
        for (int i = 0; i < kMaxPlayers; ++i) {
            const Player& pl = s.players[i];
            if (pl.present && pl.alive && pl.tile_x() == tx && pl.tile_y() == ty) {
                victim = i;
                break;
            }
        }
        if (victim >= 0) powerups_.head_hit(victim, tx, ty);
    }

    // Blocked landing tile (or a player head-hit, which never settles the
    // bomb) ⇒ hop onward (the sound already fired above); otherwise settle.
    if (!clear || victim >= 0) {
        launch(b, b.dir, 1, s.tuning.punch_arc_hop);
        return;
    }
    b.flying = false;
}

void BombSystem::slide(std::size_t index, std::int32_t budget) {
    State& s = s_;
    Bomb& b = s.bombs[index];
    Fixed dist = budget;
    while (dist > 0 && b.moving) {
        int tx = b.tile_x(), ty = b.tile_y();
        // Sliding into a flame explodes the bomb (sub_42331C checks sub_42708D
        // per pixel-step; our once-per-tile-entry check catches the same case).
        // #8 gap 1: previously the bomb just kept sliding through flame.
        if (grid::in_grid(tx, ty) && s.flame[ty][tx] > 0) {
            flames_.explode(index);
            return;
        }
        Fixed cx = grid::tile_center_x(tx), cy = grid::tile_center_y(ty);
        Fixed axis = (grid::dir_dx(b.dir) != 0) ? b.x : b.y;
        Fixed center = (grid::dir_dx(b.dir) != 0) ? cx : cy;
        int sign = grid::dir_dx(b.dir) + grid::dir_dy(b.dir);

        Fixed to_center = (center - axis) * sign;
        if (to_center <= 0) {
            // Stage-actor reactions fire ONLY when the bomb is EXACTLY on the
            // tile centre (both axes) — the original's `!v79 && !v80` gate
            // (sub_42331C ~25532). Testing only the move axis would re-fire every
            // pixel as the bomb slides away from a dirarrow, snapping it back
            // forever. dirarrow (type 0) turns the bomb to the arrow's godir;
            // warphole (type 1) teleports it to the linked exit (no RNG) and
            // latches against an immediate re-warp at the exit.
            const bool at_centre = (b.x == cx && b.y == cy);
            if (at_centre && grid::in_grid(tx, ty)) {
                const ActorType at = s.actor_type[ty][tx];
                if (at == ActorType::DirArrow) {
                    b.dir = grid::from_godir(s.actor_dir[ty][tx]);
                    b.warp_latch = false;
                    // A dirarrow overrides a pending kick-stop (sub_42331C
                    // ~25535 clears +57 in the same branch that re-steers).
                    b.stop_pending = false;
                    // recompute the axis/centre/sign for the new direction
                    axis = (grid::dir_dx(b.dir) != 0) ? b.x : b.y;
                    center = (grid::dir_dx(b.dir) != 0) ? cx : cy;
                    sign = grid::dir_dx(b.dir) + grid::dir_dy(b.dir);
                } else if (at == ActorType::Warphole) {
                    if (!b.warp_latch) {
                        const int dx = s.warp_dest_x[ty][tx], dy = s.warp_dest_y[ty][tx];
                        b.x = grid::tile_center_x(dx);
                        b.y = grid::tile_center_y(dy);
                        b.warp_latch = true;
                        s.events.push_back({Event::Type::WarpUsed, -1,
                                            static_cast<std::int8_t>(dx),
                                            static_cast<std::int8_t>(dy), 0});
                        return;  // resume next tick from the exit tile
                    }
                } else {
                    b.warp_latch = false;  // left a warphole: allow future warps
                }
            } else if (!at_centre && grid::in_grid(tx, ty) &&
                       s.actor_type[ty][tx] != ActorType::Warphole) {
                // moved off a non-warp tile mid-slide: allow future warps
                b.warp_latch = false;
            }
            // Kick+action2 stop (sub_42331C `if (+57 && v81 >= 0)`): a pending
            // stop is consumed the moment the bomb is at/past a tile centre —
            // snap onto it and halt. Checked AFTER the actor block (a dirarrow
            // just cleared it) and before probing the next tile. Never set on
            // jelly, so no reverse branch here. facts.md "Core-feel audit" §4.
            if (b.stop_pending) {
                b.x = cx;
                b.y = cy;
                b.moving = false;
                b.stop_pending = false;
                s.events.push_back({Event::Type::BombStopped, -1,
                                    static_cast<std::int8_t>(tx),
                                    static_cast<std::int8_t>(ty), 0});
                return;
            }
            int nx = tx + grid::dir_dx(b.dir), ny = ty + grid::dir_dy(b.dir);
            // The cell-entry probe mirrors sub_4230A5's order: a bomb or a
            // player on the probed cell blocks FIRST (and shields anything
            // else there); only then a visible floor powerup on the cell is
            // destroyed outright — kicked/conveyor bombs plow through
            // powerups — and the tile-type verdict decides enterability.
            bool blocked = grid::bomb_at(s, nx, ny) != nullptr;
            for (const auto& pl : s.players)
                if (pl.present && pl.alive && pl.tile_x() == nx && pl.tile_y() == ny)
                    blocked = true;
            if (!blocked && grid::in_grid(nx, ny) && s.floor[ny][nx] != PowerupType::None) {
                const PowerupType squashed = s.floor[ny][nx];
                s.events.push_back({Event::Type::PowerupBurned, -1,
                                    static_cast<std::int8_t>(nx), static_cast<std::int8_t>(ny),
                                    static_cast<std::int8_t>(squashed)});
                s.floor[ny][nx] = PowerupType::None;
                // Same skull compensation as the flame walk (sub_4230A5:
                // `if (kind == 2 && !dword_464990) sub_4255B2(2)`) — a
                // squashed Disease token relocates when diseases cannot be
                // destroyed. scatter() is our sub_4255B2 (RNG order/count).
                if (squashed == PowerupType::Disease && !s.tuning.diseases_destroyable)
                    powerups_.scatter(PowerupType::Disease);
            }
            if (!grid::tile_open(s, nx, ny)) blocked = true;
            if (blocked) {
                b.x = cx;
                b.y = cy;
                b.stop_pending = false;  // LABEL_36 clears +57 on any stop
                if (b.jelly) {
                    // Jelly (sub_42331C slide block): reverse and KEEP the
                    // moving state — it ping-pongs off obstacles instead of
                    // stopping. Remaining budget is dropped this tick.
                    b.dir = grid::from_godir(grid::to_godir(b.dir) + 2);
                    s.events.push_back({Event::Type::JellyBounced, -1,
                                        static_cast<std::int8_t>(tx),
                                        static_cast<std::int8_t>(ty), 0});
                } else {
                    b.moving = false;
                    s.events.push_back({Event::Type::BombStopped, -1,
                                        static_cast<std::int8_t>(tx),
                                        static_cast<std::int8_t>(ty), 0});
                }
                return;
            }
            Fixed boundary =
                (center - axis) * sign + ((grid::dir_dx(b.dir) != 0) ? kTileWF : kTileHF);
            Fixed step = std::min(dist, boundary);
            if (grid::dir_dx(b.dir) != 0) b.x += step * sign; else b.y += step * sign;
            dist -= step;
        } else {
            Fixed step = std::min(dist, to_center);
            if (grid::dir_dx(b.dir) != 0) b.x += step * sign; else b.y += step * sign;
            dist -= step;
        }
    }
}

void BombSystem::conveyor_carry(std::size_t index) {
    Bomb& b = s_.bombs[index];
    if (b.flying || b.moving) return;  // already in motion: leave it
    const int tx = b.tile_x(), ty = b.tile_y();
    if (!grid::in_grid(tx, ty)) return;
    if (s_.actor_type[ty][tx] != ActorType::Conveyor) return;
    // sub_42331C case 0: a resting bomb on a conveyor is pushed along the belt.
    // Set it sliding in the belt direction; slide() then carries it at belt
    // speed. It stops (and, if jelly, ping-pongs) at obstacles exactly like a
    // kicked bomb, but at getvalue(190+idx) instead of getvalue(300).
    const int nx = tx + grid::dir_dx(grid::from_godir(s_.actor_dir[ty][tx]));
    const int ny = ty + grid::dir_dy(grid::from_godir(s_.actor_dir[ty][tx]));
    if (!grid::tile_open(s_, nx, ny) || grid::bomb_at(s_, nx, ny)) return;  // blocked: stay put
    b.moving = true;
    b.dir = grid::from_godir(s_.actor_dir[ty][tx]);
}

void BombSystem::advance_bombs() {
    for (std::size_t i = 0; i < s_.bombs.size(); ++i) {
        Bomb& b = s_.bombs[i];
        if (!b.active) continue;
        // A resting bomb on a belt starts sliding along it (belt speed).
        conveyor_carry(i);
        if (b.flying) {
            fly(b);
        } else if (b.moving) {
            // Belt speed if the bomb is currently on a conveyor tile, else the
            // kicked-bomb speed (VALUELST 300). Mirrors sub_42331C, where a
            // conveyor push uses getvalue(190+idx) and a kick uses getvalue(300).
            const int tx = b.tile_x(), ty = b.tile_y();
            const bool on_belt = grid::in_grid(tx, ty) &&
                                 s_.actor_type[ty][tx] == ActorType::Conveyor;
            const std::int32_t budget =
                on_belt ? s_.tuning.conveyor_speed() : s_.tuning.kicked_bomb_speed;
            slide(i, budget);  // by index: a slide into flame detonates it
        }
    }
}

void BombSystem::tick_fuses() {
    for (std::size_t i = 0; i < s_.bombs.size(); ++i) {
        Bomb& b = s_.bombs[i];
        if (!b.active) continue;
        // A fizzling dud counts down instead of its fuse (the original's dud
        // window is measured by the always-running anim counter, and the fuse
        // gate skips state 2 — sub_42331C).
        if (b.dud_left > 0) {
            --b.dud_left;
            continue;
        }
        if (!b.flying && b.fuse > 0 && --b.fuse == 0) flames_.explode(i);
    }
}

}  // namespace bomber::sim
