#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "bomber/sim/constants.hpp"  // kMaxPlayers

// The PACING half of rollback netcode: should this pump be HELD, and how far
// ahead of its own head should this peer FILE its input. Both decide WHEN this
// peer simulates, never WHAT — so nothing here touches hashed state, and no
// change here can move a golden hash or `build_hash`.
//
// Reads no clock and holds no I/O: the caller supplies both samples, so the
// decision is a pure function pinned by tests/net/test_time_sync.cpp.
//
// The measurements behind every constant and every rule below — the
// Turkey<->Lithuania jitter sessions, the re-phase controller, the local input
// lead, and the alternatives that were rejected — are in docs/net-rollback.md §1.

namespace bomber::net {

// Pumps of frame-advantage history the re-phase controller must see an advantage
// hold across before acting on it. One second at 20 Hz.
inline constexpr int kRephaseWindowPumps = 20;

// The most local input lead the arrival-variance absorber will ever take. Two
// ticks is 100 ms of variance absorbed and 100 ms of input lag paid for it. THE
// KNOB: the only value here the player can feel, so it is meant to be judged in
// a live match (docs/net-rollback.md §1.6).
inline constexpr int kMaxLocalLeadTicks = 2;

// Spread in the peer's own prediction depth treated as ordinary rather than as
// variance worth spending input lag on.
inline constexpr int kLeadDeadbandTicks = 2;

// Clock skew (in ticks) tolerated before this peer starts giving it back. Our
// lag minus the peer's is TWICE the skew, so this engages at a full tick ahead.
inline constexpr int kRephaseAdvantageTicks = 2;

class TimeSyncController {
public:
    // What one pump's comparison came to. Everything is a READING except `hold`,
    // which is the one thing the session must act on.
    struct Decision {
        int raw = 0;        // this pump's instantaneous frame advantage
        int sustained = 0;  // the part of it that held for the whole window
        int spread = 0;     // the peer's own depth spread — the variance yardstick
        bool hold = false;  // give a tick back: do not simulate this pump
        bool suppressed = false;  // the raw reading wanted a hold and the filter kept it
    };

    // One arriving InputRange, whose LENGTH is the sender's own prediction depth.
    // `first_tick` is its confirmed frontier, which `peer_frontier_`'s staleness
    // guard needs.
    void note_peer_range(std::uint16_t seats, std::uint32_t first_tick, int depth);

    bool peer_heard() const { return peer_heard_; }  // anything to compare against yet?

    // The worst of the peers' OWN lags. `awaited` is the session's question: the
    // remote seats still exchanged at the current tick.
    int peer_lag(std::uint16_t awaited) const;

    // Ours minus theirs, which cancels the path delay and leaves twice the clock
    // skew. ONE instantaneous sample, and therefore full of arrival variance.
    int frame_advantage(int local_depth, std::uint16_t awaited) const {
        return local_depth - peer_lag(awaited);
    }

    // The part of that advantage which held for the WHOLE window. Never above the
    // current sample, so the filter can only ever hold LESS than the raw reading.
    int sustained_advantage() const;

    // The SPREAD of the peer's own prediction depth across the window — how much
    // arrival variance it is living with. Zero on any steady path however slow,
    // which is what keeps the lead off a clean link.
    int peer_depth_spread() const;

    // The whole per-pump decision. `local_depth` is how far this peer's filing
    // head leads its confirmed frontier; `eligible` is the session's precondition.
    //
    // MUST BE CALLED EVERY PUMP, including ones the caller is about to hold and
    // ones where nothing is eligible, or the windows below would span a variable
    // stretch of time rather than a fixed one.
    Decision pump(bool eligible, int local_depth, std::uint16_t awaited);

    // The local input lead currently in force, in ticks.
    int lead() const { return lead_; }

    // Move `lead_` at most one tick toward the target.
    //
    // Called ONLY on a pump that actually simulates — a pump that does not must
    // not consume a sample either, or the lead would grow by one for every
    // stalled pump. That is also why a diagnostic's lead can outlive the spread
    // that bought it: pump() runs on every pump and this does not.
    void update_lead(bool eligible);

private:
    // Push this pump's samples into the windows. An INELIGIBLE pump does not
    // contribute one, it DISCARDS the window: both situations that make a pump
    // ineligible leave the readings already taken describing a world that no
    // longer exists, and the step back to live values would register as arrival
    // variance and buy a lead nobody asked for.
    void note_advantage(bool eligible, int raw, int lag);
    int lead_target(bool eligible) const;  // what the spread justifies, deadbanded and capped

    // Each remote seat's OWN prediction depth, off the last InputRange it sent.
    // Not hashed, not sent, and part of no correctness decision.
    std::array<int, sim::kMaxPlayers> peer_depth_{};
    // The highest confirmed frontier each seat has reported. A sender's frontier
    // only ever RISES, so a datagram whose frontier has not is one that overtook
    // a newer one in flight — and the stale window it carries reads as the peer
    // having suddenly caught up, which is precisely the reading that makes this
    // peer decide it is ahead.
    std::array<std::uint32_t, sim::kMaxPlayers> peer_frontier_{};
    std::array<bool, sim::kMaxPlayers> peer_frontier_any_{};
    bool peer_heard_ = false;
    // Holds are spread over ALTERNATE pumps, so a skew is shed at half rate
    // rather than freezing the display outright.
    bool rephase_held_ = false;
    // A plain ring: the only query is a MINIMUM over the whole window, so which
    // slot is newest does not matter.
    std::array<int, kRephaseWindowPumps> advantage_{};
    std::size_t advantage_next_ = 0;
    int advantage_count_ = 0;  // below a full window, no decision
    // The peer's own depth over the same window. Shares the index and count
    // because both are sampled in the same place on the same pump.
    std::array<int, kRephaseWindowPumps> peer_window_{};
    int lead_ = 0;
};

}  // namespace bomber::net
