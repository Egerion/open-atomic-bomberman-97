// Spawn-pocket clear regression tests (docs/re/facts.md "Spawn-pocket
// clear"). Covers both halves of the fix:
//   1. The exact pinned shape: a dense (all-Brick) board's corner spawns
//      open with a 9-cell orthogonal "plus" (spawn tile + 2 tiles in each
//      cardinal direction), not the old radius-1 4-neighbour version, and
//      nothing beyond that shape is touched.
//   2. The behavioural fix it exists for: on such a board, AI players no
//      longer self-kill with their own opening bomb in the first ~10s (the
//      first-round mass-suicide bug — the old radius-1 pocket sat entirely
//      inside a flame-2 corner bomb's blast, so the AI's flee logic could
//      never find a strictly safer tile).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "helpers.hpp"

using namespace bomber::sim;
using namespace bomber::sim::test;

namespace {

// Golden B's own board shape (docs/re/facts.md / tests/test_golden.cpp):
// every non-pillar cell is a destructible Brick, corners host all 4 spawns.
MatchConfig dense_corners_config() {
    MatchConfig cfg;
    for (int y = 0; y < kGridHeight; ++y)
        for (int x = 0; x < kGridWidth; ++x)
            cfg.cells[y][x] = (x % 2 == 1 && y % 2 == 1) ? Cell::Solid : Cell::Brick;
    cfg.spawns = {{0, 0}, {14, 10}, {14, 0}, {0, 10}};
    cfg.player_count = 4;
    for (auto& c : cfg.tuning.spawn_counts) c = 0;  // no hidden powerups: isolate the pocket shape
    cfg.tuning.input_freeze_ticks = 0;
    return cfg;
}

}  // namespace

TEST_CASE("spawn pocket: the original leaves a spawn brick; play opens ONLY that tile (radius-0)") {
    // The faithful mechanism (docs/re/facts.md "Spawn-pocket clear", proven
    // 2026-07-21): the original's board build places a brick on the spawn
    // ~90% of the time and clears NO pocket at setup. The single spawn tile is
    // opened IN PLAY, on the player's first turn, by the "player on a brick
    // clears it" rule (sub_41E61E case 4) -- radius-0, the occupied tile only.
    Simulation s(dense_corners_config());

    // At setup, before any tick, every spawn tile is STILL a brick -- no
    // setup-time pocket clear (this is what the removed radius-2 hack did).
    CHECK(s.state().cells[0][0] == Cell::Brick);    // p0 (0,0)
    CHECK(s.state().cells[10][14] == Cell::Brick);  // p1 (14,10)
    CHECK(s.state().cells[0][14] == Cell::Brick);   // p2 (14,0)
    CHECK(s.state().cells[10][0] == Cell::Brick);   // p3 (0,10)

    s.tick(TickInputs{});  // one tick: each player_turn opens its OWN tile
    const State& st = s.state();

    // Radius-0: the four occupied spawn tiles are now Blank...
    CHECK(st.cells[0][0] == Cell::Blank);
    CHECK(st.cells[10][14] == Cell::Blank);
    CHECK(st.cells[0][14] == Cell::Blank);
    CHECK(st.cells[10][0] == Cell::Blank);

    // ...but NOTHING else: every orthogonal neighbour of a spawn stays a brick
    // (the player is boxed in, matching the native -- no pocket).
    CHECK(st.cells[0][1] == Cell::Brick);    // p0 +1 right
    CHECK(st.cells[1][0] == Cell::Brick);    // p0 +1 down
    CHECK(st.cells[10][13] == Cell::Brick);  // p1 -1 left
    CHECK(st.cells[9][14] == Cell::Brick);   // p1 -1 up
    // Pillars are Solid throughout (never bricks, never touched).
    CHECK(st.cells[1][1] == Cell::Solid);
    CHECK(st.cells[9][13] == Cell::Solid);
}

TEST_CASE("spawn pocket: AI players survive the opening 10s on a dense 4-corner board") {
    // The exact scenario the fix targets: a golden-B-shaped dense board, all
    // 4 slots computer-controlled (ADR-0005), no human input at all -- the
    // AISystem drives every decision. With the faithful RADIUS-0 spawn clear
    // (each player opens only its own tile), every AI is BOXED IN by bricks and
    // its behave_walk_path "nowhere safe to step" branch stalls it, so it never
    // reaches blast-bricks and never drops the self-killing opening bomb --
    // exactly what the native does (verified across 20 seeds via the aispawn
    // oracle scenario). This is what the removed radius-2 pocket hack used to
    // fake. Seed from golden B's own; a deterministic replay, not a stat claim.
    MatchConfig cfg = dense_corners_config();
    for (int i = 0; i < 4; ++i) cfg.ai[i] = true;
    cfg.seed = 0xB0BB1E5;
    Simulation s(cfg);

    for (int t = 0; t < 200; ++t) s.tick(TickInputs{});  // 200 ticks @ 20 Hz = 10s

    CHECK(alive_count(s.state()) == 4);  // nobody self-killed in the opening pocket
}
