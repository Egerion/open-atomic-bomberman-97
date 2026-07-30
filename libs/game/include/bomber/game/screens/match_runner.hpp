#pragma once

#include <cstdint>

#include "bomber/game/app_flow.hpp"      // AppInput
#include "bomber/game/match_cadence.hpp"  // MatchCadence (the F9 lever, resolved once)
#include "bomber/game/screen_context.hpp"
#include "bomber/game/screens/match_runner_state.hpp"
#include "bomber/sim/match_config.hpp"  // sim::MatchConfig (build_config)
#include "bomber/sim/simulation.hpp"    // sim::TickInputs / sim::State

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

    // The CONFIG half of start_match, split out VERBATIM (no statement moved or
    // reordered — start_match now calls this and then seeds/loads exactly as
    // before, so the golden hashes are untouched). PUBLIC because the ONLINE
    // setup stage has to build the very same config the local PLAY path would
    // build from the very same screens — and then hand it to the peer over the
    // wire — WITHOUT seeding sim_ or swapping the stage art yet
    // (GameApp::present_net_setup). Pure with respect to this object: it only
    // reads state_/ctx_.
    sim::MatchConfig build_config(std::uint32_t seed) const;

private:
    // The F9 lever as it ACTUALLY APPLIES this frame, plus the two interpolation
    // values that follow from it — all three from match_cadence.hpp's one pure
    // function, so they cannot disagree (see its header for the bug that shape
    // exists to prevent). `acc`/`tick_ns` are run()'s fixed-tick accumulator.
    MatchCadence cadence(std::uint64_t acc, std::uint64_t tick_ns) const;

    // Assembles one tick's TickInputs across every roster slot (keyboard sub 0/1,
    // joystick sub, OFF/COMPUTER neutral). DETERMINISM-sensitive — feeds the sim.
    sim::TickInputs collect_inputs() const;
    // The in-round "player row" HUD strip: "S:<wins> K:<kills>" per active slot in
    // that player's ink, with the MISC.ANI "xxx" marker over a slot dead this round.
    void draw_player_row(const sim::State& s);
    // The F8 framerate / cadence / vsync indicator overlay (top-right of the view).
    void draw_fps_overlay(int fps);
    // The two-line prompt an online Esc raises: what the FIRST press did (which
    // differs between host and guest, so the two must not be told the same thing)
    // and that a second press leaves. Drawn only while the Esc window is armed.
    void draw_net_esc_prompt();
    // Record how the local player walked out, for the match shell to read.
    void leave(NetLeave how);

    ScreenContext ctx_;
    MatchRunnerState state_;
};

}  // namespace bomber::game
