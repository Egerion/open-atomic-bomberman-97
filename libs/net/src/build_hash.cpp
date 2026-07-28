#include "bomber/net/build_hash.hpp"

#include <utility>

#include "bomber/sim/simulation.hpp"

namespace bomber::net {
namespace {

// build_hash's job is to stop two peers whose SIMULATIONS DISAGREE from ever
// reaching tick 0 together. It does that by running fixed, asset-free scenarios
// and folding their state_hashes: any behaviour change shifts the digest, the
// lobby door refuses the mismatch, and nobody discovers the divergence halfway
// through a match instead.
//
// It is therefore only as good as what the scenarios actually EXECUTE, and one
// long emergent run is not enough — the settings that reach one mechanic
// suppress another. Measured, not theorised: a single 60-tick full-clock run
// never armed the enclosure and had no AI seat, so TWO real behaviour fixes
// (facts.md "AI never bombs a warphole", and the enclosure arm sweep that
// clears warpholes/trampolines) both left the digest BYTE-IDENTICAL. A peer on
// the old build was still admitted and only diverged mid-match — exactly the
// failure this guard exists to prevent. Widening the single scenario then made
// it worse in a new way: shortening the clock so the walls arm also crushed the
// AI seat before it could act, so the AI path went dark again.
//
// Hence one scenario PER MECHANIC CLASS, each free to pick settings that suit
// it. When you add a system to libs/sim, add or extend a scenario here, and
// CHECK IT: build the digest before and after your change and confirm it moved.
// Coverage by coincidence is not coverage.

// The classic (odd,odd) pillar arena with a deterministic brick fill, so
// setup's powerup-hiding RNG runs.
sim::MatchConfig pillar_arena() {
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
    const int rx = kGridWidth - 1;
    const int by = kGridHeight - 1;
    // Keep the four spawn corners clear so nobody is walled in — otherwise the
    // canned inputs would do nothing and weaken the digest.
    for (auto [cx, cy] : {std::pair{0, 0}, std::pair{rx, 0}, std::pair{0, by}, std::pair{rx, by}}) {
        cfg.cells[cy][cx] = Cell::Blank;
    }
    cfg.spawns = {{0, 0}, {rx, 0}, {0, by}, {rx, by}};
    cfg.player_count = 4;  // MatchConfig's ctor fills `active` all-true
    cfg.seed = 0x424F4D42u;             // "BOMB" — fixed so the scenario is reproducible
    cfg.tuning.input_freeze_ticks = 0;  // act from tick 0
    return cfg;
}

// The four walk toward the centre and stagger a bomb drop, so their inputs
// differ from each other and from tick to tick. `humans` bounds which seats are
// driven; the rest are left neutral for the AI to fill.
sim::TickInputs canned_inputs(int t, int humans) {
    sim::TickInputs in;
    for (int p = 0; p < humans; ++p) {
        in.players[p].right = (p == 0 || p == 2);
        in.players[p].left = (p == 1 || p == 3);
        in.players[p].down = (p == 0 || p == 1);
        in.players[p].up = (p == 2 || p == 3);
        in.players[p].action1 = (t % 8 == p);
    }
    return in;
}

// 1. CORE: movement, bombs, flames, brick destruction, powerup pickups.
std::uint64_t core_scenario_hash() {
    sim::Simulation sim(pillar_arena());
    for (int t = 0; t < 60; ++t) sim.tick(canned_inputs(t, 4));
    return sim.hash();
}

// 2. ENCLOSURE + STAGE ACTORS + ROUND END. A deliberately SHORT match clock, so
// the run reaches the hurry window: the walls arm, the arm sweep clears the
// actor types it clears (warphole and trampoline, but NOT conveyor or dirarrow),
// players get crushed, and the round-end freeze engages. None of that is
// reachable on a full clock, which is why it went untested.
std::uint64_t enclosure_scenario_hash() {
    using namespace sim;
    MatchConfig cfg = pillar_arena();
    const int rx = kGridWidth - 1, by = kGridHeight - 1;
    cfg.tuning.game_seconds = 8;

    // One of each type the sweep discriminates on.
    cfg.cells[3][3] = Cell::Blank;
    cfg.actor_type[3][3] = ActorType::Warphole;
    cfg.warp_dest_x[3][3] = static_cast<std::uint8_t>(rx - 3);
    cfg.warp_dest_y[3][3] = static_cast<std::uint8_t>(by - 3);
    cfg.cells[by - 3][rx - 3] = Cell::Blank;
    cfg.actor_type[by - 3][rx - 3] = ActorType::Trampoline;
    cfg.cells[3][rx - 3] = Cell::Blank;
    cfg.actor_type[3][rx - 3] = ActorType::Conveyor;

    Simulation sim(cfg);
    for (int t = 0; t < 400; ++t) sim.tick(canned_inputs(t, 4));
    return sim.hash();
}

// 3. THE AI BRAIN. A FULL clock on purpose: the enclosure must not arm here, or
// it crushes the AI seat before the brain has done anything worth hashing (that
// is not hypothetical — it is what a combined scenario actually did). The AI
// seat spawns ON a warphole whose destination is a second warphole.
//
// KNOWN GAP, measured and not yet closed: this scenario does NOT yet
// discriminate the "AI never bombs a warphole" fix — the digest is byte-
// identical with and without it, so a peer missing that fix would still be
// admitted. Seating the AI on a warphole was not enough; the likely reason is
// that a player on a warphole spends its time in the warp movement states
// rather than deciding to drop, so the guarded branch is never reached. Closing
// this needs a scenario built against the AI's actual decision path, verified
// the same way everything here must be: build the digest before and after the
// fix and confirm it moves. Until then, treat AI behaviour as UNCOVERED by
// build_hash.
std::uint64_t ai_scenario_hash() {
    using namespace sim;
    MatchConfig cfg = pillar_arena();
    const int rx = kGridWidth - 1, by = kGridHeight - 1;
    const int wx = rx - 2, wy = by - 2;
    cfg.ai[3] = true;  // seat 3 spawns at (rx, by)

    cfg.cells[wy][wx] = Cell::Blank;
    cfg.actor_type[by][rx] = ActorType::Warphole;
    cfg.warp_dest_x[by][rx] = static_cast<std::uint8_t>(wx);
    cfg.warp_dest_y[by][rx] = static_cast<std::uint8_t>(wy);
    cfg.actor_type[wy][wx] = ActorType::Warphole;
    cfg.warp_dest_x[wy][wx] = static_cast<std::uint8_t>(rx);
    cfg.warp_dest_y[wy][wx] = static_cast<std::uint8_t>(by);

    Simulation sim(cfg);
    for (int t = 0; t < 300; ++t) sim.tick(canned_inputs(t, 3));  // seat 3 is AI-driven
    return sim.hash();
}

// Seat 0 walks right along the cleared row 0, then STOPS and idles, then walks
// again. The idle is the point: the actor trigger predicate and the re-entry
// guard only diverge for a player that comes to rest ON an actor tile.
sim::TickInputs actor_inputs(int t) {
    sim::TickInputs in;
    in.players[0].right = (t < 100) || (t >= 200);
    return in;
}

// 4. STAGE ACTORS, DRIVEN. Scenarios 2 and 3 both PLACE warpholes and
// trampolines, and neither discriminates a change to them: #2's four seats walk
// to the centre and never reach the actor tiles, #3's AI seat sits in the warp
// states rather than deciding anything (see its comment). Measured the way this
// file demands: the "trigger is the -1 APPROACH, not arrival" fix plus the
// removal of the tramp_latch/warp_latch player fields left the digest
// BYTE-IDENTICAL across all three, so a peer without that fix was still
// admitted and would desync on any board carrying an actor — which every stock
// warphole map does. This scenario exists to make that impossible: it walks a
// player ONTO a trampoline and THROUGH a warphole whose exit is itself a
// warphole (the ping-pong case the removed latch used to suppress).
std::uint64_t stage_actor_scenario_hash() {
    using namespace sim;
    MatchConfig cfg = pillar_arena();

    // A clear corridor so the walk actually reaches the actors; row 0 carries no
    // pillars (y % 2 == 1 is false) but does carry the brick fill.
    for (int x = 0; x < kGridWidth; ++x) cfg.cells[0][x] = Cell::Blank;

    cfg.actor_type[0][4] = ActorType::Trampoline;
    // Paired warpholes: the exit is a warphole too, so arriving re-satisfies the
    // entry predicate and only the guard stops an infinite hop.
    cfg.cells[4][2] = Cell::Blank;
    cfg.actor_type[0][8] = ActorType::Warphole;
    cfg.warp_dest_x[0][8] = 2;
    cfg.warp_dest_y[0][8] = 4;
    cfg.actor_type[4][2] = ActorType::Warphole;
    cfg.warp_dest_x[4][2] = 8;
    cfg.warp_dest_y[4][2] = 0;

    Simulation sim(cfg);
    for (int t = 0; t < 300; ++t) sim.tick(actor_inputs(t));
    return sim.hash();
}

std::uint32_t fold64(std::uint64_t h) {
    return static_cast<std::uint32_t>(h ^ (h >> 32));
}

}  // namespace

std::uint32_t build_hash() {
    static const std::uint32_t cached = [] {
        // Order matters and is part of the digest; append new scenarios, never
        // reorder, or every existing build looks incompatible for no reason.
        std::uint64_t h = core_scenario_hash();
        for (const std::uint64_t s :
             {enclosure_scenario_hash(), ai_scenario_hash(), stage_actor_scenario_hash()}) {
            h ^= s + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        }
        std::uint32_t v = fold64(h);
        v ^= kWireProtocolVersion * 0x9E3779B1u;  // mix the wire-protocol version
        return v;
    }();
    return cached;
}

}  // namespace bomber::net
