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

// Warphole knockout (Gap: sub_4056CA case 1's block, the one guarded on the
// actor's +146 field being clear). A warphole's
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

// Random Start (docs/re/results-and-options.md §3 row 1, `random_start=`):
// CONFIRMED as the original's 200-random-pair-swap shuffle over the spawn
// slots (sub_421793 / sub_40133F; docs/re/facts.md "Options toggles"),
// mirrored in build_match_config off a setup-only LCG (never sim::State::rng).
namespace {

assets::sch::Scheme make_spawn_scheme(int count) {
    assets::sch::Scheme s;
    s.version = 2;
    s.name = "spawns";
    s.brick_density = 0;
    for (int y = 0; y < sim::kGridHeight; ++y) s.rows.push_back(std::string(sim::kGridWidth, '.'));
    for (int i = 0; i < count; ++i) {
        assets::sch::Spawn sp;
        sp.player = i;
        sp.x = i + 1;  // distinct, easily identified positions
        sp.y = 1;
        s.spawns.push_back(sp);
    }
    return s;
}

}  // namespace

TEST_CASE("random_start=false (default): spawns keep the scheme's own player order") {
    auto cfg = match::build_match_config(make_spawn_scheme(4), 4, 0x1234u);
    for (int i = 0; i < 4; ++i) CHECK(cfg.spawns[i].x == i + 1);
}

TEST_CASE("random_start=true: spawns are a permutation of the scheme's own slots") {
    auto cfg = match::build_match_config(make_spawn_scheme(4), 4, 0x1234u, nullptr,
                                         /*random_start=*/true);
    // Every original x (1..4) appears exactly once, just possibly reassigned.
    bool seen[5] = {};
    for (int i = 0; i < 4; ++i) {
        int x = cfg.spawns[i].x;
        REQUIRE(x >= 1);
        REQUIRE(x <= 4);
        CHECK_FALSE(seen[x]);
        seen[x] = true;
    }
}

TEST_CASE("random_start=true: deterministic per seed, varies across seeds") {
    auto a = match::build_match_config(make_spawn_scheme(4), 4, 0xABCDu, nullptr, true);
    auto b = match::build_match_config(make_spawn_scheme(4), 4, 0xABCDu, nullptr, true);
    for (int i = 0; i < 4; ++i) CHECK(a.spawns[i].x == b.spawns[i].x);  // same seed -> same shuffle

    bool differ = false;
    for (std::uint32_t seed : {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u}) {
        auto cfg = match::build_match_config(make_spawn_scheme(4), 4, seed, nullptr, true);
        if (cfg.spawns[0].x != 1) { differ = true; break; }
    }
    CHECK(differ);  // at least one seed actually reorders player 0's slot
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

// THE WARPHOLE IS THE ONLY ACTOR THAT CLEARS A CELL (docs/re/facts.md "Stage
// actors do not clear the tile they sit on"). sub_4056CA case 1 writes cells
// through sub_425E9B; cases 0/2/3 write no cell at all and only GATE THEIR
// DRAWING on `!sub_425FB9(x,y)`. apply_actors used to blank every actor tile,
// which cost ANCIENT EGYPT 43% of its bricks (44 dirarrows) and INNER CITY
// TRASH 40% (32 conveyors), and made those tiles walkable from round start
// where the original has them brick-blocked until somebody bombs them.
namespace {

// One non-warphole actor of `kind` at an interior tile of an all-brick board.
sim::MatchConfig one_actor_board(assets::extra::Kind kind, int dir = 0) {
    auto cfg = all_brick_config();
    std::vector<assets::extra::Actor> actors;
    assets::extra::Actor a;
    a.kind = kind;
    a.x = 7;
    a.y = 5;
    a.dir = dir;
    actors.push_back(a);
    match::apply_actors(cfg, actors, 0xC0FFEEu);
    return cfg;
}

}  // namespace

TEST_CASE("stage actors: conveyor/dirarrow/trampoline leave the brick under them") {
    for (auto kind : {assets::extra::Kind::Conveyor, assets::extra::Kind::DirArrow,
                      assets::extra::Kind::Trampoline}) {
        auto cfg = one_actor_board(kind);
        CHECK(cfg.cells[5][7] == sim::Cell::Brick);  // the tile is NOT cleared
        CHECK(count_blanks(cfg) == 0);               // and nothing else is either
        CHECK(cfg.actor_type[5][7] != sim::ActorType::None);  // the actor is still placed
    }
}

TEST_CASE("stage actors: the actor and its direction land regardless of the cell") {
    auto cfg = one_actor_board(assets::extra::Kind::Conveyor, /*dir=*/2);
    CHECK(cfg.actor_type[5][7] == sim::ActorType::Conveyor);
    CHECK(cfg.actor_dir[5][7] == 2);
    CHECK(cfg.cells[5][7] == sim::Cell::Brick);
}

TEST_CASE("stage actors: a warphole still clears, and only the warphole does") {
    // Mixed board: a conveyor, a dirarrow, a fixed trampoline and one warphole.
    auto cfg = all_brick_config();
    std::vector<assets::extra::Actor> actors;
    auto add = [&](assets::extra::Kind k, int x, int y) {
        assets::extra::Actor a;
        a.kind = k;
        a.x = x;
        a.y = y;
        actors.push_back(a);
    };
    add(assets::extra::Kind::Conveyor, 1, 1);
    add(assets::extra::Kind::DirArrow, 3, 1);
    add(assets::extra::Kind::Trampoline, 5, 1);
    add(assets::extra::Kind::Warphole, 7, 5);
    match::apply_actors(cfg, actors, 0xC0FFEEu);

    CHECK(cfg.cells[1][1] == sim::Cell::Brick);
    CHECK(cfg.cells[1][3] == sim::Cell::Brick);
    CHECK(cfg.cells[1][5] == sim::Cell::Brick);
    CHECK(cfg.cells[5][7] == sim::Cell::Blank);  // warphole own tile (sub_425E9B)
    CHECK(count_blanks(cfg) == 2);               // + exactly one knocked-out neighbour
}

TEST_CASE("stage actors: a random '-T,H' trampoline still needs an already-blank tile") {
    // The random branch's own precondition (odd parity, unoccupied, Blank) is
    // unchanged by dropping the blanking: on an all-brick board it can never
    // place, so no tile is opened by accident.
    auto cfg = all_brick_config();
    std::vector<assets::extra::Actor> actors;
    assets::extra::Actor a;
    a.kind = assets::extra::Kind::Trampoline;
    a.random = true;
    actors.push_back(a);
    match::apply_actors(cfg, actors, 0xC0FFEEu);
    CHECK(count_blanks(cfg) == 0);
    int placed = 0;
    for (int y = 0; y < sim::kGridHeight; ++y)
        for (int x = 0; x < sim::kGridWidth; ++x)
            if (cfg.actor_type[y][x] != sim::ActorType::None) ++placed;
    CHECK(placed == 0);
}
