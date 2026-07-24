#pragma once

#include <cstdint>
#include <unordered_map>

#include "bomber/net/transport.hpp"
#include "bomber/sim/simulation.hpp"

// Deterministic INPUT-DELAY lockstep over an abstract Transport (ADR-0010).
// Owns a Simulation and drives it one CONFIRMED tick at a time, keeping two
// peers bit-identical by:
//   (a) exchanging per-tick inputs with a fixed input delay, so a packet has
//       `input_delay` ticks to reach the peer before its tick is needed
//       (latency hiding — GGPO's baseline scheme), and
//   (b) exchanging Simulation::hash() every tick, so any divergence is caught
//       LOUDLY and immediately instead of silently corrected (unlike the 1997
//       host-authoritative model, docs/re/multiplayer.md §1.5).
//
// A confirmed tick T is simulated only once every seat's input for T is known —
// the local seats from the delay buffer, the remote seats off the wire. Until
// then advance() STALLS (returns false) rather than predicting; there is no
// rollback in this increment (that is §3.3 step 4). Ticks [0, input_delay) run
// on neutral input for every seat, the standard input-delay warm-up.
//
// AI seats need no network input: the sim's AISystem drives them
// deterministically from the (identical) State on both peers, so `all_seats`
// here names only the HUMAN seats that actually exchange input.

namespace bomber::net {

class LockstepSession {
public:
    // `sim` is BORROWED (not owned) and must outlive the session — the caller
    // seeds it from an identical MatchConfig on both peers (ADR-0010: seed/roster
    // parity) and, in the game, is the same Simulation the renderer draws, so a
    // netplay match needs no separate copy. `local_seats` is the bitmask of human
    // seats THIS peer owns; `all_seats` every human seat in the match (so remote
    // = all_seats & ~local_seats). `input_delay` ticks of delay — pick it >= the
    // expected one-way latency in ticks to avoid stalls.
    LockstepSession(sim::Simulation& sim, std::uint16_t local_seats, std::uint16_t all_seats,
                    int input_delay, Transport& transport);

    // Provide the local player's input for the next un-produced input frame
    // (applied `input_delay` ticks ahead) and try to advance ONE confirmed tick.
    // Returns true if a tick was simulated, false if stalled on missing remote
    // input. During a stall the passed input is dropped (pure lockstep — a brief
    // network hitch drops local input; rollback would instead re-predict).
    bool advance(const sim::TickInputs& local_input);

    std::uint32_t confirmed_tick() const { return tick_; }  // next tick to simulate
    // The next input frame advance() will produce local input for. A caller that
    // needs its local input to line up deterministically with a tick (tests, or
    // a replay recorder) can key on this; a real game just passes "input now".
    std::uint32_t input_tick() const { return input_tick_; }
    const sim::Simulation& sim() const { return *sim_; }
    std::uint64_t hash() const { return sim_->hash(); }

    // Latched true the first time a peer's reported hash disagreed with ours.
    bool desynced() const { return desynced_; }
    std::uint32_t desync_tick() const { return desync_tick_; }

private:
    void receive();  // drain the transport: store remote inputs + check peer hashes
    void fill_seats(std::uint32_t tick, std::uint16_t seats, const sim::TickInputs& in);
    void note_peer_hash(std::uint32_t tick, std::uint64_t peer_hash);
    void prune();

    sim::Simulation* sim_;  // BORROWED — the caller owns and seeds it
    Transport* transport_;
    std::uint16_t local_seats_;
    std::uint16_t all_seats_;
    int delay_;
    std::uint32_t tick_ = 0;        // next confirmed tick to simulate
    std::uint32_t input_tick_ = 0;  // next tick to produce+send local input for

    std::unordered_map<std::uint32_t, sim::TickInputs> inputs_;  // accumulating per tick
    std::unordered_map<std::uint32_t, std::uint16_t> have_;      // seats filled per tick
    std::unordered_map<std::uint32_t, std::uint64_t> hash_;      // our hash per simulated tick
    std::unordered_map<std::uint32_t, std::uint64_t> peer_hash_;  // peer hashes awaiting our tick
    bool desynced_ = false;
    std::uint32_t desync_tick_ = 0;
};

}  // namespace bomber::net
