#pragma once

#include "bomber/frontend/results_state.hpp"
#include "bomber/game_util/app_flow.hpp"  // AppInput
#include "bomber/ui/screen_context.hpp"

// The RESULTS-tier screens (sub_42A3F6): the between-round scoreboard and the
// Goldman roulette wheel. Both draw asset/PCX screens, NOT the live match frame,
// so neither takes a MatchBackdrop — the scoreboard gets the just-ended round's
// sim::State through its own seam instead, and never ticks it.

namespace bomber::game {

// RESULTS.PCX, a header, one row per active player/team, and an outcome line.
// Any key (or, for an all-AI/attract roster, the 6 s dwell) dismisses it.
//
// The team aggregation, the clinch test and the win-target gate all come from the
// shared match_outcome.hpp predicates — the same ones the shell uses — so the two
// cannot disagree about whether the match is over.
class ScoreboardScreen {
public:
    ScoreboardScreen(ScreenContext ctx, ScoreboardState state) : ctx_(ctx), state_(state) {}
    AppInput run();

private:
    ScreenContext ctx_;
    ScoreboardState state_;
};

// The Goldman Roulette wheel (docs/re/goldman-roulette.md, sub_4034BC), run at
// the head of the Play flow; the caller applies doc §2's re-entry gate. Awards +1
// born-with inventory for the following match (§4). Back means Esc aborted the
// wheel, and the caller then skips the whole Play flow and forfeits the gold
// player (§2/§5).
class GoldmanWheelScreen {
public:
    GoldmanWheelScreen(ScreenContext ctx, GoldmanState state) : ctx_(ctx), state_(state) {}
    AppInput run();

private:
    ScreenContext ctx_;
    GoldmanState state_;
};

}  // namespace bomber::game
