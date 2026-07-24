// Per-level tile regeneration (VALUELST ids 340-350 + 695): on Haunted House
// (level index 7, the file's own comment calls it "cemetary/mortuary"),
// destroyed bricks slowly regrow at random blank tiles clear of every live
// player. A faithful port of sub_426704 (called from the enclosure stepper
// sub_426818). See docs/re/facts.md "Per-level tile regeneration".

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "helpers.hpp"

using namespace bomber::sim;
using namespace bomber::sim::test;

namespace {

// An all-blank board (no pillars) so every tile is a regen candidate.
MatchConfig blank_config(int level_index) {
    MatchConfig cfg = open_config();
    for (auto& row : cfg.cells) row.fill(Cell::Blank);
    cfg.tuning.level_index = level_index;
    return cfg;
}

int count_regrew(const Simulation& s) {
    int n = 0;
    for (const auto& e : s.state().events)
        if (e.type == Event::Type::TileRegrew) ++n;
    return n;
}

int brick_count(const State& st) {
    int n = 0;
    for (const auto& row : st.cells)
        for (Cell c : row)
            if (c == Cell::Brick) ++n;
    return n;
}

}  // namespace

TEST_CASE("tile regen is inert on every level but Haunted House (index 7)") {
    // Default level_index (0, "new traditionalist") has regen_seconds[0] == 0
    // — TileRegenSystem::update() must take its early-return branch every
    // tick: no RNG draw, no state change, regen_timer pinned at 0.
    Simulation s(blank_config(0));
    const auto rng0 = s.state().rng;
    for (int t = 0; t < 5000; ++t) {
        s.tick(TickInputs{});
        CHECK(s.state().regen_timer == 0);
        CHECK(count_regrew(s) == 0);
    }
    CHECK(s.state().rng == rng0);  // zero extra RNG draws over 5000 ticks
    CHECK(brick_count(s.state()) == 0);
}

TEST_CASE("tile regen fires on Haunted House and regrows a blank tile to a brick") {
    Simulation s(blank_config(7));
    // No players nearby: spawns default to (0,0)/(14,10) from open_config(),
    // clear across the board from the regen radius (id 695 == 4 tiles).
    int first_regrow_tick = -1;
    for (int t = 1; t <= 200 && first_regrow_tick < 0; ++t) {
        s.tick(TickInputs{});
        if (count_regrew(s) > 0) first_regrow_tick = t;
    }
    // regen_seconds[7] == 4s == 80 ticks; the timer starts at 0, so the
    // FIRST attempt cycle runs on tick 1 (docs/re/facts.md's "first attempt
    // near round start" simplification of the original's process-lifetime
    // dword_464978 clock).
    CHECK(first_regrow_tick == 1);
    CHECK(brick_count(s.state()) == 1);
    CHECK(s.state().regen_timer == 80);  // reset to a fresh interval

    // A second attempt cycle fires one full interval (80 ticks) after the
    // timer was reset, i.e. 81 ticks after the first attempt (the reset
    // itself consumes the tick it fires on before counting down).
    int second_regrow_tick = -1;
    for (int t = first_regrow_tick + 1; t <= first_regrow_tick + 200 && second_regrow_tick < 0;
         ++t) {
        s.tick(TickInputs{});
        if (count_regrew(s) > 0) second_regrow_tick = t;
    }
    REQUIRE(second_regrow_tick > 0);
    CHECK(second_regrow_tick - first_regrow_tick == 81);
    CHECK(brick_count(s.state()) == 2);
}

TEST_CASE("a live player within the clear radius blocks regen entirely") {
    // Radius covers the WHOLE board (Manhattan distance from any tile to
    // (0,0) is at most 14+10=24), so no candidate tile can ever be eligible
    // while player 0 is present and alive — a fully deterministic negative
    // test that does not depend on which random tile gets rolled.
    MatchConfig cfg = blank_config(7);
    cfg.tuning.regen_clear_radius = 100;
    Simulation s(cfg);
    for (int t = 0; t < 400; ++t) s.tick(TickInputs{});
    CHECK(brick_count(s.state()) == 0);
    // The timer still cycles (an attempt is still MADE every interval, it
    // just never finds an eligible tile) — same as the original resetting
    // dword_464978 unconditionally once the interval elapses.
    CHECK(s.state().regen_timer > 0);
}

TEST_CASE("regen never targets a tile with a bomb or a floor powerup") {
    // Fill every blank tile except exactly one with an obstacle (an active
    // bomb on odd tiles, a floor powerup on even ones), leaving a single
    // legal candidate — regen, if it fires at all, MUST land there.
    MatchConfig cfg = blank_config(7);
    cfg.tuning.regen_clear_radius = 0;  // players (far away) never gate it
    Simulation s(cfg);
    State& st = s.state();
    const int freeX = 7, freeY = 5;
    for (int y = 0; y < kGridHeight; ++y) {
        for (int x = 0; x < kGridWidth; ++x) {
            if (x == freeX && y == freeY) continue;
            if ((x + y) % 2 == 0) {
                st.floor[y][x] = PowerupType::ExtraBomb;
            } else {
                Bomb b;
                b.active = true;
                b.owner = 0;
                b.x = x * kTileWF + kTileWF / 2;
                b.y = y * kTileHF + kTileHF / 2;
                b.fuse = 100000;
                st.bombs.push_back(b);
            }
        }
    }
    int regrow_tick = -1;
    for (int t = 1; t <= 200 && regrow_tick < 0; ++t) {
        s.tick(TickInputs{});
        if (count_regrew(s) > 0) regrow_tick = t;
    }
    REQUIRE(regrow_tick > 0);
    CHECK(st.cells[freeY][freeX] == Cell::Brick);
    CHECK(brick_count(st) == 1);
}

TEST_CASE("the regen timer and per-player ice history are part of the hashed state") {
    // Same seed/board, only level_index differs: once regen has actually
    // fired on the Haunted House copy the hashes must diverge (proving the
    // new cells + regen_timer fields are mixed into state_hash()).
    Simulation plain(blank_config(0));
    Simulation haunted(blank_config(7));
    for (int t = 0; t < 5; ++t) {
        plain.tick(TickInputs{});
        haunted.tick(TickInputs{});
    }
    CHECK(count_regrew(haunted) + brick_count(haunted.state()) > 0);
    CHECK(plain.hash() != haunted.hash());
}
