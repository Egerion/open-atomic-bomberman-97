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
// the reader sub_406238's stricmp chain).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>

#include "bomber/assets/install.hpp"
#include "fixture.hpp"

using bomber::assets::KeyDef;
using bomber::assets::kNodeNameMax;
using bomber::assets::load_node_name;
using bomber::assets::load_options;
using bomber::assets::Options;
using bomber::assets::save_node_name;
using bomber::assets::save_options;
using bomber::test::TempFile;

namespace {

// A scratch options.ini that deletes itself. doctest runs every case in one
// process, so the name carries a counter to keep them distinct.
TempFile options_file(const std::string& body) {
    static int counter = 0;
    return TempFile(("bomber_opts_" + std::to_string(counter++) + ".ini").c_str(), body);
}

// The install-root nodename.ini is a different file with its own reader/writer
// pair (install.hpp's load_node_name doc), so it gets its own scratch name.
TempFile node_file(const std::string& body) {
    static int counter = 0;
    return TempFile(("bomber_node_" + std::to_string(counter++) + ".ini").c_str(), body);
}

// One parsed row: present, and holding `want`. Nine of these written out
// longhand is a wall a reader has to diff against the fixture by eye. The key
// is a std::string, NOT a `const char*`: doctest stringifies a char pointer as
// its ADDRESS, so the failure message said `key: 00007FF6DC7B7B28` and the
// shared helper cost exactly the context it was supposed to preserve.
template <typename T, typename U>
void check_row(const std::string& key, const std::optional<T>& got, const U& want) {
    INFO("key: " << key);
    REQUIRE(got.has_value());
    CHECK(*got == want);
}

// save_options is a read-modify-write: a key it owns must be REWRITTEN, never
// appended a second time. Both halves matter — searching for a duplicate
// without first proving the key is there at all passes on a file that lost it.
void check_written_once(const std::string& body, const std::string& key) {
    INFO("key: " << key);
    const std::size_t first = body.find(key);
    REQUIRE(first != std::string::npos);
    CHECK(body.find(key, first + 1) == std::string::npos);
}

}  // namespace

TEST_CASE("options.ini: conveyor_speed is parsed") {
    // A trimmed copy of the shipped options.ini (this install carries =2).
    const TempFile p = options_file(
        ";Bomberman Options file.\n"
        "levelno=1\n"
        "enclosement_depth=2\n"
        "conveyor_speed=2\n"
        "team_play=0\n");
    check_row("conveyor_speed", load_options(p.path()).conveyor_speed, 2);
}

TEST_CASE("options.ini: a missing key leaves the field empty (caller keeps default)") {
    const TempFile p = options_file("levelno=1\nplaytime=150\n");
    CHECK(!load_options(p.path()).conveyor_speed.has_value());
}

TEST_CASE("options.ini: a missing file yields empty options") {
    const auto opts = load_options(bomber::test::absent_path("bomber_opts_absent.ini"));
    CHECK(!opts.conveyor_speed.has_value());
}

TEST_CASE("options.ini: key match is case-insensitive and value is trimmed") {
    const TempFile p = options_file("Conveyor_Speed =  0 \r\n");  // CRLF + spaces + mixed case
    check_row("conveyor_speed", load_options(p.path()).conveyor_speed, 0);
}

TEST_CASE("options.ini: comment and blank lines are ignored") {
    const TempFile p = options_file(
        "; conveyor_speed=99 (this is a comment, must be ignored)\n"
        "\n"
        "conveyor_speed=1\n");
    // The real line, not the commented one.
    check_row("conveyor_speed", load_options(p.path()).conveyor_speed, 1);
}

TEST_CASE("options.ini: team_play is parsed as a bool") {
    const TempFile p = options_file("team_play=1\n");
    check_row("team_play", load_options(p.path()).team_play, true);
}

TEST_CASE("options.ini: team_play=0 parses false, missing key stays empty") {
    const TempFile p = options_file("levelno=1\nteam_play=0\n");
    check_row("team_play", load_options(p.path()).team_play, false);

    const TempFile absent = options_file("levelno=1\n");
    CHECK(!load_options(absent.path()).team_play.has_value());
}

TEST_CASE("save_options: read-modify-write preserves unknown lines and comments") {
    const TempFile p = options_file(
        ";Bomberman Options file.\n"
        "levelno=1\n"
        "enclosement_depth=2\n"
        "conveyor_speed=1\n"
        "team_play=0\n"
        "playtime=150\n");

    Options opts;
    opts.conveyor_speed = 2;
    opts.team_play = true;
    save_options(p.path(), opts);

    // Every untouched line must still be there verbatim...
    const std::string body = p.text();
    CHECK(body.find(";Bomberman Options file.") != std::string::npos);
    CHECK(body.find("levelno=1") != std::string::npos);
    CHECK(body.find("enclosement_depth=2") != std::string::npos);
    CHECK(body.find("playtime=150") != std::string::npos);

    // ...and the two owned keys rewritten in place, not duplicated.
    const Options reread = load_options(p.path());
    check_row("conveyor_speed", reread.conveyor_speed, 2);
    check_row("team_play", reread.team_play, true);
    check_written_once(body, "conveyor_speed=");
}

TEST_CASE("save_options: an absent key is appended, a missing file is created") {
    const auto path = bomber::test::absent_path("bomber_opts_new.ini");

    Options opts;
    opts.team_play = false;
    save_options(path, opts);

    const Options reread = load_options(path);
    check_row("team_play", reread.team_play, false);
    CHECK(!reread.conveyor_speed.has_value());  // never set, never written
    std::filesystem::remove(path);
}

TEST_CASE("save_options: only the fields present in Options are touched") {
    const TempFile p = options_file("conveyor_speed=0\nteam_play=1\n");

    Options opts;
    opts.conveyor_speed = 2;  // team_play left empty -> must stay untouched
    save_options(p.path(), opts);

    const Options reread = load_options(p.path());
    check_row("conveyor_speed", reread.conveyor_speed, 2);
    check_row("team_play", reread.team_play, true);  // unchanged
}

// --- §3's full 22-key table: the newly typed fields --------------------

TEST_CASE("options.ini: num_to_win_match is parsed and clamped to >= 1") {
    const TempFile p = options_file("num_to_win_match=5\n");
    check_row("num_to_win_match", load_options(p.path()).num_to_win_match, 5);

    const TempFile low = options_file("num_to_win_match=0\n");
    check_row("num_to_win_match", load_options(low.path()).num_to_win_match, 1);  // clamp: < 1 -> 1
}

TEST_CASE("options.ini: enclosement_depth is parsed and clamped to >= 0") {
    const TempFile p = options_file("enclosement_depth=-1\n");
    check_row("enclosement_depth", load_options(p.path()).enclosement_depth, 0);
}

TEST_CASE("options.ini: playtime is parsed, clamped to >= 60, and 1001 (unlimited) is exempt") {
    const TempFile p = options_file("playtime=10\n");
    check_row("playtime", load_options(p.path()).playtime, 60);  // clamp: < 60 -> 60

    // The "unlimited" sentinel is never clamped.
    const TempFile unlimited = options_file("playtime=1001\n");
    check_row("playtime", load_options(unlimited.path()).playtime, 1001);
}

TEST_CASE("options.ini: the boolean rows normalize to 0/1") {
    const TempFile p = options_file(
        "random_start=1\n"
        "stomped_bombs_detonate=0\n"
        "win_by_kills=1\n"
        "goldman=1\n"
        "assign_keyboards=0\n"
        "diseases_destroyable=1\n"
        "lost_net_revert_ai=0\n"
        "disable_game_music=1\n"
        "smallmemory=0\n");
    const Options o = load_options(p.path());
    check_row("random_start", o.random_start, true);
    check_row("stomped_bombs_detonate", o.stomped_bombs_detonate, false);
    check_row("win_by_kills", o.win_by_kills, true);
    check_row("goldman", o.goldman, true);
    check_row("assign_keyboards", o.assign_keyboards, false);
    check_row("diseases_destroyable", o.diseases_destroyable, true);
    check_row("lost_net_revert_ai", o.lost_net_revert_ai, false);
    check_row("disable_game_music", o.disable_game_music, true);
    check_row("smallmemory", o.smallmemory, false);
}

TEST_CASE("options.ini: string/int passthrough rows (schemefilename, modem/net)") {
    const TempFile p = options_file(
        "schemefilename=BASIC.SCH\n"
        "modemdial=555-1234\n"
        "modemport=1\n"
        "modembaud=2\n"
        "modemirq=3\n"
        "netprotocol=9\n"  // clamp: > 3 -> 3
        "levelno=-5\n");   // clamp: < -1 -> -1
    const Options o = load_options(p.path());
    check_row("schemefilename", o.schemefilename, "BASIC.SCH");
    check_row("modemdial", o.modemdial, "555-1234");
    check_row("modemport", o.modemport, 1);
    check_row("modembaud", o.modembaud, 2);
    check_row("modemirq", o.modemirq, 3);
    check_row("netprotocol", o.netprotocol, 3);
    check_row("levelno", o.levelno, -1);
}

TEST_CASE("options.ini: fullscreen is a port-only bool key, round-trips like the RE'd toggles") {
    // Not one of §3's confirmed 22 keys (install.hpp's Options::fullscreen
    // doc) — the 1997 binary has no fullscreen concept — but it must parse,
    // clamp-free, and read-modify-write exactly like every RE'd bool row.
    const TempFile p = options_file("levelno=1\nfullscreen=1\n");
    check_row("fullscreen", load_options(p.path()).fullscreen, true);

    // Absent key -> windowed (the caller's default).
    const TempFile absent = options_file("levelno=1\n");
    CHECK(!load_options(absent.path()).fullscreen.has_value());
}

TEST_CASE("save_options: fullscreen= is appended/rewritten in place, preserving unknown lines") {
    const TempFile p = options_file(";Bomberman Options file.\nlevelno=1\n");

    Options opts;
    opts.fullscreen = true;
    save_options(p.path(), opts);
    check_row("fullscreen", load_options(p.path()).fullscreen, true);

    // Flip it and re-save: must rewrite in place, not duplicate the line.
    opts.fullscreen = false;
    save_options(p.path(), opts);
    const std::string body = p.text();
    CHECK(body.find(";Bomberman Options file.") != std::string::npos);  // untouched line survives
    check_written_once(body, "fullscreen=");
    check_row("fullscreen", load_options(p.path()).fullscreen, false);
}

TEST_CASE("options.ini: the four Video Settings keys are port-only bools that round-trip") {
    // vsync/native_cadence/show_fps/soft_scaling (install.hpp) — none of them
    // exist in the 1997 binary, all four persist through the same
    // read-modify-write as the RE'd rows.
    const TempFile p = options_file("vsync=0\nnative_cadence=1\nshow_fps=1\nsoft_scaling=1\n");
    const Options o = load_options(p.path());
    check_row("vsync", o.vsync, false);
    check_row("native_cadence", o.native_cadence, true);
    check_row("show_fps", o.show_fps, true);
    check_row("soft_scaling", o.soft_scaling, true);

    // soft_scaling=0 is a real false, not "absent".
    const TempFile off = options_file("soft_scaling=0\n");
    check_row("soft_scaling", load_options(off.path()).soft_scaling, false);
}

TEST_CASE("options.ini: an OLDER file (no soft_scaling=) loads fine and defaults to crisp") {
    // Backward compatibility, both directions of the version skew:
    //  - a file written before the key existed must load with the field EMPTY,
    //    so GameApp falls back to its own default (OFF/crisp);
    //  - and it must not be disturbed by the absence (no throw, no other field
    //    perturbed).
    const TempFile p = options_file(";Bomberman Options file.\nlevelno=1\nvsync=1\n");
    const Options o = load_options(p.path());
    CHECK(!o.soft_scaling.has_value());
    check_row("vsync", o.vsync, true);
}

TEST_CASE("options.ini: an UNKNOWN (newer/hand-added) key never breaks the load or the save") {
    // The other direction of the same skew: an options.ini written by a NEWER
    // build (or hand-edited) carries keys this build has never heard of. They
    // must be ignored on read and survive a write VERBATIM — otherwise saving
    // from an older exe would silently strip a newer build's settings.
    const TempFile p = options_file(
        ";Bomberman Options file.\n"
        "levelno=1\n"
        "some_future_option=7\n"
        "soft_scaling=1\n");
    const Options o = load_options(p.path());
    check_row("levelno", o.levelno, 1);
    check_row("soft_scaling", o.soft_scaling, true);

    Options out;
    out.soft_scaling = false;
    save_options(p.path(), out);

    const std::string body = p.text();
    CHECK(body.find("some_future_option=7") != std::string::npos);  // untouched
    check_written_once(body, "soft_scaling=");
    check_row("soft_scaling", load_options(p.path()).soft_scaling, false);
}

TEST_CASE("save_options: soft_scaling= is appended to a file that lacks it") {
    const TempFile p = options_file("levelno=1\n");
    Options out;
    out.soft_scaling = true;
    save_options(p.path(), out);

    const Options reread = load_options(p.path());
    check_row("soft_scaling", reread.soft_scaling, true);
    CHECK(reread.levelno.has_value());  // the pre-existing key survived
}

TEST_CASE(
    "options.ini: keydef= triples parse into KeyDef, out-of-range set/action drops the line") {
    const TempFile p = options_file(
        "keydef=0,0,200\n"
        "keydef=0,4,57\n"
        "keydef=1,5,3\n"
        "keydef=2,0,99\n"    // set out of [0,1] -> dropped
        "keydef=0,10,1\n");  // action out of [0,9] -> dropped
    const Options o = load_options(p.path());
    REQUIRE(o.keydef.has_value());
    CHECK(o.keydef->scancode[0][0] == 200);
    CHECK(o.keydef->scancode[0][4] == 57);
    CHECK(o.keydef->scancode[1][5] == 3);
    // Untouched slots stay at the "absent" sentinel.
    CHECK(o.keydef->scancode[0][1] == -1);
    CHECK(o.keydef->scancode[1][0] == -1);
}

TEST_CASE("save_options: keydef= round-trips all set (set,action) triples and skips absent slots") {
    const auto path = bomber::test::absent_path("bomber_opts_keydef.ini");

    Options opts;
    KeyDef kd;
    kd.scancode[0][0] = 200;  // Up, set 0
    kd.scancode[0][4] = 57;   // Action1, set 0
    kd.scancode[1][3] = 30;   // Left, set 1
    // Every other slot stays -1 (absent) and must NOT be written.
    opts.keydef = kd;
    save_options(path, opts);

    const Options reread = load_options(path);
    REQUIRE(reread.keydef.has_value());
    CHECK(reread.keydef->scancode[0][0] == 200);
    CHECK(reread.keydef->scancode[0][4] == 57);
    CHECK(reread.keydef->scancode[1][3] == 30);
    CHECK(reread.keydef->scancode[0][1] == -1);  // never written -> absent on reread
    std::filesystem::remove(path);
}

TEST_CASE(
    "save_options: re-saving keydef= rewrites a (set,action) triple in place, not duplicated") {
    const TempFile p = options_file("keydef=0,0,200\nlevelno=1\n");

    Options opts;
    KeyDef kd;
    kd.scancode[0][0] = 205;  // rebind the SAME (set,action) to a new scancode
    opts.keydef = kd;
    save_options(p.path(), opts);

    const std::string body = p.text();
    CHECK(body.find("levelno=1") != std::string::npos);  // untouched line survives
    const Options reread = load_options(p.path());
    REQUIRE(reread.keydef.has_value());
    CHECK(reread.keydef->scancode[0][0] == 205);
    check_written_once(body, "keydef=0,0,");
}

// --- nodename.ini: the net identity's own file (sub_40C08C / sub_40C140) ----
// docs/re/network-screens.md §3 "Session model", docs/re/results-and-options.md
// "Net identity". NOT one of options.ini's 22 keys — its own one-line file with
// its own reader (boot init) and writer (shutdown hook).

TEST_CASE("nodename.ini: the first line is the name (sub_40C08C's fgets)") {
    const TempFile p = node_file("Egerion");
    CHECK(load_node_name(p.path()) == "Egerion");

    // The shipped file has no trailing newline; one written by a text editor
    // must read identically ('\n' is what sub_40C08C strips), and a second line
    // is not part of the name.
    const TempFile two_lines = node_file("Neil's House Of Pain\r\nignored second line\n");
    CHECK(load_node_name(two_lines.path()) == "Neil's House Of Pain");
}

TEST_CASE("nodename.ini: absent or blank file yields an empty name") {
    // The caller (GameApp) then draws the original's random MESSAGES 500..548
    // default — this loader never invents one.
    CHECK(load_node_name(bomber::test::absent_path("bomber_node_absent.ini")).empty());
    const TempFile blank = node_file("   \n");
    CHECK(load_node_name(blank.path()).empty());
}

TEST_CASE("nodename.ini: hostile content is sanitised on read AND on write") {
    // The name goes straight into the lobby roster, so a hand-edited file must
    // not be able to smuggle in control bytes or an over-long run.
    const TempFile control_bytes = node_file(std::string("A\x01\x02Z\x7f!"));
    CHECK(load_node_name(control_bytes.path()) == "AZ!");

    const TempFile overlong = node_file("");
    save_node_name(overlong.path(), std::string(kNodeNameMax + 25, 'X'));
    CHECK(overlong.text() == std::string(kNodeNameMax, 'X'));
    CHECK(load_node_name(overlong.path()).size() == kNodeNameMax);
}

TEST_CASE("nodename.ini: save/load round-trips and rewrites in place") {
    const TempFile p = node_file("OLD NAME");
    save_node_name(p.path(), "NEW NAME");
    // One line, nothing else (sub_40C140 is a single fputs of the buffer).
    CHECK(p.text() == "NEW NAME");
    CHECK(load_node_name(p.path()) == "NEW NAME");
}
