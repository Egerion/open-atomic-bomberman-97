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

TEST_CASE("spawn pocket: dense board corners clear to the pinned 9-cell plus, arms length 2") {
    Simulation s(dense_corners_config());
    const State& st = s.state();

    // Player 0 at (0,0): the pocket's axis-aligned cells within the grid are
    // Blank (the tile itself + 2 tiles right + 2 tiles down); the diagonal
    // pillar at (1,1) is untouched (already Solid, not Brick); everything
    // one tile past each arm's tip is untouched Brick.
    CHECK(st.cells[0][0] == Cell::Blank);  // spawn tile
    CHECK(st.cells[0][1] == Cell::Blank);  // +1 right
    CHECK(st.cells[0][2] == Cell::Blank);  // +2 right (arm tip)
    CHECK(st.cells[1][0] == Cell::Blank);  // +1 down
    CHECK(st.cells[2][0] == Cell::Blank);  // +2 down (arm tip)
    CHECK(st.cells[1][1] == Cell::Solid);  // diagonal pillar, never touched
    CHECK(st.cells[0][3] == Cell::Brick);  // +3 right: past the arm, still Brick
    CHECK(st.cells[3][0] == Cell::Brick);  // +3 down: past the arm, still Brick
    CHECK(st.cells[2][1] == Cell::Brick);  // off-axis (not on either ray): untouched

    // Player 1 at (14,10), the opposite corner: same shape, mirrored, and the
    // grid-edge clamp in the offset loop doesn't over- or under-clear.
    CHECK(st.cells[10][14] == Cell::Blank);  // spawn tile
    CHECK(st.cells[10][13] == Cell::Blank);  // -1 left
    CHECK(st.cells[10][12] == Cell::Blank);  // -2 left (arm tip)
    CHECK(st.cells[9][14] == Cell::Blank);   // -1 up
    CHECK(st.cells[8][14] == Cell::Blank);   // -2 up (arm tip)
    CHECK(st.cells[9][13] == Cell::Solid);   // diagonal pillar, never touched
    CHECK(st.cells[10][11] == Cell::Brick);  // -3 left: past the arm, still Brick
    CHECK(st.cells[7][14] == Cell::Brick);   // -3 up: past the arm, still Brick

    // Player 2 at (14,0) and player 3 at (0,10): the other two corners, spot-
    // checked for the same arm-tip/off-arm boundary.
    CHECK(st.cells[0][14] == Cell::Blank);   // spawn tile
    CHECK(st.cells[0][12] == Cell::Blank);   // -2 left (arm tip)
    CHECK(st.cells[2][14] == Cell::Blank);   // +2 down (arm tip)
    CHECK(st.cells[0][11] == Cell::Brick);   // -3 left: past the arm
    CHECK(st.cells[10][0] == Cell::Blank);   // spawn tile
    CHECK(st.cells[10][2] == Cell::Blank);   // +2 right (arm tip)
    CHECK(st.cells[8][0] == Cell::Blank);    // -2 up (arm tip)
    CHECK(st.cells[10][3] == Cell::Brick);   // +3 right: past the arm
}

TEST_CASE("spawn pocket: AI players survive the opening 10s on a dense 4-corner board") {
    // The exact scenario the fix targets: a golden-B-shaped dense board, all
    // 4 slots computer-controlled (ADR-0005), no human input at all — the
    // AISystem drives every drop/flee decision, including each player's very
    // first bomb at/near its own spawn. Before the fix (radius-1 pocket) this
    // reliably wiped out every AI within the first few seconds; the widened
    // radius-2 pocket gives the flee logic a tile outside its own blast to
    // retreat to. Seed picked from golden B's own (matches the board shape
    // this file shares with tests/test_golden.cpp); the outcome is a
    // deterministic replay, not a statistical claim.
    MatchConfig cfg = dense_corners_config();
    for (int i = 0; i < 4; ++i) cfg.ai[i] = true;
    cfg.seed = 0xB0BB1E5;
    Simulation s(cfg);

    for (int t = 0; t < 200; ++t) s.tick(TickInputs{});  // 200 ticks @ 20 Hz = 10s

    CHECK(alive_count(s.state()) == 4);  // nobody self-killed in the opening pocket
}
