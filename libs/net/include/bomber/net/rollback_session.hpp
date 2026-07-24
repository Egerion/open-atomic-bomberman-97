#pragma once

#include <array>
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

namespace bomber::net {

class RollbackSession {
public:
    // `sim` is BORROWED and seeded identically on both peers (ADR-0010 parity).
    // `local_seats` / `all_seats` as in LockstepSession (AI seats are simulated,
    // not exchanged). `max_prediction` bounds how many un-confirmed ticks the
    // display may run ahead before it must wait (a safety cap; typical GGPO
    // values are ~8): past it advance() stalls rather than predict unboundedly.
    RollbackSession(sim::Simulation& sim, std::uint16_t local_seats, std::uint16_t all_seats,
                    int max_prediction, Transport& transport);

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

    sim::Simulation* sim_;  // BORROWED
    Transport* transport_;
    std::uint16_t local_seats_;
    std::uint16_t all_seats_;
    std::uint16_t remote_seats_;
    int max_prediction_;

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

    bool desynced_ = false;
    std::uint32_t desync_tick_ = 0;
};

}  // namespace bomber::net
