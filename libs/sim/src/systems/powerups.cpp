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
            p.speed = tn.start_speed + p.skates * tn.skate_speed_bonus -
                      p.clogs * tn.clogs_speed_penalty;
            break;
        case PowerupType::Kick: p.kick = true; break;
        // Mutually exclusive glove/bomb kinds (sub_41E21E via sub_41E16A):
        // punch↔trigger, grab↔spooger, trigger↔jelly all evict each other, and
        // trigger additionally drops punch. Eviction is NOT a silent flag
        // clear: sub_41E16A SCATTERS the evicted token back onto a random
        // floor tile (sub_425BED -> sub_4255B2) whenever the count exceeds the
        // VALUELST start-with baseline, and evicting Trigger additionally
        // DOWNGRADES the player's live trigger bombs to normal timed bombs
        // with a fresh fuse (sub_424C47). The scatter draws RNG (order/count
        // contract) and the flags are hashed — golden recaptured.
        // facts.md "Core-feel audit" §2.
        case PowerupType::Punch:
            p.punch = true;
            evict(p, PowerupType::Trigger);
            break;
        case PowerupType::Grab:
            p.grab = true;
            evict(p, PowerupType::Spooger);
            break;
        case PowerupType::Spooger:
            p.spooge = true;
            evict(p, PowerupType::Grab);
            break;
        case PowerupType::Trigger:
            // Trigger pickup (sub_41E21E case 9) resets the live-trigger
            // counter (+85 = 0), refilling the placement allowance to a fresh
            // max_bombs, then sets the flag and evicts punch + jelly (in that
            // order — the scatter draws must follow it).
            p.trigger_placed = 0;
            p.trigger = true;
            evict(p, PowerupType::Punch);
            evict(p, PowerupType::Jelly);
            break;
        case PowerupType::Jelly:
            p.jelly = true;
            evict(p, PowerupType::Trigger);
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
            // Recompute keeps the clogs penalty term (sub_41F29B's per-tick
            // `base + skates*getvalue(90) - clogs*getvalue(91)`; we bake it).
            p.speed = s_.tuning.start_speed + p.skates * s_.tuning.skate_speed_bonus -
                      p.clogs * s_.tuning.clogs_speed_penalty;
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

// Mutual-exclusion eviction (sub_41E16A, flag-kind branch — sub_425C10 is
// true for exactly the five kinds the dispatcher ever evicts): when the
// player holds the kind above its VALUELST start-with baseline, the surplus
// token is SCATTERED back onto a random floor tile and the count drops to
// the baseline. Evicting Trigger with the flag ending cleared additionally
// converts the player's live trigger bombs to normal timed bombs with a
// fresh full fuse (sub_424C47: kind = 0, fuse-elapsed = 0) — they can no
// longer be detonated and will now explode on their own.
void PowerupSystem::evict(Player& p, PowerupType t) {
    const int kind = static_cast<int>(t);
    const bool baseline = kind < kPowerupKinds && s_.tuning.start_with[kind] > 0;
    const bool held = [&] {
        switch (t) {
            case PowerupType::Punch: return p.punch;
            case PowerupType::Grab: return p.grab;
            case PowerupType::Spooger: return p.spooge;
            case PowerupType::Trigger: return p.trigger;
            case PowerupType::Jelly: return p.jelly;
            default: return false;  // only flag kinds are ever evicted
        }
    }();
    if (held && !baseline) {
        scatter(t);  // sub_425BED before the count write, same draw order
        remove(p, t);
    }
    if (t == PowerupType::Trigger && !p.trigger) {
        const int owner = static_cast<int>(&p - s_.players.data());
        for (auto& b : s_.bombs)
            if (b.active && b.trigger && b.owner == owner) {
                b.trigger = false;
                b.fuse = b.fuse_init;  // relit from scratch (elapsed = 0)
            }
        // sub_424C47 matches on the bomb KIND alone — a trigger bomb of this
        // owner riding in someone's hands (motion 3 in the original; our
        // carried_* fields) converts too, and lands as a normal timed bomb.
        for (auto& q : s_.players)
            if (q.present && q.carrying && q.carried_trigger && q.carried_owner == owner)
                q.carried_trigger = false;
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

// Current accumulated count of a powerup kind (the original's per-kind player
// bytes +86..+96). The `+86+kind` layout maps kind -> byte directly (kind 0
// bombs=+86, 1 flame=+87, 3 kick=+89, 4 skate=+90, 5 punch=+91, 6 grab=+92,
// 7 spooge=+93, 8 goldflame=+94, 9 trigger=+95, 10 jelly=+96); kinds with no
// per-kind count (2 disease, 11 superdisease, 12 random, plus the pad slots
// the original loops over) report 0, so they never register surplus.
int PowerupSystem::held_count(const Player& p, int kind) const {
    switch (static_cast<PowerupType>(kind)) {
        case PowerupType::ExtraBomb: return p.max_bombs;
        case PowerupType::Flame: return p.flame;
        case PowerupType::Kick: return p.kick ? 1 : 0;
        case PowerupType::Skate: return p.skates;
        case PowerupType::Punch: return p.punch ? 1 : 0;
        case PowerupType::Grab: return p.grab ? 1 : 0;
        case PowerupType::Spooger: return p.spooge ? 1 : 0;
        // Goldflame (kind 8, byte +94) IS a droppable kind: id 58 (goldflame
        // start-with) = 0, so a set flag counts as surplus over the baseline.
        case PowerupType::Goldflame: return p.goldflame ? 1 : 0;
        case PowerupType::Trigger: return p.trigger ? 1 : 0;
        case PowerupType::Jelly: return p.jelly ? 1 : 0;
        default: return 0;  // disease/superdisease/random & pad kinds: no count
    }
}

// Write a kind's accumulated count back to its start-with baseline. Mirrors
// sub_41DBFE / sub_41E16A, which write ONLY the count byte: the derived speed
// stat (+70/+74) is left untouched, so a scattered skate does NOT recompute
// `speed` here — the field is only ever reset on a dead player (death_scatter)
// whose speed is never read again, keeping the byte-level write faithful.
void PowerupSystem::reset_to_baseline(Player& p, int kind, int baseline) {
    switch (static_cast<PowerupType>(kind)) {
        case PowerupType::ExtraBomb: p.max_bombs = baseline; break;
        case PowerupType::Flame: p.flame = baseline; break;
        case PowerupType::Skate: p.skates = baseline; break;
        case PowerupType::Kick: p.kick = false; break;
        case PowerupType::Punch: p.punch = false; break;
        case PowerupType::Grab: p.grab = false; break;
        case PowerupType::Spooger: p.spooge = false; break;
        case PowerupType::Goldflame: p.goldflame = false; break;
        case PowerupType::Trigger: p.trigger = false; break;
        case PowerupType::Jelly: p.jelly = false; break;
        default: break;  // no count field to reset
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
        return kind < kPowerupKinds && held_count(p, kind) > s.tuning.start_with[kind];
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

// A player dies (sub_41DBFE, invoked from sub_41F29B LABEL_26 when the death
// animation completes). Scatters EVERY powerup the player accumulated ABOVE
// its VALUELST start-with baseline back onto random floor tiles: iterate kinds
// 0..14 in index order and, for each, drop the surplus.
//
// This is NOT the head hit. The head hit drops a rand-limited COUNT
// (getvalue(670)+rand%getvalue(671)) of RANDOMLY ROLLED kinds (rand%15); death
// drops the player's WHOLE surplus with NO kind roll and NO count roll. The
// only RNG draws are sub_4255B2's per-token tile selection (via scatter()), in
// kind order — THAT sequence is the determinism contract. The original splits
// flag kinds (sub_425C10: 5/6/7/9/10 -> scatter one, reset) from counted kinds
// (scatter one per surplus, decrement); since every real flag kind's count is
// 0/1, a single `surplus = have - baseline` loop reproduces BOTH branches'
// draw order and board result exactly.
//
// The death-animation VARIANT roll (sub_41DE63 `rand%getvalue(105)+1`, which
// DIE*.ANI plays) is a cosmetic death-sprite pick and stays presentation-side
// per CLAUDE.md determinism rule 6 — it is NOT drawn on State::rng, so this
// sim's stream carries only the scatter draws.
//
// TIMING — a deliberate, documented divergence: the original defers this
// scatter to the death animation's final frame (tens of ticks later; the
// DIE*.ANI length is asset data the SDL-free sim must not know). We scatter on
// the death TICK, the same animation-delay collapse this sim applies
// everywhere else (a dead player is immediately inert). The scatter CONTENTS
// (kinds/counts/tiles) and the RNG arithmetic are identical; only the tick the
// tokens appear differs. The dead player is already `alive = false` at every
// call site, so — matching the original, where +8 (dead) is set before the
// anim-end scatter — sub_4255B2's live-player occupancy check (grid::player_at,
// which gates on `alive`) lets a token land on the victim's own tile.
void PowerupSystem::death_scatter(Player& p) {
    for (int kind = 0; kind < kPowerupKinds; ++kind) {
        const int baseline = s_.tuning.start_with[kind];
        const auto t = static_cast<PowerupType>(kind);
        for (int have = held_count(p, kind); have > baseline; --have) scatter(t);
        reset_to_baseline(p, kind, baseline);
    }
}

}  // namespace bomber::sim
