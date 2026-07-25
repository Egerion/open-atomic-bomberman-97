#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

#include "bomber/net/transport.hpp"
#include "bomber/sim/constants.hpp"  // kMaxPlayers
#include "bomber/sim/simulation.hpp"
#include "bomber/sim/state.hpp"

// GGPO-style ROLLBACK netcode over an abstract Transport (ADR-0010 §3.3 step 4).
// Where LockstepSession STALLS until every seat's input for a tick has arrived,
// RollbackSession never stalls the display: it PREDICTS an absent remote input
// (repeat the peer's last known input), simulates ahead speculatively, and when
// the real input arrives and disagrees, RESTORES a snapshot and RE-SIMULATES the
// affected ticks with the corrected input. This hides latency — the local player
// sees no input delay — at the cost of an occasional visible correction.
//
// The port has exactly the two things rollback usually makes hard, for free:
//   * cheap full-state snapshots — `State` is a plain value type, so save is
//     `State s = sim.state()` and restore is `sim.state() = s`; and
//   * a fast pure re-sim — `Simulation::tick(inputs)` is a deterministic pure
//     function of (State, inputs).
// `state_hash` still guards correctness: CONFIRMED ticks (no prediction) are
// exchanged and compared, so a genuine divergence is caught loudly even though
// speculative frames legitimately differ between peers moment to moment.
//
// This is a SEPARATE class from LockstepSession on purpose: input-delay lockstep
// is the simpler, already-shipped strategy; rollback is the low-latency upgrade,
// selectable per match without disturbing the other. Both share the wire codec
// (protocol.hpp) and the Transport seam.
//
// PEER DROP -> AI HANDOFF (ADR-0011 Risks, "Dropped/late peers"). Prediction
// covers a briefly late peer and the `max_prediction` cap covers a very late
// one, but a peer that never comes back would stall everyone forever. Past a
// hard silence timeout the host announces MsgType::Drop and every peer, at the
// SAME tick, sets `Player::ai` for that seat: the deterministic AISystem then
// derives its input from the shared `State`, identically everywhere, and the
// confirmed-hash exchange keeps agreeing. See DropPolicy below (gated on the
// RE'd Options row 12) and the `handoff_` schedule at the bottom of the class
// (the rollback-safe part).

namespace bomber::net {

// What the session does when a remote seat goes SILENT for a hard window
// (ADR-0011 Risks, "Dropped/late peers"). Within `max_prediction` the seat is
// merely predicted; past the cap the session stalls; past this timeout it is
// declared DROPPED and one of two things happens, decided by the RE'd Options
// row 12 "Lost net players revert to AI" (`dword_464928`,
// docs/re/results-and-options.md §row 12) that `revert_to_ai` carries:
//
//   ON  — the host broadcasts MsgType::Drop and EVERY peer hands the seat to
//         the deterministic AISystem at the same tick; the match plays on.
//   OFF — the drop is a match-ending condition: `aborted()` latches, advance()
//         becomes a no-op, and the caller ends the match. Never a hang.
//
// The option lives in CFG.INI (`assets::Options::lost_net_revert_ai`) and
// on the Options screen; libs/net cannot see either (it depends on bomber::sim
// only), so the CALLER copies it in here. Detection is local to every peer, but
// SCHEDULING is host-only: a guest must never mutate the hashed `State` on its
// own authority, it only obeys the host's Drop message.
struct DropPolicy {
    bool revert_to_ai = false;  // Options row 12; see above
    bool is_host = false;       // only the hub/host schedules + broadcasts a handoff
    // Pumps of TOTAL SILENCE from a seat before it is declared dropped. 0 (the
    // default) disables drop detection entirely, so existing callers and every
    // pre-existing scenario behave exactly as before. At 20 Hz a pump is 50 ms.
    //
    // Size this GENEROUSLY. The counter resets the moment any input from the
    // seat arrives, so a peer that comes back inside the window costs nothing —
    // but crossing it is IRREVERSIBLE with Options row 12 off (`aborted_`
    // latches and the match is over). An early value of 50 (2.5 s) killed
    // matches whenever a player merely dragged their window: Windows blocks the
    // message pump for the whole drag, the peer sees silence, and the match died
    // with no way back. Alt-tab, a stalled disk, a laptop sleeping for a moment
    // and ordinary network hiccups all blow past a few seconds too, so the
    // threshold must mean "genuinely gone", not "briefly busy".
    int timeout_ticks = 0;
};

class RollbackSession {
public:
    // `sim` is BORROWED and seeded identically on both peers (ADR-0010 parity).
    // `local_seats` / `all_seats` as in LockstepSession (AI seats are simulated,
    // not exchanged). `max_prediction` bounds how many un-confirmed ticks the
    // display may run ahead before it must wait (a safety cap; typical GGPO
    // values are ~8): past it advance() stalls rather than predict unboundedly.
    // `drop` defaults to "detection off" — additive, so nothing changes for a
    // caller that does not opt in.
    RollbackSession(sim::Simulation& sim, std::uint16_t local_seats, std::uint16_t all_seats,
                    int max_prediction, Transport& transport, const DropPolicy& drop = {});

    // Feed the local player's input for the next frame and advance the DISPLAYED
    // (speculative) state by one tick, unless the prediction cap is reached and
    // the peer has not caught up (then it holds). Applies any pending rollback
    // first, so on return sim() reflects the best current estimate. Never blocks.
    void advance(const sim::TickInputs& local_input);

    std::uint32_t predicted_tick() const { return tick_; }   // next tick to simulate speculatively
    std::uint32_t confirmed_tick() const { return confirmed_; }  // highest all-inputs-known tick
    const sim::Simulation& sim() const { return *sim_; }
    std::uint64_t hash() const { return sim_->hash(); }

    bool desynced() const { return desynced_; }
    std::uint32_t desync_tick() const { return desync_tick_; }

    // Seats declared dropped so far (0 while everyone is live). With
    // DropPolicy::revert_to_ai these are the seats now driven by the AI; without
    // it, the seats whose loss ended the match.
    std::uint16_t dropped_seats() const { return dropped_; }
    // The agreed handoff tick for a dropped seat, or kNoHandoff if that seat is
    // still a live network peer. Every peer holds the same value.
    std::uint32_t handoff_tick(int seat) const {
        return handoff_[static_cast<std::size_t>(seat)];
    }
    // LOUD match-ending state: a peer dropped while Options row 12 was OFF, so
    // there is no legal way to keep simulating. advance() is a no-op from here
    // on — the caller polls this and ends the match (it never hangs).
    bool aborted() const { return aborted_; }

    static constexpr std::uint32_t kNoHandoff = 0xFFFFFFFFU;

private:
    struct Slot {
        sim::TickInputs inputs;         // seat inputs used to simulate this tick (confirmed or predicted)
        std::uint16_t confirmed = 0;    // which seats are CONFIRMED (rest are predicted)
    };

    void receive();  // drain transport -> input slots + peer hashes, flag rollbacks
    void apply_remote(std::uint32_t tick, std::uint16_t seats, const sim::TickInputs& in);
    sim::TickInputs assemble(std::uint32_t tick);   // confirmed seats + predicted (last-known) remote seats
    void resimulate(std::uint32_t from);            // restore snapshot[from], replay to tick_
    void advance_confirmed();                       // raise confirmed_ over the contiguous all-known prefix
    void send_local(std::uint32_t from);            // redundant range of recent local inputs [from, tick_)
    void note_peer_hash(std::uint32_t tick, std::uint64_t peer_hash);
    void prune();

    // Seats whose input is still EXCHANGED at `tick`: all_seats_ minus every
    // seat already handed to the AI by then. The single tick-keyed predicate
    // behind both "what does the sim get fed" (assemble) and "whose input must
    // still arrive" (advance_confirmed / apply_remote / drop detection).
    std::uint16_t seats_awaited(std::uint32_t tick) const;
    // Re-assert the schedule on the hashed State at the START of simulating
    // `tick`. IDEMPOTENT (`ai = true` twice is `ai = true`), and called on the
    // first pass AND on every re-simulation of that tick — that is what makes
    // the handoff survive a rollback to a snapshot taken before it.
    void apply_handoffs(std::uint32_t tick);
    void heard(std::uint16_t seats);  // a packet carrying these seats arrived: they are alive
    void schedule_handoff(int seat, std::uint32_t at_tick);
    void detect_drops();        // hard-timeout scan; host schedules, everyone can abort
    void broadcast_handoffs();  // host-side redundant re-send, like send_local's

    sim::Simulation* sim_;  // BORROWED
    Transport* transport_;
    std::uint16_t local_seats_;
    std::uint16_t all_seats_;
    std::uint16_t remote_seats_;
    int max_prediction_;
    DropPolicy drop_;

    std::uint32_t tick_ = 0;        // next speculative tick to simulate
    std::uint32_t confirmed_ = 0;   // highest tick whose inputs are ALL confirmed (+1 = next unconfirmed)
    std::uint32_t rollback_to_ = 0;
    bool rollback_pending_ = false;

    std::unordered_map<std::uint32_t, Slot> slots_;            // per-tick inputs (confirmed/predicted)
    std::unordered_map<std::uint32_t, sim::State> snapshots_;  // State BEFORE simulating that tick
    // Prediction source: the most recent CONFIRMED input for each seat, and the
    // tick it came from (so a later confirmation wins). Predicting a remote
    // seat's next input = "repeat its last confirmed input".
    sim::TickInputs last_remote_input_;  // neutral until a seat is first confirmed
    std::array<std::uint32_t, sim::kMaxPlayers> last_remote_tick_{};
    std::unordered_map<std::uint32_t, std::uint64_t> hash_;  // our hash after each CONFIRMED tick
    std::unordered_map<std::uint32_t, std::uint64_t> peer_hash_;

    // The peer-drop SCHEDULE — the durable, tick-keyed record that survives
    // rollback. Deliberately NOT a one-shot mutation of State: `Player::ai` is
    // hashed, so restoring a pre-handoff snapshot would silently erase a flag
    // set once. Kept here (outside State, like the input slots) and re-applied
    // by apply_handoffs() at the head of EVERY simulation of a tick >= at_tick,
    // first pass or re-sim alike, so the flag is a pure function of the tick.
    std::array<std::uint32_t, sim::kMaxPlayers> handoff_;  // kNoHandoff = live peer
    // Pumps since anything at all was heard carrying that seat. LIVENESS, not
    // input progress: a peer stalled behind a THIRD peer's silence still sends
    // its redundant window every pump, and must not be accused of dropping.
    std::array<int, sim::kMaxPlayers> silence_{};
    // First tick for which we hold NO input from a seat (0 = never heard from
    // it). The host uses it as the handoff tick — see DropFrame on why that is
    // retroactive rather than in the future.
    std::array<std::uint32_t, sim::kMaxPlayers> remote_next_{};
    std::uint16_t dropped_ = 0;
    bool aborted_ = false;

    bool desynced_ = false;
    std::uint32_t desync_tick_ = 0;
};

}  // namespace bomber::net
