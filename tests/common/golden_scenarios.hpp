#pragma once

// The six golden scenarios, as data rather than as test-case bodies.
//
// They live here because TWO suites need them and only one of them is
// tests/sim/test_golden.cpp. The other is tests/net/test_build_hash.cpp's
// determinism-rule-7 detector, which needs an answer to "did sim BEHAVIOUR
// change?" that does not come from build_hash's own scenarios — otherwise the
// question and the thing being questioned are the same measurement, and the
// failure mode rule 7 exists to catch (behaviour moved, digest did not) is
// invisible by construction.
//
// Each scenario is a `make()` that returns a Simulation prepared exactly as the
// golden case prepares it, an `input(t)` that returns the tick's keys, and a
// tick count. test_golden.cpp drives them with its checkpoint CHECKs; the
// detector drives them to the end and folds the final hashes. Nothing here may
// change behaviour: every constant is the golden case's own, and moving one
// silently retires a pinned scenario.

#include <cstdint>

#include "bomber/sim/simulation.hpp"

namespace bomber::testing::golden {

inline sim::TickInputs pattern(std::uint64_t t) {
    sim::TickInputs in{};
    for (int p = 0; p < sim::kMaxPlayers; ++p) {
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

inline sim::MatchConfig pillars_config() {
    sim::MatchConfig cfg;
    for (int y = 0; y < sim::kGridHeight; ++y)
        for (int x = 0; x < sim::kGridWidth; ++x)
            cfg.cells[y][x] = (x % 2 == 1 && y % 2 == 1) ? sim::Cell::Solid : sim::Cell::Blank;
    // Disarm the round-start input freeze (facts.md "Round-start input freeze",
    // VALUELST id 30 ≈ 1 s of dead input): these scenarios were captured acting
    // from tick 0 and golden E's choreography depends on it. test_freeze.cpp
    // pins the freeze itself.
    cfg.tuning.input_freeze_ticks = 0;
    return cfg;
}

// --- A: empty state, 10000 ticks -------------------------------------------

inline constexpr std::uint64_t kTicksA = 10000;

inline sim::Simulation make_a() {
    sim::Simulation a;
    a.state().rng = 42u;
    // The bare ctor zero-inits ticks_left, which the enclosure reads as "time's
    // up" and would close walls from tick 0. A was never meant to exercise the
    // spiral (docs/re/enclosure.md §2/§6 documents it as having no clock), so
    // seed a countdown generous enough to keep the stepper dormant.
    a.state().ticks_left = 9999 * sim::kTicksPerSecond;
    return a;
}

inline sim::TickInputs input_a(std::uint64_t t) {
    return pattern(t);
}

// --- B: 4-player brick match with all abilities ------------------------------

inline constexpr std::uint64_t kTicksB = 3000;

inline sim::Simulation make_b() {
    using namespace sim;
    MatchConfig cfg;
    for (int y = 0; y < kGridHeight; ++y)
        for (int x = 0; x < kGridWidth; ++x)
            cfg.cells[y][x] = (x % 2 == 1 && y % 2 == 1) ? Cell::Solid : Cell::Brick;
    cfg.spawns = {{0, 0}, {14, 10}, {14, 0}, {0, 10}};
    cfg.player_count = 4;
    cfg.seed = 0xB0BB1E5;
    cfg.tuning.input_freeze_ticks = 0;  // see pillars_config's disarm note
    // "Born with" is a starting-inventory BASELINE, not a grant replayed through
    // PowerupSystem::apply (facts.md "The .SCH -P row's 2nd field is a COUNT
    // that REPLACES the starting inventory"): the scheme field writes VALUELST
    // id 50+kind, which IS Tuning::start_with[].
    cfg.tuning.start_with[static_cast<int>(PowerupType::Kick)] = 1;
    cfg.tuning.start_with[static_cast<int>(PowerupType::Punch)] = 1;
    cfg.tuning.start_with[static_cast<int>(PowerupType::Grab)] = 1;
    cfg.tuning.start_with[static_cast<int>(PowerupType::Spooger)] = 1;
    cfg.tuning.start_with[static_cast<int>(PowerupType::Jelly)] = 1;
    return Simulation(cfg);
}

inline sim::TickInputs input_b(std::uint64_t t) {
    return pattern(t);
}

// --- C: trigger duel on a short 70 s clock -----------------------------------
// (The title once promised "a fast hurry phase" too; the round decides at tick
// 11 and the round-end freeze holds the stepper, so F carries that coverage.)

inline constexpr std::uint64_t kTicksC = 1500;

inline sim::Simulation make_c() {
    using namespace sim;
    MatchConfig cfg = pillars_config();
    cfg.cells[0][2] = Cell::Brick;
    cfg.spawns = {{0, 0}, {14, 10}};
    cfg.player_count = 2;
    cfg.seed = 99;
    cfg.tuning.game_seconds = 70;
    cfg.tuning.start_with[static_cast<int>(PowerupType::Trigger)] = 1;  // baseline, see B
    return Simulation(cfg);
}

inline sim::TickInputs input_c(std::uint64_t t) {
    return pattern(t * 7 + 3);
}

// --- D: the disease gauntlet -------------------------------------------------

inline constexpr std::uint64_t kTicksD = 800;

inline sim::Simulation make_d() {
    using namespace sim;
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
            if (m == 0)
                s.state().floor[y][x] = PowerupType::Disease;
            else if (m == 1)
                s.state().floor[y][x] = PowerupType::SuperDisease;
            else if (m == 2)
                s.state().floor[y][x] = PowerupType::Skate;
            else if (m == 3)
                s.state().floor[y][x] = PowerupType::Flame;
        }
    return s;
}

inline sim::TickInputs input_d(std::uint64_t t) {
    sim::TickInputs in = pattern(t);
    for (int p = 0; p < sim::kMaxPlayers; ++p) {
        in.players[p].action1 = false;
        in.players[p].action2 = false;
    }
    return in;
}

// --- E: jelly ping-pong and a veering punched flight -------------------------

inline constexpr std::uint64_t kTicksE = 300;

inline sim::Simulation make_e() {
    using namespace sim;
    // Jelly mechanics per docs/re/facts.md "Bomb machine" (sub_42331C): a kicked
    // jelly reverses off obstacles, a flying jelly rolls the 1-in-getvalue(667)
    // veer at each landing boundary. Choreography: drop a jelly bomb, kick it
    // into a wall so it ping-pongs between the wall and the player, then drop a
    // second bomb on a free row and punch it east (the veer roll consumes RNG).
    MatchConfig cfg = pillars_config();
    cfg.cells[0][7] = Cell::Solid;  // kick wall
    cfg.spawns = {{2, 0}, {14, 10}};
    cfg.player_count = 2;
    cfg.seed = 4242;
    for (auto& c : cfg.tuning.spawn_counts) c = 0;
    cfg.tuning.fuse_frames = 200;  // long fuse: room for the ping-pong
    cfg.tuning.start_with[0] = 3;  // three bombs
    cfg.tuning.start_with[static_cast<int>(PowerupType::Kick)] = 1;   // baseline, see B
    cfg.tuning.start_with[static_cast<int>(PowerupType::Punch)] = 1;  // baseline, see B
    cfg.tuning.start_with[static_cast<int>(PowerupType::Jelly)] = 1;  // baseline, see B
    return Simulation(cfg);
}

inline sim::TickInputs input_e(std::uint64_t t) {
    sim::TickInputs in{};
    auto& p = in.players[0];
    if (t == 0)
        p.action1 = true;  // drop jelly bomb at (2,0)
    else if (t >= 1 && t <= 10)
        p.left = true;  // step off westward
    else if (t >= 11 && t <= 18)
        p.right = true;  // walk back -> kick east
    else if (t >= 19 && t <= 26)
        p.down = true;  // leave row 0 to the ping-pong
    else if (t == 32)
        p.action1 = true;  // drop bomb #2 at (2,2)
    else if (t >= 33 && t <= 36)
        p.left = true;  // one tile west of it
    else if (t == 40)
        p.right = true;  // face east (no contact)
    else if (t == 44)
        p.action2 = true;  // punch #2 -> flight + veer RNG
    return in;
}

// --- F: a full hurry phase with the round still undecided --------------------

inline constexpr std::uint64_t kTicksF = 1000;

inline sim::Simulation make_f() {
    using namespace sim;
    MatchConfig cfg = pillars_config();
    // Both spawns are ring-4 tiles (min(x, y, 14-x, 10-y) == 4); with
    // enclosement_depth = 1 the walls close rings 0-1 only, so neither player is
    // ever crushed and the round stays undecided to the last tick. That is the
    // whole point: nobody presses a key, nobody dies, sides_remaining stays 2,
    // and the stepper's round-end gate never trips.
    cfg.spawns = {{6, 4}, {8, 6}};
    cfg.player_count = 2;
    cfg.seed = 0xEC105u;
    cfg.tuning.game_seconds = 30;
    cfg.tuning.hurry_seconds = 25;
    cfg.tuning.enclosement_depth = 1;
    // One of each actor type, all on ring-4 tiles the spiral never reaches and
    // none under a player, so the ONLY thing that can change them is the arm
    // sweep: the warphole pair and the trampoline must go, the belt and the
    // arrow must stay (sub_405D0C, docs/re/enclosure.md §5.1).
    cfg.actor_type[4][4] = ActorType::Warphole;
    cfg.warp_dest_x[4][4] = 10;
    cfg.warp_dest_y[4][4] = 6;
    cfg.actor_type[6][10] = ActorType::Warphole;
    cfg.warp_dest_x[6][10] = 4;
    cfg.warp_dest_y[6][10] = 4;
    cfg.actor_type[6][4] = ActorType::Trampoline;
    cfg.actor_type[4][10] = ActorType::Conveyor;
    cfg.actor_dir[4][10] = 1;
    cfg.actor_type[6][6] = ActorType::DirArrow;
    cfg.actor_dir[6][6] = 2;
    return Simulation(cfg);
}

inline sim::TickInputs input_f(std::uint64_t) {
    return sim::TickInputs{};
}

// --- The fold ----------------------------------------------------------------

// The six final hashes folded into one number: "what the golden scenarios say
// sim behaviour is". Only the detector uses it; test_golden.cpp keeps pinning
// every intermediate checkpoint, which this deliberately does not replace.
//
// The fold is the same boost/hash_combine mixer build_hash.cpp uses, so an
// added scenario cannot cancel an existing one by XOR symmetry.
inline std::uint64_t fingerprint() {
    auto run = [](sim::Simulation s, sim::TickInputs (*in)(std::uint64_t), std::uint64_t ticks) {
        for (std::uint64_t t = 0; t < ticks; ++t) s.tick(in(t));
        return s.hash();
    };
    const std::uint64_t parts[6] = {
        run(make_a(), input_a, kTicksA), run(make_b(), input_b, kTicksB),
        run(make_c(), input_c, kTicksC), run(make_d(), input_d, kTicksD),
        run(make_e(), input_e, kTicksE), run(make_f(), input_f, kTicksF),
    };
    std::uint64_t h = 0;
    for (const std::uint64_t p : parts) h ^= p + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
}

}  // namespace bomber::testing::golden
