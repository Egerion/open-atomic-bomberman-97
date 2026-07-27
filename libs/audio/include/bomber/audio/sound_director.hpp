#pragma once

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

#include "bomber/audio/audio_engine.hpp"
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
    // Voice lines scheduled a beat after their trigger: (due tick, group base).
    std::vector<std::pair<std::uint64_t, int>> pending_;
    std::array<int, sim::kMaxPlayers> pickups_{};
    // Wall-slam SFX id, latched on the FIRST `WallClosed` event since reset()
    // and replayed for every one after — the original draws `rand() % 3`
    // once when the enclosure ARMS (not once per dropped tile), docs/re/
    // facts.md "Wall-slam SFX". -1 = not yet rolled this round.
    int wall_slam_id_ = -1;
};

}  // namespace bomber::game
