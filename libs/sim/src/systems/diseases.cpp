#include "systems/diseases.hpp"

#include <cstdlib>
#include <utility>

#include "bomber/sim/rng.hpp"

namespace bomber::sim {

void DiseaseSystem::clear(Player& p) {
    p.disease.fill(false);
    p.disease_timer = 0;
    p.disease_fresh = 0;
}

bool DiseaseSystem::has_swap_target(int idx) const {
    for (int j = 0; j < kMaxPlayers; ++j)
        if (j != idx && s_.players[j].present && s_.players[j].alive && s_.players[j].stun == 0)
            return true;
    return false;
}

void DiseaseSystem::give(int idx, Disease d, bool announce) {
    State& s = s_;
    Player& p = s.players[idx];
    if (d == Disease::Swap) {
        // sub_41DFB6's target scan requires the SAME "valid other player"
        // triple its contagion sibling uses (sub_41F29B): not self, active,
        // and NOT STUNNED (`v3 != v5 && *v3 && !v3[2]`; see spread_and_age).
        int targets[kMaxPlayers], n = 0;
        for (int j = 0; j < kMaxPlayers; ++j)
            if (j != idx && s.players[j].present && s.players[j].alive && s.players[j].stun == 0)
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
        p.disease_timer = s.tuning.disease_frames[i];
        p.disease_fresh = s.tuning.disease_freshness;
    }
    if (announce)
        s.events.push_back({Event::Type::Infected, static_cast<std::int8_t>(idx), -1, -1,
                            static_cast<std::int8_t>(d)});
}

void DiseaseSystem::assign_random(int idx, int count) {
    // One rand % 9 per disease (sub_41DFB6; the 200-try reroll loop there is
    // NET-GAME-only — a local game accepts the first roll). A Swap with no
    // valid target is simply LOST (the original's 200-try random-player scan
    // finds nobody and falls through assigning nothing) — it does NOT reroll
    // into a different disease. Our target pick inside give() replaces that
    // scan with one draw over the valid set (same outcome distribution,
    // documented internal-RNG deviation).
    for (int c = 0; c < count; ++c) {
        auto d = static_cast<Disease>(random_below(s_, kDiseaseKinds));
        if (d == Disease::Swap && !has_swap_target(idx)) continue;
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
    // contagion scan (~22943-22974) — all nested inside "not stunned"
    // (`if (!+8)` ~22904: a stunned player's disease neither ages nor
    // spreads, exactly like the rest of their per-tick update is frozen
    // while stunned). Doing age-then-spread in one pass per player, rather
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
        if (!p.present || !p.alive || p.stun > 0) continue;
        if (p.disease_fresh > 0) --p.disease_fresh;
        if (p.disease_timer > 0 && s.tuning.diseases_time_limited && --p.disease_timer <= 0)
            clear(p);
    }

    // Contagion: a diseased player overlapping a healthy one hands the whole
    // set over (sub_41F29B ~22943, overlap |dx| <= 30 & |dy| <= 26). Both
    // ends must be unstunned (the source gate below + the target's own
    // `!v103[2]`). multiply=1 (default) infects EVERY valid target found
    // this tick and the source keeps it; multiply=0 infects only the first
    // (slot order) and clears the source right there, ending the scan.
    // Contagion is silent in the original — no event.
    for (int i = 0; i < kMaxPlayers; ++i) {
        Player& src = s.players[i];
        if (!src.present || !src.alive || src.stun > 0 || src.disease_timer <= 0 ||
            src.disease_fresh > 0)
            continue;
        for (int j = 0; j < kMaxPlayers; ++j) {
            if (j == i) continue;
            Player& dst = s.players[j];
            if (!dst.present || !dst.alive || dst.stun > 0 || dst.disease_timer > 0) continue;
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
