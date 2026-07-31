#include "bomber/net/time_sync.hpp"

#include <algorithm>

namespace bomber::net {

void TimeSyncController::note_peer_range(std::uint16_t seats, std::uint32_t first_tick, int depth) {
    for (int s = 0; s < sim::kMaxPlayers; ++s) {
        if ((seats & static_cast<std::uint16_t>(1U << s)) == 0) continue;
        const std::size_t si = static_cast<std::size_t>(s);
        if (peer_frontier_any_[si] && first_tick < peer_frontier_[si]) continue;
        peer_frontier_[si] = first_tick;
        peer_frontier_any_[si] = true;
        peer_depth_[si] = depth;
    }
    peer_heard_ = true;
}

int TimeSyncController::peer_lag(std::uint16_t awaited) const {
    int worst = 0;
    for (int s = 0; s < sim::kMaxPlayers; ++s) {
        if ((awaited & static_cast<std::uint16_t>(1U << s)) == 0) continue;
        const int d = peer_depth_[static_cast<std::size_t>(s)];
        if (d > worst) worst = d;
    }
    return worst;
}

void TimeSyncController::note_advantage(bool eligible, int raw, int lag) {
    if (!eligible) {
        // Throw the window away rather than pad it (see the header). Both readings
        // are stale by however long the hub was gone, and the step back to live
        // values would read as arrival variance — buying a lead for a peer whose
        // actual problem is that it is behind and needs to catch up.
        advantage_count_ = 0;
        advantage_next_ = 0;
        return;
    }
    advantage_[advantage_next_] = raw;
    peer_window_[advantage_next_] = lag;
    advantage_next_ = (advantage_next_ + 1) % advantage_.size();
    if (advantage_count_ < static_cast<int>(advantage_.size())) ++advantage_count_;
}

int TimeSyncController::sustained_advantage() const {
    // Not enough history to tell a skew from a burst yet, so claim no advantage
    // rather than guess at one. Costs the first second of a round, where the two
    // peers have not settled into a phase relationship worth correcting anyway.
    if (advantage_count_ < static_cast<int>(advantage_.size())) return 0;
    return *std::min_element(advantage_.begin(), advantage_.end());
}

int TimeSyncController::peer_depth_spread() const {
    // ARRIVAL VARIANCE AS THE PEER EXPERIENCES IT, for free, off the wire. The
    // LEVEL of the peer's prediction depth is the path's delay and is none of our
    // business — a slow link is still a perfectly playable one. Its SPREAD is the
    // part a lead can remove, and a steady path has none however slow it is.
    if (advantage_count_ < static_cast<int>(peer_window_.size())) return 0;
    const auto [lo, hi] = std::minmax_element(peer_window_.begin(), peer_window_.end());
    return *hi - *lo;
}

TimeSyncController::Decision TimeSyncController::pump(bool eligible, int local_depth,
                                                      std::uint16_t awaited) {
    // ONE reading of the peers' lag for the whole pump: the raw advantage below
    // and the window sample beside it are the same measurement asked for twice,
    // and taking it twice could only ever let them disagree.
    const int lag = peer_lag(awaited);
    Decision d;
    d.raw = eligible ? local_depth - lag : 0;
    note_advantage(eligible, d.raw, lag);
    d.sustained = eligible ? sustained_advantage() : 0;
    d.spread = peer_depth_spread();
    // The decision is the window's MINIMUM, or — for an advantage larger than the
    // measured variance can explain — the current sample on its own, so that a
    // frozen peer's several ticks of skew are not left to spend the whole
    // prediction budget while the window fills. Both arms imply
    // `raw >= kRephaseAdvantageTicks`, which is what keeps this a subset of the
    // unfiltered controller's holds. See the header's jitter note for why each
    // half is shaped the way it is, and what was measured with and without them.
    const bool raw_asks = eligible && d.raw >= kRephaseAdvantageTicks;
    const bool sustained_asks = eligible && (d.sustained >= kRephaseAdvantageTicks ||
                                             d.raw >= d.spread + kRephaseAdvantageTicks);
    d.suppressed = !rephase_held_ && raw_asks && !sustained_asks;
    // Alternate pumps, never two in a row: a skew is shed at half rate rather
    // than by freezing the display outright.
    d.hold = !rephase_held_ && sustained_asks;
    rephase_held_ = d.hold;
    return d;
}

int TimeSyncController::lead_target(bool eligible) const {
    if (!eligible) return 0;  // no peer to help, or the round is ending
    const int want = peer_depth_spread() - kLeadDeadbandTicks;
    return std::clamp(want, 0, kMaxLocalLeadTicks);
}

void TimeSyncController::update_lead(bool eligible) {
    const int target = lead_target(eligible);
    if (target > lead_)
        ++lead_;
    else if (target < lead_)
        --lead_;
}

}  // namespace bomber::net
