#include "systems/bombs.hpp"

#include <algorithm>

#include "bomber/sim/rng.hpp"
#include "grid.hpp"
#include "systems/flames.hpp"
#include "systems/powerups.hpp"

namespace bomber::sim {

void BombSystem::place(Player& p, std::uint8_t owner, int tx, int ty) {
    State& s = s_;
    Bomb b;
    b.active = true;
    b.owner = owner;
    b.x = grid::tile_center_x(tx);
    b.y = grid::tile_center_y(ty);
    if (p.trigger) {
        b.fuse = -1;
    } else {
        b.fuse = s.tuning.fuse_frames;
        if (p.sick(Disease::ShortFuse)) b.fuse = std::max(1, b.fuse / 3);
    }
    b.flame = p.sick(Disease::ShortFlame) ? 1 : p.flame;
    // Bomb kind is exclusive in the original (sub_41EB13): the trigger branch
    // overrides the jelly flag, so a trigger bomb never bounces.
    b.jelly = p.jelly && !p.trigger;
    b.trigger = p.trigger;
    ++p.bombs_placed;
    s.bombs.push_back(b);
    s.events.push_back({Event::Type::BombPlaced, static_cast<std::int8_t>(owner),
                        static_cast<std::int8_t>(tx), static_cast<std::int8_t>(ty), 0});
}

void BombSystem::drop(Player& p, std::uint8_t owner) {
    int tx = p.tile_x(), ty = p.tile_y();
    if (!grid::tile_open(s_, tx, ty) || grid::bomb_at(s_, tx, ty)) return;
    if (p.bombs_placed >= p.max_bombs) return;
    place(p, owner, tx, ty);
}

void BombSystem::spooge_ahead(Player& p, std::uint8_t owner) {
    int cx = p.tile_x(), cy = p.tile_y();
    const int dx = grid::dir_dx(p.facing), dy = grid::dir_dy(p.facing);
    while (p.bombs_placed < p.max_bombs) {
        cx += dx;
        cy += dy;
        if (!grid::tile_open(s_, cx, cy)) break;
        if (grid::bomb_at(s_, cx, cy)) break;
        if (s_.floor[cy][cx] != PowerupType::None) break;
        place(p, owner, cx, cy);
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
    int tx = p.tile_x() + grid::dir_dx(p.facing), ty = p.tile_y() + grid::dir_dy(p.facing);
    Bomb* b = grid::bomb_at(s_, tx, ty);
    if (!b) return;
    launch(*b, p.facing, 3, s_.tuning.punch_arc_first);
    s_.events.push_back({Event::Type::BombPunched, static_cast<std::int8_t>(who),
                         static_cast<std::int8_t>(tx), static_cast<std::int8_t>(ty), 0});
}

bool BombSystem::try_grab(Player& p, int who) {
    Bomb* b = grid::bomb_at(s_, p.tile_x(), p.tile_y());
    if (!b || b->moving) return false;
    p.carrying = true;
    p.carried_owner = b->owner;
    p.carried_fuse = b->fuse;
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
    nb.fuse = p.carried_fuse;
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
    for (std::size_t bi = 0; bi < s_.bombs.size(); ++bi) {
        Bomb& b = s_.bombs[bi];
        if (b.active && b.trigger && b.owner == owner && !b.moving && !b.flying) {
            flames_.explode(bi);
            return true;
        }
    }
    return false;
}

void BombSystem::try_kick(Player& p, Direction d) {
    if (!p.kick) return;
    int tx = p.tile_x() + grid::dir_dx(d), ty = p.tile_y() + grid::dir_dy(d);
    Bomb* b = grid::bomb_at(s_, tx, ty);
    if (!b || b->moving) return;
    int nx = tx + grid::dir_dx(d), ny = ty + grid::dir_dy(d);
    if (!grid::tile_open(s_, nx, ny) || grid::bomb_at(s_, nx, ny)) return;
    b->moving = true;
    b->dir = d;
    s_.events.push_back({Event::Type::BombKicked, -1, static_cast<std::int8_t>(tx),
                         static_cast<std::int8_t>(ty), 0});
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

    // A live player on the landing tile gets bonked on the head.
    int victim = -1;
    for (int i = 0; i < kMaxPlayers; ++i) {
        const Player& pl = s.players[i];
        if (pl.present && pl.alive && pl.tile_x() == tx && pl.tile_y() == ty) {
            victim = i;
            break;
        }
    }
    if (victim >= 0) powerups_.head_hit(victim, tx, ty);

    bool occupied = victim >= 0 || !grid::tile_open(s, tx, ty) ||
                    grid::bomb_at(s, tx, ty) != nullptr;
    if (occupied) {
        launch(b, b.dir, 1, s.tuning.punch_arc_hop);
        s.events.push_back({Event::Type::BombBounced, -1, static_cast<std::int8_t>(tx),
                            static_cast<std::int8_t>(ty), 0});
        return;
    }
    b.flying = false;
}

void BombSystem::slide(Bomb& b) {
    State& s = s_;
    Fixed dist = s.tuning.kicked_bomb_speed;
    while (dist > 0 && b.moving) {
        int tx = b.tile_x(), ty = b.tile_y();
        Fixed cx = grid::tile_center_x(tx), cy = grid::tile_center_y(ty);
        Fixed axis = (grid::dir_dx(b.dir) != 0) ? b.x : b.y;
        Fixed center = (grid::dir_dx(b.dir) != 0) ? cx : cy;
        int sign = grid::dir_dx(b.dir) + grid::dir_dy(b.dir);

        Fixed to_center = (center - axis) * sign;
        if (to_center <= 0) {
            int nx = tx + grid::dir_dx(b.dir), ny = ty + grid::dir_dy(b.dir);
            bool blocked = !grid::tile_open(s, nx, ny) || grid::bomb_at(s, nx, ny) != nullptr;
            for (const auto& pl : s.players)
                if (pl.present && pl.alive && pl.tile_x() == nx && pl.tile_y() == ny)
                    blocked = true;
            if (blocked) {
                b.x = cx;
                b.y = cy;
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

void BombSystem::advance_bombs() {
    for (auto& b : s_.bombs) {
        if (!b.active) continue;
        if (b.flying) fly(b);
        else if (b.moving) slide(b);
    }
}

void BombSystem::tick_fuses() {
    for (std::size_t i = 0; i < s_.bombs.size(); ++i) {
        Bomb& b = s_.bombs[i];
        if (b.active && !b.flying && b.fuse > 0 && --b.fuse == 0) flames_.explode(i);
    }
}

}  // namespace bomber::sim
