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

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "bomber/assets/install.hpp"

namespace fs = std::filesystem;
using bomber::assets::load_options;
using bomber::assets::save_options;
using bomber::assets::Options;

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
