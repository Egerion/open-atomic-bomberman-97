#include "systems/powerups.hpp"

#include <algorithm>
#include <array>

#include "bomber/sim/rng.hpp"
#include "grid.hpp"

namespace bomber::sim {

namespace {

// Per-kind powerup behaviour, collapsing the five parallel per-type switches
// (apply / remove / held_count / reset_to_baseline / evict) into one data table.
//
// ENUM-ORDER-FROZEN: kPowerups is indexed by static_cast<int>(PowerupType), and
// the RNG roll->kind mappings cast an integer index straight to PowerupType
// (simulation.cpp Random reroll, head_hit's rand%15, death_scatter's kind loop).
// NEVER insert or reorder entries — append a new kind before the end and bump
// kPowerupKinds only, or the roll distribution and the golden hashes shift.
//
// Storage: `count` (ExtraBomb/Flame/Skate accumulation byte) OR `flag` (the seven
// boolean glove/ability kinds). Disease/SuperDisease/Random carry no per-kind
// count, so both are null and every op no-ops on them (the original's default:).
//
// Mutual exclusion (sub_41E21E via sub_41E16A): punch<->trigger, grab<->spooger,
// trigger<->jelly evict each other and Trigger additionally drops Punch. Eviction
// is NOT a silent clear — evict() SCATTERS the surplus token (drawing State::rng,
// an order/count contract) and, for Trigger, downgrades the player's live trigger
// bombs (sub_424C47). The evictN fields carry that in the ORDER the scatter draws
// must follow: Trigger drops Punch (evict1) THEN Jelly (evict2). facts.md
// "Core-feel audit" §2. Goldflame stores only its flag (+94); the blast reach is
// computed at drop time (BombSystem::place, sub_41EB13). Trigger also resets the
// live-trigger allowance (+85 = 0) via reset_trigger_placed.
struct PowerupSpec {
    std::int32_t Player::* count = nullptr;   // accumulation byte (ExtraBomb/Flame/Skate)
    bool Player::* flag = nullptr;            // boolean kind (Kick/Punch/Grab/Spooger/Goldflame/Trigger/Jelly)
    std::int32_t count_min = 0;               // remove() decrement floor (1 for bombs/flame, 0 for skate)
    bool affects_speed = false;               // Skate recomputes the derived speed stat on apply/remove
    bool reset_trigger_placed = false;        // Trigger refills the live-trigger allowance (+85 = 0)
    PowerupType evict1 = PowerupType::None;   // apply-time mutual-exclusion eviction #1
    PowerupType evict2 = PowerupType::None;   // apply-time eviction #2 (Trigger: Punch then Jelly)
};

constexpr std::array<PowerupSpec, kPowerupKinds> kPowerups = {{
    /* 0  ExtraBomb    */ {&Player::max_bombs, nullptr, 1, false, false, PowerupType::None, PowerupType::None},
    /* 1  Flame        */ {&Player::flame, nullptr, 1, false, false, PowerupType::None, PowerupType::None},
    /* 2  Disease      */ {nullptr, nullptr, 0, false, false, PowerupType::None, PowerupType::None},
    /* 3  Kick         */ {nullptr, &Player::kick, 0, false, false, PowerupType::None, PowerupType::None},
    /* 4  Skate        */ {&Player::skates, nullptr, 0, true, false, PowerupType::None, PowerupType::None},
    /* 5  Punch        */ {nullptr, &Player::punch, 0, false, false, PowerupType::Trigger, PowerupType::None},
    /* 6  Grab         */ {nullptr, &Player::grab, 0, false, false, PowerupType::Spooger, PowerupType::None},
    /* 7  Spooger      */ {nullptr, &Player::spooge, 0, false, false, PowerupType::Grab, PowerupType::None},
    /* 8  Goldflame    */ {nullptr, &Player::goldflame, 0, false, false, PowerupType::None, PowerupType::None},
    /* 9  Trigger      */ {nullptr, &Player::trigger, 0, false, true, PowerupType::Punch, PowerupType::Jelly},
    /* 10 Jelly        */ {nullptr, &Player::jelly, 0, false, false, PowerupType::Trigger, PowerupType::None},
    /* 11 SuperDisease */ {nullptr, nullptr, 0, false, false, PowerupType::None, PowerupType::None},
    /* 12 Random       */ {nullptr, nullptr, 0, false, false, PowerupType::None, PowerupType::None},
}};

}  // namespace

// The derived speed stat (sub_41F29B's per-tick `base + skates*getvalue(90) -
// clogs*getvalue(91)`, baked here), recomputed whenever the skate count changes
// (apply/remove of a Skate). death_scatter's reset deliberately does NOT call it:
// the original writes only the count byte on death, and a dead player's speed is
// never read again.
void PowerupSystem::recompute_speed(Player& p) {
    const Tuning& tn = s_.tuning;
    p.speed = tn.start_speed + p.skates * tn.skate_speed_bonus - p.clogs * tn.clogs_speed_penalty;
}

void PowerupSystem::apply(Player& p, PowerupType t) {
    const int kind = static_cast<int>(t);
    if (kind >= kPowerupKinds) return;  // None / out-of-range: no-op (the original's default:)
    const PowerupSpec& sp = kPowerups[kind];
    if (sp.count) {
        // Per-kind accumulation limit (VALUELST 550..562): cap only when > 0.
        const std::int32_t lim = s_.tuning.limits[kind];
        const std::int32_t v = p.*sp.count + 1;
        p.*sp.count = (lim > 0) ? std::min(v, lim) : v;
    } else if (sp.flag) {
        p.*sp.flag = true;
    }
    if (sp.reset_trigger_placed) p.trigger_placed = 0;
    if (sp.affects_speed) recompute_speed(p);
    // Mutual-exclusion evictions LAST, in table order — the scatter RNG draws
    // (Trigger: Punch then Jelly) are part of the determinism contract.
    if (sp.evict1 != PowerupType::None) evict(p, sp.evict1);
    if (sp.evict2 != PowerupType::None) evict(p, sp.evict2);
}

void PowerupSystem::remove(Player& p, PowerupType t) {
    const int kind = static_cast<int>(t);
    if (kind >= kPowerupKinds) return;  // None / out-of-range: no-op
    const PowerupSpec& sp = kPowerups[kind];
    if (sp.count) {
        if (p.*sp.count > sp.count_min) --(p.*sp.count);
    } else if (sp.flag) {
        p.*sp.flag = false;
    }
    // Skate: recompute keeps the clogs penalty term (sub_41F29B's per-tick
    // `base + skates*getvalue(90) - clogs*getvalue(91)`), unconditionally after
    // the guarded decrement — as the original does.
    if (sp.affects_speed) recompute_speed(p);
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
    // evict() is only ever called with the five flag kinds, whose held_count is
    // the boolean flag as 0/1 — so `> 0` reproduces the original per-kind switch
    // (and the kind-guard keeps a hypothetical out-of-range kind at false, the
    // switch's old default).
    const bool held = kind < kPowerupKinds && held_count(p, kind) > 0;
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
    // Count kinds report their accumulation byte; flag kinds report 0/1 (so a set
    // Goldflame flag, whose start-with baseline is 0, counts as surplus); the
    // no-storage kinds (disease/superdisease/random & pad slots) report 0.
    const PowerupSpec& sp = kPowerups[kind];
    if (sp.count) return p.*sp.count;
    if (sp.flag) return p.*sp.flag ? 1 : 0;
    return 0;
}

// Write a kind's accumulated count back to its start-with baseline. Mirrors
// sub_41DBFE / sub_41E16A, which write ONLY the count byte: the derived speed
// stat (+70/+74) is left untouched, so a scattered skate does NOT recompute
// `speed` here — the field is only ever reset on a dead player (death_scatter)
// whose speed is never read again, keeping the byte-level write faithful.
void PowerupSystem::reset_to_baseline(Player& p, int kind, int baseline) {
    // Count kinds are written back to the baseline; flag kinds are cleared to
    // false (the original ignores `baseline` for flags — every flag kind's
    // start-with is 0, so false == baseline anyway).
    const PowerupSpec& sp = kPowerups[kind];
    if (sp.count)
        p.*sp.count = baseline;
    else if (sp.flag)
        p.*sp.flag = false;
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
    // sub_421F7E also sets the player-state word +78 to 3 and zeroes the anim
    // counter +80 — an UNCONDITIONAL
    // overwrite of that state word — and its caller's victim probe
    // (sub_421CB5, pseudo.c 24207) accepts any active-and-not-dead player with
    // NO +78 guard. The original therefore cannot hold state 4 (pickup-pause),
    // 5 (trampoline hop) or 6/7 (warp out/in) past a head hit: the ONE state
    // word is clobbered to 3, cancelling the pause/flight in place (a warp hit
    // during warp-out never relocates; during warp-in it stays at the exit,
    // since the position writes only happen inside the state-6/7 branches the
    // player no longer takes). Our port keeps these as separate fields, so
    // mirror the overwrite explicitly — without this, "bouncing/warping while
    // head-stunned" is a flag combination the original cannot express
    // (facts.md "Player state machine (+78) — COMPLETE"). A player left
    // standing on the actor's tile by the cancel does NOT immediately re-trigger
    // it: the only trigger is the mover's along-axis offset reaching -1, which
    // needs a fresh walk INTO the centre from outside (facts.md
    // "Warphole/trampoline entry predicate").
    p.pickup_pause = 0;
    p.bounce = 0;
    p.warp = 0;

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

// A player dies (sub_41DBFE, invoked from sub_41F29B's death-animation branch
// when that animation completes). Scatters EVERY powerup the player accumulated ABOVE
// its VALUELST start-with baseline back onto random floor tiles: iterate the
// kPowerupKinds real kinds (0..12) in index order and, for each, drop the
// surplus. (The native's pad slots 13/14 are seeded to their own baseline by
// sub_4214BC, so they never carry surplus — iterating them would be a no-op.)
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
// The death-animation VARIANT roll (sub_41DE63 draws rand % getvalue(105) and
// adds 1, deciding which
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
