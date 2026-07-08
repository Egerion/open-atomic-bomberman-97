// Round-trip checks for the .SCH writer (sub_403C16 @0x403C16, docs/re/
// results-and-options.md #5). Contract: parse(write(scheme)) == scheme for
// every field load() reads (version/name/density/rows/spawns/powerups) —
// SYNTHETIC schemes only, never a real shipped .SCH (never committed).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include "bomber/assets/sch.hpp"

using namespace bomber::assets::sch;

namespace {

Scheme make_basic_scheme() {
    Scheme s;
    s.version = 3;
    s.name = "TEST ARENA";
    s.brick_density = 90;
    s.rows = {
        "###############",
        "#.............#",
        "#.###.#.#.###.#",
        "#.............#",
        "#.#.#.#.#.#.#.#",
        "#.............#",
        "#.#.#.#.#.#.#.#",
        "#.............#",
        "#.###.#.#.###.#",
        "#.............#",
        "###############",
    };
    for (int i = 0; i < 10; ++i) {
        Spawn sp;
        sp.player = i;
        sp.x = 1 + i;
        sp.y = 1;
        sp.extra = i % 2;
        s.spawns.push_back(sp);
    }
    for (int i = 0; i < 13; ++i) {
        PowerupRule pr;
        pr.id = i;
        pr.born_with = (i == 0) ? 1 : 0;
        pr.has_override = (i == 1) ? 1 : 0;
        pr.override_value = (i == 1) ? 5 : 0;
        pr.forbidden = (i == 2) ? 1 : 0;
        pr.comment = "powerup " + std::to_string(i);
        s.powerups.push_back(pr);
    }
    return s;
}

void check_round_trip(const Scheme& s, const char* file_name) {
    auto path = std::filesystem::temp_directory_path() / file_name;
    write(s, path);
    Scheme back = load(path);

    CHECK(back.version == s.version);
    CHECK(back.name == s.name);
    CHECK(back.brick_density == s.brick_density);
    REQUIRE(back.rows.size() == s.rows.size());
    for (std::size_t i = 0; i < s.rows.size(); ++i) CHECK(back.rows[i] == s.rows[i]);
    REQUIRE(back.spawns.size() == s.spawns.size());
    for (std::size_t i = 0; i < s.spawns.size(); ++i) {
        CHECK(back.spawns[i].player == s.spawns[i].player);
        CHECK(back.spawns[i].x == s.spawns[i].x);
        CHECK(back.spawns[i].y == s.spawns[i].y);
        CHECK(back.spawns[i].extra == s.spawns[i].extra);
    }
    REQUIRE(back.powerups.size() == s.powerups.size());
    for (std::size_t i = 0; i < s.powerups.size(); ++i) {
        CHECK(back.powerups[i].id == s.powerups[i].id);
        CHECK(back.powerups[i].born_with == s.powerups[i].born_with);
        CHECK(back.powerups[i].has_override == s.powerups[i].has_override);
        CHECK(back.powerups[i].override_value == s.powerups[i].override_value);
        CHECK(back.powerups[i].forbidden == s.powerups[i].forbidden);
        CHECK(back.powerups[i].comment == s.powerups[i].comment);
    }

    std::filesystem::remove(path);
}

}  // namespace

TEST_CASE("round-trips a typical scheme") {
    check_round_trip(make_basic_scheme(), "obm_test_basic.sch");
}

TEST_CASE("round-trips an all-solid scheme") {
    Scheme s = make_basic_scheme();
    for (auto& row : s.rows) row = std::string(row.size(), '#');
    check_round_trip(s, "obm_test_all_solid.sch");
}

TEST_CASE("round-trips an all-blank scheme") {
    Scheme s = make_basic_scheme();
    for (auto& row : s.rows) row = std::string(row.size(), '.');
    check_round_trip(s, "obm_test_all_blank.sch");
}

TEST_CASE("round-trips an all-brick scheme") {
    Scheme s = make_basic_scheme();
    for (auto& row : s.rows) row = std::string(row.size(), ':');
    check_round_trip(s, "obm_test_all_brick.sch");
}

TEST_CASE("round-trips every powerup override kind") {
    // Every kind forbidden, born-with, AND overridden simultaneously — the
    // widest per-row combination the format allows (§5: "per-kind forbidden/
    // override rows"), so a single test exercises every flag path at once.
    Scheme s = make_basic_scheme();
    for (auto& pr : s.powerups) {
        pr.born_with = 1;
        pr.has_override = 1;
        pr.override_value = 42;
        pr.forbidden = 1;
    }
    check_round_trip(s, "obm_test_all_override.sch");
}

TEST_CASE("round-trips zero density, zero version, empty name, no comment") {
    Scheme s = make_basic_scheme();
    s.version = 0;
    s.name.clear();
    s.brick_density = 0;
    for (auto& pr : s.powerups) pr.comment.clear();
    check_round_trip(s, "obm_test_edge_empty.sch");
}

TEST_CASE("round-trips max density and no spawns/powerups") {
    Scheme s = make_basic_scheme();
    s.brick_density = 100;
    s.spawns.clear();
    s.powerups.clear();
    check_round_trip(s, "obm_test_edge_no_extras.sch");
}

TEST_CASE("to_text emits the documented directive order") {
    // sub_403C16 (§5): "-V" version, "-N" name, "-B" density, the "-R" row
    // array, 10 "-S" player starts, then 13 "-P" powerup rows, in that
    // fixed order — not merely present, but sequenced this way.
    Scheme s = make_basic_scheme();
    std::string text = to_text(s);
    auto v = text.find("-V,");
    auto n = text.find("-N,");
    auto b = text.find("-B,");
    auto r = text.find("-R,");
    auto sPos = text.find("-S,");
    auto p = text.find("-P,");
    REQUIRE(v != std::string::npos);
    REQUIRE(n != std::string::npos);
    REQUIRE(b != std::string::npos);
    REQUIRE(r != std::string::npos);
    REQUIRE(sPos != std::string::npos);
    REQUIRE(p != std::string::npos);
    CHECK(v < n);
    CHECK(n < b);
    CHECK(b < r);
    CHECK(r < sPos);
    CHECK(sPos < p);
}
