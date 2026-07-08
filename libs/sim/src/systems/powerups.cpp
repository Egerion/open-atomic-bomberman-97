#include "systems/powerups.hpp"

#include <algorithm>

#include "bomber/sim/rng.hpp"
#include "grid.hpp"

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
        // Goldflame (sub_41E21E case 8 sets flag +94): the flag itself is the
        // state; the blast reach is computed at drop time as max(gridW,gridH)
        // in BombSystem::place (sub_41EB13). Storing a flag (not flame=99) is
        // the literal port and keeps the stored `flame` untouched.
        case PowerupType::Goldflame: p.goldflame = true; break;
        case PowerupType::Skate:
            p.skates = limited(p.skates + 1, t);
            p.speed = tn.start_speed + p.skates * tn.skate_speed_bonus;
            break;
        case PowerupType::Kick: p.kick = true; break;
        // Mutually exclusive glove/bomb kinds (sub_41E21E via sub_41E16A):
        // punch↔trigger, grab↔spooger, trigger↔jelly all evict each other, and
        // trigger additionally drops punch. Picking one strips the conflicting
        // kind(s) — remove() only flips flags (no RNG), so the draw order is
        // unaffected; the hashed flags change, so golden must be recaptured.
        case PowerupType::Punch:
            p.punch = true;
            remove(p, PowerupType::Trigger);
            break;
        case PowerupType::Grab:
            p.grab = true;
            remove(p, PowerupType::Spooger);
            break;
        case PowerupType::Spooger:
            p.spooge = true;
            remove(p, PowerupType::Grab);
            break;
        case PowerupType::Trigger:
            // Trigger pickup (sub_41E21E case 9) resets the live-trigger
            // counter (+85 = 0), refilling the placement allowance to a fresh
            // max_bombs, then sets the flag and evicts punch + jelly.
            p.trigger_placed = 0;
            p.trigger = true;
            remove(p, PowerupType::Punch);
            remove(p, PowerupType::Jelly);
            break;
        case PowerupType::Jelly:
            p.jelly = true;
            remove(p, PowerupType::Trigger);
            break;
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
        case PowerupType::Goldflame: p.goldflame = false; break;
        case PowerupType::Punch: p.punch = false; break;
        case PowerupType::Grab: p.grab = false; break;
        case PowerupType::Spooger: p.spooge = false; break;
        case PowerupType::Trigger: p.trigger = false; break;
        case PowerupType::Jelly: p.jelly = false; break;
        default: break;
    }
}

// Faithful port of the token drop (sub_4255B2): the token lands on a RANDOM
// tile — up to 100 placement attempts, each with an inner budget of 100
// coordinate rolls. Two RNG draws per roll (x then y) — the draw pattern is
// part of the contract. The token is LOST if everything fails.
//
// Occupancy predicate (docs/re/facts.md "Scatter occupancy test", pinned
// from sub_4255B2 @ pseudo.c 26458-26479): a re-rolled tile first needs
// sub_425FB9(x,y) == 0 (blank cell type; solid/brick just re-roll silently,
// burning NO placement attempt). Past that, the tile is rejected — burning
// ONE placement attempt — when it holds a GROUNDED bomb (sub_422E48), ANY
// powerup record (sub_42542D, hidden or visible), OR a live player
// (sub_421CB5). This is facts.md's flagged fidelity gap: our previous port
// also rejected on FLAME (never checked by the original — a scattered token
// can land on burning ground) and never checked for a PLAYER standing on the
// tile (the original does). Fixed to match the binary predicate exactly.
void PowerupSystem::scatter(PowerupType t) {
    State& s = s_;
    int outer = 0;
    while (outer < 100) {
        int guard = 100;
        while (true) {
            int x = static_cast<int>(random_below(s, kGridWidth));
            int y = static_cast<int>(random_below(s, kGridHeight));
            if (--guard <= 0) return;  // give up entirely (token lost)
            if (s.cells[y][x] != Cell::Blank || s.burning[y][x] > 0) continue;
            if (grid::bomb_at(s, x, y) || s.floor[y][x] != PowerupType::None ||
                grid::player_at(s, x, y)) {
                ++outer;  // occupied floor tile: burn one placement attempt
                break;
            }
            s.floor[y][x] = t;
            return;
        }
    }
}

// A bomb bonks a player on the head (sub_421F7E): a hardcoded 16-tick stun
// (the +58 countdown), then powers_lost_min + rand % powers_lost_rand
// upgrades are picked by ROLLING A KIND (rand % 15, up to 200 tries) that
// the player holds above the VALUELST start-with baseline; each hit kind is
// removed and its token scattered to a random tile.
void PowerupSystem::head_hit(int victim, int tx, int ty) {
    State& s = s_;
    Player& p = s.players[victim];
    p.stun = s.tuning.head_stun_frames;  // plain overwrite, as the original

    auto surplus = [&](int kind) -> bool {
        int have = 0;
        switch (static_cast<PowerupType>(kind)) {
            case PowerupType::ExtraBomb: have = p.max_bombs; break;
            case PowerupType::Flame: have = p.flame; break;
            case PowerupType::Kick: have = p.kick ? 1 : 0; break;
            case PowerupType::Skate: have = p.skates; break;
            case PowerupType::Punch: have = p.punch ? 1 : 0; break;
            case PowerupType::Grab: have = p.grab ? 1 : 0; break;
            case PowerupType::Spooger: have = p.spooge ? 1 : 0; break;
            // Goldflame (kind 8, byte +94) IS a droppable head-hit kind in the
            // original: sub_421F7E rolls `rand()%15` uniformly over ALL kinds and
            // accepts any whose per-kind count `player[+86+kind]` exceeds the
            // VALUELST start-with baseline getvalue(50+kind). id 58 (goldflame
            // start-with) = 0, so a set goldflame flag counts as surplus and the
            // token scatters like the others (remove() clears +94, scatter()
            // drops a Goldflame token). Confirmed against sub_421F7E; enabling it
            // shifts the head-hit kind-roll acceptance (hence the per-hit RNG
            // draw count) → GOLDEN.
            case PowerupType::Goldflame: have = p.goldflame ? 1 : 0; break;
            case PowerupType::Trigger: have = p.trigger ? 1 : 0; break;
            case PowerupType::Jelly: have = p.jelly ? 1 : 0; break;
            default: return false;  // disease/random & pad kinds: no count
        }
        return kind < kPowerupKinds && have > s.tuning.start_with[kind];
    };

    int n = s.tuning.powers_lost_min +
            static_cast<int>(random_below(
                s, static_cast<std::uint32_t>(
                       std::max<std::int32_t>(1, s.tuning.powers_lost_rand))));
    for (int i = 0; i < n; ++i) {
        for (int tries = 0; tries < 200; ++tries) {
            int kind = static_cast<int>(random_below(s, 15));
            if (!surplus(kind)) continue;
            auto t = static_cast<PowerupType>(kind);
            remove(p, t);
            scatter(t);
            break;
        }
    }
    s.events.push_back({Event::Type::HeadHit, static_cast<std::int8_t>(victim),
                        static_cast<std::int8_t>(tx), static_cast<std::int8_t>(ty), 0});
}

}  // namespace bomber::sim
