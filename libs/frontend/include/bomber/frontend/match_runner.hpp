#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "bomber/frontend/match_runner_state.hpp"
#include "bomber/game_util/app_flow.hpp"       // AppInput
#include "bomber/game_util/match_cadence.hpp"  // MatchCadence (the F9 lever, resolved once)
#include "bomber/game_util/net_esc.hpp"        // NetEscState (the online Esc rule)
#include "bomber/platform/frame_pacer.hpp"     // platform::FramePacer
#include "bomber/sim/match_config.hpp"         // sim::MatchConfig (build_config)
#include "bomber/sim/simulation.hpp"           // sim::TickInputs / sim::State / sim::Event
#include "bomber/ui/screen_context.hpp"

// The MATCH RUNTIME — the deterministic sim's presentation-side driver. run()
// plays one match to its end on the fixed 20 Hz path or the F9 native-cadence
// one. GOLDEN-SENSITIVE: start_match/build_config reproduce the sim's seeding and
// collect_inputs its per-tick input assembly statement-for-statement.
//
// The loop's rationale — pacing, the sub-frame lattice, the tap latch, the
// round-end rules, the online Esc contract — is docs/frontend-match-loop.md.

namespace bomber::game {

class MatchRunner {
public:
    MatchRunner(ScreenContext ctx, MatchRunnerState state) : ctx_(ctx), state_(state) {}

    // One match to its end. Quit if the window closed, else MatchOver — which is
    // also what ANY input returns in attract mode.
    AppInput run();

    // Builds the MatchConfig, resets sim + renderer, seeds the match.
    // RNG/seed-sensitive. PUBLIC because the --demo path builds a match through it.
    void start_match(std::uint32_t seed);

    // The CONFIG half of start_match. PUBLIC because the ONLINE setup stage has to
    // build the very same config the local PLAY path would — and hand it to the
    // peer over the wire — WITHOUT seeding the sim or swapping the stage art yet.
    sim::MatchConfig build_config(std::uint32_t seed) const;

private:
    // Per-player "action key seen down at a frame sample since the last consumed
    // tick" — the frame-cadence tap capture.
    struct TapLatch {
        bool action1 = false, action2 = false;
    };

    // ONE match's loop state, so each phase below can be a named method rather
    // than a closure over two hundred lines of locals.
    struct RunLoop {
        std::uint64_t tick_ns = 0;
        std::uint64_t sub_frame_ns = 0;
        std::uint64_t display_period_ns = 0;
        std::uint64_t last = 0;
        std::uint64_t acc = 0;
        std::uint64_t delta_ns = 0;
        // Round-end gating: `over_ticks` is a plain countdown (the campaign
        // hazard-clear grace, and the netplay fallback); `await_death_fx` is the
        // animation-driven wait the deciding kill arms instead.
        int over_ticks = -1;
        bool await_death_fx = false;
        std::uint64_t fps_frames = 0;
        std::uint64_t fps_window_start_ns = 0;
        int shown_fps = 0;
        std::array<TapLatch, sim::kMaxPlayers> tap_latch{};
        NetEscState esc;
        std::vector<sim::Event> net_events;  // the reusable buffer the net tally fills
        sim::TickInputs frame_in;            // this frame's sample, before the latch
        platform::FramePacer pacer{1, 0};
    };

    // --- the phases of one frame ---
    std::optional<AppInput> pump_events(RunLoop& loop);
    std::optional<AppInput> handle_key(const SDL_Event& ev, RunLoop& loop);
    std::optional<AppInput> on_escape_key(RunLoop& loop);
    std::optional<AppInput> on_help_key(RunLoop& loop);
    void sample_frame(RunLoop& loop);
    std::optional<AppInput> advance_sim(RunLoop& loop);
    std::optional<AppInput> advance_native(RunLoop& loop);
    std::optional<AppInput> advance_fixed(RunLoop& loop);
    std::optional<AppInput> tick_once(RunLoop& loop);
    void drive_tick(const sim::TickInputs& in);
    void tally_round_kills(RunLoop& loop);
    // Returns whether the NATIVE cadence applied to the frame it drew, which is
    // the one thing the pacer below needs from it.
    bool present_frame(RunLoop& loop);
    void pace_next_present(RunLoop& loop, bool native);
    platform::FramePacer::Wait plan_wait(RunLoop& loop, bool native, std::uint64_t now_ns);

    // --- round end ---
    // True once the post-round linger has elapsed and run() should hand back to
    // the Results flow. Called once per advanced 50 ms tick, from BOTH paths.
    bool advance_round_end(RunLoop& loop);
    bool campaign_round_end(RunLoop& loop);
    void latch_campaign_verdict(RunLoop& loop, const sim::State& s);
    bool normal_round_end(RunLoop& loop);
    void arm_round_linger(RunLoop& loop);
    // Closes the online kill tally and returns the exit code, so every agreed
    // exit path closes it the same way. A LOCAL match has no session and this is
    // a plain MatchOver.
    AppInput finish_round(RunLoop& loop);
    bool stop_outstanding() const;

    // --- helpers ---
    // The F9 lever as it ACTUALLY APPLIES this frame, plus the two interpolation
    // values that follow from it — all three from match_cadence.hpp's one pure
    // function, so they cannot disagree.
    MatchCadence cadence(std::uint64_t acc, std::uint64_t tick_ns) const;
    // DETERMINISM-sensitive: this is what feeds the sim.
    sim::TickInputs collect_inputs() const;
    sim::TickInputs latched_inputs(const RunLoop& loop) const;
    void clear_latch(RunLoop& loop) const;
    // The in-round HUD strip: "S:<wins> K:<kills>" per active slot in that
    // player's ink, with the "xxx" marker over a slot dead this round.
    void draw_player_row(const sim::State& s);
    void draw_player_score(int i, SDL_FPoint at);
    void draw_eliminated_marker(SDL_FPoint at);
    void draw_fps_overlay(int fps);
    void draw_net_esc_prompt();  // drawn only while the online Esc window is armed
    void leave(NetLeave how);    // how the local player walked out, for the shell

    ScreenContext ctx_;
    MatchRunnerState state_;
};

}  // namespace bomber::game
