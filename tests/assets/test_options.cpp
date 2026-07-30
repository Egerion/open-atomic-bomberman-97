// Checks for the install-root options.ini reader/writer (bomber::assets::
// load_options / save_options, a faithful skim of sub_406238's parse). The
// original splits each line on '=', matches the key case-insensitively, and
// atoi's the value; a missing key/file leaves the field empty so the caller
// keeps the binary default. conveyor_speed is the confirmed Conveyor Speed
// game option (dword_464930, docs/re/stage-actors.md §3); team_play is our
// own bridging key for the Options screen's Team Play toggle (not itself
// RE'd — see the Options::team_play doc comment in install.hpp).
// save_options is presentation-side persistence (read-modify-write, keeps
// unknown lines intact) for the new interactive Options screen.
//
// docs/re/results-and-options.md §3 "The full options.ini key list" pins ALL
// 22 keys (writer sub_405DE3's fixed fprintf order, positionally matching
// the reader sub_406238's stricmp chain); the cases below cover the newly
// typed fields (num_to_win_match, keydef=, and the rest of the boolean/int
// rows) on top of the pre-existing conveyor_speed/team_play coverage.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "bomber/assets/install.hpp"

namespace fs = std::filesystem;
using bomber::assets::KeyDef;
using bomber::assets::kNodeNameMax;
using bomber::assets::load_node_name;
using bomber::assets::load_options;
using bomber::assets::Options;
using bomber::assets::save_node_name;
using bomber::assets::save_options;

namespace {

// Writes `body` to a unique temp file and returns its path. doctest runs each
// case in-process; a per-case counter keeps the names distinct.
fs::path write_temp(const std::string& body) {
    static int counter = 0;
    fs::path p = fs::temp_directory_path() / ("bomber_opts_" + std::to_string(counter++) + ".ini");
    std::ofstream f(p, std::ios::binary);
    f << body;
    return p;
}

// Same, for the install-root nodename.ini (a different file, a different
// reader/writer pair — see install.hpp's load_node_name doc).
fs::path write_temp_node(const std::string& body) {
    static int counter = 0;
    fs::path p = fs::temp_directory_path() / ("bomber_node_" + std::to_string(counter++) + ".ini");
    std::ofstream f(p, std::ios::binary);
    f << body;
    return p;
}

std::string read_all(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

TEST_CASE("options.ini: conveyor_speed is parsed") {
    // A trimmed copy of the shipped options.ini (this install carries =2).
    auto p = write_temp(
        ";Bomberman Options file.\n"
        "levelno=1\n"
        "enclosement_depth=2\n"
        "conveyor_speed=2\n"
        "team_play=0\n");
    auto opts = load_options(p);
    REQUIRE(opts.conveyor_speed.has_value());
    CHECK(*opts.conveyor_speed == 2);
    fs::remove(p);
}

TEST_CASE("options.ini: a missing key leaves the field empty (caller keeps default)") {
    auto p = write_temp("levelno=1\nplaytime=150\n");
    auto opts = load_options(p);
    CHECK(!opts.conveyor_speed.has_value());
    fs::remove(p);
}

TEST_CASE("options.ini: a missing file yields empty options") {
    auto opts = load_options(fs::temp_directory_path() / "definitely_not_here_12345.ini");
    CHECK(!opts.conveyor_speed.has_value());
}

TEST_CASE("options.ini: key match is case-insensitive and value is trimmed") {
    auto p = write_temp("Conveyor_Speed =  0 \r\n");  // CRLF + spaces + mixed case
    auto opts = load_options(p);
    REQUIRE(opts.conveyor_speed.has_value());
    CHECK(*opts.conveyor_speed == 0);
    fs::remove(p);
}

TEST_CASE("options.ini: comment and blank lines are ignored") {
    auto p = write_temp(
        "; conveyor_speed=99 (this is a comment, must be ignored)\n"
        "\n"
        "conveyor_speed=1\n");
    auto opts = load_options(p);
    REQUIRE(opts.conveyor_speed.has_value());
    CHECK(*opts.conveyor_speed == 1);  // the real line, not the commented one
    fs::remove(p);
}

TEST_CASE("options.ini: team_play is parsed as a bool") {
    auto p = write_temp("team_play=1\n");
    auto opts = load_options(p);
    REQUIRE(opts.team_play.has_value());
    CHECK(*opts.team_play == true);
    fs::remove(p);
}

TEST_CASE("options.ini: team_play=0 parses false, missing key stays empty") {
    auto p = write_temp("levelno=1\nteam_play=0\n");
    auto opts = load_options(p);
    REQUIRE(opts.team_play.has_value());
    CHECK(*opts.team_play == false);

    auto p2 = write_temp("levelno=1\n");
    auto opts2 = load_options(p2);
    CHECK(!opts2.team_play.has_value());
    fs::remove(p);
    fs::remove(p2);
}

TEST_CASE("save_options: read-modify-write preserves unknown lines and comments") {
    auto p = write_temp(
        ";Bomberman Options file.\n"
        "levelno=1\n"
        "enclosement_depth=2\n"
        "conveyor_speed=1\n"
        "team_play=0\n"
        "playtime=150\n");

    Options opts;
    opts.conveyor_speed = 2;
    opts.team_play = true;
    save_options(p, opts);

    // The file must still contain every untouched line verbatim... (scoped so
    // the handle closes before fs::remove — Windows won't delete an open file).
    std::string body;
    {
        std::ifstream in(p);
        std::ostringstream ss;
        ss << in.rdbuf();
        body = ss.str();
    }
    CHECK(body.find(";Bomberman Options file.") != std::string::npos);
    CHECK(body.find("levelno=1") != std::string::npos);
    CHECK(body.find("enclosement_depth=2") != std::string::npos);
    CHECK(body.find("playtime=150") != std::string::npos);

    // ...and the two owned keys must be rewritten in place, not duplicated.
    auto opts2 = load_options(p);
    REQUIRE(opts2.conveyor_speed.has_value());
    CHECK(*opts2.conveyor_speed == 2);
    REQUIRE(opts2.team_play.has_value());
    CHECK(*opts2.team_play == true);
    std::size_t first = body.find("conveyor_speed=");
    CHECK(body.find("conveyor_speed=", first + 1) == std::string::npos);
    fs::remove(p);
}

TEST_CASE("save_options: an absent key is appended, a missing file is created") {
    auto p = fs::temp_directory_path() / "bomber_opts_new.ini";
    fs::remove(p);  // ensure it does not exist yet

    Options opts;
    opts.team_play = false;
    save_options(p, opts);

    auto reread = load_options(p);
    REQUIRE(reread.team_play.has_value());
    CHECK(*reread.team_play == false);
    CHECK(!reread.conveyor_speed.has_value());  // never set, never written
    fs::remove(p);
}

TEST_CASE("save_options: only the fields present in Options are touched") {
    auto p = write_temp("conveyor_speed=0\nteam_play=1\n");

    Options opts;
    opts.conveyor_speed = 2;  // team_play left empty -> must stay untouched
    save_options(p, opts);

    auto reread = load_options(p);
    REQUIRE(reread.conveyor_speed.has_value());
    CHECK(*reread.conveyor_speed == 2);
    REQUIRE(reread.team_play.has_value());
    CHECK(*reread.team_play == true);  // unchanged
    fs::remove(p);
}

// --- §3's full 22-key table: the newly typed fields --------------------

TEST_CASE("options.ini: num_to_win_match is parsed and clamped to >= 1") {
    auto p = write_temp("num_to_win_match=5\n");
    auto opts = load_options(p);
    REQUIRE(opts.num_to_win_match.has_value());
    CHECK(*opts.num_to_win_match == 5);
    fs::remove(p);

    auto p2 = write_temp("num_to_win_match=0\n");
    auto opts2 = load_options(p2);
    REQUIRE(opts2.num_to_win_match.has_value());
    CHECK(*opts2.num_to_win_match == 1);  // clamp: < 1 -> 1
    fs::remove(p2);
}

TEST_CASE("options.ini: enclosement_depth is parsed and clamped to >= 0") {
    auto p = write_temp("enclosement_depth=-1\n");
    auto opts = load_options(p);
    REQUIRE(opts.enclosement_depth.has_value());
    CHECK(*opts.enclosement_depth == 0);
    fs::remove(p);
}

TEST_CASE("options.ini: playtime is parsed, clamped to >= 60, and 1001 (unlimited) is exempt") {
    auto p = write_temp("playtime=10\n");
    auto opts = load_options(p);
    REQUIRE(opts.playtime.has_value());
    CHECK(*opts.playtime == 60);  // clamp: < 60 -> 60
    fs::remove(p);

    auto p2 = write_temp("playtime=1001\n");
    auto opts2 = load_options(p2);
    REQUIRE(opts2.playtime.has_value());
    CHECK(*opts2.playtime == 1001);  // the "unlimited" sentinel is never clamped
    fs::remove(p2);
}

TEST_CASE("options.ini: the boolean rows normalize to 0/1") {
    auto p = write_temp(
        "random_start=1\n"
        "stomped_bombs_detonate=0\n"
        "win_by_kills=1\n"
        "goldman=1\n"
        "assign_keyboards=0\n"
        "diseases_destroyable=1\n"
        "lost_net_revert_ai=0\n"
        "disable_game_music=1\n"
        "smallmemory=0\n");
    auto opts = load_options(p);
    REQUIRE(opts.random_start.has_value());
    CHECK(*opts.random_start == true);
    REQUIRE(opts.stomped_bombs_detonate.has_value());
    CHECK(*opts.stomped_bombs_detonate == false);
    REQUIRE(opts.win_by_kills.has_value());
    CHECK(*opts.win_by_kills == true);
    REQUIRE(opts.goldman.has_value());
    CHECK(*opts.goldman == true);
    REQUIRE(opts.assign_keyboards.has_value());
    CHECK(*opts.assign_keyboards == false);
    REQUIRE(opts.diseases_destroyable.has_value());
    CHECK(*opts.diseases_destroyable == true);
    REQUIRE(opts.lost_net_revert_ai.has_value());
    CHECK(*opts.lost_net_revert_ai == false);
    REQUIRE(opts.disable_game_music.has_value());
    CHECK(*opts.disable_game_music == true);
    REQUIRE(opts.smallmemory.has_value());
    CHECK(*opts.smallmemory == false);
    fs::remove(p);
}

TEST_CASE(
    "options.ini: string/int passthrough rows (schemefilename, playtime-adjacent modem/net)") {
    auto p = write_temp(
        "schemefilename=BASIC.SCH\n"
        "modemdial=555-1234\n"
        "modemport=1\n"
        "modembaud=2\n"
        "modemirq=3\n"
        "netprotocol=9\n"  // clamp: > 3 -> 3
        "levelno=-5\n");   // clamp: < -1 -> -1
    auto opts = load_options(p);
    REQUIRE(opts.schemefilename.has_value());
    CHECK(*opts.schemefilename == "BASIC.SCH");
    REQUIRE(opts.modemdial.has_value());
    CHECK(*opts.modemdial == "555-1234");
    REQUIRE(opts.modemport.has_value());
    CHECK(*opts.modemport == 1);
    REQUIRE(opts.modembaud.has_value());
    CHECK(*opts.modembaud == 2);
    REQUIRE(opts.modemirq.has_value());
    CHECK(*opts.modemirq == 3);
    REQUIRE(opts.netprotocol.has_value());
    CHECK(*opts.netprotocol == 3);
    REQUIRE(opts.levelno.has_value());
    CHECK(*opts.levelno == -1);
    fs::remove(p);
}

TEST_CASE("options.ini: fullscreen is a port-only bool key, round-trips like the RE'd toggles") {
    // Not one of §3's confirmed 22 keys (install.hpp's Options::fullscreen
    // doc) — the 1997 binary has no fullscreen concept — but it must parse,
    // clamp-free, and read-modify-write exactly like every RE'd bool row.
    auto p = write_temp("levelno=1\nfullscreen=1\n");
    auto opts = load_options(p);
    REQUIRE(opts.fullscreen.has_value());
    CHECK(*opts.fullscreen == true);
    fs::remove(p);

    auto p2 = write_temp("levelno=1\n");
    auto opts2 = load_options(p2);
    CHECK(!opts2.fullscreen.has_value());  // absent key -> windowed (caller's default)
    fs::remove(p2);
}

TEST_CASE("save_options: fullscreen= is appended/rewritten in place, preserving unknown lines") {
    auto p = write_temp(";Bomberman Options file.\nlevelno=1\n");

    Options opts;
    opts.fullscreen = true;
    save_options(p, opts);

    auto reread = load_options(p);
    REQUIRE(reread.fullscreen.has_value());
    CHECK(*reread.fullscreen == true);

    // Flip it and re-save: must rewrite in place, not duplicate the line.
    opts.fullscreen = false;
    save_options(p, opts);
    std::string body;
    {
        std::ifstream in(p);
        std::ostringstream ss;
        ss << in.rdbuf();
        body = ss.str();
    }
    CHECK(body.find(";Bomberman Options file.") != std::string::npos);  // untouched line survives
    std::size_t first = body.find("fullscreen=");
    REQUIRE(first != std::string::npos);
    CHECK(body.find("fullscreen=", first + 1) == std::string::npos);
    auto reread2 = load_options(p);
    REQUIRE(reread2.fullscreen.has_value());
    CHECK(*reread2.fullscreen == false);
    fs::remove(p);
}

TEST_CASE("options.ini: the four Video Settings keys are port-only bools that round-trip") {
    // vsync/native_cadence/show_fps/soft_scaling (install.hpp) — none of them
    // exist in the 1997 binary, all four persist through the same
    // read-modify-write as the RE'd rows.
    auto p = write_temp("vsync=0\nnative_cadence=1\nshow_fps=1\nsoft_scaling=1\n");
    auto opts = load_options(p);
    REQUIRE(opts.vsync.has_value());
    CHECK(*opts.vsync == false);
    REQUIRE(opts.native_cadence.has_value());
    CHECK(*opts.native_cadence == true);
    REQUIRE(opts.show_fps.has_value());
    CHECK(*opts.show_fps == true);
    REQUIRE(opts.soft_scaling.has_value());
    CHECK(*opts.soft_scaling == true);
    fs::remove(p);

    // soft_scaling=0 is a real false, not "absent".
    auto p2 = write_temp("soft_scaling=0\n");
    auto opts2 = load_options(p2);
    REQUIRE(opts2.soft_scaling.has_value());
    CHECK(*opts2.soft_scaling == false);
    fs::remove(p2);
}

TEST_CASE("options.ini: an OLDER file (no soft_scaling=) loads fine and defaults to crisp") {
    // Backward compatibility, both directions of the version skew:
    //  - a file written before the key existed must load with the field EMPTY,
    //    so GameApp falls back to its own default (OFF/crisp);
    //  - and it must not be disturbed by the absence (no throw, no other field
    //    perturbed).
    auto p = write_temp(";Bomberman Options file.\nlevelno=1\nvsync=1\n");
    auto opts = load_options(p);
    CHECK(!opts.soft_scaling.has_value());
    REQUIRE(opts.vsync.has_value());
    CHECK(*opts.vsync == true);
    fs::remove(p);
}

TEST_CASE("options.ini: an UNKNOWN (newer/hand-added) key never breaks the load or the save") {
    // The other direction of the same skew: an options.ini written by a NEWER
    // build (or hand-edited) carries keys this build has never heard of. They
    // must be ignored on read and survive a write VERBATIM — otherwise saving
    // from an older exe would silently strip a newer build's settings.
    auto p = write_temp(
        ";Bomberman Options file.\n"
        "levelno=1\n"
        "some_future_option=7\n"
        "soft_scaling=1\n");
    auto opts = load_options(p);
    REQUIRE(opts.levelno.has_value());
    REQUIRE(opts.soft_scaling.has_value());
    CHECK(*opts.soft_scaling == true);

    Options out;
    out.soft_scaling = false;
    save_options(p, out);
    std::string body;
    {
        std::ifstream in(p);
        std::ostringstream ss;
        ss << in.rdbuf();
        body = ss.str();
    }
    CHECK(body.find("some_future_option=7") != std::string::npos);  // untouched
    std::size_t first = body.find("soft_scaling=");
    REQUIRE(first != std::string::npos);
    CHECK(body.find("soft_scaling=", first + 1) == std::string::npos);  // rewritten in place
    auto reread = load_options(p);
    REQUIRE(reread.soft_scaling.has_value());
    CHECK(*reread.soft_scaling == false);
    fs::remove(p);
}

TEST_CASE("save_options: soft_scaling= is appended to a file that lacks it") {
    auto p = write_temp("levelno=1\n");
    Options out;
    out.soft_scaling = true;
    save_options(p, out);
    auto reread = load_options(p);
    REQUIRE(reread.soft_scaling.has_value());
    CHECK(*reread.soft_scaling == true);
    CHECK(reread.levelno.has_value());  // the pre-existing key survived
    fs::remove(p);
}

TEST_CASE(
    "options.ini: keydef= triples parse into KeyDef, out-of-range set/action drops the line") {
    auto p = write_temp(
        "keydef=0,0,200\n"
        "keydef=0,4,57\n"
        "keydef=1,5,3\n"
        "keydef=2,0,99\n"    // set out of [0,1] -> dropped
        "keydef=0,10,1\n");  // action out of [0,9] -> dropped
    auto opts = load_options(p);
    REQUIRE(opts.keydef.has_value());
    CHECK(opts.keydef->scancode[0][0] == 200);
    CHECK(opts.keydef->scancode[0][4] == 57);
    CHECK(opts.keydef->scancode[1][5] == 3);
    // Untouched slots stay at the "absent" sentinel.
    CHECK(opts.keydef->scancode[0][1] == -1);
    CHECK(opts.keydef->scancode[1][0] == -1);
    fs::remove(p);
}

TEST_CASE("save_options: keydef= round-trips all set (set,action) triples and skips absent slots") {
    auto p = fs::temp_directory_path() / "bomber_opts_keydef.ini";
    fs::remove(p);

    Options opts;
    KeyDef kd;
    kd.scancode[0][0] = 200;  // Up, set 0
    kd.scancode[0][4] = 57;   // Action1, set 0
    kd.scancode[1][3] = 30;   // Left, set 1
    // Every other slot stays -1 (absent) and must NOT be written.
    opts.keydef = kd;
    save_options(p, opts);

    auto reread = load_options(p);
    REQUIRE(reread.keydef.has_value());
    CHECK(reread.keydef->scancode[0][0] == 200);
    CHECK(reread.keydef->scancode[0][4] == 57);
    CHECK(reread.keydef->scancode[1][3] == 30);
    CHECK(reread.keydef->scancode[0][1] == -1);  // never written -> absent on reread
    fs::remove(p);
}

TEST_CASE(
    "save_options: re-saving keydef= rewrites a (set,action) triple in place, not duplicated") {
    auto p = write_temp("keydef=0,0,200\nlevelno=1\n");

    Options opts;
    KeyDef kd;
    kd.scancode[0][0] = 205;  // rebind the SAME (set,action) to a new scancode
    opts.keydef = kd;
    save_options(p, opts);

    std::string body;
    {
        std::ifstream in(p);
        std::ostringstream ss;
        ss << in.rdbuf();
        body = ss.str();
    }
    CHECK(body.find("levelno=1") != std::string::npos);  // untouched line survives
    auto reread = load_options(p);
    REQUIRE(reread.keydef.has_value());
    CHECK(reread.keydef->scancode[0][0] == 205);
    // Only ONE "0,0," triple line should exist.
    std::size_t first = body.find("keydef=0,0,");
    REQUIRE(first != std::string::npos);
    CHECK(body.find("keydef=0,0,", first + 1) == std::string::npos);
    fs::remove(p);
}

// --- nodename.ini: the net identity's own file (sub_40C08C / sub_40C140) ----
// docs/re/network-screens.md §3 "Session model", docs/re/results-and-options.md
// "Net identity". NOT one of options.ini's 22 keys — its own one-line file with
// its own reader (boot init) and writer (shutdown hook).

TEST_CASE("nodename.ini: the first line is the name (sub_40C08C's fgets)") {
    auto p = write_temp_node("Egerion");
    CHECK(load_node_name(p) == "Egerion");
    fs::remove(p);

    // The shipped file has no trailing newline; one written by a text editor
    // must read identically ('\n' is what sub_40C08C strips), and a second line
    // is not part of the name.
    auto q = write_temp_node("Neil's House Of Pain\r\nignored second line\n");
    CHECK(load_node_name(q) == "Neil's House Of Pain");
    fs::remove(q);
}

TEST_CASE("nodename.ini: absent or blank file yields an empty name") {
    // The caller (GameApp) then draws the original's random MESSAGES 500..548
    // default — this loader never invents one.
    CHECK(load_node_name(fs::temp_directory_path() / "bomber_no_such_nodename.ini").empty());
    auto p = write_temp_node("   \n");
    CHECK(load_node_name(p).empty());
    fs::remove(p);
}

TEST_CASE("nodename.ini: hostile content is sanitised on read AND on write") {
    // The name goes straight into the lobby roster, so a hand-edited file must
    // not be able to smuggle in control bytes or an over-long run.
    auto p = write_temp_node(std::string("A\x01\x02Z\x7f!"));
    CHECK(load_node_name(p) == "AZ!");
    fs::remove(p);

    auto q = write_temp_node("");
    save_node_name(q, std::string(kNodeNameMax + 25, 'X'));
    CHECK(read_all(q) == std::string(kNodeNameMax, 'X'));
    CHECK(load_node_name(q).size() == kNodeNameMax);
    fs::remove(q);
}

TEST_CASE("nodename.ini: save/load round-trips and rewrites in place") {
    auto p = write_temp_node("OLD NAME");
    save_node_name(p, "NEW NAME");
    // One line, nothing else (sub_40C140 is a single fputs of the buffer).
    CHECK(read_all(p) == "NEW NAME");
    CHECK(load_node_name(p) == "NEW NAME");
    fs::remove(p);
}
