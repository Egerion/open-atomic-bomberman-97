// The two Options-screen toggles with sim consumers (docs/re/facts.md
// "Options toggles: stomped_bombs_detonate / diseases_destroyable"):
//
//  - stomped_bombs_detonate (Tuning::wall_detonates, VALUELST 46 seed,
//    options.ini stomped_bombs_detonate=): a closing enclosement wall landing
//    on a GROUNDED bomb detonates it (ON, the original default) or silently
//    eats it (OFF) — sub_426818 ~27260, sub_423209 vs sub_424841. Airborne
//    bombs are exempt (sub_422E48 skips motion 2/3).
//
//  - diseases_destroyable (Tuning::diseases_destroyable, VALUELST 120 seed,
//    options.ini diseases_destroyable=): destroying a floor skull is
//    UNCONDITIONAL, but with the toggle OFF a fresh skull relocates to a
//    random free tile (sub_4255B2(2) == PowerupSystem::scatter). Consumers:
//    the flame walk (sub_42331C ~25626/25653) and the sliding-bomb cell-entry
//    probe (sub_4230A5), which also destroys any powerup the bomb plows into.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "helpers.hpp"

using namespace bomber::sim;
using test::open_config;
using test::run;

namespace {

// Counts Disease tokens visible on the floor.
int skulls_on_floor(const State& s) {
    int n = 0;
    for (int y = 0; y < kGridHeight; ++y)
        for (int x = 0; x < kGridWidth; ++x)
            if (s.floor[y][x] == PowerupType::Disease) ++n;
    return n;
}

// A config whose walls close almost immediately: game 10 s, banner at 7 s
// remaining (tick 41), walls arming at 3 s remaining (tick 121), first drop
// at tick 126 (see test_sim.cpp "hurry walls" for the pinned cadence).
MatchConfig closing_config() {
    MatchConfig cfg = open_config();
    cfg.spawns = {{7, 4}, {8, 4}};  // interior blanks, off the two closing rings
    cfg.tuning.game_seconds = 10;
    cfg.tuning.hurry_seconds = 8;
    cfg.tuning.enclosement_depth = 1;
    return cfg;
}

// Parks a grounded bomb with a practically infinite fuse on tile (tx,ty).
void park_bomb(State& s, int tx, int ty, int owner = 1) {
    Bomb b;
    b.active = true;
    b.owner = static_cast<std::uint8_t>(owner);
    b.x = tx * kTileWF + kTileWF / 2;
    b.y = ty * kTileHF + kTileHF / 2;
    b.fuse = 30000;
    b.flame = 2;
    s.bombs.push_back(b);
    ++s.players[owner].bombs_placed;
}

}  // namespace

TEST_CASE("stomped_bombs_detonate ON: the closing wall detonates the bomb") {
    MatchConfig cfg = closing_config();
    cfg.tuning.wall_detonates = 1;  // the original default (getvalue(46) = 1)
    Simulation s(cfg);
    run(s, 125);  // one tick before the first wall drops at (0,0), tick 126
    park_bomb(s.state(), 0, 0);
    s.tick({});  // tick 126: wall drops on (0,0)
    // sub_423209(bomb, -1) only QUEUES the detonation (sub_42331C's once-per-
    // frame drain runs BEFORE sub_426818 each frame, so a bomb queued by
    // THIS frame's wall drop isn't force-fired until the FOLLOWING frame —
    // see docs/re/enclosure.md §3 and test_sim.cpp's "hurry walls" test). The
    // bomb is still here immediately after the drop...
    CHECK(!s.state().bombs.empty());
    bool exploded = false;
    s.tick({});  // tick 127: the forced fuse expires, tick_fuses() detonates it
    for (const auto& e : s.state().events)
        if (e.type == Event::Type::Explosion && e.x == 0 && e.y == 0) exploded = true;
    CHECK(exploded);
    CHECK(s.state().bombs.empty());
    // A real explosion spreads flame past the dropped wall tile.
    CHECK(s.state().flame[0][1] > 0);
    CHECK(s.state().players[1].bombs_placed == 0);  // slot freed by the blast
}

TEST_CASE("stomped_bombs_detonate OFF: the closing wall silently eats the bomb") {
    MatchConfig cfg = closing_config();
    cfg.tuning.wall_detonates = 0;
    Simulation s(cfg);
    run(s, 125);
    park_bomb(s.state(), 0, 0);
    bool exploded = false;
    s.tick({});  // tick 126: sub_424841 zeroes the bomb directly, no queue, no defer
    for (const auto& e : s.state().events)
        if (e.type == Event::Type::Explosion) exploded = true;
    CHECK(!exploded);
    CHECK(s.state().bombs.empty());
    // No blast: nothing burns, the owner's slot is refunded (sub_424841 just
    // zeroes the record; the original frees the count the same way).
    for (int y = 0; y < kGridHeight; ++y)
        for (int x = 0; x < kGridWidth; ++x) CHECK(s.state().flame[y][x] == 0);
    CHECK(s.state().players[1].bombs_placed == 0);
}

TEST_CASE("a flying bomb sails over the dropping wall (sub_422E48 skips motion 2/3)") {
    MatchConfig cfg = closing_config();
    cfg.tuning.wall_detonates = 1;
    Simulation s(cfg);
    run(s, 125);
    // A bomb mid-flight whose interpolated position is over (0,0) this tick.
    Bomb b;
    b.active = true;
    b.owner = 1;
    b.x = kTileWF / 2;
    b.y = kTileHF / 2;  // tile (0,0)
    b.fuse = 30000;
    b.flame = 2;
    b.flying = true;
    b.from_x = b.x;
    b.from_y = b.y;
    b.to_x = b.x + 3 * kTileWF;
    b.to_y = b.y;
    b.dir = Direction::Right;
    b.fly_total = 12;
    b.fly_ticks = 12;
    s.state().bombs.push_back(b);
    ++s.state().players[1].bombs_placed;
    s.tick({});  // wall drops on (0,0) while the bomb is airborne above it
    CHECK(s.state().cells[0][0] == Cell::Solid);
    REQUIRE(s.state().bombs.size() == 1);
    CHECK(s.state().bombs[0].active);
    CHECK(s.state().bombs[0].flying);
}

TEST_CASE("diseases_destroyable ON (default): a burned skull is simply gone") {
    MatchConfig cfg = open_config();
    REQUIRE(cfg.tuning.diseases_destroyable);  // original default (getvalue(120) = 1)
    Simulation s(cfg);
    s.state().floor[0][2] = PowerupType::Disease;  // in a corner bomb's blast line
    park_bomb(s.state(), 0, 0, 1);
    s.state().bombs.back().fuse = 2;
    const std::uint32_t rng_before = s.state().rng;
    run(s, 2);  // fuse expires, flame reaches (2,0)
    CHECK(skulls_on_floor(s.state()) == 0);
    // The default path draws nothing for the toggle (relocation never runs).
    CHECK(s.state().rng == rng_before);
}

TEST_CASE("diseases_destroyable OFF: a burned skull relocates to a free tile") {
    MatchConfig cfg = open_config();
    cfg.tuning.diseases_destroyable = false;
    Simulation s(cfg);
    s.state().floor[0][2] = PowerupType::Disease;
    park_bomb(s.state(), 0, 0, 1);
    s.state().bombs.back().fuse = 2;
    run(s, 2);
    // Destroyed at (2,0) — and compensated: exactly one fresh skull elsewhere.
    CHECK(s.state().floor[0][2] == PowerupType::None);
    CHECK(skulls_on_floor(s.state()) == 1);

    // Deterministic: the same seed relocates to the same tile.
    Simulation s2(cfg);
    s2.state().floor[0][2] = PowerupType::Disease;
    park_bomb(s2.state(), 0, 0, 1);
    s2.state().bombs.back().fuse = 2;
    run(s2, 2);
    for (int y = 0; y < kGridHeight; ++y)
        for (int x = 0; x < kGridWidth; ++x)
            CHECK((s.state().floor[y][x] == PowerupType::Disease) ==
                  (s2.state().floor[y][x] == PowerupType::Disease));
}

TEST_CASE("a sliding bomb plows through a floor powerup (sub_4230A5 side effect)") {
    MatchConfig cfg = open_config();
    Simulation s(cfg);
    s.state().floor[0][4] = PowerupType::Flame;  // in the slide path
    park_bomb(s.state(), 0, 0, 1);
    Bomb& b = s.state().bombs.back();
    b.moving = true;
    b.dir = Direction::Right;
    bool squashed = false;
    for (int t = 0; t < 30; ++t) {
        s.tick({});
        for (const auto& e : s.state().events)
            if (e.type == Event::Type::PowerupBurned && e.x == 4 && e.y == 0) squashed = true;
    }
    CHECK(squashed);
    CHECK(s.state().floor[0][4] == PowerupType::None);
    // The powerup did not block: the bomb slid on past the tile.
    REQUIRE(!s.state().bombs.empty());
    CHECK(s.state().bombs[0].tile_x() > 4);
}

TEST_CASE("a sliding bomb squashing a skull relocates it when destroyable is off") {
    MatchConfig cfg = open_config();
    cfg.tuning.diseases_destroyable = false;
    Simulation s(cfg);
    s.state().floor[0][4] = PowerupType::Disease;
    park_bomb(s.state(), 0, 0, 1);
    Bomb& b = s.state().bombs.back();
    b.moving = true;
    b.dir = Direction::Right;
    run(s, 30);
    CHECK(s.state().floor[0][4] == PowerupType::None);
    CHECK(skulls_on_floor(s.state()) == 1);  // relocated, not lost
}

TEST_CASE("VALUELST ids 46/120 feed the toggles (Tuning::apply)") {
    Tuning t;
    CHECK(t.wall_detonates == 1);        // getvalue(46) default
    CHECK(t.diseases_destroyable);       // getvalue(120) default
    CHECK(t.apply(46, 0));
    CHECK(t.wall_detonates == 0);
    CHECK(t.apply(120, 0));
    CHECK(!t.diseases_destroyable);
}
