#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "bomber/assets/campaign.hpp"

using bomber::assets::res::parse_campaign;

// NB: all sample text here is SYNTHETIC — the real .CAM files (CROUTON.CAM,
// GHOSTS.CAM, SIMPLE.CAM) are the user's own game data and are neither
// embedded nor committed (CLAUDE.md). These only exercise the "-C,<9 fields>"
// grammar documented in docs/re/campaign.md (confirmed against
// DATA/RES/SIMPLE.CAM's own header comment).

TEST_CASE("campaign: -C stage lines, comments, and header commentary skipped") {
    auto c = parse_campaign(
        "; Field descriptions:\n"
        ";\t0. campaign name\n"
        ";\t1. levelno\n"
        ";\t2. scheme to use\n"
        "\n"
        "-C,Just One Ghost,           1,basic,    0,  0, 1,150, 0, 50\n"
        "-C,Second Stage,2,advanced,1,100,0,0,2,75\n");
    REQUIRE(c.stages.size() == 2);
    CHECK(c.stages[0].name == "Just One Ghost");
    CHECK(c.stages[0].level_no == 1);
    CHECK(c.stages[0].scheme == "basic");
    CHECK(c.stages[0].rovers == 0);
    CHECK(c.stages[0].rover_speed == 0);
    CHECK(c.stages[0].ghosts == 1);
    CHECK(c.stages[0].ghost_speed == 150);
    CHECK(c.stages[0].ai_count == 0);
    CHECK(c.stages[0].ai_difficulty == 50);

    CHECK(c.stages[1].name == "Second Stage");
    CHECK(c.stages[1].level_no == 2);
    CHECK(c.stages[1].scheme == "advanced");
    CHECK(c.stages[1].rovers == 1);
    CHECK(c.stages[1].rover_speed == 100);
    CHECK(c.stages[1].ai_count == 2);
    CHECK(c.stages[1].ai_difficulty == 75);
    CHECK(c.warnings.empty());
}

TEST_CASE("campaign: case-insensitive marker, malformed -C line warned") {
    // Lowercase '-c' is accepted (toupper() == 'C' per the loader).
    auto ok = parse_campaign("-c,Lower Case,1,basic,0,0,1,150,0,50\n");
    REQUIRE(ok.stages.size() == 1);
    CHECK(ok.stages[0].name == "Lower Case");

    // Fewer than 9 fields after the marker is malformed, not silently kept.
    auto bad = parse_campaign("-C,Too Few Fields,1,basic\n");
    CHECK(bad.stages.empty());
    CHECK(!bad.warnings.empty());
}

TEST_CASE("campaign: non-marker lines (plain ; comments, blanks, other dashes) skipped") {
    auto c = parse_campaign(
        "; just a comment\n"
        "\n"
        "-V,1\n"  // a foreign '-X' marker (e.g. from a .SCH-like file) is not "-C"
        "-C,Only One,5,basic,0,0,0,0,10,0\n");
    REQUIRE(c.stages.size() == 1);
    CHECK(c.stages[0].name == "Only One");
    CHECK(c.stages[0].level_no == 5);
    CHECK(c.stages[0].ai_count == 10);
}

TEST_CASE("campaign: empty input is empty, no crash") {
    auto c = parse_campaign("");
    CHECK(c.stages.empty());
    CHECK(c.warnings.empty());
    CHECK(c.empty());
}
