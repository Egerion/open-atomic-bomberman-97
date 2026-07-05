// bomber::match tests: the per-match destructible-brick fill in
// build_match_config (docs/re/stage-actors.md / facts.md brick-fill: sub_4260F5
// ~26928). A scheme's ':' cells are brick CANDIDATES; each is kept as a real
// brick with brick_density% probability, off a setup-only seed — never the
// sim's per-tick RNG. Regression guard: the fill used to be skipped (every ':'
// became a brick at 100%), giving every match the same fixed layout.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "bomber/assets/extra.hpp"
#include "bomber/match/match_factory.hpp"

using namespace bomber;

namespace {

// A full-width/height scheme whose every playable cell is a brick candidate,
// except a few '#'/'.' cells to check verbatim copying.
assets::sch::Scheme make_scheme(int density) {
    assets::sch::Scheme s;
    s.version = 2;
    s.name = "test";
    s.brick_density = density;
    for (int y = 0; y < sim::kGridHeight; ++y) {
        std::string row(sim::kGridWidth, ':');  // all brick candidates
        s.rows.push_back(row);
    }
    // Fixed landmarks: a solid at (0,0) and a blank at (1,0); these must survive
    // any density unchanged (the fill only touches ':' cells).
    s.rows[0][0] = '#';
    s.rows[0][1] = '.';
    return s;
}

int count_bricks(const sim::MatchConfig& cfg) {
    int n = 0;
    for (int y = 0; y < sim::kGridHeight; ++y)
        for (int x = 0; x < sim::kGridWidth; ++x)
            if (cfg.cells[y][x] == sim::Cell::Brick) ++n;
    return n;
}

}  // namespace

TEST_CASE("brick fill: density 0 removes every brick candidate") {
    auto cfg = match::build_match_config(make_scheme(0), 2, 0x1234u);
    CHECK(count_bricks(cfg) == 0);
    // The non-candidate cells are untouched.
    CHECK(cfg.cells[0][0] == sim::Cell::Solid);
    CHECK(cfg.cells[0][1] == sim::Cell::Blank);
}

TEST_CASE("brick fill: density 100 keeps every brick candidate") {
    auto cfg = match::build_match_config(make_scheme(100), 2, 0x1234u);
    // Every ':' becomes a brick; only the two landmark cells are not bricks.
    const int candidates = sim::kGridWidth * sim::kGridHeight - 2;
    CHECK(count_bricks(cfg) == candidates);
    CHECK(cfg.cells[0][0] == sim::Cell::Solid);
    CHECK(cfg.cells[0][1] == sim::Cell::Blank);
}

TEST_CASE("brick fill: partial density is deterministic per seed") {
    auto a = match::build_match_config(make_scheme(50), 2, 0xABCDu);
    auto b = match::build_match_config(make_scheme(50), 2, 0xABCDu);
    // Same seed + scheme -> identical layout (the fix's determinism property).
    for (int y = 0; y < sim::kGridHeight; ++y)
        for (int x = 0; x < sim::kGridWidth; ++x)
            CHECK(a.cells[y][x] == b.cells[y][x]);
    // A partial fill lands strictly between empty and full.
    const int candidates = sim::kGridWidth * sim::kGridHeight - 2;
    CHECK(count_bricks(a) > 0);
    CHECK(count_bricks(a) < candidates);
}

TEST_CASE("brick fill: different seeds give different layouts") {
    auto a = match::build_match_config(make_scheme(50), 2, 0x1111u);
    auto b = match::build_match_config(make_scheme(50), 2, 0x2222u);
    bool differ = false;
    for (int y = 0; y < sim::kGridHeight && !differ; ++y)
        for (int x = 0; x < sim::kGridWidth; ++x)
            if (a.cells[y][x] != b.cells[y][x]) {
                differ = true;
                break;
            }
    CHECK(differ);  // per-match seed advance really varies the board (Gap 1)
}

TEST_CASE("brick fill: solid and blank cells never become bricks") {
    // Even at full density, '#'/'.'  are copied verbatim; sweep several seeds.
    for (std::uint32_t seed : {0u, 1u, 7u, 0xB0BB1E5u}) {
        auto cfg = match::build_match_config(make_scheme(100), 2, seed);
        CHECK(cfg.cells[0][0] == sim::Cell::Solid);
        CHECK(cfg.cells[0][1] == sim::Cell::Blank);
    }
}

// Warphole knockout (Gap: sub_4056CA case 1 `if (!+146)` block). A warphole's
// one-time activation clears its own tile AND one RANDOM adjacent tile. Ported
// into apply_actors off the setup-only LCG (never State::rng). See
// docs/re/stage-actors.md.
namespace {

// A config whose every cell is a brick, so a knocked-out tile is detectable as
// a Blank. spawns/tuning are irrelevant to apply_actors.
sim::MatchConfig all_brick_config() {
    sim::MatchConfig cfg;
    for (int y = 0; y < sim::kGridHeight; ++y)
        for (int x = 0; x < sim::kGridWidth; ++x) cfg.cells[y][x] = sim::Cell::Brick;
    return cfg;
}

int count_blanks(const sim::MatchConfig& cfg) {
    int n = 0;
    for (int y = 0; y < sim::kGridHeight; ++y)
        for (int x = 0; x < sim::kGridWidth; ++x)
            if (cfg.cells[y][x] == sim::Cell::Blank) ++n;
    return n;
}

}  // namespace

TEST_CASE("warphole knockout: clears own tile plus exactly one cardinal neighbour") {
    auto cfg = all_brick_config();
    std::vector<assets::extra::Actor> actors;
    assets::extra::Actor w;
    w.kind = assets::extra::Kind::Warphole;
    w.x = 7;
    w.y = 5;  // interior tile: all four neighbours are in-bounds bricks
    actors.push_back(w);

    match::apply_actors(cfg, actors, 0xC0FFEEu);

    // The warphole's own tile is walkable (place() clears it).
    CHECK(cfg.cells[5][7] == sim::Cell::Blank);
    // Exactly one of the four cardinal neighbours was knocked out; the diagonal
    // and farther tiles are untouched (the dir tables never pick a diagonal).
    int neigh_blank = (cfg.cells[4][7] == sim::Cell::Blank) + (cfg.cells[6][7] == sim::Cell::Blank) +
                      (cfg.cells[5][6] == sim::Cell::Blank) + (cfg.cells[5][8] == sim::Cell::Blank);
    CHECK(neigh_blank == 1);
    // Own tile + one neighbour = two blanks total (nothing else disturbed).
    CHECK(count_blanks(cfg) == 2);
    // Diagonals stay bricks (proof the centre/diagonal is never the target).
    CHECK(cfg.cells[4][6] == sim::Cell::Brick);
    CHECK(cfg.cells[6][8] == sim::Cell::Brick);
}

TEST_CASE("warphole knockout: deterministic per seed, varies across seeds") {
    auto one = [](std::uint32_t seed) {
        auto cfg = all_brick_config();
        std::vector<assets::extra::Actor> actors;
        assets::extra::Actor w;
        w.kind = assets::extra::Kind::Warphole;
        w.x = 7;
        w.y = 5;
        actors.push_back(w);
        match::apply_actors(cfg, actors, seed);
        // Encode which neighbour was cleared (0..3), or -1.
        if (cfg.cells[4][7] == sim::Cell::Blank) return 0;  // up
        if (cfg.cells[5][8] == sim::Cell::Blank) return 1;  // right
        if (cfg.cells[6][7] == sim::Cell::Blank) return 2;  // down
        if (cfg.cells[5][6] == sim::Cell::Blank) return 3;  // left
        return -1;
    };
    CHECK(one(0xC0FFEEu) == one(0xC0FFEEu));  // same seed → same knockout
    // Across a seed sweep, more than one direction is observed (the roll varies).
    bool seen[4] = {false, false, false, false};
    for (std::uint32_t seed : {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u}) {
        int d = one(seed);
        REQUIRE(d >= 0);
        seen[d] = true;
    }
    int distinct = seen[0] + seen[1] + seen[2] + seen[3];
    CHECK(distinct >= 2);
}

TEST_CASE("warphole knockout: an edge warphole never clears out of bounds") {
    // A warphole in the top-left corner: only Right(1)/Down(2) are in-bounds, so
    // the retry loop must reject Up/Left and land on one of the two valid tiles.
    auto cfg = all_brick_config();
    std::vector<assets::extra::Actor> actors;
    assets::extra::Actor w;
    w.kind = assets::extra::Kind::Warphole;
    w.x = 0;
    w.y = 0;
    actors.push_back(w);
    match::apply_actors(cfg, actors, 42u);

    CHECK(cfg.cells[0][0] == sim::Cell::Blank);  // own tile
    bool right = cfg.cells[0][1] == sim::Cell::Blank;
    bool down = cfg.cells[1][0] == sim::Cell::Blank;
    CHECK((right != down));           // exactly one of the two in-bounds neighbours
    CHECK(count_blanks(cfg) == 2);    // own + one neighbour; no OOB write happened
}
