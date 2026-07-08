// Locks the Goldman wheel's sim-side surface (docs/re/goldman-roulette.md
// §4/§8): MatchConfig::born_with_extra is a per-player OVERLAY applied in
// setup.cpp AFTER the global born_with loop, through the same
// PowerupSystem::apply path — so it behaves exactly like an extra born_with
// entry for ONE slot instead of every active player. Default all-false, so
// an untouched config's setup is byte-identical to before this field
// existed (the golden suite, test_golden.cpp, stays untouched/green as the
// zero-golden-edits proof).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "helpers.hpp"

using namespace bomber::sim;
using namespace bomber::sim::test;

TEST_CASE("born_with_extra defaults to all-false: setup is unaffected") {
    MatchConfig cfg = open_config();
    Simulation s(cfg);
    // Baseline starting inventory (VALUELST 50/51 defaults: 1 bomb, 2 flame,
    // rest 0) for both players — no goldman award applied.
    CHECK(s.state().players[0].max_bombs == cfg.tuning.start_with[static_cast<int>(PowerupType::ExtraBomb)]);
    CHECK(s.state().players[0].skates == 0);
    CHECK(s.state().players[1].skates == 0);
}

TEST_CASE("born_with_extra grants exactly one player the extra powerup at setup") {
    MatchConfig cfg = open_config();
    cfg.born_with_extra[0][static_cast<int>(PowerupType::Skate)] = true;
    Simulation s(cfg);
    // Player 0 (the gold player in this scenario) got the skate bump...
    CHECK(s.state().players[0].skates == 1);
    // ...player 1 (not gold) did not.
    CHECK(s.state().players[1].skates == 0);
}

TEST_CASE("born_with_extra composes with the global born_with (both apply)") {
    MatchConfig cfg = open_config();
    cfg.born_with[static_cast<int>(PowerupType::Kick)] = true;  // every player
    cfg.born_with_extra[1][static_cast<int>(PowerupType::Skate)] = true;  // player 1 only
    Simulation s(cfg);
    CHECK(s.state().players[0].kick == true);
    CHECK(s.state().players[1].kick == true);
    CHECK(s.state().players[0].skates == 0);
    CHECK(s.state().players[1].skates == 1);
}

TEST_CASE("born_with_extra on an inactive/absent slot is a no-op (no OOB write)") {
    MatchConfig cfg = open_config();
    cfg.player_count = 2;
    // Slot 5 has no spawn and is beyond player_count -> build_state skips it
    // entirely; setting an overlay bit there must not crash or affect anyone.
    cfg.born_with_extra[5][static_cast<int>(PowerupType::Skate)] = true;
    Simulation s(cfg);
    CHECK(s.state().players[0].skates == 0);
    CHECK(s.state().players[1].skates == 0);
}

TEST_CASE("born_with_extra stacks with multiple applications like the original's ++player_byte") {
    // sub_4214BC does `++player_byte[86+prize]` — a plain increment, so
    // repeated grants across rounds (the same match, doc §4 "the current gold
    // player receives the prize again at each round start") accumulate.
    // Model two consecutive setups (i.e. two round inits) to confirm each one
    // independently applies the +1 via PowerupSystem::apply (accumulation
    // caps, VALUELST 550-562, are PowerupSystem's own concern — this just
    // checks the overlay is re-applied, not a one-shot).
    MatchConfig cfg = open_config();
    cfg.born_with_extra[0][static_cast<int>(PowerupType::Skate)] = true;
    Simulation s1(cfg);
    Simulation s2(cfg);
    CHECK(s1.state().players[0].skates == 1);
    CHECK(s2.state().players[0].skates == 1);  // re-applying the SAME config each round-init
}
