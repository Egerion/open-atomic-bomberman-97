// THE options.ini ROUND TRIP, EXHAUSTIVELY — the suite app_settings.hpp asks
// for: "load and save live in one file because they must agree key for key — a
// row added to one and forgotten in the other silently stops persisting."
//
// tests/assets/test_options.cpp pins the reader's parse rules key by key; what
// nothing pinned is the AGREEMENT — that every field the writer emits is a field
// the reader parses back, for ALL of them at once. This suite sets EVERY typed
// field of assets::Options to a distinctive in-range value (the clamps live in
// the reader, so an out-of-range value would round-trip "wrong" by design),
// saves to a scratch file, loads it back, and requires equality field by field.
//
// Discrimination proven at authoring time, in both directions: commenting the
// WRITER's playtime= line out made exactly the playtime row go red, and
// commenting the READER's playtime match did the same — one failing row each
// time, named in the failure message.
//
// SDL-free: assets::load_options/save_options are libs/assets, which the
// headless preset builds, so this runs inside the pre-push gate — unlike
// app_settings.cpp itself, whose apply/fill halves live in libs/game behind
// SDL and are compiled only by the SDL presets.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <filesystem>
#include <optional>
#include <string>

#include "bomber/assets/install.hpp"

using bomber::assets::KeyDef;
using bomber::assets::load_options;
using bomber::assets::Options;
using bomber::assets::save_options;

namespace {

// Every typed key, every value in the reader's accepted range, no two bools
// alike where adjacency could mask a swap.
Options fully_populated() {
    Options o;
    o.levelno = 3;
    o.num_to_win_match = 7;
    o.enclosement_depth = 2;
    o.conveyor_speed = 1;
    o.team_play = true;
    o.random_start = false;
    o.stomped_bombs_detonate = true;
    o.win_by_kills = false;
    o.goldman = true;
    o.schemefilename = "OHNO.SCH";
    o.playtime = 240;
    o.assign_keyboards = true;
    o.diseases_destroyable = false;
    o.lost_net_revert_ai = true;
    o.disable_game_music = false;
    o.modemport = 4;
    o.modembaud = 57600;
    o.modemirq = 5;
    o.modemdial = "555-0199";
    o.netprotocol = 2;
    o.smallmemory = true;
    KeyDef kd;
    kd.scancode[0][0] = 200;
    kd.scancode[1][9] = 83;  // the last slot: the writer must reach all 20
    o.keydef = kd;
    o.fullscreen = true;
    o.vsync = false;
    o.native_cadence = true;
    o.show_fps = false;
    o.soft_scaling = true;
    return o;
}

// std::string, not const char*: doctest stringifies a char pointer as its
// ADDRESS, which is the failure-message trap tests/assets/test_options.cpp
// already documents.
template <typename T>
void check_field(const std::string& key, const std::optional<T>& got,
                 const std::optional<T>& want) {
    INFO("key: " << key);
    REQUIRE(got.has_value() == want.has_value());
    if (want.has_value()) CHECK(*got == *want);
}

}  // namespace

TEST_CASE("options.ini: every typed key survives a save->load round trip") {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "bomber_opts_roundtrip.ini";
    std::filesystem::remove(path);

    const Options out = fully_populated();
    save_options(path, out);
    const Options in = load_options(path);
    std::filesystem::remove(path);

    check_field("levelno", in.levelno, out.levelno);
    check_field("num_to_win_match", in.num_to_win_match, out.num_to_win_match);
    check_field("enclosement_depth", in.enclosement_depth, out.enclosement_depth);
    check_field("conveyor_speed", in.conveyor_speed, out.conveyor_speed);
    check_field("team_play", in.team_play, out.team_play);
    check_field("random_start", in.random_start, out.random_start);
    check_field("stomped_bombs_detonate", in.stomped_bombs_detonate, out.stomped_bombs_detonate);
    check_field("win_by_kills", in.win_by_kills, out.win_by_kills);
    check_field("goldman", in.goldman, out.goldman);
    check_field("schemefilename", in.schemefilename, out.schemefilename);
    check_field("playtime", in.playtime, out.playtime);
    check_field("assign_keyboards", in.assign_keyboards, out.assign_keyboards);
    check_field("diseases_destroyable", in.diseases_destroyable, out.diseases_destroyable);
    check_field("lost_net_revert_ai", in.lost_net_revert_ai, out.lost_net_revert_ai);
    check_field("disable_game_music", in.disable_game_music, out.disable_game_music);
    check_field("modemport", in.modemport, out.modemport);
    check_field("modembaud", in.modembaud, out.modembaud);
    check_field("modemirq", in.modemirq, out.modemirq);
    check_field("modemdial", in.modemdial, out.modemdial);
    check_field("netprotocol", in.netprotocol, out.netprotocol);
    check_field("smallmemory", in.smallmemory, out.smallmemory);
    check_field("fullscreen", in.fullscreen, out.fullscreen);
    check_field("vsync", in.vsync, out.vsync);
    check_field("native_cadence", in.native_cadence, out.native_cadence);
    check_field("show_fps", in.show_fps, out.show_fps);
    check_field("soft_scaling", in.soft_scaling, out.soft_scaling);

    REQUIRE(in.keydef.has_value());
    CHECK(in.keydef->scancode[0][0] == 200);
    CHECK(in.keydef->scancode[1][9] == 83);
    CHECK(in.keydef->scancode[0][1] == -1);  // never set, never written, absent on reread
}

TEST_CASE("options.ini: an all-empty Options writes nothing a reader would misread") {
    // The other agreement direction: a field the caller never set must not leak
    // a default into the file (flush_settings only ever writes SET fields, but
    // the writer itself must uphold it too).
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "bomber_opts_roundtrip_empty.ini";
    std::filesystem::remove(path);

    save_options(path, Options{});
    const Options in = load_options(path);
    std::filesystem::remove(path);

    CHECK(!in.levelno.has_value());
    CHECK(!in.team_play.has_value());
    CHECK(!in.playtime.has_value());
    CHECK(!in.schemefilename.has_value());
    CHECK(!in.keydef.has_value());
    CHECK(!in.soft_scaling.has_value());
}
