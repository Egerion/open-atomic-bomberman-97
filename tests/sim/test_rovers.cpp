// Campaign rover/ghost hazard actors: spawn placement, the wander mover's RNG
// stream, flame death + kill-score event, the landing-tile player kill (NOT a
// punch — sub_41DE63 is the death entry point, docs/re/campaign.md mover
// clause 4), and the "all hazards dead" grace timer. See docs/re/campaign.md
// "Rover/ghost/AI roster", "Per-tick mover", "Round pacing".

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "helpers.hpp"

using namespace bomber::sim;
using namespace bomber::sim::test;

namespace {

Fixed centre_x(int tx) {
    return tx * kTileWF + kTileWF / 2;
}
Fixed centre_y(int ty) {
    return ty * kTileHF + kTileHF / 2;
}

bool any_event(const Simulation& s, Event::Type t) {
    for (const auto& e : s.state().events)
        if (e.type == t) return true;
    return false;
}

}  // namespace

// ---- Spawn determinism ------------------------------------------------

TEST_CASE("rover/ghost spawn is deterministic and draws no RNG when count==0") {
    MatchConfig cfg = open_config();
    Simulation a(cfg);
    Simulation b(cfg);
    // Same seed, same config, no campaign hazards configured: identical
    // hashes AND an empty rovers vector (the default-config no-op path).
    CHECK(a.hash() == b.hash());
    CHECK(a.state().rovers.empty());
    CHECK(b.state().rovers.empty());
}

TEST_CASE("a non-campaign config leaves the golden RNG stream untouched") {
    // The pair has to be count==0 against count>0. Setting campaign_rovers = 0
    // explicitly, as this case used to, assigns the field its own DEFAULT
    // (match_config.hpp) — so both sims were built from byte-identical configs
    // and the case compared a value with itself. Spawning for real is what makes
    // "the early-out draws nothing" a claim that can be wrong: the stream has to
    // stay put on the zero side and MOVE on the other.
    MatchConfig cfg = open_config();
    Simulation baseline(cfg);
    cfg.campaign_rovers = 3;
    cfg.campaign_rover_speed = 200;
    Simulation with_rovers(cfg);
    CHECK(baseline.state().rovers.empty());
    CHECK_FALSE(with_rovers.state().rovers.empty());
    CHECK(baseline.hash() != with_rovers.hash());
    CHECK(baseline.state().rng != with_rovers.state().rng);
}

TEST_CASE("rovers spawn at least 3 tiles from every player and never on a wall") {
    MatchConfig cfg = open_config();
    cfg.campaign_rovers = 3;
    cfg.campaign_rover_speed = 200;
    Simulation s(cfg);
    const State& st = s.state();
    // REQUIRE, not a `<= 3` ceiling: zero rovers satisfied that bound AND emptied
    // every per-rover assertion below, so a spawn that placed nothing went green.
    REQUIRE(st.rovers.size() == 3);
    for (const auto& r : st.rovers) {
        CHECK(r.alive);
        CHECK(r.kind == RoverKind::Rover);
        CHECK(st.cells[r.tile_y()][r.tile_x()] != Cell::Solid);
        for (const auto& p : st.players) {
            if (!p.present) continue;
            int d = std::abs(p.tile_x() - r.tile_x()) + std::abs(p.tile_y() - r.tile_y());
            CHECK(d > 3);
        }
    }
}

TEST_CASE("ghosts and rovers both spawn from campaign counts, ghosts seeded first") {
    MatchConfig cfg = open_config();
    cfg.campaign_rovers = 1;
    cfg.campaign_ghosts = 1;
    Simulation s(cfg);
    const State& st = s.state();
    REQUIRE(st.rovers.size() == 2);
    // sub_40151B calls sub_401B05 (ghost) before sub_401AAE (rover), so the
    // ghost occupies slot 0 and the rover slot 1 — this order is part of the
    // RNG-draw-order contract (each spawn call draws from the shared stream
    // in this sequence), not just a labelling nicety.
    CHECK(st.rovers[0].kind == RoverKind::Ghost);
    CHECK(st.rovers[1].kind == RoverKind::Rover);
}

// ---- Wander mover: type-dependent walkability + turn RNG --------------

TEST_CASE("a ghost passes through bricks; a rover does not") {
    MatchConfig cfg = open_config();
    cfg.tuning.spawn_counts[0] = 0;  // no powerups complicating brick tiles
    Simulation s(cfg);
    State& st = s.state();
    st.rovers.clear();
    st.campaign_hazards_active = true;

    // Corridor: brick directly east of the actor, open beyond it.
    st.cells[5][6] = Cell::Blank;
    st.cells[5][7] = Cell::Brick;
    st.cells[5][8] = Cell::Blank;

    Rover ghost;
    ghost.alive = true;
    ghost.kind = RoverKind::Ghost;
    ghost.x = centre_x(6);
    ghost.y = centre_y(5);
    ghost.dir = 1;       // East
    ghost.speed = 2000;  // fast: guarantees it reaches the brick tile this tick
    st.rovers.push_back(ghost);

    for (int i = 0; i < 5; ++i) s.tick(TickInputs{});
    // The ghost should have crossed the brick tile (x >= brick's far edge),
    // proving it did not treat the brick as a wall.
    CHECK(st.rovers[0].tile_x() >= 7);
}

TEST_CASE("a rover stops at a brick wall it cannot pass") {
    MatchConfig cfg = open_config();
    Simulation s(cfg);
    State& st = s.state();
    st.rovers.clear();
    st.campaign_hazards_active = true;

    st.cells[5][6] = Cell::Blank;
    st.cells[5][7] = Cell::Brick;
    // Surround with solid so the forced turn has nowhere better to go except
    // bounce between the two open tiles — the important assertion is just
    // that it never enters tile (7,5).
    for (int x = 0; x < kGridWidth; ++x) st.cells[4][x] = Cell::Solid;
    for (int x = 0; x < kGridWidth; ++x) st.cells[6][x] = Cell::Solid;
    st.cells[5][5] = Cell::Blank;

    Rover rover;
    rover.alive = true;
    rover.kind = RoverKind::Rover;
    rover.x = centre_x(6);
    rover.y = centre_y(5);
    rover.dir = 1;  // East, straight at the brick
    rover.speed = 2000;
    st.rovers.push_back(rover);

    for (int i = 0; i < 30; ++i) s.tick(TickInputs{});
    CHECK(st.rovers[0].tile_x() < 7);  // never crossed into the brick tile
}

TEST_CASE("the wander turn roll draws exactly the documented RNG shape") {
    // At a tile centre with the ahead tile OPEN: 1 draw (turn chance) plus,
    // only if that draw selects "turn", a 2nd draw (direction) — never more
    // than 2, never 0 (the centring test itself costs nothing).
    //
    // The speed-0 walk makes both halves of that claim reachable: 9 x (+100
    // flat) budget per tick is nine 1-px steps, so ticks 1-4 walk candidate
    // offsets 1..36 of the 40-px tile (kTileW) — no centre, so the "costs
    // nothing" half must hold as ZERO draws, not as an always-true disjunct
    // (this case's original one-tick shape allowed `rng == before` and could
    // not fail). Tick 5's steps 37..45 land step 40 exactly on the next
    // centre: turn_at_centre runs once, and the stream must sit on exactly
    // the 1- or 2-iteration successor of where it started.
    MatchConfig cfg = open_config();
    Simulation s(cfg);
    State& st = s.state();
    st.rovers.clear();
    st.campaign_hazards_active = true;
    for (auto& row : st.cells) row.fill(Cell::Blank);  // open field, no walls

    Rover r;
    r.alive = true;
    r.kind = RoverKind::Rover;
    r.x = centre_x(7);
    r.y = centre_y(5);
    r.dir = 1;
    r.speed = 0;  // exactly 1 pixel of budget (+100 flat) per sub-frame
    st.rovers.push_back(r);

    const std::uint32_t rng_before = st.rng;
    for (int i = 0; i < 4; ++i) {
        s.tick(TickInputs{});
        CHECK(st.rng == rng_before);  // 9-36 px walked, no centre: zero draws
    }
    s.tick(TickInputs{});  // crosses the centre 40 px east of the start

    std::uint32_t s1 = rng_before ^ (rng_before << 13);
    s1 ^= s1 >> 17;
    s1 ^= s1 << 5;
    std::uint32_t s2 = s1 ^ (s1 << 13);
    s2 ^= s2 >> 17;
    s2 ^= s2 << 5;
    CHECK(st.rng != rng_before);            // the turn logic really ran...
    CHECK((st.rng == s1 || st.rng == s2));  // ...and drew exactly 1 or 2
}

// ---- Flame death + kill-score event ------------------------------------

TEST_CASE("a rover dies stepping into an active flame and emits RoverDied with the owner") {
    MatchConfig cfg = open_config();
    Simulation s(cfg);
    State& st = s.state();
    st.rovers.clear();
    st.campaign_hazards_active = true;

    Rover r;
    r.alive = true;
    r.kind = RoverKind::Rover;
    r.x = centre_x(6);
    r.y = centre_y(5);
    r.dir = 1;  // East, straight into the flame tile
    r.speed = 2000;
    st.rovers.push_back(r);

    st.flame[5][7] = 5;
    st.flame_owner[5][7] = 3;

    for (int i = 0; i < 5 && !st.rovers.empty(); ++i) s.tick(TickInputs{});

    CHECK(st.rovers.empty());  // reaped after death
    bool found = false;
    for (const auto& e : st.events) {
        if (e.type == Event::Type::RoverDied) {
            found = true;
            CHECK(e.data == 3);  // flame owner slot
        }
    }
    CHECK(found);
}

TEST_CASE("a rover dying to a flame with no owner reports -1, not a garbage slot") {
    MatchConfig cfg = open_config();
    Simulation s(cfg);
    State& st = s.state();
    st.rovers.clear();
    st.campaign_hazards_active = true;

    Rover r;
    r.alive = true;
    r.kind = RoverKind::Ghost;
    r.x = centre_x(6);
    r.y = centre_y(5);
    r.dir = 1;
    r.speed = 2000;
    st.rovers.push_back(r);

    st.flame[5][7] = 5;
    st.flame_owner[5][7] =
        255;  // no attributable owner (matches Player slot sentinel usage elsewhere)

    for (int i = 0; i < 5 && !st.rovers.empty(); ++i) s.tick(TickInputs{});
    CHECK(st.rovers.empty());  // reaped after death
    // The -1 in the case title was never actually asserted: reaping is all the
    // case used to check, so a death that reported a garbage slot passed. Same
    // shape as the owned-flame case above, which does pin its slot.
    bool found = false;
    for (const auto& e : st.events) {
        if (e.type != Event::Type::RoverDied) continue;
        found = true;
        CHECK(e.data == -1);  // no attributable owner, not slot 255 truncated
    }
    CHECK(found);
}

// ---- Landing-tile kill (NOT a punch) ------------------------------------

TEST_CASE("a rover KILLS a human player on its landing tile, not just stuns it") {
    MatchConfig cfg = open_config();
    Simulation s(cfg);
    State& st = s.state();
    st.rovers.clear();
    st.campaign_hazards_active = true;
    st.cells[5][7] = Cell::Blank;  // (7,5) is an odd,odd pillar in open_config(); clear it

    Player& victim = st.players[1];
    victim.present = true;
    victim.alive = true;
    victim.ai = false;
    victim.x = centre_x(7);
    victim.y = centre_y(5);

    Rover r;
    r.alive = true;
    r.kind = RoverKind::Rover;
    r.x = centre_x(6);
    r.y = centre_y(5);
    r.dir = 1;  // East, straight at the victim's tile
    r.speed = 2000;
    st.rovers.push_back(r);

    // Events are per-tick (cleared at the start of run_tick), so check them
    // on the exact tick the kill happens, not after further ticks flush them.
    bool saw_kill_event = false, saw_death_event = false;
    for (int i = 0; i < 5 && victim.alive; ++i) {
        s.tick(TickInputs{});
        saw_kill_event = saw_kill_event || any_event(s, Event::Type::RoverKilledPlayer);
        saw_death_event = saw_death_event || any_event(s, Event::Type::PlayerDied);
    }

    CHECK_FALSE(victim.alive);  // dead, not merely stunned
    CHECK(victim.stun == 0);    // sub_41DE63 is the death path, not sub_421F7E's stun
    CHECK(saw_kill_event);
    CHECK(saw_death_event);
}

TEST_CASE("a rover passes through a COMPUTER (AI) player without killing it") {
    MatchConfig cfg = open_config();
    Simulation s(cfg);
    State& st = s.state();
    st.rovers.clear();
    st.campaign_hazards_active = true;
    st.cells[5][7] = Cell::Blank;  // (7,5) is an odd,odd pillar in open_config(); clear it

    Player& victim = st.players[1];
    victim.present = true;
    victim.alive = true;
    victim.ai = true;  // COMPUTER slot: immune per sub_401B5C's +16==1 check
    victim.x = centre_x(7);
    victim.y = centre_y(5);

    Rover r;
    r.alive = true;
    r.kind = RoverKind::Rover;
    r.x = centre_x(6);
    r.y = centre_y(5);
    r.dir = 1;
    r.speed = 2000;
    st.rovers.push_back(r);

    for (int i = 0; i < 5; ++i) s.tick(TickInputs{});

    CHECK(victim.alive);
    CHECK_FALSE(any_event(s, Event::Type::RoverKilledPlayer));
}

// ---- Grace timer --------------------------------------------------------

TEST_CASE("hazard_clear_timer holds at 0 while a rover is alive, then counts up once clear") {
    MatchConfig cfg = open_config();
    Simulation s(cfg);
    State& st = s.state();
    st.rovers.clear();
    st.campaign_hazards_active = true;

    Rover r;
    r.alive = true;
    r.kind = RoverKind::Rover;
    r.x = centre_x(6);
    r.y = centre_y(5);
    r.speed = 0;
    st.rovers.push_back(r);

    s.tick(TickInputs{});
    CHECK(st.hazard_clear_timer == 0);

    st.rovers.clear();  // simulate the last hazard having died
    s.tick(TickInputs{});
    CHECK(st.hazard_clear_timer == 1);
    s.tick(TickInputs{});
    CHECK(st.hazard_clear_timer == 2);
}

TEST_CASE("hazards_just_cleared fires exactly once, on the kHazardClearTicks-th clear tick") {
    MatchConfig cfg = open_config();
    Simulation s(cfg);
    State& st = s.state();
    st.rovers.clear();  // starts empty: every tick from t=0 counts as "clear"
    st.campaign_hazards_active = true;

    int cleared_count = 0;
    for (int i = 0; i < kHazardClearTicks + 5; ++i) {
        s.tick(TickInputs{});
        // hazards_just_cleared() is exposed on RoverSystem, not State/events;
        // approximate via the timer's own edge since the system is
        // reconstructed each tick (stack object, simulation.cpp). We assert
        // the timer reaches exactly kHazardClearTicks and keeps counting
        // (it is not clamped), which is what hazards_just_cleared() gates on.
        if (st.hazard_clear_timer == kHazardClearTicks) ++cleared_count;
    }
    CHECK(cleared_count == 1);
    CHECK(st.hazard_clear_timer == kHazardClearTicks + 5);
}

// ---- Golden-adjacent: an empty-rover scenario hashes exactly like before ----

TEST_CASE("state_hash with an empty rovers vector matches a hand-computed empty-suffix bump") {
    // Not a literal golden constant (those live in test_golden.cpp and are
    // recaptured there in the same commit as this hash-layout change) — this
    // just proves two independently-built empty-rover states still agree,
    // i.e. the new hash terms are deterministic and don't depend on vector
    // capacity/incidental construction order.
    MatchConfig cfg = open_config();
    Simulation a(cfg);
    Simulation b(cfg);
    for (int i = 0; i < 50; ++i) {
        a.tick(TickInputs{});
        b.tick(TickInputs{});
    }
    CHECK(a.hash() == b.hash());
}
