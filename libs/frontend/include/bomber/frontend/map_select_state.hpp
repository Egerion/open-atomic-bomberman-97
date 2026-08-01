#pragma once

#include <cstdint>

#include "bomber/frontend/options_model.hpp"  // OptionsSnapshot

// The non-service state the LEVEL & ROUNDS screen (sub_406DDE) reads and writes,
// bundled by reference so MapSelectScreen needs no GameApp&.

namespace bomber::game {

struct MapSelectState {
    // The committed globals (dword_464998 / dword_464A7C): read once to seed the
    // screen's working copies, written back only on an Enter/Space commit —
    // Escape discards the working copies and leaves these untouched.
    int& selected_level;
    int& win_target;
    // The shared presentation LCG (never sim::State::rng), advanced by pick_glue
    // AND by the sample-block preview's per-cell re-rolls. Draw order and count
    // are observable, so every pre-match screen shares this one.
    std::uint32_t& setup_lcg;
    const OptionsSnapshot& options;  // read-only: the win_by_kills Wins/Kills word
    int& gold_player;                // Esc forfeits the pending Goldman winner
};

}  // namespace bomber::game
