#include "bomber/net/net_stats.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>  // std::abs(int)

namespace bomber::net {

const char* path_name(NetPath p) {
    switch (p) {
        case NetPath::Direct: return "direct";
        case NetPath::Relayed: return "relayed";
        case NetPath::StarHub: return "starhub";
        case NetPath::Loopback: return "loopback";
        case NetPath::Unknown: break;
    }
    return "unknown";
}

const char* end_reason_name(SessionEndReason r) {
    switch (r) {
        case SessionEndReason::MatchCompleted: return "match-completed";
        case SessionEndReason::RoundAbandoned: return "round-abandoned";
        case SessionEndReason::Desync: return "desync";
        case SessionEndReason::PeerDropped: return "peer-dropped";
        case SessionEndReason::PeerLostBetweenRounds: return "peer-lost-between-rounds";
        case SessionEndReason::WindowClosed: return "window-closed";
        case SessionEndReason::LeftSession: return "left-session";
        case SessionEndReason::LeftStalled: return "left-stalled";
        case SessionEndReason::Unknown: break;
    }
    return "unknown";
}

void NetStatsTracker::begin(NetPath path, std::uint16_t remote_seats, std::uint32_t start_tick,
                            int max_prediction) {
    s_ = NetStats{};
    s_.path = path;
    s_.tick = start_tick;
    s_.confirmed = start_tick;
    s_.max_prediction = max_prediction;
    remote_seats_ = remote_seats;
    start_tick_ = start_tick;
    sent_at_.fill(-1);
    sent_tick_.fill(0);
    sent_any_ = false;
    peer_acked_.fill(0);
    peer_acked_any_.fill(false);
    rtt_prev_.fill(-1);
    bucket_worst_.fill(-1);
    win_recv_.fill(0);
    for (int s = 0; s < sim::kMaxPlayers; ++s) {
        const bool remote = (remote_seats & static_cast<std::uint16_t>(1U << s)) != 0;
        s_.peers[static_cast<std::size_t>(s)].tracked = remote;
    }
}

void NetStatsTracker::begin_pump(std::int64_t now_ms) {
    now_ms_ = now_ms;
    if (now_ms < 0) return;  // no clock: tick-derived stats only
    s_.clocked = true;
    if (begin_ms_ < 0) {
        begin_ms_ = now_ms;
        window_start_ms_ = now_ms;
        bucket_start_ms_ = now_ms;
    }
    s_.elapsed_ms = now_ms - begin_ms_;

    while (bucket_start_ms_ >= 0 && now_ms - bucket_start_ms_ >= kRttBucketMs) {
        close_rtt_bucket();
        bucket_start_ms_ += kRttBucketMs;
    }

    if (window_start_ms_ >= 0 && now_ms - window_start_ms_ >= kRateWindowMs) roll_windows();
}

void NetStatsTracker::close_rtt_bucket() {
    // A bucket with no sample in it is pushed as 0 — a GAP, not a carried-forward
    // level: "no acknowledgement arrived in that 100 ms" is exactly what we want
    // to be able to see, and holding the previous value would hide it.
    for (int s = 0; s < sim::kMaxPlayers; ++s) {
        const std::size_t si = static_cast<std::size_t>(s);
        PeerStats& p = s_.peers[si];
        if (!p.tracked) continue;
        const int worst = bucket_worst_[si];
        const auto value = static_cast<std::uint16_t>(worst < 0 ? 0 : std::min(worst, 0xFFFF));
        // A shifted fixed-size FIFO, not a ring: 48 uint16 moved ten times a
        // second is far below the noise floor of anything this measures, and it
        // keeps the drawer free of ring-index arithmetic.
        const bool full = (p.rtt_history_len == kRttHistory);
        if (full) std::copy(p.rtt_history.begin() + 1, p.rtt_history.end(), p.rtt_history.begin());
        if (!full) ++p.rtt_history_len;
        p.rtt_history[p.rtt_history_len - 1] = value;
        bucket_worst_[si] = -1;
        p.rtt_recent_max_ms = -1;
        for (std::size_t i = 0; i < p.rtt_history_len; ++i)
            if (p.rtt_history[i] != 0)
                p.rtt_recent_max_ms =
                    std::max(p.rtt_recent_max_ms, static_cast<int>(p.rtt_history[i]));
    }
}

void NetStatsTracker::roll_windows() {
    // A completed-window count scaled to one second — not a moving average, so
    // the number on screen is a fact about a known span.
    const std::int64_t span = std::max<std::int64_t>(1, now_ms_ - window_start_ms_);
    const auto per_sec = [span](int n) {
        return static_cast<int>((static_cast<std::int64_t>(n) * 1000 + span / 2) / span);
    };
    s_.rx_per_sec = per_sec(win_rx_);
    s_.rollbacks_per_sec = per_sec(win_rollbacks_);
    s_.resim_ticks_per_sec = per_sec(win_resim_);
    s_.stalls_per_sec = per_sec(win_stalls_);
    for (int s = 0; s < sim::kMaxPlayers; ++s) {
        const std::size_t si = static_cast<std::size_t>(s);
        PeerStats& p = s_.peers[si];
        if (!p.tracked) continue;
        p.recv_per_sec = per_sec(win_recv_[si]);
        // A seat handed to the AI legitimately stops sending; reporting that as
        // loss would point the finger at the network for a peer that is gone.
        p.loss_pct_est = p.live ? std::clamp(100 - p.recv_per_sec * 100 / kPumpHz, 0, 100) : 0;
        win_recv_[si] = 0;
    }
    win_rx_ = win_rollbacks_ = win_resim_ = win_stalls_ = 0;
    window_start_ms_ = now_ms_;
}

void NetStatsTracker::on_local_tick(std::uint32_t tick) {
    if (now_ms_ < 0) return;
    const std::size_t i = tick % kSendRing;
    sent_at_[i] = now_ms_;
    sent_tick_[i] = tick;
    sent_any_ = true;
}

void NetStatsTracker::on_datagram(bool decoded) {
    ++s_.rx_packets;
    ++win_rx_;
    if (!decoded) ++s_.rx_malformed;
}

void NetStatsTracker::on_input_from(int seat, std::uint32_t first_tick) {
    if (seat < 0 || seat >= sim::kMaxPlayers) return;
    const std::size_t si = static_cast<std::size_t>(seat);
    PeerStats& p = s_.peers[si];
    ++p.input_packets;
    ++win_recv_[si];
    // 0 = "this datagram carries no confirmed frontier" (a single-tick Input
    // frame). It still counts as traffic, but treating its tick as an
    // acknowledgement would fabricate a round trip that was never proven.
    if (first_tick == 0) return;

    // THE ACK-RTT. An InputRange always starts at the sender's confirmed
    // frontier, and a peer cannot confirm tick T without our input for T. So a
    // frontier that has RISEN to `first_tick` proves our input for first_tick-1
    // completed a round trip. Sampling only on a RISE is essential: the peer
    // re-sends its whole unconfirmed window every pump, so measuring every
    // datagram at an unchanged frontier would report a round trip that grows
    // without bound.
    const bool rose = !peer_acked_any_[si] || first_tick > peer_acked_[si];
    if (!rose) return;
    peer_acked_any_[si] = true;
    peer_acked_[si] = first_tick;
    if (now_ms_ < 0 || !sent_any_ || first_tick == 0 || first_tick <= start_tick_) return;

    const std::uint32_t acked = first_tick - 1;
    const std::size_t i = acked % kSendRing;
    if (sent_at_[i] < 0 || sent_tick_[i] != acked) return;  // wrapped or never sent: no sample
    const std::int64_t sample = now_ms_ - sent_at_[i];
    if (sample < 0 || sample > kRttSaneMaxMs) return;
    push_rtt(seat, static_cast<int>(sample));
}

void NetStatsTracker::push_rtt(int seat, int sample) {
    const std::size_t si = static_cast<std::size_t>(seat);
    PeerStats& p = s_.peers[si];
    if (rtt_prev_[si] >= 0) {
        const int delta = std::abs(sample - rtt_prev_[si]);
        // EWMA at 1/4 weight, integer: enough smoothing to be readable, cheap
        // enough to be free, and it reacts within a few samples.
        p.jitter_ms = (p.jitter_ms * 3 + delta) / 4;
    }
    rtt_prev_[si] = sample;
    p.rtt_ms = sample;
    p.rtt_smooth_ms = (p.rtt_smooth_ms < 0) ? sample : (p.rtt_smooth_ms * 3 + sample) / 4;
    p.rtt_min_ms = (p.rtt_min_ms < 0) ? sample : std::min(p.rtt_min_ms, sample);
    p.rtt_max_ms = std::max(p.rtt_max_ms, sample);
    bucket_worst_[si] = std::max(bucket_worst_[si], sample);
    p.rtt_recent_max_ms = std::max(p.rtt_recent_max_ms, sample);
}

void NetStatsTracker::on_dup_input(int seat) {
    if (seat < 0 || seat >= sim::kMaxPlayers) return;
    ++s_.peers[static_cast<std::size_t>(seat)].dup_inputs;
}

void NetStatsTracker::on_rollback(std::uint32_t from, std::uint32_t head) {
    ++s_.rollbacks;
    ++win_rollbacks_;
    const std::uint32_t depth = (head > from) ? head - from : 0;
    s_.resim_ticks += depth;
    win_resim_ += static_cast<int>(depth);
}

void NetStatsTracker::on_stall() {
    ++s_.stall_pumps;
    ++win_stalls_;
}

void NetStatsTracker::on_rephase_hold() { ++s_.rephase_holds; }

void NetStatsTracker::on_timing(const TimingSample& t) {
    s_.frame_advantage = t.raw_advantage;
    s_.sustained_advantage = t.sustained;
    s_.local_lead = t.lead;
    s_.peer_depth_spread = t.spread;
    if (t.lead > 0) ++s_.lead_pumps;
    if (t.suppressed) ++s_.rephase_suppressed;
}

void NetStatsTracker::update_peer(int seat, std::uint32_t tick, std::uint32_t next, bool live) {
    PeerStats& p = s_.peers[static_cast<std::size_t>(seat)];
    p.live = live;
    p.lag_ticks = static_cast<int>(tick > next ? tick - next : 0);
    if (p.live) p.worst_lag_ticks = std::max(p.worst_lag_ticks, p.lag_ticks);
    // Is the ack-RTT measuring the wire, or this peer's tick offset from us? A
    // healthy reading sits near 2x the lag and a contaminated one near 1x, so the
    // cut at 1.5x has a factor of two of margin on both sides; a local lead
    // breaks the premise outright and is declared rather than detected. The two
    // models this falls out of are in docs/net-rollback.md §4.2.
    const int lag_ms = p.lag_ticks * (1000 / kPumpHz);
    p.rtt_offset_bound =
        p.rtt_ms >= 0 && (s_.local_lead > 0 || (p.lag_ticks >= 2 && p.rtt_ms * 2 < lag_ms * 3));
}

void NetStatsTracker::end_pump(const PumpSnapshot& snap) {
    s_.tick = snap.tick;
    s_.confirmed = snap.confirmed;
    s_.prediction_depth =
        static_cast<int>(snap.tick > snap.confirmed ? snap.tick - snap.confirmed : 0);
    s_.worst_prediction_depth = std::max(s_.worst_prediction_depth, s_.prediction_depth);
    s_.dropped_seats = snap.dropped;
    s_.desynced = snap.desynced;
    s_.desync_tick = snap.desync_tick;
    s_.aborted = snap.aborted;
    for (int s = 0; s < sim::kMaxPlayers; ++s) {
        const std::size_t si = static_cast<std::size_t>(s);
        if (!s_.peers[si].tracked) continue;
        const bool live = (snap.dropped & static_cast<std::uint16_t>(1U << s)) == 0;
        update_peer(s, snap.tick, snap.remote_next[si], live);
    }
}

namespace {

// snprintf into a stack buffer and append — no iostreams, and a bounded write
// whatever the values are. Session-end only, so the allocation in `out` is fine.
// NOT console output (coding-standards §11): this formats into the caller's
// string, which the caller then writes to netdiag.log or draws on the overlay.
template <typename... Args>
void appendf(std::string& out, const char* fmt, Args... args) {
    std::array<char, 256> buf{};
    const int n = std::snprintf(buf.data(), buf.size(), fmt, args...);
    if (n > 0)
        out.append(buf.data(),
                   static_cast<std::size_t>(std::min(n, static_cast<int>(buf.size()) - 1)));
}

}  // namespace

std::string format_session_log_line(const SessionSummary& s) {
    const NetStats& n = s.stats;
    std::string out;
    out.reserve(512);
    out.append(s.timestamp.empty() ? "-" : s.timestamp);
    appendf(out, " netdiag end=%s path=%s host=%d round=%d local_seats=0x%03X all_seats=0x%03X",
            end_reason_name(s.reason), path_name(n.path), s.is_host ? 1 : 0, s.round,
            static_cast<unsigned>(s.local_seats), static_cast<unsigned>(s.all_seats));
    appendf(out, " elapsed=%llds tick=%u confirmed=%u depth=%d/%d depth_max=%d",
            static_cast<long long>(n.elapsed_ms / 1000), static_cast<unsigned>(n.tick),
            static_cast<unsigned>(n.confirmed), n.prediction_depth, n.max_prediction,
            n.worst_prediction_depth);
    appendf(out, " stalls=%u rephase=%u absorbed=%u adv=%d/%d lead=%dt(%u) spread=%dt",
            static_cast<unsigned>(n.stall_pumps), static_cast<unsigned>(n.rephase_holds),
            static_cast<unsigned>(n.rephase_suppressed), n.frame_advantage, n.sustained_advantage,
            n.local_lead, static_cast<unsigned>(n.lead_pumps), n.peer_depth_spread);
    appendf(out, " rollbacks=%u resim_ticks=%u rx=%u bad=%u",
            static_cast<unsigned>(n.rollbacks), static_cast<unsigned>(n.resim_ticks),
            static_cast<unsigned>(n.rx_packets), static_cast<unsigned>(n.rx_malformed));
    if (n.desynced) appendf(out, " desync_tick=%u", static_cast<unsigned>(n.desync_tick));
    if (n.dropped_seats != 0)
        appendf(out, " dropped_seats=0x%03X", static_cast<unsigned>(n.dropped_seats));
    if (!n.clocked) out.append(" clock=none");
    for (int i = 0; i < sim::kMaxPlayers; ++i) {
        const PeerStats& p = n.peers[static_cast<std::size_t>(i)];
        if (!p.tracked) continue;
        appendf(out, " | seat%d live=%d lag=%dt lag_max=%dt", i, p.live ? 1 : 0, p.lag_ticks,
                p.worst_lag_ticks);
        appendf(out, " rtt=%d/%d/%d(last/min/max)ms%s jitter=%dms", p.rtt_ms, p.rtt_min_ms,
                p.rtt_max_ms, p.rtt_offset_bound ? "[OFFSET-BOUND,NOT-PATH]" : "", p.jitter_ms);
        appendf(out, " rx=%u(%d/s) dup=%u loss~%d%%", static_cast<unsigned>(p.input_packets),
                p.recv_per_sec, static_cast<unsigned>(p.dup_inputs), p.loss_pct_est);
    }
    if (!s.note.empty()) {
        out.append(" note=");
        // One line, always: a note with a newline in it would split the record.
        for (const char c : s.note) out.push_back((c == '\n' || c == '\r') ? ' ' : c);
    }
    out.push_back('\n');
    return out;
}

}  // namespace bomber::net
