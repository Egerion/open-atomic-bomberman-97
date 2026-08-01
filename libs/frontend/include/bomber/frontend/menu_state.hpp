#pragma once

#include <array>
#include <cstdint>
#include <filesystem>

#include "bomber/assets/sch.hpp"
#include "bomber/sim/constants.hpp"  // sim::kMaxPlayers

// The non-service state the main MENU screen reads and writes, bundled by
// reference so MenuScreen needs no GameApp&.
//
// campaign_active/gold_player are deliberately ABSENT: attract mode leaves
// campaign and goldman state untouched, so the menu never reads or writes them.

namespace bomber::game {

// The roster/level/team snapshot attract mode saves before overwriting them for
// the demo roster (sub_4224E2), restored by GameApp::restore_from_attract()
// (sub_422552) — "menu re-entry restores everything", so the player's own
// pre-attract choices survive untouched. Named here so both MenuState and
// GameApp's own member can refer to it.
struct AttractSaved {
    std::array<int, sim::kMaxPlayers> type{};
    std::array<int, sim::kMaxPlayers> sub{};
    std::array<int, sim::kMaxPlayers> team{};
    int level = -1;
    bool team_play = false;
};

struct MenuState {
    // The menu's own persistent cursor, attract idle clock and Ctrl+E counter.
    int& menu_index;
    std::uint64_t& menu_idle_since_ms;
    int& editor_trigger_count;
    // The attract save plus the two presentation-LCG rolls that overwrite the
    // live roster/level/team.
    bool& attract;
    AttractSaved& attract_saved;
    std::uint32_t& attract_lcg;
    std::array<int, sim::kMaxPlayers>& setup_type;
    std::array<int, sim::kMaxPlayers>& setup_sub;
    std::array<int, sim::kMaxPlayers>& setup_team;
    int& selected_level;
    bool& team_play;
    // The F10 Video Settings toggles, handed straight on as a VideoToggleRefs.
    bool& uncap_fps;
    bool& native_cadence;
    bool& show_fps;
    bool& soft_scaling;
    bool& options_dirty;
    // What the Ctrl+E×6 editor needs to build an EditorEditState.
    std::uint32_t& setup_lcg;
    assets::sch::Scheme& scheme;
    const std::filesystem::path& game_dir;
    std::filesystem::path& scheme_path;
};

}  // namespace bomber::game
