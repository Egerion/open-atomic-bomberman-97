#pragma once

#include <cstdint>

#include "bomber/game/app_flow.hpp"  // AppInput
#include "bomber/game/screen_context.hpp"
#include "bomber/game/screens/match_runner_state.hpp"
#include "bomber/sim/simulation.hpp"  // sim::TickInputs / sim::State

// The MATCH RUNTIME, extracted VERBATIM from GameApp (ADR-0009 §10) — the
// deterministic sim's presentation-side driver. run() plays one match to its end:
// it ticks sim_ (fixed 20 Hz + inter-tick interp, OR the F9 native-cadence
// per-frame Simulation::frame path), renders through the Renderer each displayed
// frame, draws the player-row HUD + F8 fps overlay, and handles the in-round keys
// (Ctrl+Q/Esc abort, F1 help modal, the attract any-input abort). GOLDEN-SENSITIVE:
// start_match builds the MatchConfig and seeds the match, collect_inputs assembles
// the per-tick TickInputs, and the tick order / RNG-affecting seed path are all
// reproduced statement-for-statement — tests/test_golden.cpp must stay
// byte-identical after this move.
//
// Two seams, stored BY VALUE (cheap reference bundles): ScreenContext (the stable
// presentation services) + MatchRunnerState (the match-runtime members GameApp
// owns). GameApp::run_match forwards to run(); GameApp::start_match stays as a
// thin forwarder to start_match() because GameApp::run()'s --demo path also builds
// a match through it (run_demo then ticks sim_ directly).

namespace bomber::game {

class MatchRunner {
public:
    MatchRunner(ScreenContext ctx, MatchRunnerState state) : ctx_(ctx), state_(state) {}

    // Runs one match to its end (one side left or time up). Returns Quit if the
    // window closed mid-match, else MatchOver (also the instant ANY input arrives
    // in attract_ mode). Was GameApp::run_match.
    AppInput run();

    // Builds the MatchConfig (scheme + VALUELST + base tuning + roster + options +
    // goldman award + level/actors/campaign hazards), resets sim_ + renderer_, and
    // seeds the match. RNG/seed-sensitive. PUBLIC because GameApp::start_match
    // forwards to it for the --demo path (GameApp::run()); run() calls it itself at
    // the head of every round.
    void start_match(std::uint32_t seed);

private:
    // Assembles one tick's TickInputs across every roster slot (keyboard sub 0/1,
    // joystick sub, OFF/COMPUTER neutral). DETERMINISM-sensitive — feeds the sim.
    sim::TickInputs collect_inputs() const;
    // The in-round "player row" HUD strip: "S:<wins> K:<kills>" per active slot in
    // that player's ink, with the MISC.ANI "xxx" marker over a slot dead this round.
    void draw_player_row(const sim::State& s);
    // The F8 framerate / cadence / vsync indicator overlay (top-right of the view).
    void draw_fps_overlay(int fps);

    ScreenContext ctx_;
    MatchRunnerState state_;
};

}  // namespace bomber::game
