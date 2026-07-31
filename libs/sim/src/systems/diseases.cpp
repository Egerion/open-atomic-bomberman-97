#include "systems/diseases.hpp"

#include <array>
#include <cstdlib>
#include <utility>

#include "bomber/sim/rng.hpp"

namespace bomber::sim {

namespace {

// Per-disease ASSIGNMENT metadata (give()): whether the roll installs a
// persistent per-kind flag + shared countdown, and which Tuning::disease_frames
// slot times it. The disease EFFECTS are NOT here — they stay inline at their
// nine gameplay hooks (movement Slow/Fast/Reversed, bombs ShortFuse/ShortFlame,
// auto-drop Diarrhea/Super, ...). Only Swap is transient: it teleport-swaps at
// assignment and leaves no flag, so its whole effect lives in give() below.
//
// ENUM-ORDER-FROZEN: kDiseases is indexed by static_cast<int>(Disease), and the
// assignment roll casts random_below(kDiseaseKinds) straight to Disease
// (assign_random). NEVER insert or reorder entries — append a new disease before
// the end and bump kDiseaseKinds only, or the roll distribution / golden shift.
struct DiseaseSpec {
    Disease tag;      // identity (== the array index as an enum), for legibility
    bool persistent;  // installs a per-kind flag + shared countdown (false only for Swap)
    int duration_id;  // index into Tuning::disease_frames for the countdown length
};

constexpr std::array<DiseaseSpec, kDiseaseKinds> kDiseases = {{
    {Disease::Slow,         true,  0},
    {Disease::Fast,         true,  1},
    {Disease::Constipation, true,  2},
    {Disease::Diarrhea,     true,  3},
    {Disease::ShortFlame,   true,  4},
    {Disease::Super,        true,  5},
    {Disease::ShortFuse,    true,  6},
    {Disease::Swap,         false, 7},  // transient — teleport-swap, no flag/countdown
    {Disease::Reversed,     true,  8},
}};

}  // namespace

void DiseaseSystem::clear(Player& p) {
    // diseases.md finding 2: neither original cure site touches the freshness
    // field (+128). sub_41DF4C (the direct cure) zeroes only age (+120),
    // duration (+124) and the 14-byte flag block (+132..+145); the contagion
    // source-clear (multiply off) likewise leaves +128 alone. disease_fresh is
    // written ONLY on a fresh infection (give()) and read only via a live
    // disease (disease_timer > 0), so a stale value on a healthy player is
    // unobservable — but zeroing it here diverged from a byte-accurate oracle
    // mirror on that hashed field alone. Leave it untouched to stay byte-exact.
    p.disease.fill(false);
    p.disease_timer = 0;
}

void DiseaseSystem::give(int idx, Disease d, bool announce) {
    State& s = s_;
    Player& p = s.players[idx];
    // Announce FIRST — BEFORE the Swap branch ever checks for a target
    // (diseases.md finding 1). sub_41DFB6 resolves and plays the pickup voice
    // line as soon as the rolled disease id and the announce argument are known
    // (batch_0x41DAA7.cpp:308-314), strictly before the Swap target scan at line
    // 315. So a rolled Swap that finds nobody to swap with STILL makes its sound;
    // the sound is a pure function of the id plus the flag, never of whether the
    // scan succeeded. The announce is NOT a State::rng draw — the
    // per-disease-vs-"oh no" pick is presentation-side (determinism rule 6).
    if (announce)
        s.events.push_back({Event::Type::Infected, static_cast<std::int8_t>(idx), -1, -1,
                            static_cast<std::int8_t>(d)});
    const DiseaseSpec& sp = kDiseases[static_cast<int>(d)];
    if (sp.persistent) {
        const int i = static_cast<int>(d);
        p.disease[i] = true;
        p.disease_timer = s.tuning.disease_frames[sp.duration_id];  // duration_id == i
        p.disease_fresh = s.tuning.disease_freshness;
        return;
    }

    // Swap. sub_41DFB6's target scan uses the SAME "valid other player" test its
    // contagion sibling uses: not the player itself, presence byte +16 set, and
    // the +8 dword zero (pseudo.c 22073). That +8 is died-this-round = our
    // !alive, NOT the +58 stun, so a stunned-but-alive player is a valid target
    // (an earlier mislabel added a spurious `stun == 0`).
    std::array<int, kMaxPlayers> targets{};
    int n = 0;
    for (int j = 0; j < kMaxPlayers; ++j)
        if (j != idx && s.players[j].present && s.players[j].alive) targets[n++] = j;
    if (n == 0) return;
    // The original's swap is a 2-field XOR trick on the integer-pixel position
    // ONLY (+0x1c/+0x20). Nothing else moves: no facing, no carried bomb, no
    // move_budget — an earlier port also swapped move_budget, which has no
    // counterpart in sub_41DFB6.
    Player& q = s.players[targets[random_below(s, static_cast<std::uint32_t>(n))]];
    std::swap(p.x, q.x);
    std::swap(p.y, q.y);
}

void DiseaseSystem::assign_random(int idx, int count) {
    // One rand % 9 per disease (sub_41DFB6; the 200-try reroll loop there is
    // NET-GAME-only — a local game accepts the first roll). A Swap with no valid
    // target is simply LOST and does NOT reroll into a different disease, but
    // give() is still called unconditionally so the ANNOUNCE fires either way.
    //
    // DOCUMENTED INTERNAL-RNG DEVIATION: give()'s target pick is one draw over
    // the valid set where the original runs a 200-try random-player scan. Same
    // outcome distribution, and no draw at all when the set is empty — matching
    // the original's fall-through, so the RNG COUNT is unchanged.
    for (int c = 0; c < count; ++c) {
        auto d = static_cast<Disease>(random_below(s_, kDiseaseKinds));
        give(idx, d, c == 0);
    }
}

void DiseaseSystem::maybe_cure_on_pickup(Player& p) {
    if (p.disease_timer > 0 && s_.tuning.diseases_curable && s_.tuning.disease_cure_chance > 0 &&
        random_below(s_, static_cast<std::uint32_t>(s_.tuning.disease_cure_chance)) == 0)
        clear(p);
}

// sub_41F29B processes each player in slot order as freshness-- (~22927), then
// age += frameDelta / cure (~22929-22942), all nested inside the ALIVE gate at
// ~22904 — which requires the +8 dword (died-this-round = our !alive) to be zero,
// NOT the +58 head-hit stun, which is decremented INSIDE this same block at
// ~22982 and so cannot gate it. A merely-stunned-but-alive player DOES age and
// spread (facts.md "Stun does NOT gate flame-death or pickup"; an earlier
// mislabel added a `stun > 0` skip here).
void DiseaseSystem::age_and_expire() {
    for (Player& p : s_.players) {
        if (!p.present || !p.alive) continue;
        // Freshness (+128) is a RAW per-FRAME counter — sub_41F29B decrements it
        // by 1 every displayed frame (batch_0x41F29B.cpp:279-281), exactly like
        // the sibling head-stun +58 (:331) that player_turn already burns
        // kSubFrames per tick. It is NOT the ms-delta disease AGE (+120, which is
        // 1/tick below). Burning it 1/tick left a freshly-infected player
        // contagion-locked ~10 TICKS instead of the native's ~10 FRAMES, so
        // multi-hop spread propagated ~kSubFrames x too slowly. (facts.md's old
        // "counts down by 1/tick" note was stale, pre-ADR-0006.)
        if (p.disease_fresh > 0)
            p.disease_fresh = p.disease_fresh > kSubFrames ? p.disease_fresh - kSubFrames : 0;
        // Expiry is UNCONDITIONAL (facts.md "VALUELST id 121 is dead in the
        // original"). The per-frame ager at 0x41F671-0x41F697 tests only the age
        // field, adds the frame delta, compares against the duration (+124) and
        // cures via sub_41DF4C — it reads no global at all. getvalue(121) IS read
        // once, at 0x410A5B, into dword_464988, and a whole-image scan finds that
        // one write and NO reader. Gating on it invented permanent,
        // permanently-infectious diseases the original cannot produce.
        // Tuning::diseases_time_limited stays (it is on the MatchConfig wire) but
        // is inert.
        if (p.disease_timer > 0 && --p.disease_timer <= 0) clear(p);
    }
}

// Contagion from one source (sub_41F29B ~22943): a diseased player overlapping a
// healthy one hands the whole set over. Both ends must be ALIVE — the target test
// at pseudo.c 22951 is on the target's own +8 dword, not the +58 stun, so a
// stunned-but-alive player both spreads and catches. multiply = 1 (default)
// infects EVERY valid target found this tick and the source keeps it; multiply =
// 0 infects only the first in slot order and clears the source right there,
// ending the scan. Contagion is silent in the original — no event.
void DiseaseSystem::spread_from(int i) {
    State& s = s_;
    Player& src = s.players[i];
    if (!src.present || !src.alive || src.disease_timer <= 0 || src.disease_fresh > 0) return;
    for (int j = 0; j < kMaxPlayers; ++j) {
        if (j == i) continue;
        Player& dst = s.players[j];
        if (!dst.present || !dst.alive || dst.disease_timer > 0) continue;
        const int dx = (src.x - dst.x) / kScale, dy = (src.y - dst.y) / kScale;
        if (std::abs(dx) > kTileW - 10 || std::abs(dy) > kTileH - 10)
            continue;  // the |30|/|26| box
        dst.disease = src.disease;
        dst.disease_timer = src.disease_timer;
        dst.disease_fresh = s.tuning.disease_freshness;
        if (s.tuning.diseases_multiply) continue;
        clear(src);
        return;
    }
}

// The order is load-bearing: the original cures an expiring disease (zeroing
// +120) BEFORE that player's own contagion scan runs, so a player on its last
// sick tick does not spread. Splitting it into two whole-roster passes preserves
// that per-player relationship.
//
// It deliberately does NOT reproduce the original's single-pass cross-player
// quirk, where a source at a LOWER slot index hands a target at a HIGHER index a
// disease that then gets one bonus age-tick the same frame, because the target's
// own turn still runs later in that sweep. That is an index-order-dependent
// sub-tick artifact of in-place mutation, in the same "documented, no
// player-visible effect" class as the swap-target-pick and healthy-cure-roll
// notes in facts.md "Core-feel audit 2026-07-10".
void DiseaseSystem::spread_and_age() {
    age_and_expire();
    for (int i = 0; i < kMaxPlayers; ++i) spread_from(i);
}

}  // namespace bomber::sim
