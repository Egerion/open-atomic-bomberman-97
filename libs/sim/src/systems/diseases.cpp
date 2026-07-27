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
    // Announce FIRST — before the Swap branch ever checks for a target
    // (diseases.md finding 1). sub_41DFB6 (batch_0x41DAA7.cpp:308-314, pseudo.c
    // 26290+/22041+) resolves and plays the pickup voice line as soon as the
    // rolled disease id and the announce argument are known, at lines 308-314 —
    // strictly BEFORE the Swap target scan at line 315, which is the branch
    // taken when that disease id is 7. A rolled
    // Swap that finds nobody alive to swap with therefore STILL makes its sound
    // (the sound is a pure function of the disease id plus the announce flag,
    // never of whether the scan
    // succeeds). The earlier port `continue`d past give() on a no-target Swap,
    // silencing the whole pickup (and, for a SuperDisease skull, all three
    // rolls, since only the first announces). The announce is NOT a State::rng
    // draw: the per-disease-vs-"oh no" pick is presentation-side (SoundDirector,
    // determinism-contract rule 6), so restoring it shifts no RNG.
    if (announce)
        s.events.push_back({Event::Type::Infected, static_cast<std::int8_t>(idx), -1, -1,
                            static_cast<std::int8_t>(d)});
    // Assignment metadata (kDiseases): !persistent selects Swap, the sole
    // transient kind; persistent kinds install their flag + countdown below.
    const DiseaseSpec& sp = kDiseases[static_cast<int>(d)];
    if (!sp.persistent) {
        // sub_41DFB6's target scan requires the SAME "valid other player" test
        // its contagion sibling (sub_41F29B) uses: the candidate must not be the
        // player itself, its presence byte at +16 must be set, the record's
        // leading state dword at +0 participates in the same clause, and the +8
        // dword must be zero (pseudo.c 22073). That +8 dword is the
        // died-this-round flag = our !alive
        // — NOT the +58 head-hit stun countdown, so a stunned-but-alive player
        // is still a valid swap target (facts.md "Stun does NOT gate flame-
        // death or pickup"; an earlier mislabel added a spurious `stun == 0`).
        int targets[kMaxPlayers], n = 0;
        for (int j = 0; j < kMaxPlayers; ++j)
            if (j != idx && s.players[j].present && s.players[j].alive)
                targets[n++] = j;
        if (n > 0) {
            // The original's swap is a 2-field XOR trick on the player's
            // integer-pixel position ONLY (+0x1c/+0x20 — our x/y). Nothing
            // else moves: no facing, no carried bomb, no move_budget. An
            // earlier port also swapped move_budget, which has no
            // counterpart in sub_41DFB6 — removed (facts.md "Disease
            // system").
            Player& q = s.players[targets[random_below(s, static_cast<std::uint32_t>(n))]];
            std::swap(p.x, q.x);
            std::swap(p.y, q.y);
        }
    } else {
        int i = static_cast<int>(d);
        p.disease[i] = true;
        p.disease_timer = s.tuning.disease_frames[sp.duration_id];  // duration_id == i
        p.disease_fresh = s.tuning.disease_freshness;
    }
}

void DiseaseSystem::assign_random(int idx, int count) {
    // One rand % 9 per disease (sub_41DFB6; the 200-try reroll loop there is
    // NET-GAME-only — a local game accepts the first roll). A Swap with no
    // valid target is simply LOST (the original's 200-try random-player scan
    // finds nobody and falls through assigning nothing) — it does NOT reroll
    // into a different disease. give() handles that no-target case itself: its
    // Swap branch scans the valid set and does nothing when it is empty. It is
    // still called unconditionally so the pickup ANNOUNCE fires either way
    // (diseases.md finding 1 — the original announces before the target scan);
    // the earlier `continue` here skipped give() entirely and silenced the cue.
    // Our target pick inside give() replaces the original's 200-try scan with
    // one draw over the valid set (same outcome distribution, documented
    // internal-RNG deviation; no draw at all when the set is empty, matching
    // the original's fall-through — RNG count unchanged by this fix).
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

void DiseaseSystem::spread_and_age() {
    State& s = s_;

    // Age the freshness gate and expire finished diseases FIRST. sub_41F29B
    // processes each player in slot order as freshness-- (~22927), then
    // age+=frameDelta/cure (~22929-22942), THEN that SAME player's own
    // contagion scan (~22943-22974) — all nested inside the ALIVE gate
    // (the gate at ~22904 requires the +8 dword to be zero, +8 being the
    // died-this-round flag = our !alive,
    // NOT the +58 head-hit stun countdown, which is decremented INSIDE this
    // same block at ~22982; a field cannot gate a block that only decrements
    // itself). A merely-stunned-but-alive player DOES age and spread its
    // disease (facts.md "Stun does NOT gate flame-death or pickup" — an
    // earlier mislabel wrongly added a `stun > 0` skip here). Doing
    // age-then-spread in one pass per player, rather
    // than spread-then-age globally, matters on the exact tick a disease
    // expires: the original cures it (zeroing +120) BEFORE the contagion
    // check runs, so an expiring player does not spread on its last tick.
    // Our two-pass split (all players age, then all players spread)
    // reproduces that per-player age-then-spread relationship. It does NOT
    // reproduce the original's single-pass cross-player quirk where a
    // source at a LOWER slot index can hand a target at a HIGHER index a
    // disease that then gets one bonus age-tick the SAME frame (the
    // target's own turn, later in the original's single sweep, still runs
    // after receiving it) — an index-order-dependent, sub-tick artifact of
    // the original's in-place mutation, deliberately not replicated (same
    // "documented, no player-visible effect" spirit as the swap-target-pick
    // and healthy-cure-roll notes in facts.md "Core-feel audit 2026-07-10").
    for (int i = 0; i < kMaxPlayers; ++i) {
        Player& p = s.players[i];
        if (!p.present || !p.alive) continue;  // !+8 (alive) only; +58 stun does not freeze aging
        // Freshness (+128) is a RAW per-FRAME counter in the native — sub_41F29B
        // decrements it by 1 every displayed frame (batch_0x41F29B.cpp:279-281),
        // exactly like the sibling head-stun counter +58 (:331) that this port
        // already burns kSubFrames/tick (player_turn's sub-frame loop). It is NOT
        // the ms-delta disease AGE (+120, decremented 1/tick correctly below).
        // Burning it 1/tick made a freshly-infected player contagion-locked ~10
        // ticks instead of the native's ~10 FRAMES (~1 tick), so multi-hop
        // spread propagated ~kSubFrames× too slowly. Burn kSubFrames/tick to
        // match. (facts.md's old "counts down by 1/tick" note was stale/pre-ADR-
        // 0006.) No RNG draw — order/count unchanged; hashed field -> goldens
        // recaptured.
        if (p.disease_fresh > 0)
            p.disease_fresh = p.disease_fresh > kSubFrames ? p.disease_fresh - kSubFrames : 0;
        if (p.disease_timer > 0 && s.tuning.diseases_time_limited && --p.disease_timer <= 0)
            clear(p);
    }

    // Contagion: a diseased player overlapping a healthy one hands the whole
    // set over (sub_41F29B ~22943, overlap |dx| <= 30 & |dy| <= 26). Both
    // ends must be ALIVE: the source gate below sits inside the same
    // "+8 is zero" alive block as the ager, and the target is tested the same
    // way — the zero-check at pseudo.c 22951 is on the target's own +8 dword
    // (not-dead) — NOT the +58 stun, so a stunned-but-alive
    // player both spreads and catches. multiply=1 (default) infects EVERY
    // valid target found this tick and the source keeps it; multiply=0 infects
    // only the first (slot order) and clears the source right there, ending
    // the scan. Contagion is silent in the original — no event.
    for (int i = 0; i < kMaxPlayers; ++i) {
        Player& src = s.players[i];
        if (!src.present || !src.alive || src.disease_timer <= 0 || src.disease_fresh > 0)
            continue;
        for (int j = 0; j < kMaxPlayers; ++j) {
            if (j == i) continue;
            Player& dst = s.players[j];
            if (!dst.present || !dst.alive || dst.disease_timer > 0) continue;
            int dx = (src.x - dst.x) / kScale, dy = (src.y - dst.y) / kScale;
            if (std::abs(dx) > kTileW - 10 || std::abs(dy) > kTileH - 10) continue;
            dst.disease = src.disease;
            dst.disease_timer = src.disease_timer;
            dst.disease_fresh = s.tuning.disease_freshness;
            if (!s.tuning.diseases_multiply) {
                clear(src);
                break;
            }
        }
    }
}

}  // namespace bomber::sim
