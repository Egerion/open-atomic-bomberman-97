// EXTRA<N>.RES stage-actor parser (docs/re/stage-actors.md §2). Another
// untrusted-input text parser that had no suite of its own — its only coverage
// was indirect, through the sim's stage_actors tests, which start from actors
// that are already placed.
//
// Every case drives the REAL parser over a file on disk. Synthetic files only.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "bomber/assets/extra.hpp"
#include "fixture.hpp"

using namespace bomber::assets::extra;
using bomber::test::TempFile;

namespace {

// The board the shipped schemes use, and what the coordinate normalisation in
// sub_404E99 is measured against below.
constexpr int kW = 15;
constexpr int kH = 11;

std::vector<Actor> parse_text(const char* name, const std::string& text) {
    const TempFile tf(name, text);
    return parse(tf.path(), kW, kH);
}

}  // namespace

TEST_CASE("arrows and conveyors carry their godir and tile") {
    const auto actors = parse_text("obm_extra_dirs.res",
                                   "; a comment\n"
                                   "-A,N, 2, 3\n"
                                   "-A,e,4,5\n"
                                   "-C,S,6,7\n"
                                   "-C,w,8,9\n");
    REQUIRE(actors.size() == 4);
    CHECK(actors[0].kind == Kind::DirArrow);
    CHECK(actors[0].dir == 0);  // n -> Up
    CHECK(actors[0].x == 2);
    CHECK(actors[0].y == 3);
    CHECK(actors[1].dir == 1);  // e -> Right
    CHECK(actors[2].kind == Kind::Conveyor);
    CHECK(actors[2].dir == 2);  // s -> Down
    CHECK(actors[3].dir == 3);  // w -> Left
}

TEST_CASE("a trampoline is placed, or deferred to the caller's RNG") {
    const auto actors = parse_text("obm_extra_tramp.res", "-T,3,4\n-T,H,H\n");
    REQUIRE(actors.size() == 2);
    CHECK(actors[0].kind == Kind::Trampoline);
    CHECK_FALSE(actors[0].random);
    CHECK(actors[0].x == 3);
    CHECK(actors[0].y == 4);
    // '-T,H,H' leaves x/y unset and asks the caller to resolve them.
    CHECK(actors[1].random);
    CHECK(actors[1].x == 0);
    CHECK(actors[1].y == 0);
}

TEST_CASE("a warphole keeps its id and link target") {
    const auto actors = parse_text("obm_extra_warp.res", "-W,W,1,5,6,2\n");
    REQUIRE(actors.size() == 1);
    CHECK(actors[0].kind == Kind::Warphole);
    CHECK(actors[0].idno == 1);
    CHECK(actors[0].x == 5);
    CHECK(actors[0].y == 6);
    CHECK(actors[0].linkto == 2);
}

TEST_CASE("negative coordinates wrap up from the far edge and over-large ones clamp") {
    // sub_404E99: `while (v < 0) v += extent`, then clamp to extent-1. On a
    // 15x11 board -1 is column 14, -15 is column 0, and 99 clamps to 14.
    const auto actors = parse_text("obm_extra_norm.res",
                                   "-A,n,-1,-1\n"
                                   "-A,n,-15,-11\n"
                                   "-A,n,-16,-12\n"
                                   "-A,n,99,99\n");
    REQUIRE(actors.size() == 4);
    CHECK(actors[0].x == 14);
    CHECK(actors[0].y == 10);
    CHECK(actors[1].x == 0);
    CHECK(actors[1].y == 0);
    CHECK(actors[2].x == 14);
    CHECK(actors[2].y == 10);
    CHECK(actors[3].x == 14);
    CHECK(actors[3].y == 10);
}

TEST_CASE("malformed and unknown lines are skipped rather than aborting the file") {
    // The original aborts the whole load; this port keeps going (extra.hpp).
    const auto actors = parse_text("obm_extra_junk.res",
                                   "\n"
                                   "; comment\n"
                                   "-\n"
                                   "-A,q,1,1\n"    // unrecognised direction letter
                                   "-A,n,1\n"      // too few fields
                                   "-T,1\n"        // too few fields
                                   "-W,W,1,1,1\n"  // too few fields
                                   "-Z,1,2,3\n"    // unknown type letter
                                   "not a directive\n"
                                   "-A,n,1,2\n");
    REQUIRE(actors.size() == 1);
    CHECK(actors[0].x == 1);
    CHECK(actors[0].y == 2);
}

TEST_CASE("a board with no EXTRA file simply has no actors") {
    const auto actors = parse(bomber::test::absent_path("obm_extra_absent.res"), kW, kH);
    CHECK(actors.empty());
}

// SECURITY. The coordinate wrap was transcribed from the binary literally as
// `while (v < 0) v += extent`, which makes the ITERATION COUNT a function of
// the file: the smallest int a field can hold spins ~143 million times per
// coordinate on a 15-wide board, and this file asks for it 256 times — roughly
// 37 billion additions, minutes of wall clock, from a 4 KB file. The closed
// form must land on the same answer instantly.
//
// A regression here does not throw, it HANGS, so this suite carries a ctest
// TIMEOUT (tests/assets/CMakeLists.txt) to turn that into a red test rather
// than a stuck pre-push gate.
TEST_CASE("SECURITY: an extreme negative coordinate normalises in constant time") {
    std::string text;
    for (int i = 0; i < 256; ++i) text += "-A,n,-2147483648,0\n";
    const auto actors = parse_text("obm_extra_dos.res", text);
    REQUIRE(actors.size() == 256);
    // -2147483648 mod 15 == 7 (15 * 143165577 - 2147483648 == 7), which is
    // exactly where the repeated addition would have landed.
    for (const Actor& a : actors) CHECK(a.x == 7);
}

TEST_CASE("SECURITY: a zero-extent board terminates instead of looping forever") {
    // `while (v < 0) v += 0` has no terminating case at all. A board with no
    // tiles has no position to normalise to, so every coordinate collapses to 0.
    const TempFile tf("obm_extra_zero.res", std::string("-A,n,-1,-1\n-T,-5,-5\n"));
    const auto actors = parse(tf.path(), 0, 0);
    REQUIRE(actors.size() == 2);
    CHECK(actors[0].x == 0);
    CHECK(actors[0].y == 0);
    CHECK(actors[1].x == 0);
    CHECK(actors[1].y == 0);
}
