#include "bomber/net/build_hash.hpp"

#include <utility>

#include "bomber/sim/simulation.hpp"

namespace bomber::net {
namespace {

// build_hash's job is to stop two peers whose SIMULATIONS DISAGREE from ever
// reaching tick 0 together: fixed asset-free scenarios are run and their
// state_hashes folded, so any behaviour change shifts the digest and the lobby
// door refuses the mismatch.
//
// IT IS ONLY AS GOOD AS WHAT THE SCENARIOS EXECUTE, and one long emergent run is
// not enough — the settings that reach one mechanic suppress another, which is
// why there is one scenario PER MECHANIC CLASS. When you add a system to
// libs/sim, add or extend a scenario here and CHECK IT: build the digest before
// and after and confirm it moved. Coverage by coincidence is not coverage.
//
// docs/net-build-hash.md is the evidence register — what each scenario is
// MEASURED to discriminate, the reverts that prove it, and the gaps still open.
//
// Scenarios 7 and 8 additionally carry EXECUTION WITNESSES (build_hash_coverage,
// asserted by tests/net/test_build_hash.cpp): counts of the bounces, detonations
// and rover deaths they reach. A hash cannot tell you a scenario stopped
// reaching its mechanic, and that — not a wrong number — is how this file has
// lost coverage three times.

// The classic (odd,odd) pillar arena's fill rule, as a pure function of the
// tile: pillars on odd/odd, a deterministic brick pattern elsewhere so setup's
// powerup-hiding RNG runs.
sim::Cell arena_fill(int x, int y) {
    if (x % 2 == 1 && y % 2 == 1) return sim::Cell::Solid;  // fixed pillars
    if ((x + y) % 3 == 0) return sim::Cell::Brick;          // destructible, hides powerups
    return sim::Cell::Blank;
}

sim::MatchConfig pillar_arena() {
    using namespace sim;
    MatchConfig cfg;
    for (int y = 0; y < kGridHeight; ++y)
        for (int x = 0; x < kGridWidth; ++x) cfg.cells[y][x] = arena_fill(x, y);
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
// it crushes the AI seat before the brain has done anything worth hashing.
//
// KNOWN GAP, still open FOR THIS SCENARIO: it does NOT discriminate the "AI
// never bombs a warphole" fix — the digest is byte-identical with and without
// it. Scenario 5 covers AI DECISIONS wholesale; the warphole drop-refusal
// specifically is still uncovered (docs/net-build-hash.md §3).
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

// 4. STAGE ACTORS, DRIVEN. Scenarios 2 and 3 PLACE warpholes and trampolines
// without ever reaching them, and PLACEMENT IS NOT COVERAGE: the "trigger is the
// -1 APPROACH, not arrival" fix left the digest byte-identical across both
// (docs/net-build-hash.md §4). This one walks a player ONTO a trampoline and
// THROUGH a warphole whose exit is itself a warphole.
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

// 5. THE AI's KEY PRESSES — the glove path, and the only scenario built against
// the brain's DECISIONS. The AI seat is BORN holding the grab and punch gloves,
// so the run drives all four AI key-write sites: behaviour 3's drop, behaviour
// 0's grab and carrying release, behaviour 1's punch (facts.md "AI key presses
// manufacture their own edge").
//
// EVERY CONSTANT HERE WAS MEASURED — as tuned the run makes 3 drops, 3 grabs and
// 3 throws, first grab at tick 10 — and the brick POCKET is load-bearing rather
// than decorative. If you edit this scenario, re-measure: tests/sim/test_ai.cpp
// "build_hash scenario 5 really drives the glove path" asserts the grab, so it
// fails loudly if an edit makes the brain idle. Do NOT fold this into scenario
// 3; the full clock and the spawn OFF an actor tile are both load-bearing.
// docs/net-build-hash.md §5 has the three reverts this is measured against.
std::uint64_t ai_gloves_scenario_hash() {
    using namespace sim;
    MatchConfig cfg = pillar_arena();
    const int rx = kGridWidth - 1, by = kGridHeight - 1;
    cfg.ai[3] = true;  // seat 3 spawns at (rx, by), plain floor, full clock

    // Born with the gloves whose behaviours press keys — grab (kind 6) drives
    // behaviour 0, punch (kind 5) drives behaviour 1 — plus the spare bomb that
    // lets behaviour 3 re-drop while an earlier bomb is still live.
    cfg.born_with_extra[3][static_cast<std::size_t>(PowerupType::Grab)] = true;
    cfg.born_with_extra[3][static_cast<std::size_t>(PowerupType::Punch)] = true;
    cfg.born_with_extra[3][static_cast<std::size_t>(PowerupType::ExtraBomb)] = true;

    // The brick pocket: fill the AI's 5x5 corner (its own spawn tile excepted)
    // and leave exactly one open step out of the corner.
    for (int y = by - 4; y <= by; ++y)
        for (int x = rx - 4; x <= rx; ++x)
            if (cfg.cells[y][x] == Cell::Blank && !(x == rx && y == by))
                cfg.cells[y][x] = Cell::Brick;
    cfg.cells[by - 1][rx] = Cell::Blank;

    Simulation sim(cfg);
    for (int t = 0; t < 400; ++t) sim.tick(canned_inputs(t, 3));  // seat 3 is AI-driven
    return sim.hash();
}

// Seat 0 walks right into the brick wall, drops one bomb, retreats far enough
// to survive it, comes back through the gap it blew (picking up the skull that
// was under the brick), then walks back past the parked seat 1 and off again.
// Seat 1 never presses anything: it is the contagion target, parked one tile
// from seat 0's spawn and outside the blast.
sim::TickInputs disease_inputs(int t) {
    sim::TickInputs in;
    in.players[0].right = (t < 60) || (t >= 110 && t < 200) || t >= 260;
    in.players[0].left = (t >= 60 && t < 110) || (t >= 200 && t < 260);
    in.players[0].action1 = (t == 59);
    return in;
}

// 6. DISEASES — infection, contagion and EXPIRY. Scenarios 1-5 all leave
// `Tuning::diseases_time_limited` at its default, so the whole "VALUELST id 121
// is dead in the original" fix was invisible to the digest: every scenario took
// the same branch either way. A DEFAULT IS NOT COVERAGE. This one turns the flag
// off, the only setting the fix changes anything for.
//
// Its board and inputs were arrived at by measurement: pillar_arena plus the
// shared canned_inputs reaches ZERO infections in 400 ticks. The corridor
// guarantees the pickup instead — every brick in the wall hides a skull, and
// seat 0 has to blast through to continue (docs/net-build-hash.md §6).
std::uint64_t disease_scenario_hash() {
    using namespace sim;
    MatchConfig cfg;
    for (auto& row : cfg.cells) row.fill(Cell::Blank);
    for (int y = 0; y < kGridHeight; ++y) cfg.cells[y][5] = Cell::Brick;
    cfg.spawns = {{0, 0}, {1, 0}};
    cfg.player_count = 2;
    cfg.seed = 0x534B554Cu;  // "SKUL"
    cfg.tuning.input_freeze_ticks = 0;
    for (int k = 0; k < kPowerupKinds; ++k) cfg.spawn_override[k] = 0;
    cfg.spawn_override[static_cast<std::size_t>(PowerupType::Disease)] = 60;
    for (auto& f : cfg.tuning.disease_frames) f = 40;
    cfg.tuning.disease_cure_chance = 0;        // no cure roll to mask the infection
    cfg.tuning.diseases_time_limited = false;  // the dead VALUELST id 121

    Simulation sim(cfg);
    for (int t = 0; t < 400; ++t) sim.tick(disease_inputs(t));
    return sim.hash();
}

// Seat 0 drops a JELLY bomb, steps west off it, walks back east to kick it into
// the wall at column 7, then leaves row 0 to the ping-pong. Seat 1 drops a
// TRIGGER bomb, walks clear, fires it with action2, and repeats — the second
// cycle is past the allowance, where the placement DOWNGRADES to a timed bomb
// (facts.md "Trigger allowance").
sim::TickInputs bomb_kind_inputs(int t) {
    sim::TickInputs in;
    auto& jelly = in.players[0];
    jelly.action1 = (t == 0);
    jelly.left = (t >= 1 && t <= 10);
    jelly.right = (t >= 11 && t <= 18);  // walk back INTO the bomb -> kick east
    jelly.down = (t >= 19 && t <= 26);

    auto& trig = in.players[1];
    trig.action1 = (t == 0 || t == 60);
    trig.down = (t >= 1 && t <= 12) || (t >= 61 && t <= 72);
    trig.action2 = (t == 20 || t == 80);
    return in;
}

// 7. BOMB KINDS, DRIVEN. Scenarios 1-6 reach neither a jelly bomb nor a trigger
// bomb: nothing grants either kind, and no bomb in them is ever kicked. The gap
// was MEASURED, not assumed — reversing the jelly bounce direction left the
// digest byte-identical at 3364373141 while golden E moved, which is determinism
// rule 7 failing exactly as it has three times before.
sim::MatchConfig bomb_kind_arena() {
    using namespace sim;
    MatchConfig cfg;
    for (auto& row : cfg.cells) row.fill(Cell::Blank);
    cfg.cells[0][7] = Cell::Solid;  // the wall the kicked jelly reverses off
    cfg.spawns = {{2, 0}, {2, 4}};
    cfg.player_count = 2;
    cfg.seed = 0x4A454C4Cu;  // "JELL"
    cfg.tuning.input_freeze_ticks = 0;
    cfg.tuning.fuse_frames = 200;  // long enough for the kick to happen first
    cfg.born_with_extra[0][static_cast<std::size_t>(PowerupType::Jelly)] = true;
    cfg.born_with_extra[0][static_cast<std::size_t>(PowerupType::Kick)] = true;
    cfg.born_with_extra[1][static_cast<std::size_t>(PowerupType::Trigger)] = true;
    return cfg;
}

bool holds_live_trigger_bomb(const sim::State& s, int seat) {
    for (const auto& b : s.bombs)
        if (b.owner == seat && b.trigger && b.fuse < 0) return true;
    return false;
}

void note_bomb_kind_events(const sim::State& s, int t, BuildHashCoverage& cov) {
    // Only a trigger detonation can put an explosion in these windows: the sole
    // timed bomb on the board is seat 0's jelly, on a 200-frame fuse. A few ticks
    // of slack because the key sets the fuse and the blast resolves on the bomb
    // system's next pass.
    const bool after_action2 = (t >= 20 && t <= 24) || (t >= 80 && t <= 84);
    for (const auto& e : s.events) {
        if (e.type == sim::Event::Type::JellyBounced) ++cov.jelly_bounces;
        if (e.type == sim::Event::Type::Explosion && after_action2) ++cov.trigger_detonations;
        // Seat 1's placements only, and only the ones that really came out as a
        // key-waiting bomb: both seats drop on tick 0, so reading bombs.back()
        // per BombPlaced event counted seat 0's jelly as a trigger bomb and
        // reported two where there is one.
        if (e.type == sim::Event::Type::BombPlaced && e.player == 1 &&
            holds_live_trigger_bomb(s, 1))
            ++cov.trigger_bombs;
    }
}

std::uint64_t bomb_kind_scenario_hash(BuildHashCoverage* cov) {
    sim::Simulation sim(bomb_kind_arena());
    for (int t = 0; t < 300; ++t) {
        sim.tick(bomb_kind_inputs(t));
        if (cov != nullptr) note_bomb_kind_events(sim.state(), t, *cov);
    }
    return sim.hash();
}

// Seat 0 walks a lap and drops a bomb every 24 ticks, so the board carries flame
// the wandering rovers can walk into, and so a rover has a stationary target to
// land on between drops.
sim::TickInputs rover_inputs(int t) {
    sim::TickInputs in;
    in.players[0].right = (t % 96) < 24;
    in.players[0].down = (t % 96) >= 24 && (t % 96) < 48;
    in.players[0].left = (t % 96) >= 48 && (t % 96) < 72;
    in.players[0].up = (t % 96) >= 72;
    in.players[0].action1 = (t % 24 == 0);
    return in;
}

// 8. CAMPAIGN ROVERS. Their per-tick wander mover draws on State::rng every tick
// (docs/re/campaign.md), so the whole hazard subsystem — spawn placement, the
// mover, flame death, the landing-tile kill — is invisible to a digest whose
// scenarios all leave campaign_rovers at 0. A DEFAULT IS NOT COVERAGE, the same
// lesson scenario 6 records about diseases_time_limited.
void note_rover_events(const sim::State& s, BuildHashCoverage& cov) {
    for (const auto& e : s.events) {
        if (e.type == sim::Event::Type::RoverSpawned) ++cov.rovers_spawned;
        if (e.type == sim::Event::Type::RoverDied) ++cov.rover_deaths;
        if (e.type == sim::Event::Type::RoverKilledPlayer) ++cov.rover_kills;
    }
}

std::uint64_t rover_scenario_hash(BuildHashCoverage* cov) {
    using namespace sim;
    MatchConfig cfg;
    for (auto& row : cfg.cells) row.fill(Cell::Blank);
    cfg.spawns = {{1, 1}};
    cfg.player_count = 1;
    cfg.seed = 0x524F5652u;  // "ROVR"
    cfg.tuning.input_freeze_ticks = 0;
    cfg.campaign_rovers = 6;
    cfg.campaign_rover_speed = 300;

    Simulation sim(cfg);
    // Spawn events are emitted by the CONSTRUCTOR and the first tick() rebuilds
    // the list, so they have to be read before the loop starts.
    if (cov != nullptr) note_rover_events(sim.state(), *cov);
    for (int t = 0; t < 400; ++t) {
        sim.tick(rover_inputs(t));
        if (cov != nullptr) note_rover_events(sim.state(), *cov);
    }
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
             {enclosure_scenario_hash(), ai_scenario_hash(), stage_actor_scenario_hash(),
              ai_gloves_scenario_hash(), disease_scenario_hash(), bomb_kind_scenario_hash(nullptr),
              rover_scenario_hash(nullptr)}) {
            h ^= s + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        }
        std::uint32_t v = fold64(h);
        v ^= kWireProtocolVersion * 0x9E3779B1u;  // mix the wire-protocol version
        return v;
    }();
    return cached;
}

BuildHashCoverage build_hash_coverage() {
    BuildHashCoverage cov;
    // Re-runs the two scenarios rather than sharing state with build_hash's
    // cached fold: an accumulator the digest path also wrote would be one more
    // thing that could make reading the coverage change the number.
    bomb_kind_scenario_hash(&cov);
    rover_scenario_hash(&cov);
    return cov;
}

}  // namespace bomber::net
