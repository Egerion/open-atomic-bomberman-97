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
#include "bomber/net/rematch_session.hpp"   // net::RematchSession (the post-match rendezvous)
#include "bomber/net/rollback_session.hpp"  // net::RollbackSession + net::DropPolicy
#include "bomber/net/round_rotation.hpp"    // net::round_seed / round_tick_base
#include "bomber/net/setup_session.hpp"     // net::SetupSession (the round-rotation gate)
#include "bomber/net/transport.hpp"         // net::Transport
#include "bomber/netplay/net_settle.hpp"
#include "bomber/netui/net_overlay.hpp"  // the netdiag.log record + the session-end modal

namespace bomber::game {

namespace {

// THE BETWEEN-ROUNDS GATE for an online match (net_round_gate.hpp), implemented
// over the SAME net::SetupSession the pre-match setup stage already uses: round
// N+1's config is just another host confirmation, so the whole rotation needs no
// new MsgType and no protocol-version bump.
//
//   HOST  — pre-builds the next round's config, confirms it the moment the local
//           player accepts the outcome screen (the original's host-only
//           screen-advance, docs/re/network-screens.md §7 kind 32), and keeps the
//           screen up until the guest has acknowledged those exact bytes.
//   GUEST — never dismisses (`sub_40C06A() == 1`); its screen ends when the
//           host's confirmation FOR THIS ROUND arrives.
//
// "For this round" is checked against net::round_seed(): a round's config carries
// its own seed, derived identically on both peers from the match seed + the round
// index, so a blob replayed out of an earlier round can never be mistaken for the
// next one. That is the whole identity check — the config itself still travels in
// full, because a guest whose .SCH/EXTRA/VALUELST differ would build a different
// board from the same seed (setup_session.hpp, "the final may not be approximate").
//
// The host also publishes ONE heartbeat preview: SetupSession's guest liveness
// clock only advances on inbound setup traffic, so without it a host that reads
// the scoreboard for longer than the session timeout would look, to the guest,
// exactly like a host that had quit.
// N-PEER NOTE: `ready()` on the host is SetupSession::peer_acked(), which is now
// "every guest acked", not "somebody did" — so a 3+ peer round gate holds the
// scoreboard up until the whole table has the next round's bytes.
class RoundRotationGate final : public NetRoundGate {
public:
    RoundRotationGate(net::SetupSession& session, bool host, sim::MatchConfig next,
                      std::uint32_t expect_seed)
        : session_(&session), next_(std::move(next)), expect_seed_(expect_seed), host_(host) {
        if (host_) session_->publish(net::SetupPreviewFrame{});  // liveness only; never displayed
    }

    void pump() override { session_->step(static_cast<std::int64_t>(SDL_GetTicks())); }
    bool readonly() const override { return !host_; }

    void accept() override {
        if (!host_ || confirmed_) return;
        session_->confirm(next_);
        confirmed_ = true;
    }

    bool ready() const override {
        if (host_) return confirmed_ && session_->peer_acked();
        return session_->has_final_config() && session_->final_config().seed == expect_seed_;
    }

    bool failed() const override { return session_->failed(); }

    // The agreed next-round config: our own bytes on the host, the host's exact
    // decoded bytes on the guest (match_config_codec.hpp). Only valid once
    // ready() — the caller checks that first.
    const sim::MatchConfig& next_config() const { return host_ ? next_ : session_->final_config(); }

private:
    net::SetupSession* session_;
    sim::MatchConfig next_;
    std::uint32_t expect_seed_;
    bool host_;
    bool confirmed_ = false;
};

// After the gate opens, the guest has sent exactly ONE ack and is about to hand
// the socket to the match session. If that ack was lost the host would sit until
// its own timeout, so keep pumping briefly — the same 300 ms settle, for the same
// reason, as the setup stage's own exit (whose comment carries the full
// argument for why swallowing a few of the peer's early input datagrams here is
// harmless: RollbackSession re-sends its whole unconfirmed window every pump).
constexpr std::uint64_t kRoundHandoffSettleMs = 300;

// THE POST-MATCH GATE — the same NetRoundGate seam the between-ROUNDS rotation
// uses (screens/net_round_gate.hpp), driving net::RematchSession instead of a
// config exchange, because a decided match has no next round to agree on. It has
// TWO PHASES over one session, and the split is the whole design:
//
//   PHASE A — the clinch RESULTS scoreboard. Each peer dismisses its OWN, exactly
//     as before this change: the outcome was computed identically on both sides
//     with no traffic, so there is nothing to agree. The gate is present only to
//     PUMP — which is what keeps the host's liveness flowing while it reads the
//     board, and what drains the socket of the round that just ended.
//
//   PHASE B — the VICTORY screen, the LAST thing before the setup stage. Here the
//     original's rule applies (docs/re/network-screens.md §7): the machine
//     driving the game dismisses the shared screen and a client follows. It has
//     to, because the guest's next SetupSession starts a liveness timeout the
//     moment it is built — a guest that walked into the roster screen ahead of a
//     host still reading VICTORY would time out and report THE HOST LEFT THE
//     GAME. Which is the disconnect this whole change removes, thirty seconds
//     later.
//
// Escape is exempt on both screens (asset_screen.cpp / results_screens.cpp both
// route it around the gate): leaving the session is always the local player's own
// call, and it simply means no rematch.
class RematchGate final : public NetRoundGate {
public:
    explicit RematchGate(net::RematchSession& session) : session_(&session) {}

    void pump() override { session_->step(static_cast<std::int64_t>(SDL_GetTicks())); }

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
    bool final_phase_ = false;
    bool local_accepted_ = false;
};

// A peer that has been told the round ends at tick X may still be short of it,
// and it can only get there on OUR input window. Keep pumping the (now
// non-simulating) session this long before the between-rounds gate takes the
// socket. Longer than the round-handoff settle above because it covers a real
// catch-up, not just one lost ack.
constexpr std::uint64_t kAbandonSettleMs = 600;

// THE EVIDENCE A DEAD SESSION LEAVES BEHIND.
//
// "It suddenly cut out" was unanswerable because by the time the player has
// alt-tabbed to say so, the window and everything on it are gone. So the reason
// and the last numbers are written to netdiag.log next to the executable — and
// written from a DESTRUCTOR, not from each of the match loop's several
// exits, because the one path guaranteed to matter is the one nobody remembered
// to instrument. A session that dies leaves a record however it died.
//
// The reason itself is latched as the match runs. Unknown means the function
// left by a path that had no opinion, which is itself worth seeing in the log
// rather than being papered over with a plausible guess.
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
    const net::SessionSummary& summary() const { return summary_; }

private:
    net::SessionSummary summary_;
};

// ONE MATCH, as an object rather than as a 438-line function with seven
// parameters. Everything the round loop keeps alive across its steps — the
// transport, the seat topology, the round counter, the config the next round
// will run, the AppInput on its way out, and the netdiag recorder — is a member
// here, which is what lets each step below be a short named method instead of
// another block of an unbroken body. Constructed, run once, destroyed.
class MatchLoop {
public:
    MatchLoop(const NetplaySeams& seams, NetplayState& state, net::Transport& transport,
              NetSeats seats, const sim::MatchConfig& cfg, NetSessionCarry* carry)
        : seams_(&seams),
          state_(&state),
          transport_(&transport),
          carry_(carry),
          round_cfg_(cfg),
          // The MATCH seed. Round 0's config carries it and every later round
          // derives its own from it (net::round_seed) — identically on both
          // peers, with no traffic — so the two never disagree about which
          // round they are entering.
          match_seed_(cfg.seed),
          // Where this match's rounds sit in the shared tick space (see
          // NetSessionCarry): 0 for a one-match caller, the running total for a
          // session that keeps the transport alive across matches.
          base_round_(carry != nullptr ? carry->round_base : 0),
          seats_(seats),
          recorder_(seats) {}

    AppInput run();

private:
    // What the rotation step decided, once the outcome screens are done with.
    enum class Rotation : std::uint8_t { NextRound, EndMatch, WindowClosed };

    // The round's verdict: who survived (-1 = draw) and who, if anyone, that
    // just won the MATCH for (-1 = not decided). Two numbers rather than two
    // calls, because the clinch is only asked about a round that had a winner.
    struct RoundScore {
        int winner = -1;
        int clinched = -1;
    };

    // Where round `round_` sits in the shared tick space.
    int wrapped_round() const { return (base_round_ + round_) % kNetRoundBaseWrap; }

    // Seed the sim from the agreed config and rebuild everything the round is
    // played through: stage art, music, the HUD and the presentation roster.
    void begin_round();
    void seed_presentation_roster();
    // Drive the round itself through the SAME MatchRunner a local match uses,
    // with the fixed ticks routed through `session`.
    AppInput drive_round(net::RollbackSession& session, NetLeave& left);
    // The four ways a round can end the whole MATCH rather than just itself.
    // Latches the reason, shows the session-end modal where there is still a
    // window to show it on, and returns true when the loop must stop.
    bool stopped_after_round(net::RollbackSession& session, NetLeave left);
    // The catch-up pump after an abandoned round. False = the window closed.
    bool settle_abandon(net::RollbackSession& session);
    // Tally the round win and report the clinch (the RESULTS tier's scoring
    // step, sub_42A3F6's tail).
    RoundScore score_round(bool abandoned);
    // Say on screen what the log already latched. A window close under the
    // modal is itself the result.
    void report_session_end();
    // The netdiag note for a local walk-out: the numbers at the moment they
    // gave up are the whole value of the record.
    void note_walkout(const net::RollbackSession& session, bool stalled);
    // Round N+1's config: the host builds it, a guest builds nothing and takes
    // the host's exact bytes off the wire.
    sim::MatchConfig next_round_config(std::uint32_t next_seed) const;
    // The rotation gate opened on nothing — an abandon, an Escape, or a dead
    // link. Reports which, and how the match ends.
    Rotation no_next_round(const RoundRotationGate& gate);
    // The clinch tier: RESULTS + VICTORY under a RematchGate. Always ends the
    // match — the caller breaks unconditionally.
    void present_clinch(int clinched);
    // The DRAW prefix, when the round had no survivor. False = stop the loop.
    bool present_draw();
    // Agree round N+1 with the peer over the outcome screens.
    Rotation rotate(int winner, const net::RollbackSession& session);

    const NetplaySeams* seams_;
    NetplayState* state_;
    net::Transport* transport_;
    NetSessionCarry* carry_;
    sim::MatchConfig round_cfg_;
    std::uint32_t match_seed_;
    int base_round_;
    int round_ = 0;
    NetSeats seats_;
    AppInput result_ = AppInput::Advance;
    // Diagnostics (screens/net_overlay.hpp): whatever happens below — including
    // the window closing mid-round — this object writes one netdiag.log line on
    // the way out with the reason and the session's last numbers.
    NetSessionRecorder recorder_;
};

AppInput MatchLoop::run() {
    // Fresh MATCH tally (the local path's reset_match_scores, minus its
    // win_target_ write): win_target_ is the LEVEL & ROUNDS screen's WINS row and
    // was already agreed during the setup stage — the host committed its own,
    // the guest committed the mirrored preview (map_select_screen.cpp) — so
    // stamping getvalue(310) over it here would silently shorten the match on
    // both peers. The CLI (--host/--join) has no level screen and keeps whatever
    // default init() seeded.
    state_->win_count.fill(0);
    state_->kill_count.fill(0);

    // is_team_mode()/draw_player_row read the team GATE, not just the per-slot
    // bytes, and the agreed config is the only authority for it online (team play
    // off leaves every cfg.team[] at 0 — build_config's `cfg.team.fill(0)`).
    // Restored after the MATCH so a following LOCAL game keeps the player's own
    // Options setting; team_play is a mirror of options_.team_play and never
    // reaches options.ini on its own, so nothing is persisted either way.
    const TeamPlayScope team_gate(state_->team_play);

    if (carry_ != nullptr) carry_->rematch = false;

    for (round_ = 0;; ++round_) {
        if (carry_ != nullptr) carry_->round_base = wrapped_round();
        begin_round();

        // ONE SESSION PER ROUND, over the same socket — but NOT restarting the
        // tick count: round N is based at net::round_tick_base(N) so a datagram
        // straggling out of round N-1 carries a tick below this round's
        // confirmed_ and is dropped by the session's existing guard, instead of
        // being filed as a far-future input or compared as a phantom peer hash
        // (rollback_session.hpp's `start_tick`).
        //
        // Peer-drop policy (ADR-0011 Risks): past a hard silence window a seat is
        // declared dropped. With the RE'd Options row 12 ON the HOST announces the
        // handoff and every peer moves that seat to the deterministic AISystem at
        // the same tick, so the match plays on; with it OFF the drop ends the
        // match loudly instead of hanging. This is that option's FIRST consumer —
        // it reached CFG.INI and the Options screen and stopped there until now.
        // The window is deliberately LONG (600 pumps = 30 s at 20 Hz): crossing it
        // is irreversible with row 12 off, and the counter resets on any input, so
        // a peer that returns inside it costs nothing. An earlier 2.5 s killed a
        // match whenever someone dragged their window — Windows blocks the message
        // pump for the whole drag, so the peer just stops existing for as long as
        // the mouse is held. This must mean "genuinely gone", not "briefly busy".
        const net::DropPolicy drop{state_->options.lost_net_revert_ai, seats_.host,
                                   /*timeout_ticks=*/600};
        net::RollbackSession session(state_->sim, seats_.local, seats_.all, /*max_prediction=*/8,
                                     *transport_, drop, net::round_tick_base(wrapped_round()));
        NetLeave left = NetLeave::None;
        result_ = drive_round(session, left);
        // Whatever this round ended as, the log line should carry ITS numbers.
        recorder_.snapshot(session, round_);

        if (stopped_after_round(session, left)) break;

        // AN ABANDONED ROUND (somebody pressed Esc). The decision was the HOST's
        // and it travelled as MatchCtlKind::EndRound, so both peers stopped at
        // the same tick and both take this branch — it is not a local reading of
        // a local keypress. The round is a DRAW BY DECREE: nothing below asks the
        // frozen state who won, which is what makes the abandon immune to the two
        // peers' last speculative ticks differing.
        //
        // round_ended(), NOT end_round_scheduled(): the round must actually have
        // REACHED the agreed tick. An abandon is announced a second or so ahead
        // of itself, and Ctrl+Q inside that window still has to mean "leave the
        // session" rather than being swallowed into a draw the player never got
        // to see.
        const bool abandoned = session.round_ended();
        if (abandoned && !settle_abandon(session)) return AppInput::Quit;

        const RoundScore score = score_round(abandoned);
        if (score.clinched >= 0) {
            present_clinch(score.clinched);
            break;
        }

        const Rotation rot = rotate(score.winner, session);
        if (rot == Rotation::WindowClosed) return AppInput::Quit;
        if (rot == Rotation::EndMatch) break;
    }

    return result_;
}

MatchLoop::RoundScore MatchLoop::score_round(bool abandoned) {
    // THE OUTCOME, computed with no traffic at all: both peers ran the same
    // deterministic sim over the same inputs, so round_winner()/the tally/the
    // clinch agree by construction — there is nothing here for the host to
    // announce. (The Goldman wheel and the campaign round-pacing overrides the
    // LOCAL results tail also runs are both local-only in the original —
    // `sub_4034BC` is gated `!sub_40C06A()`, docs/re/goldman-roulette.md §2 —
    // so an online match legitimately skips them.)
    const sim::State& s = state_->sim.state();
    const int w = abandoned ? -1 : ::bomber::game::round_winner(s);
    if (w < 0) return {w, -1};  // a draw scores nobody and can clinch nothing
    // Same team mirror as the local tail (sub_421B56) — both peers run it
    // over identical state, so the tallies stay identical too.
    ::bomber::game::award_round_win(state_->win_count, w, state_->team_play, s, state_->setup_team);
    return {w, ::bomber::game::match_clinch(s, state_->team_play, state_->setup_team,
                                            state_->options.win_by_kills, state_->kill_count,
                                            state_->win_count, state_->win_target)};
}

void MatchLoop::begin_round() {
    // The config arrives whole (the setup stage's confirmation on the host,
    // SetupSession::final_config()'s exact bytes on the guest, or the
    // canonical build on the CLI; for round > 0, the same confirm/ack
    // exchange run by RoundRotationGate below) — grid, actors, warps, spawns,
    // roster, tuning and seed all included, so nothing here re-derives
    // anything per machine.
    const ScreenContext ctx = seams_->sctx();
    const int stage = round_cfg_.tuning.level_index;
    state_->sim = sim::Simulation(round_cfg_);

    // Presentation setup — mirrors MatchRunner::start_match's tail (which
    // run() SKIPS for a netplay match, since we seed the sim from the agreed
    // config here): stage art + a live music track + a fresh renderer/HUD +
    // sound state. Re-run every round because a RANDOM level rotates the
    // stage between rounds exactly as it does locally (the host's
    // build_config resolves it from that round's seed).
    //
    // "disable_game_music is presentation-only (never sim), so netplay just
    // keeps music on" — that stood here and was wrong twice over. Being
    // presentation-only is exactly why the option can be honoured online: it
    // needs no agreement with the peer, it moves no hash, and each player's
    // own options.ini is the only thing that should decide whether their
    // machine plays music. The original agrees — the guard at 0x410E88 is a
    // bare test of dword_4648C0 with no network arm (docs/re/sound-engine.md
    // §9), and the ONE place the round-music path does consult sub_40C06A is
    // inside sub_4293E5, choosing GENERIC over the per-level track, never
    // whether music plays at all. So this shares round_music_id() with the
    // local path rather than keeping a second, divergent copy.
    if (ctx.assets.load_stage(stage)) ctx.seqs.resolve_stage(ctx.assets, stage);
    // Not gated on the art loading, for the same reason as the local path.
    const int track = round_music_id(stage, state_->options.disable_game_music,
                                     [&ctx](int id) { return ctx.audio.has_track(id); });
    if (track == kRoundMusicSilent)
        ctx.audio.stop_music();
    else
        ctx.audio.start_music(track);
    // Untimed HUD from the AGREED config, not from this peer's own
    // options.ini. Play Time travels as part of the host's Tuning, so the
    // CLOCK was already the host's — but this flag was hardcoded false, so
    // a host playing "Infinite" left both peers watching the 99999 s
    // stand-in count down (~27 h) instead of hiding the clock. The mirror
    // case was worse: a guest whose own options.ini said Infinite hid the
    // clock on a match that really did end on time.
    state_->renderer.reset_match(is_unlimited_game_seconds(round_cfg_.tuning.game_seconds));
    ctx.sounds.reset();

    seed_presentation_roster();
}

void MatchLoop::seed_presentation_roster() {
    // PRESENTATION roster, derived from the AGREED config so both peers show
    // the same thing. It drives MatchRunner::collect_inputs (which only ever
    // reads a KEYBOARD/JOYSTICK slot) and the in-round player-row HUD; the
    // match roster itself is the config's own active/ai/team.
    //   * a LOCAL seat reads keyboard key-set 0 (the arrow keys), so the human
    //     at this machine plays with arrows regardless of which seat they own;
    //   * an AI slot is COMPUTER — collect_inputs leaves it neutral and the
    //     deterministic AISystem drives it identically on both peers;
    //   * every other active slot is type 4 OTHER, the original's own marker
    //     for "someone else's player" (docs/re/network-screens.md §7,
    //     sub_40D372). collect_inputs leaves it neutral too and the rollback
    //     session overwrites it from the wire before every tick().
    state_->setup_type.fill(static_cast<int>(SlotInputType::Off));
    state_->setup_sub.fill(0);
    state_->setup_team.fill(0);
    // Driven off the MASK, not a single seat index: today exactly one bit is
    // set (2-seat rooms), but reading the mask means a future peer that owns
    // two seats gets its second one mapped to key-set 1 with no change here.
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
        // MatchConfig::team[] is the +84 byte shifted up by one
        // (build_config's "sim team 0 = solo side" note); shift it back for
        // the presentation roster the HUD/outcome helpers read.
        state_->setup_team[slot] = round_cfg_.team[slot] != 0 ? round_cfg_.team[slot] - 1 : 0;
    }
    state_->team_play = std::any_of(round_cfg_.team.begin(), round_cfg_.team.end(),
                                    [](std::uint8_t t) { return t != 0; });
}

AppInput MatchLoop::drive_round(net::RollbackSession& session, NetLeave& left) {
    // Drive the SAME MatchRunner as a local match (reusing all its render /
    // present / pacing / round-end): the seam's net_session routes each fixed
    // tick through the ROLLBACK session. Rollback gives ZERO input delay (it
    // predicts the peer's input and re-simulates on a miss) so the match feels
    // local even over the wire — the input-delay lockstep this replaced added
    // a fixed ~200 ms of lag. max_prediction=8 ticks caps how far the display
    // may run ahead of the peer.
    MatchRunnerState mrs = seams_->match_runner_state();
    mrs.net_session = &session;
    mrs.net_local_seats = seats_.local;
    mrs.net_is_host = seats_.host;
    mrs.net_leave = &left;
    return MatchRunner(seams_->sctx(), mrs).run();
}

void MatchLoop::report_session_end() {
    // ON SCREEN as well as in the log: this used to be a stderr line on a
    // GUI build nobody sees, so from the player's side the match simply
    // stopped for no stated reason.
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

bool MatchLoop::stopped_after_round(net::RollbackSession& session, NetLeave left) {
    if (session.desynced()) {
        log_warn("netplay: DESYNC at tick %u — peers diverged (config/seed mismatch?)",
                 session.desync_tick());
        recorder_.latch(net::SessionEndReason::Desync);
        report_session_end();
        return true;
    }
    if (session.aborted()) {
        // Options row 12 off: a peer went silent and the match ends rather
        // than handing its seat to the AI. Say so — not a normal round end.
        log_warn(
            "netplay: a player dropped; match ended (turn on \"Lost net players "
            "revert to AIs\" to play on)");
        recorder_.latch(net::SessionEndReason::PeerDropped);
        recorder_.note("silence past the 600-pump timeout with Options row 12 off");
        report_session_end();
        return true;
    }
    if (result_ != AppInput::MatchOver) {
        // The window closed under the match. No modal — there is nothing left
        // to show it on — but the log line is exactly why this case is worth
        // latching: it is the one the player cannot report themselves.
        recorder_.latch(net::SessionEndReason::WindowClosed);
        return true;
    }
    // THE LOCAL PLAYER WALKED OUT — Ctrl+Q's faithful forfeit, or the
    // double-Esc bail-out from a match that stopped responding. Checked FIRST
    // and read from an explicit flag, not inferred: the old test for Ctrl+Q
    // ("more than one side alive and time left") sat BELOW the abandon branch,
    // so a bail-out pressed after an abandon had been agreed would have been
    // read as a draw and rotated into another round instead of leaving.
    //
    // Everything a bail-out needs happens by simply LEAVING THE ROUND LOOP, which
    // is what makes it unilateral: the loop's exit runs the RollbackSession
    // destructor, returns Advance with carry_->rematch still false, so
    // NetplayRunner's session loop returns, the enclosing present_net_* frame
    // drops its UdpTransport (whose destructor closes the socket), and run_app
    // maps NetHost/NetJoin+Advance back to the main menu. No confirmation, no
    // message, nothing waited on.
    if (left == NetLeave::None) {
        // (The Ctrl+Q forfeit used to be DEDUCED after this point, from
        // "MatchOver but more than one side is alive and the clock has time
        // left". It is now stated outright by the branch below, alongside the
        // double-Esc bail-out it shares a teardown with — see that branch for
        // why the deduction had to go rather than merely move.)
        return false;
    }
    const bool stalled = left == NetLeave::Stalled;
    result_ = AppInput::Advance;  // straight out to the menu
    recorder_.latch(stalled ? net::SessionEndReason::LeftStalled
                            : net::SessionEndReason::LeftSession);
    note_walkout(session, stalled);
    return true;
}

bool MatchLoop::settle_abandon(net::RollbackSession& session) {
    // The peer may still be short of the agreed tick, and only OUR input
    // window can get it there. advance() no longer simulates once
    // round_ended() — it just receives, re-announces and re-sends — so
    // this is a pure catch-up pump.
    if (!net_settle(kAbandonSettleMs, [&session] {
            session.advance(sim::TickInputs{}, static_cast<std::int64_t>(SDL_GetTicks()));
        })) {
        recorder_.latch(net::SessionEndReason::WindowClosed);
        return false;
    }
    recorder_.snapshot(session, round_);  // the catch-up pump moved the numbers
    // Latched, not final: the match usually carries on into another
    // round, and any later exit overwrites this. It matters for the case
    // where it does NOT — an Esc at the DRAW or scoreboard right after —
    // so the log says "somebody abandoned" rather than a bare "left".
    recorder_.latch(net::SessionEndReason::RoundAbandoned);
    log_info("netplay: round %d abandoned at tick %u — draw", round_,
             static_cast<unsigned>(session.end_round_tick()));
    return true;
}

void MatchLoop::present_clinch(int clinched) {
    // MATCH win — the same clinch tier the local path shows: the RESULTS
    // scoreboard carrying the "WINS THE MATCH!" line with the 2000 winner
    // voice under it, then VICTORY<n>/TEAM<n>. Both peers reach this
    // independently and identically (same sim, same tally), so the
    // OUTCOME needs no agreement.
    //
    // WHAT DOES need agreeing is what happens NEXT. This used to be the
    // end of the road: the loop broke, the caller returned, and the
    // transport the peers had punched a path for was destroyed — so two
    // people who had just finished a game and wanted another one were
    // back at the lobby. Instead both peers now walk back to the SETUP
    // screens over the SAME link (NetplayRunner's session loop), and the
    // RematchGate below is the door: it keeps the host's liveness flowing
    // under these two screens and makes the exit from VICTORY the host's
    // call, so the guest's next SetupSession is never built into silence.
    const ScreenContext ctx = seams_->sctx();
    net::RematchSession rematch_session(*transport_, seats_.host);
    RematchGate gate(rematch_session);
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
    // Advance means the gate opened (the host walked back to setup and
    // said so); Back means this player pressed Escape and is done. A
    // failed gate is the peer having vanished — also done, quietly: the
    // match itself is complete either way.
    if (carry_ != nullptr && result_ == AppInput::Advance && gate.ready()) carry_->rematch = true;
    result_ = AppInput::Advance;
    recorder_.latch(net::SessionEndReason::MatchCompleted);
}

bool MatchLoop::present_draw() {
    // DRAW is a PREFIX to the tally, not an alternative (raw
    // 0x42A875-0x42A88B falls through into RESULTS) — so a drawn round
    // shows DRAW.PCX first here too. It is deliberately NOT gated: no
    // ticks run under either screen, and the tally behind it IS the
    // synchronisation point, so the two peers dismissing DRAW at
    // different moments costs nothing but each waiting on the tally
    // instead. (The original instead broadcasts a second advance for
    // this screen — kind 32 payload 904 — which our wire has no need of
    // once the config exchange is the barrier.)
    const ScreenContext ctx = seams_->sctx();
    ctx.audio.play_sting(kDrawStingLo, kDrawStingHi);
    ScreenDef ds = draw_screen();
    // ADVANCING IS THE HOST'S. The host waits for its own Enter (dwell 0);
    // a GUEST is never asked to press anything and simply auto-advances on
    // DRAW's own 6 s dwell — the original's `sub_42A3F6` auto-advance — into
    // the scoreboard, where it already waits for the host's next-round
    // confirmation. Requiring Enter on BOTH machines here is what the owner
    // hit: two people staring at DRAW.PCX, each waiting for the other.
    //
    // Deliberately NOT gated on the rotation gate: that gate's ready() is
    // "the next round is agreed", and consuming it here would flash the
    // scoreboard past before either player could read it.
    if (seats_.host) ds.dwell_ms = 0;
    result_ = present_asset_screen(ctx, ds);
    if (result_ != AppInput::Advance) {
        recorder_.latch(result_ == AppInput::Quit ? net::SessionEndReason::WindowClosed
                                                  : net::SessionEndReason::LeftSession);
        if (result_ != AppInput::Quit) result_ = AppInput::Advance;  // Esc: abandon
        return false;
    }
    return true;
}

sim::MatchConfig MatchLoop::next_round_config(std::uint32_t next_seed) const {
    // A GUEST builds nothing: it takes the host's exact bytes off the wire, so
    // no future change to libs/match can make the two disagree.
    if (!seats_.host) return {};
    // The host builds it from the SAME screens' state that produced round 0, so
    // the roster and the level choice carry over; a RANDOM level rotates because
    // the seed moved.
    return MatchRunner(seams_->sctx(), seams_->match_runner_state()).build_config(next_seed);
}

MatchLoop::Rotation MatchLoop::no_next_round(const RoundRotationGate& gate) {
    // Escape (either peer abandoning the match) or the link died under
    // the screen — either way there is no agreed next round.
    if (!gate.failed()) {
        recorder_.latch(net::SessionEndReason::LeftSession);
        recorder_.note("left at the between-rounds scoreboard");
        result_ = AppInput::Advance;
        return Rotation::EndMatch;
    }
    log_warn("netplay: lost the peer between rounds; match ended");
    recorder_.latch(net::SessionEndReason::PeerLostBetweenRounds);
    // The screen the player was looking at gave no hint of this: the
    // scoreboard simply stopped accepting Enter. Say it out loud.
    if (present_net_session_end(seams_->sctx(), recorder_.summary()) == AppInput::Quit)
        return Rotation::WindowClosed;
    result_ = AppInput::Advance;
    return Rotation::EndMatch;
}

MatchLoop::Rotation MatchLoop::rotate(int winner, const net::RollbackSession& session) {
    // NOT DECIDED — another round. The host builds it and confirms it through
    // the gate; the guest takes the host's exact bytes.
    const ScreenContext ctx = seams_->sctx();
    net::SetupSession rotate_session(*transport_, seats_.host, seats_.local, seats_.remote());
    const std::uint32_t next_seed = net::round_seed(match_seed_, round_ + 1);
    RoundRotationGate gate(rotate_session, seats_.host, next_round_config(next_seed), next_seed);

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

AppInput run_netplay_match(const NetplaySeams& seams, NetplayState& state,
                           net::Transport& transport, NetSeats seats, const sim::MatchConfig& cfg,
                           NetSessionCarry* carry) {
    return MatchLoop(seams, state, transport, seats, cfg, carry).run();
}

}  // namespace bomber::game
