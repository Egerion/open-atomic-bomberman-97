// Locks the Goldman wheel's sim-side surface (docs/re/goldman-roulette.md
// §4/§8): MatchConfig::born_with_extra is a per-player OVERLAY applied in
// setup.cpp AFTER the Tuning::start_with starting-inventory baseline, through
// PowerupSystem::apply — so it behaves exactly like one more inventory unit
// for ONE slot instead of every active player. Default all-false, so
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

TEST_CASE("born_with_extra composes with the start_with baseline (both apply)") {
    MatchConfig cfg = open_config();
    cfg.tuning.start_with[static_cast<int>(PowerupType::Kick)] = 1;  // every player
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

// Clogs (Goldman wheel booby prize, docs/re/goldman-roulette.md §9): a
// speed-penalty count fed via MatchConfig::born_with_clogs, OUTSIDE the
// kPowerupKinds/PowerupSystem::apply space (§9.2 — never a normal pickup).

TEST_CASE("born_with_clogs defaults to 0: setup speed is unaffected") {
    MatchConfig cfg = open_config();
    Simulation s(cfg);
    CHECK(s.state().players[0].clogs == 0);
    CHECK(s.state().players[0].speed == cfg.tuning.start_speed);
}

TEST_CASE("born_with_clogs subtracts clogs_speed_penalty from start_speed, mirroring skate's addition") {
    // §9.1's pinned arithmetic: base + skates*skate_speed_bonus -
    // clogs*clogs_speed_penalty. One clogs count with default tuning (923
    // start_speed, 150 clogs_speed_penalty) -> 773.
    MatchConfig cfg = open_config();
    cfg.born_with_clogs[0] = 1;
    Simulation s(cfg);
    CHECK(s.state().players[0].clogs == 1);
    CHECK(s.state().players[0].speed ==
          cfg.tuning.start_speed - cfg.tuning.clogs_speed_penalty);
    CHECK(s.state().players[0].speed == 773);
    // Player 1 (not gold) is unaffected.
    CHECK(s.state().players[1].clogs == 0);
    CHECK(s.state().players[1].speed == cfg.tuning.start_speed);
}

TEST_CASE("born_with_clogs and born_with_extra Skate compose additively (skate then clogs, §9.1 order)") {
    // A player who is simultaneously the wheel's gold player for skate AND
    // clogs in the same round (not reachable via the real wheel, which grants
    // exactly one prize per spin, but the sim-level composition must still
    // match the pinned `base + skates*90 - clogs*91` formula regardless of
    // provenance).
    MatchConfig cfg = open_config();
    cfg.born_with_extra[0][static_cast<int>(PowerupType::Skate)] = true;
    cfg.born_with_clogs[0] = 1;
    Simulation s(cfg);
    CHECK(s.state().players[0].skates == 1);
    CHECK(s.state().players[0].clogs == 1);
    CHECK(s.state().players[0].speed ==
          cfg.tuning.start_speed + cfg.tuning.skate_speed_bonus -
              cfg.tuning.clogs_speed_penalty);
}

TEST_CASE("born_with_clogs stacking is a plain overlay value, not an accumulator (§9.3)") {
    // Unlike a persistent counter, the caller (game_app.cpp) SETS this to 1
    // each round for the gold player/team (sub_4214BC resets the whole
    // inventory to baseline before granting, so clogs is always exactly 0-or-1
    // per round, never a growing total, §9.3). Re-applying the SAME config
    // across two round-inits must yield the SAME clogs=1, not 2.
    MatchConfig cfg = open_config();
    cfg.born_with_clogs[0] = 1;
    Simulation s1(cfg);
    Simulation s2(cfg);
    CHECK(s1.state().players[0].clogs == 1);
    CHECK(s2.state().players[0].clogs == 1);
}

TEST_CASE("born_with_clogs on an inactive/absent slot is a no-op (no OOB write)") {
    MatchConfig cfg = open_config();
    cfg.player_count = 2;
    cfg.born_with_clogs[5] = 1;  // slot 5 has no spawn (beyond player_count)
    Simulation s(cfg);
    CHECK(s.state().players[0].clogs == 0);
    CHECK(s.state().players[1].clogs == 0);
}
