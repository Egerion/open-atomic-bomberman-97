#include "bomber/net/build_hash.hpp"

#include <utility>

#include "bomber/sim/simulation.hpp"

namespace bomber::net {
namespace {

// A fixed, asset-free reference match: the classic (odd,odd) pillar arena with a
// deterministic brick fill (so setup's powerup-hiding RNG runs) and four players
// in the corners, driven through a canned move/bomb pattern so movement, bombs,
// flames, brick destruction and pickups all execute. Any change to that
// behaviour shifts the resulting state_hash — the whole point of build_hash.
std::uint64_t reference_scenario_hash() {
    using namespace sim;
    MatchConfig cfg;
    for (int y = 0; y < kGridHeight; ++y) {
        for (int x = 0; x < kGridWidth; ++x) {
            if (x % 2 == 1 && y % 2 == 1) {
                cfg.cells[y][x] = Cell::Solid;  // fixed pillars
            } else if ((x + y) % 3 == 0) {
                cfg.cells[y][x] = Cell::Brick;  // destructible, hides powerups
            } else {
                cfg.cells[y][x] = Cell::Blank;
            }
        }
    }
    // Keep the four spawn corners (and an escape step) clear so nobody is walled
    // in — otherwise the canned inputs would do nothing and weaken the digest.
    const int rx = kGridWidth - 1;
    const int by = kGridHeight - 1;
    for (auto [cx, cy] : {std::pair{0, 0}, std::pair{rx, 0}, std::pair{0, by}, std::pair{rx, by}}) {
        cfg.cells[cy][cx] = Cell::Blank;
    }
    cfg.spawns = {{0, 0}, {rx, 0}, {0, by}, {rx, by}};
    cfg.player_count = 4;
    cfg.seed = 0x424F4D42u;  // "BOMB" — fixed so the scenario is reproducible
    cfg.tuning.input_freeze_ticks = 0;  // act from tick 0

    Simulation sim(cfg);
    for (int t = 0; t < 60; ++t) {
        TickInputs in;
        for (int p = 0; p < 4; ++p) {
            // Seat 0→SE, 1→SW, 2→NE, 3→NW: the four walk toward the centre and
            // stagger a bomb drop every 8 ticks so their inputs differ.
            in.players[p].right = (p == 0 || p == 2);
            in.players[p].left = (p == 1 || p == 3);
            in.players[p].down = (p == 0 || p == 1);
            in.players[p].up = (p == 2 || p == 3);
            in.players[p].action1 = (t % 8 == p);
        }
        sim.tick(in);
    }
    return sim.hash();
}

std::uint32_t fold64(std::uint64_t h) {
    return static_cast<std::uint32_t>(h ^ (h >> 32));
}

}  // namespace

std::uint32_t build_hash() {
    static const std::uint32_t cached = [] {
        std::uint32_t v = fold64(reference_scenario_hash());
        v ^= kWireProtocolVersion * 0x9E3779B1u;  // mix the wire-protocol version
        return v;
    }();
    return cached;
}

}  // namespace bomber::net
