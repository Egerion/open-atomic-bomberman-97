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
    std::printf(" | B=%d F=%d\n", count_live_bombs(s), count_flame_cells(s));
}

}  // namespace

int main(int argc, char** argv) {
    std::uint32_t seed = 0x12345678u;
    int ticks = 200;
    int players = 2;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--seed=", 7) == 0)
            seed = static_cast<std::uint32_t>(std::strtoul(argv[i] + 7, nullptr, 0));
        else if (std::strncmp(argv[i], "--ticks=", 8) == 0)
            ticks = std::atoi(argv[i] + 8);
        else if (std::strncmp(argv[i], "--players=", 10) == 0)
            players = std::atoi(argv[i] + 10);
    }
    std::fprintf(stderr, "mirror: seed=0x%08x ticks=%d players=%d\n", seed, ticks, players);

    // Match the native --oracle config EXACTLY: slot 1 is an IDLE HUMAN, not an
    // AI. AI would diverge immediately by RNG (native LCG vs clean-room
    // xorshift), which is NOT a mechanic bug and would swamp the comparison.
    // The native side sets both slots to controller type 2 (local human);
    // slot 0 is scripted, slot 1 receives no input.
    Simulation sim(make_config(seed, players, /*slot1_ai=*/false));
    // Digest the initial state as tick 0, then each post-tick state.
    emit_digest(0, sim.state());
    for (int t = 1; t <= ticks; ++t) {
        sim.tick(scripted_inputs(static_cast<std::uint64_t>(t)));
        emit_digest(static_cast<std::uint64_t>(t), sim.state());
    }
    return 0;
}

// Build (standalone, from repo root, 64-bit MSVC dev prompt):
//   cl /std:c++20 /EHsc /I libs\sim\include tools\oracle_mirror\mirror.cpp ^
//      libs\sim\src\setup.cpp libs\sim\src\hash.cpp libs\sim\src\simulation.cpp ^
//      libs\sim\src\systems\*.cpp /Fe:mirror.exe
// Or add a CMake exe target linking bomber::sim (see tools/oracle_mirror/CMakeLists.txt).
