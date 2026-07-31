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
    b.id = s.next_bomb_id++;
    b.created_tick = s.tick;  // bomb +64 (sub_422EDE); gates same-tick trigger
    b.owner = owner;
    b.colour = owner;  // creation-time colour byte (bomb +60); never transferred
    b.x = grid::tile_center_x(tx);
    b.y = grid::tile_center_y(ty);
    // Bomb kind is exclusive and gated (sub_41EB13): jelly sets kind 2, then the
    // trigger branch OVERRIDES to kind 1 only while the player still has trigger
    // allowance (+85 < +86 max_bombs), consuming one (++85). Once the allowance
    // is spent the trigger flag is ignored and this becomes a normal timed bomb —
    // the original never blocks placement, it DOWNGRADES the bomb. The +85
    // counter is refilled only by the next Trigger pickup.
    const bool make_trigger = p.trigger && p.trigger_placed < p.max_bombs;
    if (make_trigger) ++p.trigger_placed;
    // The fuse DURATION is computed and stored for EVERY kind (sub_41EB13
    // computes it and passes it to sub_422EDE, which parks it in word +74),
    // trigger included — a trigger bomb only gates the fuse TICKING; the duration
    // survives for a later downgrade (sub_424C47) or throw restart. The spooge
    // run index staggers the countdown (+k ticks), not the duration.
    b.fuse_init = s.tuning.fuse_frames;
    if (p.sick(Disease::ShortFuse)) b.fuse_init = std::max(1, b.fuse_init / 3);
    b.fuse = make_trigger ? -1 : b.fuse_init + fuse_stagger;
    // Flame reach (sub_41EB13 ordering): short-flame forces 1, THEN goldflame
    // (+94) overrides to max(gridW,gridH) — so goldflame beats short-flame, and
    // that literal max replaces the port's old flame=99 sentinel.
    b.flame = p.sick(Disease::ShortFlame) ? 1 : p.flame;
    if (p.goldflame) b.flame = std::max(kGridWidth, kGridHeight);
    b.jelly = p.jelly && !make_trigger;
    b.trigger = make_trigger;
    // Duds (sub_422EDE): only regular bombs can fizzle, and only while the global
    // gate is open. The gate re-arms base + rand(spread) SECONDS ahead BEFORE the
    // 1-in-N roll (sub_422C13 runs first) — RNG order contract. Units per
    // VALUELST 320/321's own legend ("minimum/additional random number of SECONDS
    // between potential dud bombs"): 180+rand%180 s = 3-6 minutes per
    // opportunity. The re-arm ADDS to the previous deadline (sub_422C13
    // accumulates into dword_464AF4) rather than anchoring on "now", so a
    // long-idle gate banks consecutive openings exactly like the original.
    // facts.md "Dud bombs" (units corrected 2026-07-09).
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
    // Diarrhea/super players drop with a wet "poops" splat: sub_41F29B's
    // forced-drop branch plays a random SOUNDLST 550-554 instead of the normal
    // drop 100/101. The flag rides in the (unhashed) event data.
    const bool poop = p.sick(Disease::Diarrhea) || p.sick(Disease::Super);
    s.events.push_back({Event::Type::BombPlaced, static_cast<std::int8_t>(owner),
                        static_cast<std::int8_t>(tx), static_cast<std::int8_t>(ty),
                        static_cast<std::int8_t>(poop ? 1 : 0)});
}

void BombSystem::drop(Player& p, std::uint8_t owner) {
    int tx = p.tile_x(), ty = p.tile_y();
    // Only ONE of the three "can't place here" outcomes is audible in the
    // original: a WARPHOLE drop. A blocked/occupied tile or a spent allotment
    // short-circuits placement SILENTLY in sub_41F29B — no sub_427961(40) is
    // attached to those guards — so the port plays no deny SFX for them either,
    // for humans OR AI.
    if (!grid::tile_open(s_, tx, ty) || grid::bomb_at(s_, tx, ty)) return;
    if (p.bombs_placed >= p.max_bombs) return;
    // No bombs on a WARPHOLE (sub_41F29B drop block ~23354: an actor of type 1
    // under the player short-circuits the placement; sound 40 plays unless the
    // drop was disease-forced). facts.md "Core-feel audit" §3. DropRefused is
    // emitted for ANY player — sub_41F29B is the shared human+AI input processor
    // and sub_427961(40) is a GLOBAL SFX with no per-source gate — but in the
    // original an AI never reaches it: sub_423188 rejects a tile carrying a
    // type-1 actor and BOTH AI drop behaviours gate on it before pressing the
    // key. That gate was missing from this port's AI, which machine-gunned the
    // deny SFX; fixed in ai_grids.cpp's drop_tile_clear (facts.md "AI never bombs
    // a warphole"). This branch itself was always correct.
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
    // record (sub_42542D), a blocked tile (!sub_41E5C3), or when the supply runs
    // out. The run counter k feeds sub_41EB13 -> a +k-tick fuse stagger.
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
    // sub_424A50 (the +91 punch-glove handler) returns unconditionally, and its
    // caller sets the punch anim state 2 whenever it does — only the bomb launch
    // and the SOUNDLST 150 SFX live inside `if (bomb ahead)`. So a press with no
    // bomb in front still animates, but is silent and launches nothing. The
    // event's (unhashed) data flags whether a bomb was actually hit, so the
    // SoundDirector plays the hit sound only then.
    int tx = p.tile_x() + grid::dir_dx(p.facing), ty = p.tile_y() + grid::dir_dy(p.facing);
    Bomb* b = grid::bomb_at(s_, tx, ty);
    if (b) launch(*b, p.facing, 3, s_.tuning.punch_arc_first);
    s_.events.push_back({Event::Type::BombPunched, static_cast<std::int8_t>(who),
                         static_cast<std::int8_t>(tx), static_cast<std::int8_t>(ty),
                         static_cast<std::int8_t>(b ? 1 : 0)});
}

bool BombSystem::try_grab(Player& p, int who) {
    // The underfoot probe is sub_422E48: motion states 0 (resting) AND 1
    // (sliding) both qualify — a player can pluck their own bomb mid-slide. Only
    // flying (2) and carried (3) are exempt, and bomb_at already excludes those.
    Bomb* b = grid::bomb_at(s_, p.tile_x(), p.tile_y());
    if (!b) return false;
    p.carrying = true;
    p.carried_owner = b->owner;
    p.carried_colour = b->colour;
    // Store the creation-time DURATION, not the frozen remnant: the throw
    // restarts the fuse from scratch (sub_41F29B's tail zeroes the elapsed-fuse
    // dword at +68 before calling sub_424987), so the remnant is never consumed.
    p.carried_fuse = b->fuse_init;
    p.carried_flame = b->flame;
    p.carried_jelly = b->jelly;
    p.carried_trigger = b->trigger;
    b->active = false;  // the slot stays reserved (bombs_placed unchanged)
    // pickup_pause (player state +78==4), NOT p.stun (+58, head-hit only) — two
    // confirmed-independent counters (facts.md "Player state machine (+78) —
    // COMPLETE", docs/re/player-turn.md §1).
    //
    // The window is getvalue(665) + 1 ticks, not getvalue(665). CONFIRMED at the
    // instruction level 2026-07-30 (the reading had been NEEDS-VERIFY because the
    // decompiler named the compared value after a temporary, not after +80): the
    // gate at 0x41FA42-0x41FA57 loads the dword at player+78, shifts the high
    // half down to isolate the anim counter +80, calls getvalue with 665, and the
    // branch that SKIPS the block is a "greater" jump — so the block runs on the
    // NON-STRICT `+80 <= getvalue(665)`. +80 is zeroed by the grab and rises one
    // step per 50 ms anim tick, so the default 2 covers +80 = 0, 1, 2 = THREE
    // ticks of blocked input. Seeding the countdown with getvalue(665) gave two.
    p.pickup_pause = s_.tuning.pickup_pause + 1;
    s_.events.push_back({Event::Type::BombGrabbed, static_cast<std::int8_t>(who),
                         static_cast<std::int8_t>(p.tile_x()),
                         static_cast<std::int8_t>(p.tile_y()), 0});
    return true;
}

void BombSystem::throw_carried(Player& p, int who) {
    Bomb nb;
    nb.active = true;
    nb.id = s_.next_bomb_id++;
    nb.created_tick = s_.tick;  // a freshly-launched bomb is stamped this tick
    nb.owner = p.carried_owner;
    nb.colour = p.carried_colour;
    // The bomb leaves from where the CARRIER IS STANDING, not from the tile
    // centre: sub_41F29B's +37 release block writes `bomb[+28] = player[+28]` and
    // `bomb[+32] = player[+32]` immediately before sub_424987 (pseudo.c
    // 23290-23291). That is also where the carried bomb was already being drawn,
    // so the release is continuous; the tile centre instead made a bomb thrown
    // mid-stride jump up to half a tile sideways on the release frame. The
    // landing tile is unaffected (|offset| < tile/2) and launch takes fly_total
    // from the tile count, so only the start point and the interpolation move.
    // facts.md "The carry FREEZES the carrier's body, and the RELEASE is the
    // throw animation".
    nb.x = p.x;
    nb.y = p.y;
    // Fresh full fuse on release (the tail zeroes +68 right before the launch): a
    // thrown bomb always lands with its complete creation-time duration ahead of
    // it, not the remnant frozen at grab time. Trigger bombs stay fuse-less.
    // facts.md "Core-feel audit" §5.
    nb.fuse_init = p.carried_fuse;  // carried_fuse holds fuse_init since the grab
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
    // trigger bomb is a legal remote-detonation target — and its scan requires
    // the candidate's creation stamp be STRICTLY EARLIER than the current tick
    // (the running-best is seeded with dword_464994 = this tick), so a trigger
    // bomb placed the SAME tick as the press is not yet detonable: the player
    // must wait one tick (bombs.md finding 4, pseudo.c 26036). Our vector is in
    // creation order, so the first eligible bomb is the oldest == the original's
    // min-stamp scan.
    //
    // The detonation goes through the pending-chain QUEUE, not a direct call
    // (sub_424B41's tail is `sub_423209(bomb, 0)`, the SAME queue a flame-arm
    // chain uses) — but unlike an arm-chain hit it still resolves THIS tick: the
    // drain runs once, at the top of the bomb pass, which the whole player pass
    // already precedes within the same tick. facts.md "Chain-reaction timing".
    for (std::size_t bi = 0; bi < s_.bombs.size(); ++bi) {
        Bomb& b = s_.bombs[bi];
        if (b.active && b.trigger && b.owner == owner && !b.flying && b.created_tick < s_.tick) {
            flames_.queue_chain(b.id);
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
    // sub_42464B on an already-sliding bomb: the same direction is a silent no-op
    // (only the speed is re-set — a constant for us); a DIFFERENT direction snaps
    // it to its tile centre and redirects it, with the kick sound (played when
    // `dir != new || !moving`).
    if (b->moving && b->dir == d) return;
    if (b->moving) {
        b->x = grid::tile_center_x(b->tile_x());
        b->y = grid::tile_center_y(b->tile_y());
    }
    b->moving = true;
    b->dir = d;
    s_.events.push_back({Event::Type::BombKicked, static_cast<std::int8_t>(who),
                         static_cast<std::int8_t>(tx), static_cast<std::int8_t>(ty), 0});
}

void BombSystem::stop_own_sliding(int owner) {
    // sub_4247C5: every active bomb of this owner that is SLIDING (motion 1) and
    // not jelly (kind 2) gets the stop flag; slide() halts it on the next tile
    // centre. No sound here — the stop itself plays 130 when it lands.
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

    // Jelly veer (sub_42331C flight block): at each tile boundary from the third
    // tile on, a flying jelly bomb rolls a 1-in-N ±90° turn (VALUELST 667) BEFORE
    // the landing checks — the turn only shows when it has to hop onward. Rolled
    // only while the unwrapped position is in-bounds; the roll order is part of
    // the RNG contract.
    if (b.jelly && b.to_x >= 0 && b.to_x < kFW && b.to_y >= 0 && b.to_y < kFH) {
        auto n = static_cast<std::uint32_t>(std::max<std::int32_t>(1, s.tuning.jelly_turn_chance));
        if (random_below(s, n) == 0) {
            int side = static_cast<int>(random_below(s, 2));  // 0 left, 1 right
            b.dir = grid::from_godir(grid::to_godir(b.dir) - 1 + 2 * side);
        }
    }

    // Landing sound (sub_42331C case 2, ~25441): sub_427961(160) fires at EVERY
    // tile boundary once the bomb has travelled >= 3 tiles, UNCONDITIONALLY and
    // before the head-hit/settle/re-hop branch — so a thrown or punched bomb
    // plays it on its final landing too, not only when it hops onward.
    s.events.push_back({Event::Type::BombBounced, -1, static_cast<std::int8_t>(tx),
                        static_cast<std::int8_t>(ty), 0});

    // Landing-tile verdict (sub_42331C ~25443): the tile counts as clear only
    // when all three probes come back empty — sub_425FB9 (tile type), sub_422E48
    // (grounded bomb) and sub_42542D (floor powerup, hidden OR visible: it
    // returns the record regardless of state). Unlike the sliding-bomb probe
    // below, the flight path does NOT squash the powerup — it just can't land
    // there.
    bool clear = grid::tile_open(s, tx, ty) && grid::bomb_at(s, tx, ty) == nullptr &&
                 (!grid::in_grid(tx, ty) || s.floor[ty][tx] == PowerupType::None);

    // A WARPHOLE actor blocks SETTLING but NOT the head hit. The victim scan
    // (sub_421CB5) is the first statement inside the clear verdict at pseudo.c
    // 25443-25449, whereas sub_405654's actor lookup only happens in the
    // victim-less else at 25452 — so a player standing on a warphole IS head-hit
    // by a landing bomb, and only a victim-less warphole makes the bomb hop on.
    // That is reachable: a head hit CANCELS a warp (sub_421F7E's +78=3
    // overwrite), so a player hit during warp-out is stranded on the entry
    // warphole. Folding the warphole into the same `clear` verdict as walls
    // skipped the victim scan on warphole tiles entirely (corrected 2026-07-11,
    // facts.md "Player state machine (+78) — COMPLETE").
    //
    // The settle condition at ~25453 also carries a middle term that decompiles
    // to a bare reference to the statically-linked, NEVER-CALLED CRT exponential
    // routine at 0x4443CC. The ONLY xref to it in the whole binary is the
    // immediate load at 0x423B11, right here: the address goes into eax, is
    // tested against itself, and the jump over the rest of the condition is taken
    // only when it is zero — which a compile-time-constant non-null pointer never
    // is. The term is DEAD CODE; the real condition is "no actor here, or the
    // actor is not a Warphole", the same "type 1 blocks like a wall" rule already
    // confirmed for sub_4230A5. Resolved 2026-07-10, facts.md "Bomb/warphole
    // reconciliation".
    const bool on_warphole = grid::in_grid(tx, ty) && s.actor_type[ty][tx] == ActorType::Warphole;

    const int victim = clear ? grid::player_index_at(s, tx, ty) : -1;
    if (victim >= 0) powerups_.head_hit(victim, tx, ty);

    // Blocked landing tile, a head-hit (which never settles the bomb), or a
    // victim-less warphole => hop onward; otherwise settle.
    if (!clear || victim >= 0 || on_warphole) {
        launch(b, b.dir, 1, s.tuning.punch_arc_hop);
        return;
    }
    b.flying = false;
    // A bomb that lands on an already-flaming tile settles there AND is queued
    // for forced detonation next tick (pseudo.c 25459-25465: the flame probe
    // sub_42708D runs on the landing tile and hands the bomb to sub_423209 with
    // mode 0, unconditionally — no "kind 9 / brick-burn" exemption here, unlike
    // the sliding probe; moot for us since tile_open already refuses to land on a
    // still-crumbling brick). facts.md "Chain-reaction timing".
    if (grid::in_grid(tx, ty) && s.flame[ty][tx] > 0) flames_.queue_chain(b.id);
}

// A visible floor powerup on the cell a sliding bomb enters is destroyed
// outright — kicked and belt-carried bombs plow through powerups. Same skull
// compensation as the flame walk (sub_4230A5 calls sub_4255B2 for kind 2
// whenever dword_464990, the "diseases destroyable" flag, is clear): a squashed
// Disease token relocates instead. scatter() is our sub_4255B2 (RNG order/count).
void BombSystem::squash_powerup(int nx, int ny) {
    if (!grid::in_grid(nx, ny)) return;
    const PowerupType squashed = s_.floor[ny][nx];
    if (squashed == PowerupType::None) return;
    s_.events.push_back({Event::Type::PowerupBurned, -1, static_cast<std::int8_t>(nx),
                         static_cast<std::int8_t>(ny), static_cast<std::int8_t>(squashed)});
    s_.floor[ny][nx] = PowerupType::None;
    if (squashed == PowerupType::Disease && !s_.tuning.diseases_destroyable)
        powerups_.scatter(PowerupType::Disease);
}

// sub_4230A5's cell-entry probe, and the ORDER is load-bearing: a bomb or a
// player on the probed cell blocks FIRST and thereby SHIELDS a powerup there
// from being squashed; the wall and warphole verdicts come after the squash, so
// they do not. Its final verdict is "no actor on the cell OR that actor's type
// field at +4 is not 1, AND the cell's tile-type verdict is 0" — a WARPHOLE
// (type 1) makes the cell impassable regardless of its underlying blank cell
// type, so a sliding/kicked/conveyor bomb is blocked at a warphole's doorstep
// exactly like a wall and can never enter or teleport through it (facts.md
// "Bomb/warphole reconciliation 2026-07-10").
bool BombSystem::cell_blocks_slide(int nx, int ny) {
    if (grid::bomb_at(s_, nx, ny) != nullptr) return true;
    if (grid::player_at(s_, nx, ny)) return true;
    squash_powerup(nx, ny);
    if (!grid::tile_open(s_, nx, ny)) return true;
    return grid::in_grid(nx, ny) && s_.actor_type[ny][nx] == ActorType::Warphole;
}

void BombSystem::slide(std::size_t index, std::int32_t budget) {
    State& s = s_;
    Bomb& b = s.bombs[index];
    Fixed dist = budget;

    // The SHARED "stop" block (sub_42331C): a jelly bomb REVERSES and keeps
    // sliding, exactly like bouncing off a wall; only a non-jelly bomb halts. A
    // flame entry, a blocked cell and a kick-stop all fall into it, so it is
    // written once here rather than transcribed at each of those sites.
    const auto halt_or_bounce = [&](int tx, int ty) {
        if (b.jelly) {
            b.dir = grid::from_godir(grid::to_godir(b.dir) + 2);
            s.events.push_back({Event::Type::JellyBounced, -1, static_cast<std::int8_t>(tx),
                                static_cast<std::int8_t>(ty), 0});
            return;
        }
        b.moving = false;
        s.events.push_back({Event::Type::BombStopped, -1, static_cast<std::int8_t>(tx),
                            static_cast<std::int8_t>(ty), 0});
    };

    while (dist > 0 && b.moving) {
        const int tx = b.tile_x(), ty = b.tile_y();
        // Sliding into a flame QUEUES the bomb for forced detonation next tick,
        // not an immediate explosion (sub_42331C checks sub_42708D per pixel
        // step; pseudo.c 25545-25554 hands it to sub_423209 with mode 0 and then
        // falls unconditionally into the shared stop block). The "kind 9 /
        // brick-burn" exemption the original has here is already modelled:
        // s.burning is a SEPARATE array from s.flame, and this reads only
        // s.flame. facts.md "Chain-reaction timing".
        if (grid::in_grid(tx, ty) && s.flame[ty][tx] > 0) {
            flames_.queue_chain(b.id);
            halt_or_bounce(tx, ty);
            return;
        }
        const Fixed cx = grid::tile_center_x(tx), cy = grid::tile_center_y(ty);
        Fixed axis = (grid::dir_dx(b.dir) != 0) ? b.x : b.y;
        Fixed center = (grid::dir_dx(b.dir) != 0) ? cx : cy;
        int sign = grid::dir_dx(b.dir) + grid::dir_dy(b.dir);

        // How far this step may travel: a bomb short of the tile centre stops AT
        // it; one at/past the centre runs on to the next tile boundary, which is
        // where the stage-actor and cell-entry checks below belong.
        const Fixed to_center = (center - axis) * sign;
        Fixed limit = to_center;
        if (to_center <= 0) {
            // Stage-actor reactions fire ONLY when the bomb is EXACTLY on the
            // tile centre in BOTH axes (sub_42331C ~25532 gates on both
            // axis-offset temporaries being zero). Testing only the move axis
            // would re-fire every pixel as the bomb slides away from a dirarrow,
            // snapping it back forever. Warphole (type 1) is deliberately absent:
            // a sliding bomb can never be centred on one, since cell_blocks_slide
            // refuses entry before it can step on. Bombs never call the warp
            // resolver sub_405A81 anywhere in the binary — that call site is
            // unique to the player stepper (sub_41EC84, pseudo.c 22594). A prior
            // port taught a sliding bomb to teleport here; removed as unfaithful.
            const bool on_dirarrow = b.x == cx && b.y == cy && grid::in_grid(tx, ty) &&
                                     s.actor_type[ty][tx] == ActorType::DirArrow;
            if (on_dirarrow) {
                b.dir = grid::from_godir(s.actor_dir[ty][tx]);
                b.stop_pending = false;  // a dirarrow overrides a pending kick-stop (~25535)
                axis = (grid::dir_dx(b.dir) != 0) ? b.x : b.y;
                center = (grid::dir_dx(b.dir) != 0) ? cx : cy;
                sign = grid::dir_dx(b.dir) + grid::dir_dy(b.dir);
            }
            // Kick+action2 stop: sub_42331C fires it when the +57 stop flag is set
            // AND its signed centre-offset for the move axis has gone
            // non-negative. Checked AFTER the actor block (a dirarrow just cleared
            // it) and before probing the next tile. Never set on jelly, so no
            // reverse branch here. facts.md "Core-feel audit" §4.
            if (b.stop_pending) {
                b.x = cx;
                b.y = cy;
                b.moving = false;
                b.stop_pending = false;
                s.events.push_back({Event::Type::BombStopped, -1, static_cast<std::int8_t>(tx),
                                    static_cast<std::int8_t>(ty), 0});
                return;
            }
            if (cell_blocks_slide(tx + grid::dir_dx(b.dir), ty + grid::dir_dy(b.dir))) {
                b.x = cx;
                b.y = cy;
                b.stop_pending = false;  // the shared stop block clears +57 on any stop
                halt_or_bounce(tx, ty);  // jelly reverses and KEEPS moving
                return;
            }
            limit = (center - axis) * sign + ((grid::dir_dx(b.dir) != 0) ? kTileWF : kTileHF);
        }
        const Fixed step = std::min(dist, limit);
        if (grid::dir_dx(b.dir) != 0)
            b.x += step * sign;
        else
            b.y += step * sign;
        dist -= step;
    }
}

void BombSystem::conveyor_carry(std::size_t index) {
    Bomb& b = s_.bombs[index];
    if (b.flying || b.moving) return;  // only a RESTING bomb (motion state 0) is belt-pushed
    const int tx = b.tile_x(), ty = b.tile_y();
    if (!grid::in_grid(tx, ty) || s_.actor_type[ty][tx] != ActorType::Conveyor) return;
    // sub_42331C case 0 (batch_0x422DDD.cpp:512-547): a resting bomb whose
    // CURRENT tile is a conveyor is pushed one frame's worth along the belt, and
    // the belt check is re-run EVERY tick (bombs.md finding 3). The case NEVER
    // writes the motion word to 1, so a belt bomb stays motion-state-0: the
    // instant it slides off the belt it simply stops being processed here and
    // FREEZES rather than coasting on at kicked speed, and sub_4247C5's
    // "stop my sliding bombs" (state-1-only) cannot touch it either. We slide
    // with a TRANSIENT moving flag and clear it right after, so the bomb is
    // re-evaluated as resting next tick; the belt re-imposes its direction every
    // frame, so any jelly bounce inside slide() is moot on a belt.
    b.dir = grid::from_godir(s_.actor_dir[ty][tx]);
    b.moving = true;
    // Belt speed (getvalue(190+idx)) alone — see advance_bombs for why the shared
    // move block's flat +100 is not a speed term.
    slide(index, s_.tuning.conveyor_speed());
    b.moving = false;  // back to motion state 0 (belt re-evaluated next tick)
}

void BombSystem::advance_bombs() {
    for (std::size_t i = 0; i < s_.bombs.size(); ++i) {
        Bomb& b = s_.bombs[i];
        if (!b.active) continue;
        if (b.flying) {
            fly(b);
            continue;
        }
        if (b.moving) {
            // Kicked/redirected bomb (sub_42331C case 1): slides at the kicked
            // speed (getvalue(300), the bomb's +112 field) regardless of the tile
            // underneath — the belt is consulted only by case 0.
            //
            // The shared move block the case feeds (batch_0x422DDD.cpp:551,
            // pseudo.c 25393-25400) adds a flat +100 to the budget, but the SAME
            // path first backs the position off by one direction step (subtracting
            // dword_45BECC[dir] from +28 and dword_45BEDC[dir] from +32), and the
            // move loop then spends that +100 re-advancing exactly that step. The
            // two cancel; the net per-frame displacement is the speed term alone.
            // Confirmed against the native oracle: the kicked bomb slides ~0.25
            // tile/tick at BOTH 1x and 9x frame cadence — cadence-invariant, so
            // the +100 is not a net speed term to fold. An earlier audit read it
            // as a boost and folded +100 * kSubFrames here, which ran kicked bombs
            // ~1.9x too fast; reverted 2026-07-20 after oracle adjudication.
            slide(i, s_.tuning.kicked_bomb_speed);
            continue;
        }
        conveyor_carry(i);  // resting: pushed only while it sits on a belt tile
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
