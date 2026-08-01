#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "bomber/net/link_probe.hpp"
#include "bomber/net/migrating_transport.hpp"
#include "bomber/net/transport.hpp"

// MID-MATCH PATH FAILOVER (design §4.2) — detect a direct path that died UNDER a
// running match and move both peers onto the relay, behind the session's back.
//
// THE EVIDENCE THIS IS BUILT AGAINST is netdiag.log, 2026-07-30..08-01: the same
// signature five times in one 45-minute session and on two earlier days —
//
//     depth=8/8 lag=8t stalls=65..678 rx=...(0/s) dup=1199..7297 loss~100%
//
// read as: the path died in ONE direction. No new frame arrives (the confirmed
// frontier is frozen) yet DUPLICATES flood in by the thousand — the peer is
// alive, stalled at its own prediction cap for want of OUR input, re-sending the
// same unconfirmed window forever. Its retransmissions feed the 600-pump silence
// timer (`live=1` on every stalled line), so the existing drop detector — which
// measures datagram SILENCE — can never see this. THE DETECTOR HERE MEASURES
// NEW-INPUT PROGRESS instead: the confirmed frontier advancing, which healthy
// sessions do 20 times a second and dead ones not at all for tens of seconds.
//
// TWO ARMS, because the two sides of a one-way death see different things:
//
//   * STARVED WITH TRAFFIC — frontier frozen for kStarveTrafficMs while at
//     least kStarveTrafficMinDatagrams arrived. Unambiguous: a peer that talks
//     but says nothing new is a peer that cannot hear us. Fires fast (3 s).
//     The threshold sits above every "bad but alive" case in the corpus
//     (tests/net/test_jitter_absorb.cpp's live-calibrated schedules): the worst
//     honest jitter freezes the frontier for ~1.5 s; the netdiag failures
//     freeze it for 40+ seconds at ~20 datagrams/s of duplicates.
//
//   * STARVED SILENT — frontier frozen for kStarveSilenceMs with nothing
//     arriving at all. Ambiguous (dead peer, dead path, or a peer whose whole
//     process is blocked by a window drag), hence slower (10 s) — and it is the
//     ONLY arm the far side of the logged failure can fire, because the peer of
//     a one-way death hears pure silence.
//
// HOW THE PEERS AGREE (design constraint: both must move or neither). There is
// deliberately NO new wire message and no server broadcast: the initiator's
// outbound direction is dead by diagnosis, so a "join me on the relay" datagram
// from the fast (dup-seeing) side can never arrive, and PROTOCOL.md §6 is frozen
// with no broadcast that could tell the other seat. Convergence is structural
// instead, the same absorbing-relay argument as design §4.1:
//
//   1. both sides run the SAME detector; the dup-seeing side fires the traffic
//      arm at ~3 s, the silent side fires the silence arm at ~10 s;
//   2. a firing side DETACHES from the direct path — which is also the loudest
//      remaining signal it can give a peer it cannot reach: total silence, the
//      very thing the other arm listens for;
//   3. both allocate (idempotent per seat, PROTOCOL.md §6.1 blesses the
//      mid-match retry), both probe, and the relay delivers only once both are
//      there — the mutual LinkProbe proof, reused from the connect step, is
//      what keeps the session off the relay until the peer is provably on it;
//   4. nobody ever leaves the relay, so a false-positive fire still converges:
//      the peer that was merely busy comes back to silence, fires its own
//      silence arm, and meets us there.
//
// BEFORE the relay, one cheap in-place attempt — THE WEDGE CURE. Three of the
// 2026-08-01 stalls read `rx=20/s loss~0%` with the frontier frozen: traffic in
// BOTH directions, nothing new — a path that died one-way and then HEALED, with
// the session wedged behind it (the hearing side finalised the deaf side's last
// ~cap ticks, pruned them, and resends from above the hole; see
// RollbackSession::widen_resend_window). So the traffic arm first widens the
// resend window and gives the direct path a short grace: a wedged-but-carrying
// path resumes within an RTT and no relay is spent; a dead one cannot move the
// frontier at all, and the failover proceeds.
//
// If the allocation is refused or the peer never shows, the failover LATCHES
// Failed and the existing drop policy ends the match (abort, or seat → AI) —
// the honest outcome. A traffic-arm failure stays DETACHED on purpose: the dup
// flood would otherwise feed the silence timer forever and reproduce the exact
// hang this class exists to cure.
//
// Clock-injected, SDL-free, lobby-free: the control plane is behind the
// RelayAllocator seam, so the engine builds and tests headless.
namespace bomber::net {

class RollbackSession;

// The engine's one question to the control plane: "keep my membership alive,
// and get me a relay allocation". Production implements it over LobbyFlow
// (whose step() already heartbeats every phase that holds a seat — pump() IS
// the mid-match heartbeat, since nothing else touches the matchmaker once a
// match starts and the server reaps a silent member in ~30 s, taking with it
// the membership AllocateRelay requires). Tests implement it in-memory.
class RelayAllocator {
public:
    RelayAllocator() = default;
    RelayAllocator(const RelayAllocator&) = delete;
    RelayAllocator& operator=(const RelayAllocator&) = delete;
    virtual ~RelayAllocator() = default;

    enum class Answer : std::uint8_t { Pending, Granted, Refused };

    // Called every engine pump, before anything else — the keep-alive.
    virtual void pump(std::int64_t now_ms) = 0;
    // Ask for this seat's allocation. Idempotent; the answer arrives later.
    virtual void request() = 0;
    virtual Answer answer() const = 0;
    // The relay-path Transport once Granted. BORROWED; must outlive the engine.
    virtual Transport* relay_transport() = 0;
    // False when the control plane POSITIVELY says the peer's membership is gone
    // (it quit, or was reaped) — then its AllocateRelay can only be refused and
    // there is no relay rendezvous to attempt. "Still present" proves nothing
    // (design §4.2's timing asymmetry) and only ever means "try".
    virtual bool peer_reachable() const { return true; }
};

class PathFailover {
public:
    enum class State : std::uint8_t {
        Monitoring,  // healthy, or not yet fired
        Widening,    // traffic-arm threshold hit; trying the in-place wedge cure
        Allocating,  // fired; detached from direct; waiting for the control plane
        Probing,     // allocation in hand; proving the relay carries BOTH ways
        Switched,    // the session now runs over the relay
        Failed,      // no relay materialised; the drop policy owns the ending
    };
    enum class Trigger : std::uint8_t { None, StarvedWithTraffic, StarvedSilent };
    enum class FailReason : std::uint8_t {
        None,
        PeerGone,        // the roster says the peer's membership is gone
        RelayRefused,    // the server said no (cap, budget, or no membership)
        AllocTimeout,    // no answer at all — the control plane itself is gone
        PeerNeverJoined  // allocation held, but the peer never met us on it
    };

    struct Config {
        // The traffic arm: netdiag's dead sessions receive ~17-20 datagrams/s of
        // pure duplicates while frozen, so 3 s collects ~60 against a floor of
        // 30; the corpus' worst honest freeze is half the window.
        std::int64_t starve_traffic_ms = 3000;
        std::uint64_t starve_traffic_min_datagrams = 30;
        // The silence arm: a third of the 600-pump drop window — late enough to
        // ride out an alt-tab, early enough that allocation + probe + the drop
        // backstop all still fit inside the match's patience.
        std::int64_t starve_silence_ms = 10000;
        // The in-place grace after widening the resend window: long enough for
        // the peer's own traffic arm to fire, widen, and a round trip to land
        // (the two arms open at most ~0.5 s apart, and the corpus' worst RTT
        // spike is ~1.3 s); a dead path cannot move the frontier however long
        // the grace.
        std::int64_t widen_grace_ms = 1500;
        // How long the server may take to answer AllocateRelay before the
        // control plane itself is declared gone.
        std::int64_t alloc_timeout_ms = 5000;
        // How long to wait on the relay for the peer. Generous by design: the
        // peer's own silence arm may be up to kStarveSilenceMs behind ours, plus
        // its allocation round trip — and the whole wait still resolves before
        // the 600-pump drop backstop lands.
        std::int64_t probe_deadline_ms = 20000;
        // A pump gap larger than this is OUR OWN loop stalling (window drag,
        // alt-tab): the starvation evidence is void and the window restarts.
        std::int64_t self_stall_reset_ms = 1000;
        // This peer's probe nonce (LobbyFlow::local_probe_nonce derives it from
        // the shared seed) so the LinkProbe can tell the peer's probes from a
        // reflected copy of its own.
        std::uint32_t probe_nonce = 0;
    };

    // `wire` is the indirection the session actually runs over; `direct` is its
    // pre-failover target (re-attached when a silence-arm attempt fails, so a
    // peer that was merely busy resumes as if nothing fired). All BORROWED.
    PathFailover(MigratingTransport& wire, Transport& direct, RelayAllocator& allocator,
                 const Config& cfg);

    // Once per session pump. `session` is the round's live RollbackSession, or
    // nullptr between rounds — the machine still runs (keep-alive, allocation,
    // probing) but detection needs a frontier to watch.
    void pump(std::int64_t now_ms, RollbackSession* session);

    State state() const { return state_; }
    Trigger trigger() const { return trigger_; }
    FailReason fail_reason() const { return fail_reason_; }
    // How many times the in-place widen cured a wedge without a relay.
    int widen_heals() const { return widen_heals_; }

    // The netdiag token: "" until anything happened, else e.g.
    //   switched(starved-with-dups@41s,alloc=210ms,probe=580ms)
    //   failed(peer-never-joined,starved-silent@52s)
    //   pending(allocating,starved-with-dups@41s)
    //   healed-in-place(x1@41s)              — wedge cured, no relay spent
    // @Ns is seconds since the engine's first pump — the online session's start.
    std::string log_token() const;

private:
    void monitor(std::int64_t now_ms, RollbackSession* session);
    void begin_widen(std::int64_t now_ms, RollbackSession& session);
    void watch_widen(std::int64_t now_ms, RollbackSession* session);
    void fire(std::int64_t now_ms, Trigger t);
    void await_allocation(std::int64_t now_ms);
    void probe_relay(std::int64_t now_ms);
    void fail(std::int64_t now_ms, FailReason r);
    // Progress of the frontier is only evidence while the session EXPECTS it:
    // not after an abort/desync/drop, not once a round end is agreed, not while
    // a migration holds the frontier still on purpose.
    static bool progress_expected(const RollbackSession& s);

    MigratingTransport* wire_;
    Transport* direct_;
    RelayAllocator* allocator_;
    Config cfg_;
    std::optional<LinkProbe> probe_;

    State state_ = State::Monitoring;
    Trigger trigger_ = Trigger::None;
    FailReason fail_reason_ = FailReason::None;

    // The starvation window: the frontier value it opened on, when, and the
    // wire's rx count at that instant.
    bool window_open_ = false;
    std::uint32_t window_confirmed_ = 0;
    std::int64_t window_since_ms_ = -1;
    std::uint64_t window_rx_ = 0;

    std::int64_t first_pump_ms_ = -1;
    std::int64_t last_pump_ms_ = -1;
    std::int64_t detect_ms_ = -1;
    std::int64_t alloc_ms_ = -1;
    std::int64_t switch_ms_ = -1;
    // The in-place attempt: when it began and the frontier it must move.
    std::int64_t widen_since_ms_ = -1;
    std::uint32_t widen_confirmed_ = 0;
    int widen_heals_ = 0;
    std::int64_t last_heal_ms_ = -1;
    // The switch happened on a pump with no session (between rounds): the
    // resend-window widening is delivered to the next session seen instead.
    bool pending_heal_ = false;
};

}  // namespace bomber::net
