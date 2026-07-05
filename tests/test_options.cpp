// Checks for the install-root options.ini reader (bomber::assets::load_options,
// a faithful skim of sub_406238). The original splits each line on '=', matches
// the key case-insensitively, and atoi's the value; a missing key/file leaves
// the field empty so the caller keeps the binary default. Only conveyor_speed
// is surfaced (the Conveyor Speed game option, dword_464930). See
// docs/re/stage-actors.md §3.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "bomber/assets/install.hpp"

namespace fs = std::filesystem;
using bomber::assets::load_options;

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
