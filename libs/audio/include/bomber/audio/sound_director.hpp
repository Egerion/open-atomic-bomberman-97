#pragma once

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

#include "bomber/audio/sound_sink.hpp"
#include "bomber/sim/state.hpp"

// Maps sim events onto the original SOUNDLST id ranges (docs/RE-NOTES.md):
// effects, death screams, pickup voices, taunts, AWESOME lines, skull voices.
// Purely cosmetic — schedules and plays sounds, never touches the sim.

namespace bomber::game {

class SoundDirector {
public:
    explicit SoundDirector(SoundSink& audio) : audio_(audio) {}

    // Call once per sim tick, after the tick ran: drains due scheduled voice
    // lines, then reacts to this tick's events.
    void on_tick(const sim::State& s);

    // Forgets per-match state (pending voices, pickup counters).
    void reset();

private:
    // The three event arms with logic of their own, split out so on_tick stays a
    // flat dispatch rather than three nested decisions wearing a `case` label.
    void on_bomb_placed(const sim::State& s, const sim::Event& ev);
    void on_powerup_picked(const sim::Event& ev);
    void on_player_died(const sim::State& s, const sim::Event& ev);
    // Drains voice lines scheduled for this tick or earlier.
    void drain_pending(const sim::State& s);

    SoundSink& audio_;
    // Voice lines scheduled a beat after their trigger: (due tick, group base).
    std::vector<std::pair<std::uint64_t, int>> pending_;
    std::array<int, sim::kMaxPlayers> pickups_{};
    // Latched on the FIRST WallClosed since reset() and replayed thereafter: the
    // original draws `rand() % 3` once when the enclosure ARMS, not once per
    // dropped tile (docs/re/facts.md). -1 = not yet rolled this round.
    int wall_slam_id_ = -1;
};

}  // namespace bomber::game
