#pragma once

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

#include "bomber/game/audio_engine.hpp"
#include "bomber/sim/state.hpp"

// Maps sim events onto the original SOUNDLST id ranges (docs/RE-NOTES.md):
// effects, death screams, pickup voices, taunts, AWESOME lines, skull voices.
// Purely cosmetic — schedules and plays sounds, never touches the sim.

namespace bomber::game {

class SoundDirector {
public:
    explicit SoundDirector(AudioEngine& audio) : audio_(audio) {}

    // Call once per sim tick, after the tick ran: drains due scheduled voice
    // lines, then reacts to this tick's events.
    void on_tick(const sim::State& s);

    // Forgets per-match state (pending voices, pickup counters).
    void reset();

private:
    AudioEngine& audio_;
    // Voice lines scheduled a beat after their trigger: (due tick, id range).
    std::vector<std::pair<std::uint64_t, std::pair<int, int>>> pending_;
    std::array<int, sim::kMaxPlayers> pickups_{};
};

}  // namespace bomber::game
