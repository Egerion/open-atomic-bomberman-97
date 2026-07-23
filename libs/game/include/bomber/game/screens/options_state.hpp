#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

#include "bomber/assets/sch.hpp"
#include "bomber/game/options_screen.hpp"  // OptionsSnapshot

// Seam 2 (ADR-0009 §"shared front-end state"): the non-service state the
// Options cluster mutates, bundled so the three runner screens
// (OptionsScreenRunner / KeyRemapScreenRunner / SchemePickerRunner) can each be
// their own class without threading a GameApp& — GameApp owns the members and
// hands a fresh OptionsEditState to every runner ctor alongside the
// ScreenContext services bundle. A cheap value type (references only), copied by
// value into each runner; the referenced members are GameApp members that
// outlive every screen. ScreenContext stays front-end-service-only, so the
// Options-specific mutable state lives here instead.
//
// Beyond the four "obvious" members, present_options_screen's exit path also
// writes gold_player_ (the gold-forfeit clear), team_play_, and
// conveyor_speed_index_ (the two mirrors of the just-committed snapshot), so the
// verbatim move carries those references too.

namespace bomber::game {

struct OptionsEditState {
    OptionsSnapshot& options;                  // GameApp::options_
    bool& options_dirty;                       // GameApp::options_dirty_
    assets::sch::Scheme& scheme;               // GameApp::scheme_
    std::uint32_t& setup_lcg;                  // GameApp::setup_lcg_ (for pick_glue)
    const std::filesystem::path& game_dir;     // GameApp::opts_.game_dir
    int& gold_player;                          // GameApp::gold_player_ (gold-forfeit clear)
    bool& team_play;                           // GameApp::team_play_ (mirror of options_.team_play)
    std::optional<int>& conveyor_speed_index;  // GameApp::conveyor_speed_index_
};

}  // namespace bomber::game
