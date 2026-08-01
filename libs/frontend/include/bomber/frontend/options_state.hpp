#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

#include "bomber/assets/sch.hpp"
#include "bomber/frontend/options_model.hpp"  // OptionsSnapshot

// The non-service state the Options cluster mutates, bundled by reference so the
// three runner screens need no GameApp&.

namespace bomber::game {

struct OptionsEditState {
    OptionsSnapshot& options;
    bool& options_dirty;
    assets::sch::Scheme& scheme;
    std::uint32_t& setup_lcg;  // the shared presentation LCG (pick_glue)
    const std::filesystem::path& game_dir;
    // The exit path writes these three as well: the gold-forfeit clear, and the
    // two GameApp-side mirrors of the just-committed snapshot.
    int& gold_player;
    bool& team_play;
    std::optional<int>& conveyor_speed_index;
};

}  // namespace bomber::game
