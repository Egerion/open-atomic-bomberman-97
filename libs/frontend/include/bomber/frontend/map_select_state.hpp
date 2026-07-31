#pragma once

#include <cstdint>

#include "bomber/frontend/options_screen.hpp"  // OptionsSnapshot

// Seam 2 (ADR-0009 §"shared front-end state"): the non-service state the LEVEL &
// ROUNDS screen (present_map_select, sub_406DDE) reads/writes, bundled so
// MapSelectScreen can be its own class without threading a GameApp& — GameApp
// owns the members and hands a fresh MapSelectState to the runner ctor alongside
// the ScreenContext services bundle. A cheap value type (references only), copied
// by value into the runner; the referenced members are GameApp members that
// outlive every screen. ScreenContext stays front-end-service-only, so the
// map-select-specific mutable state lives here instead.
//
// The reference set is EXACTLY what present_map_select touches:
//  - selected_level / win_target — the committed globals (dword_464998 /
//    dword_464A7C): read once to seed the working copies on entry, written back
//    on an Enter/Space commit (Escape discards the working copies untouched).
//  - setup_lcg — the shared presentation LCG (never sim::State::rng): advanced by
//    pick_glue for the GLUE<n> backdrop AND by the sample-block preview's per-cell
//    tile/field re-rolls. The draw order/count is observable, so it shares the one
//    LCG every pre-match screen advances (frontend_util.hpp's pick_glue).
//  - options — READ-ONLY, for options_.win_by_kills (the "%u %s to win match"
//    Wins/Kills word, getstring 208/209).
//  - gold_player — Escape aborts the whole Play flow and forfeits the pending
//    Goldman winner (dword_46492C = -1, goldman-roulette.md §2's "Cleared to -1
//    by" list).

namespace bomber::game {

struct MapSelectState {
    int& selected_level;             // GameApp::selected_level_ (seed on entry, commit on Enter)
    int& win_target;                 // GameApp::win_target_ (seed on entry, commit on Enter)
    std::uint32_t& setup_lcg;        // GameApp::setup_lcg_ (pick_glue + the preview cell re-rolls)
    const OptionsSnapshot& options;  // GameApp::options_ (read-only: win_by_kills word pick)
    int& gold_player;                // GameApp::gold_player_ (Esc forfeits the pending gold player)
};

}  // namespace bomber::game
