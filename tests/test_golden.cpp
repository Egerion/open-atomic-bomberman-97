// Golden-hash regression tests. These scenarios were captured from the
// pre-refactor simulation (2026-07-03) and pin the EXACT behaviour: state
// hash, RNG stream position, everything.
//
// If one of these fails you changed gameplay behaviour. That is either a bug
// (fix it) or a deliberate faithfulness improvement from new RE facts — in
// that case update the constants IN THE SAME COMMIT as the change and cite
// the docs/re/facts.md entry that justifies it. See CLAUDE.md.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "bomber/sim/simulation.hpp"

using namespace bomber::sim;

namespace {

TickInputs pattern(std::uint64_t t) {
    TickInputs in{};
    for (int p = 0; p < kMaxPlayers; ++p) {
        auto& pi = in.players[static_cast<std::size_t>(p)];
        pi.up = (t + static_cast<std::uint64_t>(p)) % 7 == 0;
        pi.down = (t + static_cast<std::uint64_t>(p)) % 11 == 1;
        pi.left = (t * 3 + static_cast<std::uint64_t>(p)) % 5 == 2;
        pi.right = (t * 5 + static_cast<std::uint64_t>(p)) % 9 == 3;
        pi.action1 = (t * 31 + static_cast<std::uint64_t>(p)) % 13 == 0;
        pi.action2 = (t * 17 + static_cast<std::uint64_t>(p)) % 23 == 0;
    }
    return in;
}

MatchConfig pillars_config() {
    MatchConfig cfg;
    for (int y = 0; y < kGridHeight; ++y)
        for (int x = 0; x < kGridWidth; ++x)
            cfg.cells[y][x] = (x % 2 == 1 && y % 2 == 1) ? Cell::Solid : Cell::Blank;
    return cfg;
}

}  // namespace

TEST_CASE("golden A: empty state, 10000 ticks") {
    Simulation a;
    a.state().rng = 42u;
    for (std::uint64_t t = 0; t < 10000; ++t) a.tick(pattern(t));
    CHECK(a.hash() == 0x7ec4b4ee673caaf0ull);
    CHECK(a.state().rng == 0x0000002au);
}

TEST_CASE("golden B: 4-player brick match with all abilities") {
    MatchConfig cfg;
    for (int y = 0; y < kGridHeight; ++y)
        for (int x = 0; x < kGridWidth; ++x)
            cfg.cells[y][x] = (x % 2 == 1 && y % 2 == 1) ? Cell::Solid : Cell::Brick;
    cfg.spawns = {{0, 0}, {14, 10}, {14, 0}, {0, 10}};
    cfg.player_count = 4;
    cfg.seed = 0xB0BB1E5;
    cfg.born_with[static_cast<int>(PowerupType::Kick)] = true;
    cfg.born_with[static_cast<int>(PowerupType::Punch)] = true;
    cfg.born_with[static_cast<int>(PowerupType::Grab)] = true;
    cfg.born_with[static_cast<int>(PowerupType::Spooger)] = true;
    cfg.born_with[static_cast<int>(PowerupType::Jelly)] = true;
    Simulation s(cfg);
    CHECK(s.hash() == 0x27bc1e6963477347ull);  // setup itself is pinned

    // Updated 2026-07-03 for the jelly-bomb mechanics (docs/re/facts.md "Bomb
    // machine", sub_42331C): this scenario grants Jelly, so punched jelly
    // flights now consume the veer roll RNG and the stream shifts. The setup
    // hash (B0) is unchanged — only in-match behaviour moved.
    static constexpr std::uint64_t kExpected[6] = {
        0x0c8d4b271345c89eull,  // tick 500
        0x81c18f222e5413a5ull,  // tick 1000
        0x7697b3bc70755075ull,  // tick 1500
        0x9708ad500001ed59ull,  // tick 2000
        0xa96be585f6b1e346ull,  // tick 2500
        0xef7fb91510c92514ull,  // tick 3000
    };
    for (std::uint64_t t = 0; t < 3000; ++t) {
        s.tick(pattern(t));
        if ((t + 1) % 500 == 0) CHECK(s.hash() == kExpected[(t + 1) / 500 - 1]);
    }
}

TEST_CASE("golden C: trigger bombs and a fast hurry phase") {
    MatchConfig cfg = pillars_config();
    cfg.cells[0][2] = Cell::Brick;
    cfg.spawns = {{0, 0}, {14, 10}};
    cfg.player_count = 2;
    cfg.seed = 99;
    cfg.tuning.game_seconds = 70;
    cfg.born_with[static_cast<int>(PowerupType::Trigger)] = true;
    Simulation s(cfg);
    for (std::uint64_t t = 0; t < 1500; ++t) s.tick(pattern(t * 7 + 3));
    CHECK(s.hash() == 0x94b65ecd7a6992fbull);
}

TEST_CASE("golden D: the disease gauntlet") {
    MatchConfig cfg = pillars_config();
    cfg.spawns = {{0, 0}, {14, 10}, {7, 0}};
    cfg.player_count = 3;
    cfg.seed = 1234;
    for (auto& c : cfg.tuning.spawn_counts) c = 0;
    Simulation s(cfg);
    int k = 0;
    for (int y = 0; y < kGridHeight; ++y)
        for (int x = 0; x < kGridWidth; ++x) {
            if (s.state().cells[y][x] != Cell::Blank) continue;
            int m = k++ % 5;
            if (m == 0) s.state().floor[y][x] = PowerupType::Disease;
            else if (m == 1) s.state().floor[y][x] = PowerupType::SuperDisease;
            else if (m == 2) s.state().floor[y][x] = PowerupType::Skate;
            else if (m == 3) s.state().floor[y][x] = PowerupType::Flame;
        }

    static constexpr std::uint64_t kExpectedHash[4] = {
        0x714da0610f74cae6ull,  // tick 200
        0xc7508ea61abb6eccull,  // tick 400
        0x0e10ffb81aa3c605ull,  // tick 600
        0x40c0e61e78f548a4ull,  // tick 800
    };
    static constexpr std::uint32_t kExpectedRng[4] = {0xc793e5b2u, 0xdf9afc20u, 0x83c939cfu,
                                                      0xca47489cu};
    for (std::uint64_t t = 0; t < 800; ++t) {
        TickInputs in = pattern(t);
        for (int p = 0; p < kMaxPlayers; ++p) {
            in.players[p].action1 = false;
            in.players[p].action2 = false;
        }
        s.tick(in);
        if ((t + 1) % 200 == 0) {
            CHECK(s.hash() == kExpectedHash[(t + 1) / 200 - 1]);
            CHECK(s.state().rng == kExpectedRng[(t + 1) / 200 - 1]);
        }
    }
}

TEST_CASE("golden E: jelly ping-pong and a veering punched flight") {
    // Captured 2026-07-03 with the jelly mechanics from docs/re/facts.md
    // "Bomb machine" (sub_42331C): kicked jelly reverses off obstacles, flying
    // jelly rolls the 1-in-getvalue(667) veer at each landing boundary.
    // Choreography: drop a jelly bomb, kick it into a wall so it ping-pongs
    // between the wall and the player, then drop a second bomb on a free row
    // and punch it east (the veer roll consumes sim RNG).
    MatchConfig cfg = pillars_config();
    cfg.cells[0][7] = Cell::Solid;  // kick wall
    cfg.spawns = {{2, 0}, {14, 10}};
    cfg.player_count = 2;
    cfg.seed = 4242;
    for (auto& c : cfg.tuning.spawn_counts) c = 0;
    cfg.tuning.fuse_frames = 200;   // long fuse: room for the ping-pong
    cfg.tuning.start_with[0] = 3;   // three bombs
    cfg.born_with[static_cast<int>(PowerupType::Kick)] = true;
    cfg.born_with[static_cast<int>(PowerupType::Punch)] = true;
    cfg.born_with[static_cast<int>(PowerupType::Jelly)] = true;
    Simulation s(cfg);

    auto script = [](std::uint64_t t) {
        TickInputs in{};
        auto& p = in.players[0];
        if (t == 0) p.action1 = true;                 // drop jelly bomb at (2,0)
        else if (t >= 1 && t <= 10) p.left = true;    // step off westward
        else if (t >= 11 && t <= 18) p.right = true;  // walk back -> kick east
        else if (t >= 19 && t <= 26) p.down = true;   // leave row 0 to the ping-pong
        else if (t == 32) p.action1 = true;           // drop bomb #2 at (2,2)
        else if (t >= 33 && t <= 36) p.left = true;   // one tile west of it
        else if (t == 40) p.right = true;             // face east (no contact)
        else if (t == 44) p.action2 = true;           // punch #2 -> flight + veer RNG
        return in;
    };

    static constexpr std::uint64_t kExpected[4] = {
        0x02592148def2e49dull,  // tick 75
        0x21710c7884c277ecull,  // tick 150
        0x2c6f12d1788c3c5aull,  // tick 225
        0x6c6f5a11a01269f3ull,  // tick 300
    };
    int bounces = 0;
    for (std::uint64_t t = 0; t < 300; ++t) {
        s.tick(script(t));
        for (const auto& e : s.state().events)
            if (e.type == Event::Type::JellyBounced) ++bounces;
        if ((t + 1) % 75 == 0) CHECK(s.hash() == kExpected[(t + 1) / 75 - 1]);
    }
    CHECK(bounces == 7);                    // the ping-pong really happened
    CHECK(s.state().rng == 0xcce3bbf8u);    // the veer roll really consumed RNG
}
