// Clean-room side of the M3 oracle diff harness (Part B).
//
// Runs bomber::sim::Simulation with a fixed seed + a fixed, DOCUMENTED scripted
// per-tick input sequence and prints one canonical digest line per tick to
// stdout. The native port (native/, ground truth by construction) emits the
// SAME digest format for the SAME seed + SAME script; a plain `diff` of the two
// files locates the FIRST tick where the two implementations diverge — i.e. the
// first mechanic the clean-room sim gets wrong (or, in principle, the native
// port, but the native port mirrors the original binary's arithmetic).
//
// This is a scratch tool under the repo root; it is NOT part of native/ and is
// NOT wired into the committed build. Build it standalone (see build note at the
// bottom) or add it to a scratch CMake target linking `bomber::sim`.
//
// DIGEST FORMAT (must byte-match the native oracle — see native/docs/M3_NOTES.md):
//   t=<tick> P=<n> | <slot>:<tx>,<ty>,<alive>,<bombs> ... | B=<liveBombs> F=<flameCells>
// Only OBSERVABLE gameplay state is compared. rng is NOT emitted (the native LCG
// and the clean-room xorshift32 are different generators). Tile coordinates are
// the robust cross-representation field (pixel/fixed-point layouts differ).

#include "bomber/sim/simulation.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace bomber::sim;

namespace {

// ----------------------------------------------------------------------------
// The scripted input. A pure function of the tick index so BOTH sides can
// reproduce it without sharing a file. Slot 0 is a scripted "human"; all other
// active slots are AI (driven by the sim's own brain + RNG). Keep this in exact
// lockstep with the native oracle's input generator.
//
//   slot 0: sweeps right for 20 ticks, left for 20 ticks (period 40), and drops
//           a bomb (action1) on every tick where (t % 25) == 24.
// ----------------------------------------------------------------------------
TickInputs scripted_inputs(std::uint64_t t) {
    TickInputs in{};
    PlayerInput& p0 = in.players[0];
    p0.right = (t % 40) < 20;
    p0.left = (t % 40) >= 20;
    p0.action1 = (t % 25) == 24;
    return in;
}

// The classic open arena used by tests/helpers.hpp open_config(): 15x11 with
// odd/odd Solid pillars, players in opposite corners.
MatchConfig make_config(std::uint32_t seed, int players, bool slot1_ai) {
    MatchConfig cfg;
    for (int y = 0; y < kGridHeight; ++y)
        for (int x = 0; x < kGridWidth; ++x)
            cfg.cells[y][x] = (x % 2 == 1 && y % 2 == 1) ? Cell::Solid : Cell::Blank;
    cfg.spawns = {{0, 0}, {14, 10}, {0, 10}, {14, 0}};
    if (players < 2) players = 2;
    if (players > 4) players = 4;
    cfg.player_count = players;
    cfg.seed = seed;
    // Disarm the ~1s round-start input freeze so scripted/AI input acts from
    // tick 0 (every golden test does this). The native oracle must match.
    cfg.tuning.input_freeze_ticks = 0;
    // No powerups hidden under bricks -> fully deterministic level (avoids the
    // RNG-driven powerup placement dominating the digest early). The native
    // oracle should load an equivalently powerup-free scheme, or this field's
    // native analogue must be zeroed too. Documented divergence axis.
    for (auto& c : cfg.tuning.spawn_counts) c = 0;
    cfg.ai[0] = false;              // slot 0 = scripted human
    for (int i = 1; i < players; ++i) cfg.ai[i] = true;
    if (!slot1_ai) cfg.ai[1] = false;
    return cfg;
}

// Tile-centre in field pixels*100 (mirrors grid::tile_center_*; grid.hpp is a
// private sim header so we recompute from the public constants).
Fixed tile_center_x(int tx) { return tx * kTileWF + kTileWF / 2; }
Fixed tile_center_y(int ty) { return ty * kTileHF + kTileHF / 2; }

// KICK/SLIDE scenario (bombs F2/F3): inject one bomb resting at tile (7,0),
// already KICKED toward +x (Direction::Right), with a huge fuse so it never
// explodes in the comparison window. It slides down the fully-open top row and
// must STOP at the right wall (col 14) — the coast-stop is F3; the per-tick
// displacement is F2. The native oracle sets up the exact same physical bomb
// (same tile, same +x direction, kicked-bomb speed, long fuse) via sub_422EDE +
// sub_42464B. Players stay idle (empty TickInputs) so nothing perturbs it.
void inject_kicked_bomb(State& s, bool jelly = false) {
    Bomb b;
    b.active = true;
    b.id = s.next_bomb_id++;
    b.owner = 9;   // no present player -> both slots keep bombs_placed 0
    b.colour = 9;
    b.x = tile_center_x(7);
    b.y = tile_center_y(0);
    b.fuse_init = 100000;
    b.fuse = 100000;  // never fires in the window (native +74 = 50*1000 likewise)
    b.flame = 2;
    b.moving = true;
    b.jelly = jelly;           // jelly reverses at the wall instead of stopping
    b.dir = Direction::Right;  // +x, toward the right wall
    s.bombs.push_back(b);
}

// CONVEYOR scenario (bombs F2 belt speed + F3 belt-exit freeze): a short EAST
// belt on tiles (2,0)+(3,0) with open floor at (4,0)+, and a RESTING bomb
// (motion 0, NOT kicked) on the belt's first tile. The belt pushes it east at
// the base conveyor_speed() (the LABEL_21 +100 is backoff-cancelled) and it
// FREEZES the instant it steps off the belt onto (4,0). The native oracle sets
// up the identical belt (sub_404E3C actor records) + resting bomb.
void inject_belt_bomb(State& s) {
    for (int x = 2; x <= 3; ++x) {
        s.actor_type[0][x] = ActorType::Conveyor;
        s.actor_dir[0][x] = 1;  // godir 1 = east (Direction::Right)
    }
    Bomb b;
    b.active = true;
    b.id = s.next_bomb_id++;
    b.owner = 9;   // no present player -> both slots keep bombs_placed 0
    b.colour = 9;
    b.x = tile_center_x(2);
    b.y = tile_center_y(0);
    b.fuse_init = 100000;
    b.fuse = 100000;  // never fires in the window
    b.flame = 2;
    b.moving = false;  // RESTING (motion 0): only the belt under it pushes it
    b.dir = Direction::Right;
    s.bombs.push_back(b);
}

// FLAME scenario (explosion arm cast): inject one bomb at interior tile (6,4)
// with flame reach 2 and a short fuse; it explodes and casts a full cross. The
// native injects the identical fixed-flame bomb (sub_422EDE), avoiding the
// player-drop flame-stat mismatch. Expect F = 1 + 4*2 = 9 on both sides.
void inject_flame_bomb(State& s) {
    Bomb b;
    b.active = true;
    b.id = s.next_bomb_id++;
    b.owner = 9;
    b.colour = 9;
    b.x = tile_center_x(6);
    b.y = tile_center_y(4);
    b.flame = 2;
    b.fuse_init = 3;
    b.fuse = 3;  // explodes a few ticks in
    s.bombs.push_back(b);
}

// FLIGHT scenario (punch arc, sub_42331C case 2): inject a bomb at (5,0) and
// launch it EAST 3 tiles (mirroring BombSystem::launch(b, Right, 3, arc)). It
// flies over from_* -> to_* and lands at (8,0). The native injects the same via
// sub_4248C6. Compare the ground track + landing tile.
void inject_flying_bomb(State& s) {
    Bomb b;
    b.active = true;
    b.id = s.next_bomb_id++;
    b.owner = 9;
    b.colour = 9;
    b.x = tile_center_x(5);
    b.y = tile_center_y(0);
    b.fuse_init = 100000;
    b.fuse = 100000;
    b.flame = 2;
    // BombSystem::launch(b, Direction::Right, 3, punch_arc_first):
    b.flying = true;
    b.moving = false;
    b.from_x = b.x;
    b.from_y = b.y;
    b.to_x = b.x + 3 * kTileWF;  // 3 tiles east
    b.to_y = b.y;
    b.dir = Direction::Right;
    b.fly_arc = s.tuning.punch_arc_first;
    const Fixed dist = 3 * kTileWF;
    b.fly_total = std::max<std::int32_t>(1, dist / std::max(1, s.tuning.punched_bomb_speed));
    b.fly_ticks = b.fly_total;
    s.bombs.push_back(b);
}

// PLAYERBELT scenario (player-on-conveyor carry): an EAST belt on row 0 from
// (0,0) to (6,0), under player 0's spawn. No bomb — the idle player 0 is
// carried east by the belt. The native sets up the same actor records.
void inject_playerbelt(State& s) {
    for (int x = 0; x <= 6; ++x) {
        s.actor_type[0][x] = ActorType::Conveyor;
        s.actor_dir[0][x] = 1;  // godir 1 = east
    }
}

int count_flame_cells(const State& s) {
    int n = 0;
    for (int y = 0; y < kGridHeight; ++y)
        for (int x = 0; x < kGridWidth; ++x)
            if (s.flame[y][x] > 0) ++n;
    return n;
}

int count_live_bombs(const State& s) {
    int n = 0;
    for (const Bomb& b : s.bombs)
        if (b.active) ++n;
    return n;
}

void emit_digest(std::uint64_t t, const State& s) {
    int present = 0;
    for (int i = 0; i < kMaxPlayers; ++i)
        if (s.players[i].present) ++present;
    std::printf("t=%llu P=%d |", static_cast<unsigned long long>(t), present);
    for (int i = 0; i < kMaxPlayers; ++i) {
        const Player& p = s.players[i];
        if (!p.present) continue;
        std::printf(" %d:%d,%d,%d,%d", i, p.tile_x(), p.tile_y(),
                    p.alive ? 1 : 0, static_cast<int>(p.bombs_placed));
    }
    // Per-live-bomb tile position + kicked-slide motion flag, in vector
    // (creation) order — the native side iterates its bomb slots 0..99 in the
    // same order. `moving` is the kicked/redirected slide state (native motion
    // word +46 == 1); a resting OR belt-carried bomb reads 0 on both sides
    // (the clean-room clears its transient conveyor `moving` before this
    // digest, matching the native's belt bomb that never leaves motion 0), and
    // a flying bomb reads 0 too. Positions are the robust pre-explosion field
    // for validating bombs F2 (kicked/conveyor speed) and F3 (coast-stop) —
    // they are unaffected by the known native flame-spread gap and the tick-
    // rotation offset until the bomb explodes/stops.
    std::printf(" | B=%d", count_live_bombs(s));
    for (const Bomb& b : s.bombs) {
        if (!b.active) continue;
        std::printf(" bomb:%d,%d,%d", b.tile_x(), b.tile_y(), b.moving ? 1 : 0);
    }
    std::printf(" F=%d\n", count_flame_cells(s));
}

}  // namespace

int main(int argc, char** argv) {
    std::uint32_t seed = 0x12345678u;
    int ticks = 200;
    int players = 2;
    const char* scenario = "base";
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--seed=", 7) == 0)
            seed = static_cast<std::uint32_t>(std::strtoul(argv[i] + 7, nullptr, 0));
        else if (std::strncmp(argv[i], "--ticks=", 8) == 0)
            ticks = std::atoi(argv[i] + 8);
        else if (std::strncmp(argv[i], "--players=", 10) == 0)
            players = std::atoi(argv[i] + 10);
        else if (std::strncmp(argv[i], "--scenario=", 11) == 0)
            scenario = argv[i] + 11;
    }
    const bool kick_scenario = std::strcmp(scenario, "kick") == 0;
    const bool conveyor_scenario = std::strcmp(scenario, "conveyor") == 0;
    const bool flame_scenario = std::strcmp(scenario, "flame") == 0;
    const bool jelly_scenario = std::strcmp(scenario, "jelly") == 0;
    const bool flight_scenario = std::strcmp(scenario, "flight") == 0;
    const bool playerbelt_scenario = std::strcmp(scenario, "playerbelt") == 0;
    const bool idle_scenario = kick_scenario || conveyor_scenario || flame_scenario ||
                               jelly_scenario || flight_scenario || playerbelt_scenario;
    std::fprintf(stderr, "mirror: seed=0x%08x ticks=%d players=%d scenario=%s\n", seed, ticks,
                 players, scenario);

    // Match the native --oracle config EXACTLY: slot 1 is an IDLE HUMAN, not an
    // AI. AI would diverge immediately by RNG (native LCG vs clean-room
    // xorshift), which is NOT a mechanic bug and would swamp the comparison.
    // The native side sets both slots to controller type 2 (local human);
    // slot 0 is scripted, slot 1 receives no input.
    Simulation sim(make_config(seed, players, /*slot1_ai=*/false));
    // The kick scenario injects a sliding bomb into the initial state and keeps
    // every player idle (empty TickInputs); the base scenario runs the scripted
    // human + bomb-drop sequence.
    if (kick_scenario) inject_kicked_bomb(sim.state());
    if (jelly_scenario) inject_kicked_bomb(sim.state(), /*jelly=*/true);
    if (conveyor_scenario) inject_belt_bomb(sim.state());
    if (flame_scenario) inject_flame_bomb(sim.state());
    if (flight_scenario) inject_flying_bomb(sim.state());
    if (playerbelt_scenario) inject_playerbelt(sim.state());
    // Digest the initial state as tick 0, then each post-tick state.
    emit_digest(0, sim.state());
    for (int t = 1; t <= ticks; ++t) {
        const TickInputs in =
            idle_scenario ? TickInputs{} : scripted_inputs(static_cast<std::uint64_t>(t));
        sim.tick(in);
        emit_digest(static_cast<std::uint64_t>(t), sim.state());
    }
    return 0;
}

// Build (standalone, from repo root, 64-bit MSVC dev prompt):
//   cl /std:c++20 /EHsc /I libs\sim\include tools\oracle_mirror\mirror.cpp ^
//      libs\sim\src\setup.cpp libs\sim\src\hash.cpp libs\sim\src\simulation.cpp ^
//      libs\sim\src\systems\*.cpp /Fe:mirror.exe
// Or add a CMake exe target linking bomber::sim (see tools/oracle_mirror/CMakeLists.txt).
