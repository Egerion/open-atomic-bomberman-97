#pragma once

#include <cstdint>
#include <filesystem>

#include "bomber/assets/sch.hpp"

// Seam 2 (ADR-0009 §"shared front-end state"): the non-service state the hidden
// scheme editor mutates, bundled so the EditorRunner screen can be its own class
// without threading a GameApp& — GameApp owns the members and hands a fresh
// EditorEditState to the runner ctor alongside the ScreenContext services
// bundle. A cheap value type (references only), copied by value into the runner;
// the referenced members are GameApp members that outlive every screen.
// ScreenContext stays front-end-service-only, so the editor-specific mutable
// state lives here instead.
//
// present_editor touches exactly these four non-service members: setup_lcg_ (the
// shared presentation LCG advanced by pick_glue for each backdrop), scheme_
// (repointed at the freshly-saved scheme so the edit is immediately selectable),
// opts_.game_dir (the *.SCH picker path + the DATA/SCHEMES save dir, read-only),
// and opts_.scheme (the session's live scheme path, repointed at the saved file
// alongside scheme_ on a confirmed save).

namespace bomber::game {

struct EditorEditState {
    std::uint32_t& setup_lcg;               // GameApp::setup_lcg_ (for pick_glue)
    assets::sch::Scheme& scheme;            // GameApp::scheme_ (repointed on a confirmed save)
    const std::filesystem::path& game_dir;  // GameApp::opts_.game_dir (picker path + save dir)
    std::filesystem::path& scheme_path;     // GameApp::opts_.scheme (repointed to the saved file)
};

}  // namespace bomber::game
