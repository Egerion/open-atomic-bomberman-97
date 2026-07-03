#include "systems/powerups.hpp"

#include <algorithm>
#include <cstdlib>
#include <vector>

#include "bomber/sim/rng.hpp"

namespace bomber::sim {

void PowerupSystem::apply(Player& p, PowerupType t) {
    const Tuning& tn = s_.tuning;
    auto limited = [&tn](std::int32_t v, PowerupType which) {
        std::int32_t lim = tn.limits[static_cast<int>(which)];
        return lim > 0 ? std::min(v, lim) : v;
    };
    switch (t) {
        case PowerupType::ExtraBomb: p.max_bombs = limited(p.max_bombs + 1, t); break;
        case PowerupType::Flame: p.flame = limited(p.flame + 1, t); break;
        case PowerupType::Goldflame: p.flame = 99; break;
        case PowerupType::Skate:
            p.skates = limited(p.skates + 1, t);
            p.speed = tn.start_speed + p.skates * tn.skate_speed_bonus;
            break;
        case PowerupType::Kick: p.kick = true; break;
        case PowerupType::Punch: p.punch = true; break;
        case PowerupType::Grab: p.grab = true; break;
        case PowerupType::Spooger: p.spooge = true; break;
        case PowerupType::Trigger: p.trigger = true; break;
        case PowerupType::Jelly: p.jelly = true; break;
        default: break;
    }
}

void PowerupSystem::remove(Player& p, PowerupType t) {
    switch (t) {
        case PowerupType::ExtraBomb: if (p.max_bombs > 1) --p.max_bombs; break;
        case PowerupType::Flame: if (p.flame > 1) --p.flame; break;
        case PowerupType::Skate:
            if (p.skates > 0) --p.skates;
            p.speed = s_.tuning.start_speed + p.skates * s_.tuning.skate_speed_bonus;
            break;
        case PowerupType::Kick: p.kick = false; break;
        case PowerupType::Punch: p.punch = false; break;
        case PowerupType::Grab: p.grab = false; break;
        case PowerupType::Spooger: p.spooge = false; break;
        case PowerupType::Trigger: p.trigger = false; break;
        case PowerupType::Jelly: p.jelly = false; break;
        default: break;
    }
}

void PowerupSystem::scatter(int cx, int cy, PowerupType t) {
    State& s = s_;
    for (int r = 0; r < kGridWidth + kGridHeight; ++r) {
        for (int dy = -r; dy <= r; ++dy) {
            for (int dx = -r; dx <= r; ++dx) {
                if (std::max(std::abs(dx), std::abs(dy)) != r) continue;
                int x = cx + dx, y = cy + dy;
                if (x < 0 || x >= kGridWidth || y < 0 || y >= kGridHeight) continue;
                if (s.cells[y][x] != Cell::Blank || s.burning[y][x] > 0) continue;
                if (s.floor[y][x] != PowerupType::None) continue;
                s.floor[y][x] = t;
                return;
            }
        }
    }
}

void PowerupSystem::head_hit(int victim, int tx, int ty) {
    State& s = s_;
    Player& p = s.players[victim];
    p.stun = std::max(p.stun, s.tuning.head_stun_frames);

    // Every upgrade beyond the starting inventory is a candidate token.
    std::vector<PowerupType> tokens;
    auto add = [&](PowerupType t, int n) {
        for (int i = 0; i < n; ++i) tokens.push_back(t);
    };
    add(PowerupType::ExtraBomb, p.max_bombs - s.tuning.start_with[0]);
    add(PowerupType::Flame, p.flame - s.tuning.start_with[1]);
    add(PowerupType::Skate, p.skates);
    if (p.kick) tokens.push_back(PowerupType::Kick);
    if (p.punch) tokens.push_back(PowerupType::Punch);
    if (p.grab) tokens.push_back(PowerupType::Grab);
    if (p.spooge) tokens.push_back(PowerupType::Spooger);
    if (p.trigger) tokens.push_back(PowerupType::Trigger);
    if (p.jelly) tokens.push_back(PowerupType::Jelly);

    int n = s.tuning.powers_lost_min;
    if (s.tuning.powers_lost_rand > 0)
        n += static_cast<int>(random_below(s, s.tuning.powers_lost_rand + 1));
    for (int i = 0; i < n && !tokens.empty(); ++i) {
        std::uint32_t pick = random_below(s, static_cast<std::uint32_t>(tokens.size()));
        PowerupType t = tokens[pick];
        tokens.erase(tokens.begin() + pick);
        remove(p, t);
        scatter(tx, ty, t);
    }
    s.events.push_back({Event::Type::HeadHit, static_cast<std::int8_t>(victim),
                        static_cast<std::int8_t>(tx), static_cast<std::int8_t>(ty), 0});
}

}  // namespace bomber::sim
