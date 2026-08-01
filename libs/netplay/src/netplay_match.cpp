#include "bomber/netplay/netplay_match.hpp"

#include <SDL3/SDL.h>

#include <algorithm>  // std::any_of
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

#include "bomber/audio/round_music.hpp"  // round_music_id (shared with MatchRunner)
#include "bomber/frontend/asset_screen.hpp"
#include "bomber/frontend/match_runner.hpp"
#include "bomber/frontend/options_screen.hpp"   // is_unlimited_game_seconds
#include "bomber/frontend/outcome_tier.hpp"     // kDrawMusicId / the stings / DRAW + VICTORY
#include "bomber/frontend/results_screens.hpp"  // ScoreboardScreen
#include "bomber/game_util/log.hpp"
#include "bomber/game_util/match_outcome.hpp"  // round_winner / award_round_win / match_clinch
#include "bomber/game_util/net_round_gate.hpp"
#include "bomber/input/input.hpp"           // SlotInputType
#include "bomber/net/net_stats.hpp"         // net::SessionSummary / SessionEndReason
#include "bomber/net/path_failover.hpp"     // net::PathFailover (mid-match dead-path failover)
#include "bomber/net/rematch_session.hpp"   // net::RematchSession (the post-match rendezvous)
#include "bomber/net/rollback_session.hpp"  // net::RollbackSession + net::DropPolicy
#include "bomber/net/round_rotation.hpp"    // net::round_seed / round_tick_base
#include "bomber/net/setup_session.hpp"     // net::SetupSession (the round-rotation gate)
#include "bomber/net/transport.hpp"         // net::Transport
#include "bomber/netplay/net_settle.hpp"
#include "bomber/netui/net_overlay.hpp"  // the netdiag.log record + the session-end modal

namespace bomber::game {

namespace {

// THE BETWEEN-ROUNDS GATE (net_round_gate.hpp), over the SAME net::SetupSession
// the pre-match setup stage uses: round N+1's config is just another host
// confirmation, so the rotation needs no new MsgType and no protocol bump. The
// HOST confirms on its own accept (the original's host-only screen-advance,
// docs/re/network-screens.md §7 kind 32) and holds the screen until every guest
// has acked those exact bytes; a GUEST never dismisses (`sub_40C06A() == 1`).
//
// "For this round" is checked against net::round_seed(), derived identically on
// both peers, so a blob replayed out of an earlier round can never be mistaken
// for the next one. The config still travels in FULL, because a guest whose
// .SCH/EXTRA/VALUELST differ would build a different board from the same seed.
class RoundRotationGate final : public NetRoundGate {
public:
    RoundRotationGate(net::SetupSession& session, bool host, sim::MatchConfig next,
                      std::uint32_t expect_seed, net::PathFailover* failover)
        : session_(&session),
          failover_(failover),
          next_(std::move(next)),
          expect_seed_(expect_seed),
          host_(host) {
        // ONE heartbeat preview: SetupSession's guest liveness clock only
        // advances on inbound setup traffic, so without it a host that reads the
        // scoreboard for longer than the session timeout would look, to the
        // guest, exactly like a host that had quit. Never displayed.
        if (host_) session_->publish(net::SetupPreviewFrame{});
    }

    void pump() override {
        session_->step(static_cast<std::int64_t>(SDL_GetTicks()));
        // No session to watch between rounds (nullptr), but the engine still
        // heart-beats the control plane and finishes an in-flight failover —
        // the matchmaker reaps a member ~30 s of silence in, scoreboard or not.
        if (failover_ != nullptr)
            failover_->pump(static_cast<std::int64_t>(SDL_GetTicks()), nullptr);
    }
    bool readonly() const override { return !host_; }

    void accept() override {
        if (!host_ || confirmed_) return;
        session_->confirm(next_);
        confirmed_ = true;
    }

    // `peer_acked()` on the host is "EVERY guest acked", not "somebody did", so a
    // 3+ peer round gate holds the scoreboard up until the whole table has the
    // next round's bytes.
    bool ready() const override {
        if (host_) return confirmed_ && session_->peer_acked();
        return session_->has_final_config() && session_->final_config().seed == expect_seed_;
    }

    bool failed() const override { return session_->failed(); }

    // Our own bytes on the host, the host's exact decoded bytes on the guest
    // (match_config_codec.hpp). Only valid once ready() — the caller checks that.
    const sim::MatchConfig& next_config() const { return host_ ? next_ : session_->final_config(); }

private:
    net::SetupSession* session_;
    net::PathFailover* failover_;
    sim::MatchConfig next_;
    std::uint32_t expect_seed_;
    bool host_;
    bool confirmed_ = false;
};

// After the gate opens, the guest has sent exactly ONE ack and is about to hand
// the socket to the match session. If that ack was lost the host would sit until
// its own timeout, so keep pumping briefly — the same 300 ms settle, for the same
// reason, as the setup stage's own exit (net_settle.hpp carries the argument).
constexpr std::uint64_t kRoundHandoffSettleMs = 300;

// THE POST-MATCH GATE — the same NetRoundGate seam, driving net::RematchSession
// instead of a config exchange, because a decided match has no next round to
// agree on. TWO PHASES over one session, and the split is the whole design.
// PHASE A (the clinch RESULTS scoreboard) is pump-only: each peer dismisses its
// own, since the outcome was computed identically on both sides with no traffic.
// PHASE B (VICTORY) applies the original's rule (docs/re/network-screens.md §7)
// — the machine driving the game dismisses and a client follows. It has to,
// because the guest's next SetupSession starts a liveness timeout the moment it
// is built: a guest that walked into the roster screen ahead of a host still
// reading VICTORY would time out and report THE HOST LEFT THE GAME.
//
// Escape is exempt on both screens (asset_screen.cpp / results_screens.cpp both
// route it around the gate): leaving is always the local player's own call.
class RematchGate final : public NetRoundGate {
public:
    RematchGate(net::RematchSession& session, net::PathFailover* failover)
        : session_(&session), failover_(failover) {}

    void pump() override {
        session_->step(static_cast<std::int64_t>(SDL_GetTicks()));
        if (failover_ != nullptr)  // control-plane keep-alive, as in the round gate
            failover_->pump(static_cast<std::int64_t>(SDL_GetTicks()), nullptr);
    }

    // Phase A: nobody is read-only. Phase B: the guest follows the host.
    bool readonly() const override { return final_phase_ && !session_->is_host(); }

    void accept() override {
        if (final_phase_)
            session_->accept();  // HOST: announce the walk back to the setup screens
        else
            local_accepted_ = true;
    }

    bool ready() const override { return final_phase_ ? session_->ready() : local_accepted_; }
    bool failed() const override { return session_->failed(); }

    // Called between the two screens. One session spans both so its liveness
    // clock never restarts and never has a gap in it.
    void begin_final_phase() { final_phase_ = true; }

private:
    net::RematchSession* session_;
    net::PathFailover* failover_;
    bool final_phase_ = false;
    bool local_accepted_ = false;
};

// A peer that has been told the round ends at tick X may still be short of it,
// and it can only get there on OUR input window. Keep pumping the (now
// non-simulating) session this long before the between-rounds gate takes the
// socket. Longer than the round-handoff settle above because it covers a real
// catch-up, not just one lost ack.
constexpr std::uint64_t kAbandonSettleMs = 600;

// THE EVIDENCE A DEAD SESSION LEAVES BEHIND. "It suddenly cut out" was
// unanswerable because by the time the player has alt-tabbed to say so, the
// window and everything on it are gone. Written from a DESTRUCTOR, not from each
// of the match loop's several exits, because the one path guaranteed to matter
// is the one nobody remembered to instrument. The reason is latched as the match
// runs; Unknown means the function left by a path that had no opinion, which is
// itself worth seeing rather than papered over with a plausible guess.
class NetSessionRecorder {
public:
    explicit NetSessionRecorder(NetSeats seats) {
        summary_.timestamp = net_log_timestamp();
        summary_.local_seats = seats.local;
        summary_.all_seats = seats.all;
        summary_.is_host = seats.host;
    }
    NetSessionRecorder(const NetSessionRecorder&) = delete;
    NetSessionRecorder& operator=(const NetSessionRecorder&) = delete;
    NetSessionRecorder(NetSessionRecorder&&) = delete;
    NetSessionRecorder& operator=(NetSessionRecorder&&) = delete;
    ~NetSessionRecorder() {
        // Stamped at the END, so the line carries when the session died rather
        // than when it started — which is the question being asked of it.
        summary_.timestamp = net_log_timestamp();
        append_net_session_log(summary_);
    }

    // One session per ROUND, so the newest snapshot is the one that was live
    // when whatever happened happened.
    void snapshot(const net::RollbackSession& s, int round) {
        summary_.stats = s.stats();
        summary_.round = round;
    }
    void latch(net::SessionEndReason r) { summary_.reason = r; }
    void note(std::string n) { summary_.note = std::move(n); }
    // The failover's verdict (PathFailover::log_token). Re-stamped at every
    // snapshot point so the line carries the LATEST state — an attempt still
    // in flight when the window closes logs as pending(...), which is itself
    // an answer.
    void note_failover(std::string t) { summary_.failover = std::move(t); }
    const net::SessionSummary& summary() const { return summary_; }

private:
    net::SessionSummary summary_;
};

// ONE MATCH. Everything the round loop keeps alive across its steps is a member
// here, which is what lets each step below be a short named method. Constructed,
// run once, destroyed.
class MatchLoop {
public:
    explicit MatchLoop(const NetMatchRun& run)
        : seams_(&run.seams),
          state_(&run.state),
          transport_(&run.transport),
          carry_(run.carry),
          failover_(run.failover),
          round_cfg_(run.config),
          // The MATCH seed. Round 0's config carries it and every later round
          // derives its own from it (net::round_seed) — identically on both
          // peers, with no traffic — so the two never disagree about which round
          // they are entering.
          match_seed_(run.config.seed),
          // Where this match's rounds sit in the shared tick space: 0 for a
          // one-match caller, the running total for a session that keeps the
          // transport alive across matches.
          base_round_(run.carry != nullptr ? run.carry->round_base : 0),
          seats_(run.seats),
          recorder_(run.seats) {}

    AppInput run();

private:
    // What the rotation step decided, once the outcome screens are done with.
    enum class Rotation : std::uint8_t { NextRound, EndMatch, WindowClosed };

    // The round's verdict: who survived (-1 = draw) and who, if anyone, that just
    // won the MATCH for (-1 = not decided). Two numbers rather than two calls,
    // because the clinch is only asked about a round that had a winner.
    struct RoundScore {
        int winner = -1;
        int clinched = -1;
    };

    int wrapped_round() const { return (base_round_ + round_) % kNetRoundBaseWrap; }

    Rotation play_round();
    net::DropPolicy drop_policy() const;
    // Seed the sim from the agreed config and rebuild stage art, music, the HUD
    // and the presentation roster.
    void begin_round();
    void start_round_music(int stage);
    void seed_presentation_roster();
    AppInput drive_round(net::RollbackSession& session, NetLeave& left);
    // The four ways a round can end the whole MATCH rather than just itself.
    // Latches the reason, shows the session-end modal where there is still a
    // window to show it on, and returns true when the loop must stop.
    bool stopped_after_round(net::RollbackSession& session, NetLeave left);
    bool session_died(const net::RollbackSession& session);
    // The catch-up pump after an abandoned round. False = the window closed.
    bool settle_abandon(net::RollbackSession& session);
    // The RESULTS tier's scoring step (sub_42A3F6's tail).
    RoundScore score_round(bool abandoned);
    void report_session_end();
    void note_walkout(const net::RollbackSession& session, bool stalled);
    sim::MatchConfig next_round_config(std::uint32_t next_seed) const;
    // The rotation gate opened on nothing — an abandon, an Escape, or a dead
    // link. Reports which, and how the match ends.
    Rotation no_next_round(const RoundRotationGate& gate);
    // The clinch tier: RESULTS + VICTORY under a RematchGate. Always ends the
    // match.
    void present_clinch(int clinched);
    // The DRAW prefix, when the round had no survivor. False = stop the loop.
    bool present_draw();
    Rotation rotate(int winner, const net::RollbackSession& session);

    const NetplaySeams* seams_;
    NetplayState* state_;
    net::Transport* transport_;
    NetSessionCarry* carry_;
    net::PathFailover* failover_ = nullptr;
    sim::MatchConfig round_cfg_;
    std::uint32_t match_seed_;
    int base_round_;
    int round_ = 0;
    NetSeats seats_;
    AppInput result_ = AppInput::Advance;
    NetSessionRecorder recorder_;
};

AppInput MatchLoop::run() {
    // Fresh MATCH tally (the local path's reset_match_scores, minus its
    // win_target_ write): win_target_ was already agreed during the setup stage,
    // so stamping getvalue(310) over it here would silently shorten the match on
    // both peers. The CLI has no level screen and keeps init()'s default.
    state_->win_count.fill(0);
    state_->kill_count.fill(0);

    // is_team_mode()/draw_player_row read the team GATE, not just the per-slot
    // bytes, and the agreed config is the only authority for it online (team play
    // off leaves every cfg.team[] at 0 — build_config's `cfg.team.fill(0)`).
    // Restored after the MATCH so a following LOCAL game keeps the player's own
    // Options setting.
    const TeamPlayScope team_gate(state_->team_play);

    if (carry_ != nullptr) carry_->rematch = false;

    for (round_ = 0;; ++round_) {
        const Rotation rot = play_round();
        if (rot == Rotation::WindowClosed || rot == Rotation::EndMatch) {
            // The failover verdict may have moved after play_round stamped it —
            // an attempt can complete (or fail) under the between-rounds gates —
            // so the line the recorder is about to write gets the final word.
            if (failover_ != nullptr) recorder_.note_failover(failover_->log_token());
        }
        if (rot == Rotation::WindowClosed) return AppInput::Quit;
        if (rot == Rotation::EndMatch) break;
    }
    return result_;
}

// Past a hard silence window a seat is declared dropped (ADR-0011 Risks) —
// handed to the deterministic AISystem at the same tick on every peer with
// Options row 12 ON, ending the match loudly with it OFF. The window is
// deliberately LONG (600 pumps = 30 s at 20 Hz) and the counter resets on any
// input. An earlier 2.5 s killed a match whenever someone dragged their window:
// Windows blocks the message pump for the whole drag, so the peer stops existing
// for as long as the mouse is held. This must mean "genuinely gone", not
// "briefly busy".
net::DropPolicy MatchLoop::drop_policy() const {
    return net::DropPolicy{state_->options.lost_net_revert_ai, seats_.host,
                           /*timeout_ticks=*/600};
}

MatchLoop::Rotation MatchLoop::play_round() {
    if (carry_ != nullptr) carry_->round_base = wrapped_round();
    begin_round();

    // ONE SESSION PER ROUND, over the same socket — but NOT restarting the tick
    // count: round N is based at net::round_tick_base(N) so a datagram straggling
    // out of round N-1 carries a tick below this round's confirmed_ and is
    // dropped by the session's existing guard, instead of being filed as a
    // far-future input or compared as a phantom peer hash.
    net::RollbackSession session(state_->sim, seats_.local, seats_.all, /*max_prediction=*/8,
                                 *transport_, drop_policy(), net::round_tick_base(wrapped_round()));
    NetLeave left = NetLeave::None;
    result_ = drive_round(session, left);
    recorder_.snapshot(session, round_);  // this round's numbers, whatever it ended as
    if (failover_ != nullptr) recorder_.note_failover(failover_->log_token());

    if (stopped_after_round(session, left)) return Rotation::EndMatch;

    // AN ABANDONED ROUND (somebody pressed Esc). The decision was the HOST's and
    // travelled as MatchCtlKind::EndRound, so both peers stopped at the same tick
    // and both take this branch. It is a DRAW BY DECREE: nothing below asks the
    // frozen state who won, which is what makes the abandon immune to the two
    // peers' last speculative ticks differing.
    //
    // round_ended(), NOT end_round_scheduled(): the round must actually have
    // REACHED the agreed tick. An abandon is announced a second or so ahead of
    // itself, and Ctrl+Q inside that window still has to mean "leave the session"
    // rather than being swallowed into a draw the player never got to see.
    const bool abandoned = session.round_ended();
    if (abandoned && !settle_abandon(session)) return Rotation::WindowClosed;

    const RoundScore score = score_round(abandoned);
    if (score.clinched >= 0) {
        present_clinch(score.clinched);
        return Rotation::EndMatch;
    }
    return rotate(score.winner, session);
}

MatchLoop::RoundScore MatchLoop::score_round(bool abandoned) {
    // THE OUTCOME, computed with no traffic at all: both peers ran the same
    // deterministic sim over the same inputs, so round_winner()/the tally/the
    // clinch agree by construction. (The Goldman wheel and the campaign
    // round-pacing overrides the LOCAL results tail also runs are both local-only
    // in the original — `sub_4034BC` is gated `!sub_40C06A()`,
    // docs/re/goldman-roulette.md §2 — so an online match legitimately skips
    // them.)
    const sim::State& s = state_->sim.state();
    const int w = abandoned ? -1 : ::bomber::game::round_winner(s);
    if (w < 0) return {w, -1};  // a draw scores nobody and can clinch nothing
    // Same team mirror as the local tail (sub_421B56) — both peers run it over
    // identical state, so the tallies stay identical too.
    ::bomber::game::award_round_win(state_->win_count, w, state_->team_play, s, state_->setup_team);
    return {w, ::bomber::game::match_clinch(s, state_->team_play, state_->setup_team,
                                            state_->options.win_by_kills, state_->kill_count,
                                            state_->win_count, state_->win_target)};
}

void MatchLoop::begin_round() {
    // The config arrives whole — grid, actors, warps, spawns, roster, tuning and
    // seed — so nothing here re-derives anything per machine.
    const ScreenContext ctx = seams_->sctx();
    const int stage = round_cfg_.tuning.level_index;
    state_->sim = sim::Simulation(round_cfg_);

    // Presentation setup — mirrors MatchRunner::start_match's tail (which run()
    // SKIPS for a netplay match, since we seed the sim from the agreed config
    // here). Re-run every round because a RANDOM level rotates the stage between
    // rounds exactly as it does locally.
    if (ctx.assets.load_stage(stage)) ctx.seqs.resolve_stage(ctx.assets, stage);
    start_round_music(stage);
    // Untimed HUD from the AGREED config, not from this peer's own options.ini.
    // Play Time travels as part of the host's Tuning, so the CLOCK was already
    // the host's — but this flag was hardcoded false, so a host playing
    // "Infinite" left both peers watching the 99999 s stand-in count down (~27 h)
    // instead of hiding the clock. The mirror case was worse: a guest whose own
    // options.ini said Infinite hid the clock on a match that really did end on
    // time.
    state_->renderer.reset_match(is_unlimited_game_seconds(round_cfg_.tuning.game_seconds));
    ctx.sounds.reset();

    seed_presentation_roster();
}

void MatchLoop::start_round_music(int stage) {
    // RETRACTED: "disable_game_music is presentation-only (never sim), so netplay
    // just keeps music on". Being presentation-only is exactly why the option CAN
    // be honoured online — it needs no agreement with the peer and moves no hash.
    // The original agrees: the guard at 0x410E88 is a bare test of dword_4648C0
    // with no network arm (docs/re/sound-engine.md §9), and the ONE place the
    // round-music path consults sub_40C06A is inside sub_4293E5, choosing GENERIC
    // over the per-level track — never whether music plays at all. Not gated on
    // the stage art loading, for the same reason as the local path.
    const ScreenContext ctx = seams_->sctx();
    const int track = round_music_id(stage, state_->options.disable_game_music,
                                     [&ctx](int id) { return ctx.audio.has_track(id); });
    if (track == kRoundMusicSilent) {
        ctx.audio.stop_music();
        return;
    }
    ctx.audio.start_music(track);
}

void MatchLoop::seed_presentation_roster() {
    // PRESENTATION roster, derived from the AGREED config so both peers show the
    // same thing; the match roster itself is the config's own active/ai/team. A
    // LOCAL seat reads keyboard key-set 0, so the human at this machine plays
    // with arrows regardless of which seat they own. Every other active human
    // slot is type 4 OTHER, the original's own marker for "someone else's player"
    // (docs/re/network-screens.md §7, sub_40D372); collect_inputs leaves those
    // and the AI slots neutral, and the rollback session or the deterministic
    // AISystem fills them before every tick().
    state_->setup_type.fill(static_cast<int>(SlotInputType::Off));
    state_->setup_sub.fill(0);
    state_->setup_team.fill(0);
    // Driven off the MASK, not a single seat index: today exactly one bit is set
    // (2-seat rooms), but reading the mask means a future peer that owns two
    // seats gets its second one mapped to key-set 1 with no change here.
    int key_set = 0;
    for (int s = 0; s < sim::kMaxPlayers; ++s) {
        const auto slot = static_cast<std::size_t>(s);
        if (!round_cfg_.active[slot]) continue;
        if (round_cfg_.ai[slot])
            state_->setup_type[slot] = static_cast<int>(SlotInputType::Computer);
        else if ((seats_.local & (1u << s)) != 0u) {
            state_->setup_type[slot] = static_cast<int>(SlotInputType::Keyboard);
            state_->setup_sub[slot] = key_set++;
        } else
            state_->setup_type[slot] = static_cast<int>(SlotInputType::Other);
        // MatchConfig::team[] is the +84 byte shifted up by one (build_config's
        // "sim team 0 = solo side" note); shift it back for the presentation
        // roster the HUD/outcome helpers read.
        state_->setup_team[slot] = round_cfg_.team[slot] != 0 ? round_cfg_.team[slot] - 1 : 0;
    }
    state_->team_play = std::any_of(round_cfg_.team.begin(), round_cfg_.team.end(),
                                    [](std::uint8_t t) { return t != 0; });
}

AppInput MatchLoop::drive_round(net::RollbackSession& session, NetLeave& left) {
    // The seam's net_session routes each fixed tick through the ROLLBACK session,
    // which gives ZERO input delay — the input-delay lockstep it replaced added a
    // fixed ~200 ms. max_prediction=8 caps how far the display may run ahead.
    MatchRunnerState mrs = seams_->match_runner_state();
    mrs.net_session = &session;
    mrs.net_failover = failover_;
    mrs.net_local_seats = seats_.local;
    mrs.net_is_host = seats_.host;
    mrs.net_leave = &left;
    return MatchRunner(seams_->sctx(), mrs).run();
}

void MatchLoop::report_session_end() {
    // ON SCREEN as well as in the log: this used to be a stderr line on a GUI
    // build nobody sees, so from the player's side the match simply stopped for
    // no stated reason.
    if (present_net_session_end(seams_->sctx(), recorder_.summary()) == AppInput::Quit)
        result_ = AppInput::Quit;
}

void MatchLoop::note_walkout(const net::RollbackSession& session, bool stalled) {
    // "left-stalled with depth 8/8" is a bug report, where a bare "left" is a
    // player who stopped enjoying themselves.
    const net::NetStats& ns = session.stats();
    char note[128];
    std::snprintf(note, sizeof(note),
                  stalled ? "double-Esc bail-out; depth=%d/%d stalls=%u rephase=%u"
                          : "Ctrl+Q forfeit; depth=%d/%d stalls=%u rephase=%u",
                  ns.prediction_depth, ns.max_prediction, static_cast<unsigned>(ns.stall_pumps),
                  static_cast<unsigned>(ns.rephase_holds));
    recorder_.note(note);
}

// The two ways the SESSION itself ended the round, each of which gets the modal.
bool MatchLoop::session_died(const net::RollbackSession& session) {
    if (session.desynced()) {
        log_warn("netplay: DESYNC at tick %u — peers diverged (config/seed mismatch?)",
                 session.desync_tick());
        recorder_.latch(net::SessionEndReason::Desync);
        report_session_end();
        return true;
    }
    if (!session.aborted()) return false;
    // Options row 12 off: a peer went silent and the match ends rather than
    // handing its seat to the AI. Say so — not a normal round end.
    log_warn(
        "netplay: a player dropped; match ended (turn on \"Lost net players "
        "revert to AIs\" to play on)");
    recorder_.latch(net::SessionEndReason::PeerDropped);
    recorder_.note("silence past the 600-pump timeout with Options row 12 off");
    report_session_end();
    return true;
}

bool MatchLoop::stopped_after_round(net::RollbackSession& session, NetLeave left) {
    if (session_died(session)) return true;
    if (result_ != AppInput::MatchOver) {
        // The window closed under the match. No modal — there is nothing left to
        // show it on — but the log line is exactly why this case is worth
        // latching: it is the one the player cannot report themselves.
        recorder_.latch(net::SessionEndReason::WindowClosed);
        return true;
    }
    // THE LOCAL PLAYER WALKED OUT — Ctrl+Q's faithful forfeit, or the double-Esc
    // bail-out from a match that stopped responding. Read from an explicit flag,
    // not inferred: the old test for Ctrl+Q ("more than one side alive and time
    // left") sat BELOW the abandon branch, so a bail-out pressed after an abandon
    // had been agreed was read as a draw and rotated into another round instead
    // of leaving.
    //
    // Everything a bail-out needs happens by simply LEAVING THE ROUND LOOP, which
    // is what makes it unilateral: the exit runs the RollbackSession destructor,
    // returns Advance with carry_->rematch still false, the enclosing frame drops
    // its UdpTransport, and run_app maps NetHost/NetJoin+Advance back to the main
    // menu. Nothing waited on.
    if (left == NetLeave::None) return false;
    const bool stalled = left == NetLeave::Stalled;
    result_ = AppInput::Advance;  // straight out to the menu
    recorder_.latch(stalled ? net::SessionEndReason::LeftStalled
                            : net::SessionEndReason::LeftSession);
    note_walkout(session, stalled);
    return true;
}

bool MatchLoop::settle_abandon(net::RollbackSession& session) {
    // The peer may still be short of the agreed tick, and only OUR input window
    // can get it there. advance() no longer simulates once round_ended() — it
    // just receives, re-announces and re-sends — so this is a pure catch-up pump.
    if (!net_settle(kAbandonSettleMs, [this, &session] {
            session.advance(sim::TickInputs{}, static_cast<std::int64_t>(SDL_GetTicks()));
            if (failover_ != nullptr)
                failover_->pump(static_cast<std::int64_t>(SDL_GetTicks()), &session);
        })) {
        recorder_.latch(net::SessionEndReason::WindowClosed);
        return false;
    }
    recorder_.snapshot(session, round_);  // the catch-up pump moved the numbers
    // Latched, not final: the match usually carries on into another round, and
    // any later exit overwrites this. It matters for the case where it does NOT —
    // an Esc at the DRAW or scoreboard right after — so the log says "somebody
    // abandoned" rather than a bare "left".
    recorder_.latch(net::SessionEndReason::RoundAbandoned);
    log_info("netplay: round %d abandoned at tick %u — draw", round_,
             static_cast<unsigned>(session.end_round_tick()));
    return true;
}

void MatchLoop::present_clinch(int clinched) {
    // MATCH win — the same clinch tier the local path shows: the RESULTS
    // scoreboard with the 2000 winner voice under it, then VICTORY<n>/TEAM<n>.
    // Both peers reach this independently and identically, so the OUTCOME needs
    // no agreement. WHAT DOES need agreeing is what happens NEXT: this used to be
    // the end of the road — the transport the peers had punched a path for was
    // destroyed, so two people who had just finished a game and wanted another
    // one were back at the lobby. Both peers now walk back to the SETUP screens
    // over the SAME link, and the RematchGate is the door.
    const ScreenContext ctx = seams_->sctx();
    net::RematchSession rematch_session(*transport_, seats_.host);
    RematchGate gate(rematch_session, failover_);
    ctx.audio.start_music(kDrawMusicId);                   // 1130 under RESULTS/VICTORY (doc §2)
    ctx.audio.play_sting(kWinnerStingLo, kWinnerStingHi);  // winner voice — clinch only
    ScoreboardState csbs = seams_->scoreboard_state();
    // Phase A: pump only — each peer still dismisses its own board.
    if (carry_ != nullptr) csbs.net_gate = &gate;
    result_ = ScoreboardScreen(ctx, csbs).run();
    if (result_ == AppInput::Quit) return;
    gate.begin_final_phase();  // Phase B: the host dismisses, the guest follows
    result_ = present_asset_screen(
        ctx,
        victory_screen(::bomber::game::is_team_mode(state_->team_play, state_->sim.state(),
                                                    state_->setup_team),
                       clinched, state_->setup_team[static_cast<std::size_t>(clinched)]),
        carry_ != nullptr ? &gate : nullptr);
    if (result_ == AppInput::Quit) return;
    // Advance means the gate opened (the host walked back to setup and said so);
    // Back means this player pressed Escape and is done. A failed gate is the
    // peer having vanished — also done, quietly: the match itself is complete
    // either way.
    if (carry_ != nullptr && result_ == AppInput::Advance && gate.ready()) carry_->rematch = true;
    result_ = AppInput::Advance;
    recorder_.latch(net::SessionEndReason::MatchCompleted);
}

bool MatchLoop::present_draw() {
    // DRAW is a PREFIX to the tally, not an alternative (raw 0x42A875-0x42A88B
    // falls through into RESULTS). It is deliberately NOT gated: no ticks run
    // under either screen and the tally behind it IS the synchronisation point,
    // so the two peers dismissing DRAW at different moments costs nothing. (The
    // original instead broadcasts a second advance — kind 32 payload 904 — which
    // our wire has no need of once the config exchange is the barrier.)
    const ScreenContext ctx = seams_->sctx();
    ctx.audio.play_sting(kDrawStingLo, kDrawStingHi);
    ScreenDef ds = draw_screen();
    // ADVANCING IS THE HOST'S. The host waits for its own Enter (dwell 0); a
    // GUEST auto-advances on DRAW's own 6 s dwell — the original's `sub_42A3F6`
    // auto-advance — into the scoreboard, where it already waits for the host's
    // next-round confirmation. Requiring Enter on BOTH machines is what the owner
    // hit: two people staring at DRAW.PCX, each waiting for the other.
    if (seats_.host) ds.dwell_ms = 0;
    result_ = present_asset_screen(ctx, ds);
    if (result_ == AppInput::Advance) return true;
    recorder_.latch(result_ == AppInput::Quit ? net::SessionEndReason::WindowClosed
                                              : net::SessionEndReason::LeftSession);
    if (result_ != AppInput::Quit) result_ = AppInput::Advance;  // Esc: abandon
    return false;
}

sim::MatchConfig MatchLoop::next_round_config(std::uint32_t next_seed) const {
    // A GUEST builds nothing: it takes the host's exact bytes off the wire, so no
    // future change to libs/match can make the two disagree.
    if (!seats_.host) return {};
    // The host builds it from the SAME screens' state that produced round 0, so
    // the roster and the level choice carry over; a RANDOM level rotates because
    // the seed moved.
    return MatchRunner(seams_->sctx(), seams_->match_runner_state()).build_config(next_seed);
}

MatchLoop::Rotation MatchLoop::no_next_round(const RoundRotationGate& gate) {
    // Escape (either peer abandoning the match) or the link died under the screen
    // — either way there is no agreed next round.
    if (!gate.failed()) {
        recorder_.latch(net::SessionEndReason::LeftSession);
        recorder_.note("left at the between-rounds scoreboard");
        result_ = AppInput::Advance;
        return Rotation::EndMatch;
    }
    log_warn("netplay: lost the peer between rounds; match ended");
    recorder_.latch(net::SessionEndReason::PeerLostBetweenRounds);
    // The screen the player was looking at gave no hint of this: the scoreboard
    // simply stopped accepting Enter. Say it out loud.
    if (present_net_session_end(seams_->sctx(), recorder_.summary()) == AppInput::Quit)
        return Rotation::WindowClosed;
    result_ = AppInput::Advance;
    return Rotation::EndMatch;
}

MatchLoop::Rotation MatchLoop::rotate(int winner, const net::RollbackSession& session) {
    // NOT DECIDED — another round. The host builds it and confirms it through the
    // gate; the guest takes the host's exact bytes.
    const ScreenContext ctx = seams_->sctx();
    net::SetupSession rotate_session(*transport_, seats_.host, seats_.local, seats_.remote());
    const std::uint32_t next_seed = net::round_seed(match_seed_, round_ + 1);
    RoundRotationGate gate(rotate_session, seats_.host, next_round_config(next_seed), next_seed,
                           failover_);

    ctx.audio.start_music(kDrawMusicId);  // 1130 under DRAW *and* RESULTS (doc §2)
    if (winner < 0 && !present_draw()) return Rotation::EndMatch;

    ScoreboardState sbs = seams_->scoreboard_state();
    sbs.net_gate = &gate;
    result_ = ScoreboardScreen(ctx, sbs).run();
    if (result_ == AppInput::Quit) {
        recorder_.latch(net::SessionEndReason::WindowClosed);
        return Rotation::EndMatch;
    }
    if (!gate.ready()) return no_next_round(gate);
    round_cfg_ = gate.next_config();
    log_info("netplay: round %d over at tick %u; next round seed 0x%08X stage %d", round_,
             static_cast<unsigned>(session.confirmed_tick()),
             static_cast<unsigned>(round_cfg_.seed), round_cfg_.tuning.level_index);
    // Cover a lost ack before the match session takes the socket back.
    if (!net_settle(kRoundHandoffSettleMs, [&gate] { gate.pump(); })) return Rotation::WindowClosed;
    return Rotation::NextRound;
}

}  // namespace

AppInput run_netplay_match(const NetMatchRun& run) {
    return MatchLoop(run).run();
}

}  // namespace bomber::game
