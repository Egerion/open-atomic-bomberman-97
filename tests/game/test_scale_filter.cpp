#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "bomber/game_util/scale_filter.hpp"

// The PORT-ONLY "Soft scaling" toggle's pure decisions (scale_filter.hpp). SDL-
// free, so this suite runs under the headless preset where the pins that matter
// actually get checked — the capture pin below is the one that has broken
// tests/visual four times over for four other options.

using bomber::game::art_filter;
using bomber::game::scale_filter_for;
using bomber::game::ScaleFilter;
using bomber::game::TextureArt;

TEST_CASE("soft scaling defaults OFF: crisp is what an untouched install renders") {
    // The whole point of the default. The 1997 build scales nothing, so nearest
    // is the closer of the two to it — a player who never opens the F10 panel
    // must keep the exact picture the port has always drawn.
    CHECK(scale_filter_for(false, false) == ScaleFilter::Crisp);
    CHECK(scale_filter_for(true, false) == ScaleFilter::Soft);
}

TEST_CASE("a capture run pins the filter crisp whatever options.ini says") {
    // tests/visual/README.md's rule: every capture-affecting options.ini value is
    // pinned in GameApp::capture_run()'s branch of load_config. This option
    // re-samples EVERY scaled pixel, so an honoured `soft_scaling=1` would
    // re-hash all five pinned shots at once.
    CHECK(scale_filter_for(true, true) == ScaleFilter::Crisp);
    CHECK(scale_filter_for(false, true) == ScaleFilter::Crisp);
}

TEST_CASE("classic art follows the toggle; DATA_HD art is linear either way") {
    // The HD overrides are 4x de-dithered truecolour upscales and have always
    // been uploaded linear (AniTextures::load_hd_overlay, asset_store.cpp). Soft
    // scaling is about the CLASSIC 1x cels; turning it off must not make the HD
    // art crisp, which would be a change to a feature this option does not own.
    CHECK(art_filter(TextureArt::Classic, ScaleFilter::Crisp) == ScaleFilter::Crisp);
    CHECK(art_filter(TextureArt::Classic, ScaleFilter::Soft) == ScaleFilter::Soft);
    CHECK(art_filter(TextureArt::HighRes, ScaleFilter::Crisp) == ScaleFilter::Soft);
    CHECK(art_filter(TextureArt::HighRes, ScaleFilter::Soft) == ScaleFilter::Soft);
}

TEST_CASE("the decisions are constexpr (usable in a static context)") {
    static_assert(scale_filter_for(true, false) == ScaleFilter::Soft);
    static_assert(scale_filter_for(true, true) == ScaleFilter::Crisp);
    static_assert(art_filter(TextureArt::HighRes, ScaleFilter::Crisp) == ScaleFilter::Soft);
    CHECK(true);
}
