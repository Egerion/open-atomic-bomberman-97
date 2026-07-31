#pragma once

#include <array>
#include <cstdint>
#include <filesystem>

#include "bomber/assets/sch.hpp"
#include "bomber/sim/constants.hpp"  // sim::kMaxPlayers

// Seam 2 (ADR-0009 §"shared front-end state"): the non-service state the main
// MENU screen (present_menu + its private roll_attract_match) reads/writes,
// bundled so MenuScreen can be its own class without threading a GameApp& —
// GameApp owns the members and hands a fresh MenuState to the runner ctor
// alongside the ScreenContext services bundle. A cheap value type (references
// only), copied by value into the runner; the referenced members are GameApp
// members that outlive every screen. ScreenContext stays front-end-service-
// only, so the menu-specific mutable state lives here instead.
//
// The reference set is EXACTLY what present_menu + roll_attract_match touch:
//  - menu_index / menu_idle_since_ms / editor_trigger_count — the menu's own
//    persistent cursor, attract idle clock, and Ctrl+E repeat counter.
//  - attract / attract_saved / attract_lcg + setup_type/setup_sub/setup_team/
//    selected_level/team_play — roll_attract_match's sub_4224E2 save plus the
//    two presentation-LCG rolls that overwrite the live roster/level/team.
//  - uncap_fps / native_cadence / show_fps / soft_scaling / options_dirty — the
//    F10 Video Settings toggles, handed straight to VideoSettingsScreen as a
//    VideoToggleRefs (the same members GameApp::present_video_settings passes).
//  - setup_lcg / scheme / game_dir / scheme_path — the four members the Ctrl+E×6
//    editor needs to build an EditorEditState (mirrors GameApp::editor_state()).
// campaign_active_/gold_player_ are deliberately ABSENT: roll_attract_match
// documents that it leaves campaign/goldman state untouched, so the menu never
// reads or writes them.

namespace bomber::game {

// The roster/level/team snapshot roll_attract_match() saves before overwriting
// them for the demo roster (sub_4224E2), restored by
// GameApp::restore_from_attract() (sub_422552) — doc: "Menu re-entry restores
// everything", so the player's own pre-attract choices survive untouched.
// Lifted out of GameApp into this seam header (ADR-0009's future AttractState
// group) so both MenuState and GameApp's own attract_saved_ member can name it.
struct AttractSaved {
    std::array<int, sim::kMaxPlayers> type{};
    std::array<int, sim::kMaxPlayers> sub{};
    std::array<int, sim::kMaxPlayers> team{};
    int level = -1;
    bool team_play = false;
};

struct MenuState {
    int& menu_index;                    // GameApp::menu_index_
    std::uint64_t& menu_idle_since_ms;  // GameApp::menu_idle_since_ms_
    int& editor_trigger_count;          // GameApp::editor_trigger_count_
    bool& attract;                      // GameApp::attract_
    AttractSaved& attract_saved;        // GameApp::attract_saved_
    std::uint32_t& attract_lcg;         // GameApp::attract_lcg_ (attract rolls)
    std::array<int, sim::kMaxPlayers>& setup_type;  // GameApp::setup_type_
    std::array<int, sim::kMaxPlayers>& setup_sub;   // GameApp::setup_sub_
    std::array<int, sim::kMaxPlayers>& setup_team;  // GameApp::setup_team_
    int& selected_level;                // GameApp::selected_level_
    bool& team_play;                    // GameApp::team_play_
    bool& uncap_fps;                    // GameApp::uncap_fps_ (F10 VideoToggleRefs)
    bool& native_cadence;               // GameApp::native_cadence_ (F10 VideoToggleRefs)
    bool& show_fps;                     // GameApp::show_fps_ (F10 VideoToggleRefs)
    bool& soft_scaling;                 // GameApp::soft_scaling_ (F10 VideoToggleRefs)
    bool& options_dirty;                // GameApp::options_dirty_ (F10 flush flag)
    std::uint32_t& setup_lcg;           // GameApp::setup_lcg_ (editor: pick_glue)
    assets::sch::Scheme& scheme;        // GameApp::scheme_ (editor: saved scheme)
    const std::filesystem::path& game_dir;   // GameApp::opts_.game_dir (editor)
    std::filesystem::path& scheme_path;      // GameApp::opts_.scheme (editor)
};

}  // namespace bomber::game
