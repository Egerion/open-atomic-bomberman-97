// Malformed-input contract for the .SCH loader. bomber::assets loaders treat
// 1997 files as untrusted and signal bad input with std::runtime_error /
// std::out_of_range (never a std::logic_error), which is what callers catch.
// A non-numeric numeric field used to reach an unguarded std::stoi and escape
// as std::invalid_argument (a std::logic_error) — these cases pin that the
// loader now re-tags such fields as std::runtime_error. SYNTHETIC schemes
// only; a real shipped .SCH is never committed.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include "bomber/assets/sch.hpp"

using namespace bomber::assets::sch;

namespace {

// Write `text` to a scratch .SCH and hand back the path; the caller removes it.
std::filesystem::path write_temp(const char* file_name, const std::string& text) {
    auto path = std::filesystem::temp_directory_path() / file_name;
    std::ofstream f(path, std::ios::binary);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    f.close();
    return path;
}

}  // namespace

TEST_CASE("non-numeric -V field throws std::runtime_error, not std::logic_error") {
    auto path = write_temp("obm_bad_version.sch", "-V,x\n-R,0,###\n-R,1,#.#\n-R,2,###\n");
    // std::invalid_argument derives from std::logic_error, NOT std::runtime_error,
    // so CHECK_THROWS_AS(..., std::runtime_error) fails if the guard regresses.
    CHECK_THROWS_AS(load(path), std::runtime_error);
    std::filesystem::remove(path);
}

TEST_CASE("non-numeric -S spawn field throws std::runtime_error") {
    auto path = write_temp("obm_bad_spawn.sch", "-V,3\n-R,0,###\n-R,1,#.#\n-R,2,###\n-S,x,2,2\n");
    CHECK_THROWS_AS(load(path), std::runtime_error);
    std::filesystem::remove(path);
}

TEST_CASE("non-numeric -P powerup field throws std::runtime_error") {
    auto path =
        write_temp("obm_bad_powerup.sch", "-V,3\n-R,0,###\n-R,1,#.#\n-R,2,###\n-P,x,0,0,0,0\n");
    CHECK_THROWS_AS(load(path), std::runtime_error);
    std::filesystem::remove(path);
}

TEST_CASE("a well-formed scheme still loads unchanged") {
    // Guards that re-tagging the numeric parse did not disturb valid-file
    // parsing: every numeric field here must round-trip to its exact value.
    auto path = write_temp("obm_good.sch",
                           "-V,3\n-N,OK\n-B,90\n-R,0,#####\n-R,1,#...#\n-R,2,#####\n-S,0,1,1\n");
    Scheme s = load(path);
    CHECK(s.version == 3);
    CHECK(s.name == "OK");
    CHECK(s.brick_density == 90);
    REQUIRE(s.rows.size() == 3);
    CHECK(s.rows[0] == "#####");
    REQUIRE(s.spawns.size() == 1);
    CHECK(s.spawns[0].x == 1);
    CHECK(s.spawns[0].y == 1);
    std::filesystem::remove(path);
}
