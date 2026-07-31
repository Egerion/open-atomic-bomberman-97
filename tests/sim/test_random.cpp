// The Random ("?") powerup — reverse-engineered from the pickup dispatcher
// sub_41E21E case 0xC: the token rerolls into a uniform kind 0..11 (Random
// itself is excluded by the modulus), retrying up to 200 times while the
// rolled kind is scheme-forbidden, then dispatches as the rolled kind — a
// skull is a legal outcome. See docs/re/facts.md "Powerup pickup dispatcher".

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "helpers.hpp"

using namespace bomber::sim;
using namespace bomber::sim::test;

TEST_CASE("random rolls into an allowed kind and applies it") {
    // Forbid everything except Kick: the reroll must deliver exactly Kick.
    MatchConfig cfg = open_config();
    for (int k = 0; k < kPowerupKinds; ++k) cfg.forbidden[k] = true;
    cfg.forbidden[static_cast<int>(PowerupType::Kick)] = false;
    Simulation s(cfg);
    Player& p = s.state().players[0];
    CHECK(!p.kick);
    s.state().floor[p.tile_y()][p.tile_x()] = PowerupType::Random;
    run(s, 1);
    CHECK(p.kick);  // rerolled into the only allowed kind
    bool picked_kick = false;
    for (const auto& e : s.state().events)
        if (e.type == Event::Type::PowerupPicked &&
            e.data == static_cast<std::int8_t>(PowerupType::Kick))
            picked_kick = true;
    CHECK(picked_kick);  // the event reports the ROLLED kind, not Random
}

TEST_CASE("random can roll a skull") {
    // Forbid everything except Disease: the reroll must infect.
    MatchConfig cfg = open_config();
    for (int k = 0; k < kPowerupKinds; ++k) cfg.forbidden[k] = true;
    cfg.forbidden[static_cast<int>(PowerupType::Disease)] = false;
    Simulation s(cfg);
    Player& p = s.state().players[0];
    s.state().floor[p.tile_y()][p.tile_x()] = PowerupType::Random;
    // Against the position BEFORE the pickup. `p.x != 0` was a tautology: x is a
    // fixed-point tile-CENTRE, so a player on tile (0,0) already holds
    // kTileWF / 2, and no roll can ever drive it to zero.
    const auto x_before = p.x;
    run(s, 1);
    bool infected = false;
    for (const auto& e : s.state().events)
        if (e.type == Event::Type::Infected) infected = true;
    CHECK(infected);
    CHECK((p.disease_timer > 0 || p.x != x_before));  // sick, or swapped if Swap rolled
}

TEST_CASE("random never yields another random") {
    // Nothing forbidden: over many pickups, every grant is a real kind.
    MatchConfig cfg = open_config();
    Simulation s(cfg);
    Player& p = s.state().players[0];
    constexpr int kPickups = 50;
    int picked = 0, infected = 0;
    for (int i = 0; i < kPickups; ++i) {
        s.state().floor[p.tile_y()][p.tile_x()] = PowerupType::Random;
        run(s, 1);
        for (const auto& e : s.state().events) {
            if (e.type == Event::Type::Infected) ++infected;
            if (e.type != Event::Type::PowerupPicked) continue;
            ++picked;
            CHECK(e.data != static_cast<std::int8_t>(PowerupType::Random));
            CHECK(e.data < 12);
        }
    }
    // The witness, without which this case is 50 laps of asserting nothing: the
    // two bounds above live inside a doubly-guarded loop, so a reroll that stops
    // firing (or a dispatcher that stops emitting the event) empties the body and
    // goes green for exactly the regression the case exists to catch.
    //
    // Every pickup lands as ONE of two events, which is why the floor is the sum
    // rather than `picked == 50`: a roll of Disease/SuperDisease announces
    // Infected instead of PowerupPicked. Measured on this seed, 42 of the 50
    // rolls are ordinary kinds and 8 are skulls.
    CHECK(picked > 0);
    CHECK(picked + infected == kPickups);
}

TEST_CASE("random consumes deterministic RNG") {
    MatchConfig cfg = open_config();
    Simulation a(cfg), b(cfg);
    for (Simulation* s : {&a, &b}) {
        Player& p = s->state().players[0];
        s->state().floor[p.tile_y()][p.tile_x()] = PowerupType::Random;
        s->tick(TickInputs{});
    }
    CHECK(a.hash() == b.hash());
    CHECK(a.state().rng == b.state().rng);
}
