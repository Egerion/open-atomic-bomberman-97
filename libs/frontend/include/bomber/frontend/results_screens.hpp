#pragma once

#include "bomber/frontend/results_state.hpp"
#include "bomber/game_util/app_flow.hpp"  // AppInput
#include "bomber/ui/screen_context.hpp"

// The RESULTS-tier screens (sub_42A3F6), extracted VERBATIM from GameApp
// (ADR-0009 §8): the between-round scoreboard and the Goldman roulette wheel.
// Both draw asset/PCX screens (RESULTS.PCX / ROULETTE.PCX), NOT the live match
// frame (renderer_->draw_frame), so neither takes a MatchBackdrop — the
// scoreboard's need for the just-ended round's sim::State is met by the
// ScoreboardState seam (a const sim::State& captured from the frozen frame; the
// screen never ticks the sim). Each owns its own nested SDL event loop and
// returns exactly what the GameApp method it replaced returned. Both seams are
// stored BY VALUE (cheap reference bundles).

namespace bomber::game {

// The between-round RESULTS cumulative-tally screen (was
// GameApp::present_scoreboard, sub_42A3F6 tail, docs/re/results-and-options.md
// §1): RESULTS.PCX backdrop, a header drawn once per round, one row per active
// player/team with a win-count + kill-count tally in per-player ink, and an
// outcome line reporting either "still need N" (match not yet clinched) or "wins
// the match" (clinched). Any key (or the 6 s idle dwell, for an all-AI/attract
// roster) dismisses it; the caller then starts the next round or, if the outcome
// line reports a clinch, shows the VICTORY screen instead (run_app's Results
// case). Returns the dismiss input (Back on Escape, else Advance; Quit on window
// close). The team-score aggregation, the clinch/outcome-line logic, and the
// win-target/win_by_kills gate are the promoted match_outcome.hpp predicates —
// the same ones run_app uses — so the two agree on whether the match is over.
class ScoreboardScreen {
public:
    ScoreboardScreen(ScreenContext ctx, ScoreboardState state) : ctx_(ctx), state_(state) {}
    AppInput run();

private:
    ScreenContext ctx_;
    ScoreboardState state_;
};

// The Goldman Roulette wheel (was GameApp::present_goldman_wheel,
// docs/re/goldman-roulette.md, sub_4034BC): run at the head of the Play flow,
// before the player-setup screen, whenever goldman is on, we're not in attract,
// it's a local game, AND a gold player is pending (the caller gates on
// options.goldman && gold_player >= 0 before calling — doc §2's re-entry gate).
// ROULETTE.PCX + the spin math (goldman_screen.hpp / goldman_wheel.hpp): 5 setup
// LCG draws off the wheel's dedicated presentation LCG (goldman_lcg, never
// sim::State::rng) then one advance per frame. Awards +1 born-with inventory
// (start_match consumes gold_prize into MatchConfig::born_with_extra, doc §4) to
// the gold player for the following match. F1 opens the SAME generic *.BM help
// browser every other F1 site opens (HelpBrowserScreen, doc §5). Returns Advance
// to continue into the setup screen, Back if Esc aborted the wheel (the caller
// then skips the whole Play flow and forfeits the gold player, doc §2/§5), Quit
// on window close.
class GoldmanWheelScreen {
public:
    GoldmanWheelScreen(ScreenContext ctx, GoldmanState state) : ctx_(ctx), state_(state) {}
    AppInput run();

private:
    ScreenContext ctx_;
    GoldmanState state_;
};

}  // namespace bomber::game
