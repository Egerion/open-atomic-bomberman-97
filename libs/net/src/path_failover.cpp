#include "bomber/net/path_failover.hpp"

#include <array>
#include <cstdio>

#include "bomber/net/rollback_session.hpp"

namespace bomber::net {

namespace {

const char* trigger_name(PathFailover::Trigger t) {
    switch (t) {
        case PathFailover::Trigger::StarvedWithTraffic: return "starved-with-dups";
        case PathFailover::Trigger::StarvedSilent: return "starved-silent";
        case PathFailover::Trigger::None: break;
    }
    return "none";
}

const char* fail_reason_name(PathFailover::FailReason r) {
    switch (r) {
        case PathFailover::FailReason::PeerGone: return "peer-gone";
        case PathFailover::FailReason::RelayRefused: return "relay-refused";
        case PathFailover::FailReason::AllocTimeout: return "alloc-timeout";
        case PathFailover::FailReason::PeerNeverJoined: return "peer-never-joined";
        case PathFailover::FailReason::None: break;
    }
    return "none";
}

const char* pending_state_name(PathFailover::State s) {
    return s == PathFailover::State::Allocating ? "allocating" : "probing";
}

// The in-place widen counts as an outcome of its own — the log answer for a
// stall that self-cured without a relay being spent.
std::string healed_token(int heals, std::int64_t at_s) {
    std::array<char, 64> buf{};
    const int n = std::snprintf(buf.data(), buf.size(), "healed-in-place(x%d@%llds)", heals,
                                static_cast<long long>(at_s));
    return n > 0 ? std::string(buf.data()) : std::string();
}

// snprintf into a stack buffer — the same bounded-write idiom net_stats.cpp
// uses; the token is built once, at session end.
template <typename... Args>
std::string formatted(const char* fmt, Args... args) {
    std::array<char, 160> buf{};
    const int n = std::snprintf(buf.data(), buf.size(), fmt, args...);
    return n > 0 ? std::string(buf.data()) : std::string();
}

}  // namespace

PathFailover::PathFailover(MigratingTransport& wire, Transport& direct, RelayAllocator& allocator,
                           const Config& cfg)
    : wire_(&wire), direct_(&direct), allocator_(&allocator), cfg_(cfg) {}

bool PathFailover::progress_expected(const RollbackSession& s) {
    if (s.aborted() || s.desynced() || s.dropped_seats() != 0) return false;
    // An agreed round end freezes the frontier at the abandon tick on purpose,
    // and a migration hold freezes it by contract.
    return !s.end_round_scheduled() && !s.migration_held();
}

void PathFailover::pump(std::int64_t now_ms, RollbackSession* session) {
    // The keep-alive runs FIRST and in every state: losing our own membership
    // mid-match is what would turn a later AllocateRelay into not_in_lobby.
    allocator_->pump(now_ms);
    if (pending_heal_ && session != nullptr) {
        session->widen_resend_window();
        pending_heal_ = false;
    }
    switch (state_) {
        case State::Monitoring: monitor(now_ms, session); break;
        case State::Widening: watch_widen(now_ms, session); break;
        case State::Allocating: await_allocation(now_ms); break;
        case State::Probing: probe_relay(now_ms); break;
        case State::Switched:
        case State::Failed: break;
    }
    last_pump_ms_ = now_ms;
}

void PathFailover::monitor(std::int64_t now_ms, RollbackSession* session) {
    if (first_pump_ms_ < 0) first_pump_ms_ = now_ms;
    if (session == nullptr || !progress_expected(*session)) {
        window_open_ = false;
        return;
    }
    // A gap in OUR OWN pumping (drag, alt-tab, a modal) voids the evidence: the
    // peer may have been talking the whole time we were not listening.
    if (last_pump_ms_ >= 0 && now_ms - last_pump_ms_ > cfg_.self_stall_reset_ms)
        window_open_ = false;

    const std::uint32_t confirmed = session->confirmed_tick();
    if (!window_open_ || confirmed != window_confirmed_) {
        window_open_ = true;
        window_confirmed_ = confirmed;
        window_since_ms_ = now_ms;
        window_rx_ = wire_->rx_polled();
        return;
    }
    const std::int64_t starved_ms = now_ms - window_since_ms_;
    const std::uint64_t rx = wire_->rx_polled() - window_rx_;
    if (starved_ms >= cfg_.starve_traffic_ms && rx >= cfg_.starve_traffic_min_datagrams) {
        // Not fire() yet: a frontier frozen WITH traffic flowing may be a wedge
        // on a path that already healed (the header's third netdiag shape), and
        // that is curable in place for the cost of a wider resend.
        begin_widen(now_ms, *session);
        return;
    }
    if (starved_ms >= cfg_.starve_silence_ms) fire(now_ms, Trigger::StarvedSilent);
}

void PathFailover::begin_widen(std::int64_t now_ms, RollbackSession& session) {
    session.widen_resend_window();
    widen_since_ms_ = now_ms;
    widen_confirmed_ = session.confirmed_tick();
    state_ = State::Widening;
}

void PathFailover::watch_widen(std::int64_t now_ms, RollbackSession* session) {
    // A session that stopped expecting progress mid-grace (abort, desync, an
    // agreed round end) voids the starvation evidence — stand down, no heal.
    if (session != nullptr && !progress_expected(*session)) {
        window_open_ = false;
        state_ = State::Monitoring;
        return;
    }
    // Between rounds there is no frontier to watch; the grace clock keeps
    // running, and a dead path expires into the failover exactly as in-round.
    if (session != nullptr && session->confirmed_tick() != widen_confirmed_) {
        // The frontier MOVED: the path carries and the wedge is cured. Only new
        // input can move it — a dead path cannot fake this — so re-arm.
        ++widen_heals_;
        last_heal_ms_ = now_ms;
        window_open_ = false;
        state_ = State::Monitoring;
        return;
    }
    if (now_ms - widen_since_ms_ >= cfg_.widen_grace_ms) fire(now_ms, Trigger::StarvedWithTraffic);
}

void PathFailover::fire(std::int64_t now_ms, Trigger t) {
    trigger_ = t;
    detect_ms_ = now_ms;
    // Condemn the direct path. Going silent on it is also the loudest signal we
    // can still give a peer we cannot reach: total silence is exactly what its
    // own silence arm is listening for. fail() re-attaches where that is the
    // honest state (its comment carries the argument).
    wire_->detach();
    // A peer whose membership is POSITIVELY gone cannot allocate, so there is no
    // rendezvous to attempt — the drop policy owns the ending. This covers the
    // reaped old-build peer too: alive, maybe still flooding duplicates, but
    // with no membership its AllocateRelay could only be refused.
    if (!allocator_->peer_reachable()) {
        fail(now_ms, FailReason::PeerGone);
        return;
    }
    allocator_->request();
    state_ = State::Allocating;
}

void PathFailover::await_allocation(std::int64_t now_ms) {
    switch (allocator_->answer()) {
        case RelayAllocator::Answer::Pending:
            if (now_ms - detect_ms_ >= cfg_.alloc_timeout_ms) fail(now_ms, FailReason::AllocTimeout);
            return;
        case RelayAllocator::Answer::Refused: fail(now_ms, FailReason::RelayRefused); return;
        case RelayAllocator::Answer::Granted: break;
    }
    Transport* relay = allocator_->relay_transport();
    if (relay == nullptr) {
        fail(now_ms, FailReason::RelayRefused);
        return;
    }
    alloc_ms_ = now_ms;
    // The mutual proof, reused from the connect step (design §4.1): the session
    // does not move until the peer has PROVEN it is on the relay too. The probe
    // owns the socket's relay plane meanwhile; the session is detached, so the
    // two never race for a datagram.
    probe_.emplace(*relay, cfg_.probe_nonce, static_cast<int>(cfg_.probe_deadline_ms));
    state_ = State::Probing;
}

void PathFailover::probe_relay(std::int64_t now_ms) {
    // Engaged by the only transition into Probing (await_allocation); the guard
    // keeps the invariant checkable rather than assumed.
    if (!probe_.has_value()) return;
    probe_->step(now_ms);
    if (probe_->verified()) {
        wire_->attach(*allocator_->relay_transport());
        switch_ms_ = now_ms;
        state_ = State::Switched;
        // The frontiers parted during the one-way outage: the side that could
        // still hear kept finalising ticks the deaf side never received, and
        // resends from a base the deaf side's hole sits below. The session must
        // widen its resend window below the frontier (note_path_switched) or
        // the healed link reconnects into a wedge — delivered now, or on the
        // next pump that has a session.
        pending_heal_ = true;
        return;
    }
    if (probe_->expired()) fail(now_ms, FailReason::PeerNeverJoined);
}

void PathFailover::fail(std::int64_t now_ms, FailReason r) {
    fail_reason_ = r;
    state_ = State::Failed;
    (void)now_ms;
    // The two arms part ways on what the dead direct path still carries. After a
    // SILENCE fire nothing was arriving, so re-attaching restores today's exact
    // behaviour — a peer that was merely busy resumes as if we never fired, and
    // a dead one keeps the silence timer running toward the drop. After a
    // TRAFFIC fire the peer's duplicate flood is still arriving, and every one
    // of them resets the session's silence timer — re-attaching would make the
    // 600-pump drop unreachable and reproduce the exact live=1 forever-stall
    // this engine exists to end. So a traffic-arm failure stays DETACHED and
    // lets the silence it creates end the match honestly.
    if (trigger_ == Trigger::StarvedSilent) wire_->attach(*direct_);
}

std::string PathFailover::log_token() const {
    if (trigger_ == Trigger::None) {
        // No failover ever fired; an in-place heal (or an attempt in flight) is
        // still worth a word, and silence still means "never starved".
        if (widen_heals_ > 0)
            return healed_token(widen_heals_,
                                first_pump_ms_ >= 0 ? (last_heal_ms_ - first_pump_ms_) / 1000 : 0);
        if (state_ == State::Widening)
            return formatted("pending(widening@%llds)",
                             first_pump_ms_ >= 0
                                 ? static_cast<long long>((widen_since_ms_ - first_pump_ms_) / 1000)
                                 : 0LL);
        return {};
    }
    const long long at_s = first_pump_ms_ >= 0 ? (detect_ms_ - first_pump_ms_) / 1000 : 0;
    switch (state_) {
        case State::Switched:
            return formatted("switched(%s@%llds,alloc=%lldms,probe=%lldms)",
                             trigger_name(trigger_), at_s,
                             static_cast<long long>(alloc_ms_ - detect_ms_),
                             static_cast<long long>(switch_ms_ - alloc_ms_));
        case State::Failed:
            return formatted("failed(%s,%s@%llds)", fail_reason_name(fail_reason_),
                             trigger_name(trigger_), at_s);
        case State::Allocating:
        case State::Probing:
            return formatted("pending(%s,%s@%llds)", pending_state_name(state_),
                             trigger_name(trigger_), at_s);
        case State::Monitoring:
        case State::Widening: break;  // unreachable with a trigger latched
    }
    return {};
}

}  // namespace bomber::net
