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
        if (j != idx && s_.players[j].present && s_.players[j].alive) return true;
    return false;
}

void DiseaseSystem::give(int idx, Disease d, bool announce) {
    State& s = s_;
    Player& p = s.players[idx];
    if (d == Disease::Swap) {
        int targets[kMaxPlayers], n = 0;
        for (int j = 0; j < kMaxPlayers; ++j)
            if (j != idx && s.players[j].present && s.players[j].alive) targets[n++] = j;
        if (n > 0) {
            Player& q = s.players[targets[random_below(s, static_cast<std::uint32_t>(n))]];
            std::swap(p.x, q.x);
            std::swap(p.y, q.y);
            std::swap(p.move_budget, q.move_budget);
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
    for (int c = 0; c < count; ++c) {
        Disease d;
        int guard = 0;
        do {
            d = static_cast<Disease>(random_below(s_, kDiseaseKinds));
        } while (d == Disease::Swap && !has_swap_target(idx) && ++guard < 64);
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

    // Contagion: a diseased player overlapping a healthy one hands the whole
    // set over (sub_41F29B, overlap |dx| <= 30 & |dy| <= 26). multiply=1 means
    // the source keeps it too. Contagion is silent in the original — no event.
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
            if (!s.tuning.diseases_multiply) clear(src);
        }
    }

    // Age the freshness gate; expire finished diseases.
    for (int i = 0; i < kMaxPlayers; ++i) {
        Player& p = s.players[i];
        if (!p.present || !p.alive) continue;
        if (p.disease_fresh > 0) --p.disease_fresh;
        if (p.disease_timer > 0 && s.tuning.diseases_time_limited && --p.disease_timer <= 0)
            clear(p);
    }
}

}  // namespace bomber::sim
