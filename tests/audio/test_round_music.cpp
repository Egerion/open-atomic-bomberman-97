#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "bomber/audio/round_music.hpp"

// Pins the round-init music decision — the tail of sub_410B6E at 0x410E88 plus
// sub_4293E5's own fallback (docs/re/sound-engine.md §9.2).
//
// This exists because the port had TWO copies of this decision, one in
// MatchRunner::start_match and one in the netplay round loop, and they had
// drifted: the netplay copy ignored "Disable music during gameplay" entirely.
// Now both call round_music_id() and this suite is what keeps them honest.
//
// Header-only and SDL-free by construction: the predicate is a template
// parameter, so nothing here needs AudioEngine or a device.

using bomber::game::kRoundMusicSilent;
using bomber::game::kStageMusicFallback;
using bomber::game::round_music_id;

namespace {

// "SOUNDLST names every id" — the shipped install's answer for 1100..1110.
constexpr auto all_present = [](int) { return true; };
// A stripped install where no per-level track resolves.
constexpr auto none_present = [](int) { return false; };

}  // namespace

TEST_SUITE("round music") {

TEST_CASE("the per-level track is 1100 + level") {
    // sub_4293E5 indexes names[1100 + level] (the +0x1130 byte offset over a
    // 4-byte-per-slot table).
    CHECK(round_music_id(0, false, all_present) == 1100);
    CHECK(round_music_id(4, false, all_present) == 1104);
    CHECK(round_music_id(10, false, all_present) == 1110);
}

TEST_CASE("an unnamed level falls back to GENERIC, not to silence") {
    // The empty-slot arm at 0x42950D plays 1120. Silence would be wrong: the
    // original always has a round track unless the option says otherwise.
    CHECK(round_music_id(3, false, none_present) == kStageMusicFallback);
    CHECK(kStageMusicFallback == 1120);
}

TEST_CASE("disable_game_music silences the round outright") {
    // dword_4648C0 set -> sub_427342 frees the music handle (0x410E91). The
    // round is SILENT; it does NOT keep the setup-screens track (1020) playing,
    // which is what "just don't start a new one" would have produced.
    CHECK(round_music_id(0, true, all_present) == kRoundMusicSilent);
    CHECK(round_music_id(7, true, all_present) == kRoundMusicSilent);
}

TEST_CASE("the option wins over the fallback, not the other way round") {
    // Order matters: the guard at 0x410E88 is tested BEFORE sub_4293E5 is
    // called at all, so a stripped install with the option set is silent rather
    // than falling back to 1120.
    CHECK(round_music_id(2, true, none_present) == kRoundMusicSilent);
}

TEST_CASE("the decision does not depend on anything but stage and option") {
    // There is no network arm and no stage-art arm in the original's guard —
    // both were port inventions. This is a shape assertion: the function simply
    // has nowhere to put such a condition, and these two calls must agree
    // because a netplay round and a local round on the same level and the same
    // options.ini are the same decision.
    for (int stage = 0; stage <= 10; ++stage) {
        CAPTURE(stage);
        CHECK(round_music_id(stage, false, all_present) ==
              round_music_id(stage, false, all_present));
        CHECK(round_music_id(stage, true, all_present) == kRoundMusicSilent);
    }
}

}  // TEST_SUITE
