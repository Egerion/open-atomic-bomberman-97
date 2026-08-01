// MID-MATCH PATH FAILOVER (path_failover.hpp) — task #56, built against the
// netdiag.log evidence of 2026-07-30..08-01: five stalls in one 45-minute
// session with the signature
//
//     depth=8/8 lag=8t stalls=65..678 rx=...(0/s) dup=1199..7297 loss~100%
//
// i.e. a path dead in ONE direction: the frontier frozen while the peer's
// retransmissions flood in, feeding the silence timer so the 600-pump drop
// detector can never fire (`live=1` on every stalled line).
//
// WHAT THIS SUITE PINS, one case each:
//   * the detector fires on the netdiag signature — the dup-seeing side on the
//     traffic arm within seconds, the silent side on the silence arm;
//   * it does NOT fire on the jitter corpus test_jitter_absorb.cpp calibrated
//     against live sessions — "bad but alive" stays untouched;
//   * a one-way blip that HEALS is cured IN PLACE (the wedge cure) with no
//     relay spent — and a symmetric blip needs not even that;
//   * the relay switch completes only after the mutual proof, and the match
//     then continues with the confirmed-hash exchange green (tick-identical);
//   * a refused allocation and a peer that never joins both degrade to the
//     EXISTING drop path — the sessions end, they do not hang;
//   * a peer whose membership is positively gone is not chased onto the relay.
//
// Both peers run on independent wall clocks over the ms_clock_harness rig; the
// one-way death is MsLink::set_blackhole, added for exactly this shape (a
// symmetric drop rate cannot express it). The relay is modelled §6-shaped where
// it matters: a datagram toward a seat that has not allocated is dropped
// (drop_unknown_dst), which is what makes the mutual proof mean something.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>
#include <cstdio>
#include <string>

#include "bomber/net/migrating_transport.hpp"
#include "bomber/net/net_path.hpp"
#include "bomber/net/net_stats.hpp"
#include "bomber/net/path_failover.hpp"
#include "bomber/net/rollback_session.hpp"
#include "bomber/net/transport.hpp"
#include "helpers.hpp"
#include "ms_clock_harness.hpp"

using namespace bomber;  // NOLINT(google-build-using-namespace) — test-local
using bomber::net::test::Driver;
using bomber::net::test::MsLink;
using bomber::net::test::MsTransport;
using bomber::sim::test::open_config;
using FState = net::PathFailover::State;
using FTrigger = net::PathFailover::Trigger;
using FReason = net::PathFailover::FailReason;

namespace {

constexpr std::uint16_t kSeat0 = 0x1;
constexpr std::uint16_t kSeat1 = 0x2;
constexpr std::uint16_t kBoth = 0x3;

// The corpus floor: ~100 ms RTT, where every live session sat.
constexpr int kOneWayMs = 50;
// The production drop policy the match loop builds (netplay_match.cpp): row 12
// off, 600 pumps of silence. The degrade cases assert against this exact value
// because "the honest outcome is the existing drop path" is a claim about it.
constexpr int kDropPumps = 600;

// The matchmaker's forwarder, modelled where the contract bites (PROTOCOL.md
// §6): its two legs are alive regardless of the direct path, and a datagram
// toward a seat that has NOT allocated is dropped — drop_unknown_dst, the rule
// that makes "the peer never joined" observable.
struct TestRelayNet {
    explicit TestRelayNet(const std::int64_t& now)
        : link(/*a_to_b=*/75, /*b_to_a=*/75, /*jitter=*/0), now(&now) {}
    MsLink link;
    const std::int64_t* now;
    bool allocated[2] = {false, false};
};

class TestRelayTransport final : public net::Transport {
public:
    TestRelayTransport(TestRelayNet& relay_net, int side) : net_(&relay_net), side_(side) {}
    void send(const std::uint8_t* data, std::size_t size) override {
        if (!net_->allocated[1 - side_]) return;  // drop_unknown_dst
        net_->link.send(side_, data, size, *net_->now);
    }
    bool poll(std::vector<std::uint8_t>* out) override {
        return net_->link.poll(side_, out, *net_->now);
    }
    net::NetPath path() const override { return net::NetPath::Relayed; }

private:
    TestRelayNet* net_;
    int side_;
};

// The control-plane seam, in-memory. Grant answers after a small delay (a WS
// round trip); Refuse answers no; Never answers nothing at all (a control plane
// that is itself gone).
class TestAllocator final : public net::RelayAllocator {
public:
    enum class Mode : std::uint8_t { Grant, Refuse, Never };

    TestAllocator(TestRelayNet& relay_net, int side, Mode mode)
        : relay_(relay_net, side), net_(&relay_net), side_(side), mode_(mode) {}

    void pump(std::int64_t now_ms) override {
        now_ = now_ms;
        if (mode_ == Mode::Grant && requested_at_ >= 0 && !granted_ &&
            now_ms - requested_at_ >= kGrantDelayMs) {
            granted_ = true;
            net_->allocated[side_] = true;
        }
    }
    void request() override {
        if (requested_at_ < 0) requested_at_ = now_;
    }
    Answer answer() const override {
        if (granted_) return Answer::Granted;
        if (mode_ == Mode::Refuse && requested_at_ >= 0) return Answer::Refused;
        return Answer::Pending;
    }
    net::Transport* relay_transport() override { return &relay_; }
    bool peer_reachable() const override { return peer_present; }

    bool requested() const { return requested_at_ >= 0; }

    static constexpr std::int64_t kGrantDelayMs = 200;
    bool peer_present = true;

private:
    TestRelayTransport relay_;
    TestRelayNet* net_;
    int side_;
    Mode mode_;
    std::int64_t now_ = 0;
    std::int64_t requested_at_ = -1;
    bool granted_ = false;
};

// One peer: session over a MigratingTransport whose initial target is the
// direct leg — the exact production wiring (netplay_runner.cpp) — plus the
// engine and its allocator, pumped beside every session pump the way
// MatchRunner::drive_tick does.
struct Peer {
    Peer(int side, MsLink& direct_link, const std::int64_t& now, TestRelayNet& relay_net,
         TestAllocator::Mode mode, const net::DropPolicy& drop = {})
        : direct(direct_link, side, now),
          wire(&direct),
          sim(open_config()),
          session(sim, side == 0 ? kSeat0 : kSeat1, kBoth, /*max_prediction=*/8, wire, drop),
          alloc(relay_net, side, mode),
          engine(wire, direct, alloc, engine_config(side)) {
        driver.seat = side;
    }

    static net::PathFailover::Config engine_config(int side) {
        net::PathFailover::Config cfg;  // PRODUCTION thresholds — that is the point
        cfg.probe_nonce = 0xF001u + static_cast<std::uint32_t>(side);
        return cfg;
    }

    void frame(std::int64_t now) {
        driver.frame(session, now,
                     [this, now](net::RollbackSession& s) { engine.pump(now, &s); });
    }

    MsTransport direct;
    net::MigratingTransport wire;
    sim::Simulation sim;
    net::RollbackSession session;
    TestAllocator alloc;
    net::PathFailover engine;
    Driver driver;
};

// First instant a predicate held, at 1 ms resolution; -1 while it never has.
struct FirstAt {
    std::int64_t at = -1;
    void sample(bool held, std::int64_t now) {
        if (at < 0 && held) at = now;
    }
};

bool fired(const net::PathFailover& e) {
    return e.trigger() != FTrigger::None;
}

}  // namespace

TEST_CASE("failover: the netdiag signature fires — dups fast, silence slower — and the "
          "relay switch completes tick-identical") {
    // Side 0's OUTBOUND dies at t=5 s (the user's logged seat: it keeps hearing
    // the peer's duplicate flood while the peer hears nothing at all). Both
    // engines run production thresholds and the production 600-pump drop
    // policy, so this case also proves the failover WINS the race against the
    // drop instead of being pre-empted by it.
    std::int64_t now = 0;
    MsLink direct(kOneWayMs, kOneWayMs, /*jitter=*/0);
    TestRelayNet relay_net(now);
    const net::DropPolicy drop_a{/*revert_to_ai=*/false, /*is_host=*/true, kDropPumps};
    const net::DropPolicy drop_b{/*revert_to_ai=*/false, /*is_host=*/false, kDropPumps};
    Peer a(0, direct, now, relay_net, TestAllocator::Mode::Grant, drop_a);
    Peer b(1, direct, now, relay_net, TestAllocator::Mode::Grant, drop_b);

    FirstAt a_widen, a_fire, b_fire, a_switch, b_switch;
    FirstAt b_widen;  // must never engage: the silent side has no traffic to read
    std::uint32_t confirmed_at_kill_a = 0;
    for (now = 0; now <= 30000; ++now) {
        if (now == 5000) {
            direct.set_blackhole(0, true);
            confirmed_at_kill_a = a.session.confirmed_tick();
        }
        if (now >= a.driver.next_frame) a.frame(now);
        if (now >= b.driver.next_frame) b.frame(now);
        a_widen.sample(a.engine.state() == FState::Widening, now);
        b_widen.sample(b.engine.state() == FState::Widening, now);
        a_fire.sample(fired(a.engine), now);
        b_fire.sample(fired(b.engine), now);
        a_switch.sample(a.engine.state() == FState::Switched, now);
        b_switch.sample(b.engine.state() == FState::Switched, now);
        if (now == 13000) {
            // Mid-flight: side 0 has long since fired and been granted, but the
            // peer has not arrived, so the relay is unproven and the session
            // must NOT be on it — the mutual-proof rule, observed in motion.
            CHECK(a.engine.state() == FState::Probing);
            CHECK_FALSE(a.wire.attached());
        }
    }

    // The DUP side: traffic arm, widen first (~freeze+3 s), then fire when the
    // grace finds the frontier still frozen (the path is dead, not wedged).
    CHECK(a.engine.trigger() == FTrigger::StarvedWithTraffic);
    CHECK(a_widen.at >= 7500);
    CHECK(a_widen.at <= 10000);
    CHECK(a_fire.at - a_widen.at >= 1500);
    CHECK(a_fire.at - a_widen.at <= 1700);
    // The SILENT side: nothing arrives, so only the slow arm can speak.
    CHECK(b.engine.trigger() == FTrigger::StarvedSilent);
    CHECK(b_widen.at == -1);
    CHECK(b_fire.at >= 14500);
    CHECK(b_fire.at <= 17000);

    // Both switched, and only after both had allocated (the silent side's fire
    // bounds the earliest possible proof).
    CHECK(a.engine.state() == FState::Switched);
    CHECK(b.engine.state() == FState::Switched);
    CHECK(a_switch.at >= b_fire.at);
    CHECK(a_switch.at <= 18000);
    CHECK(b_switch.at <= 18000);
    CHECK(a.wire.path() == net::NetPath::Relayed);
    CHECK(b.wire.path() == net::NetPath::Relayed);

    // The match CONTINUES, tick-identical: the session's own confirmed-hash
    // exchange compares every confirmed tick across the switch and stays green,
    // both frontiers clear the freeze by hundreds of ticks, and the 600-pump
    // drop never had to fire.
    CHECK_FALSE(a.session.desynced());
    CHECK_FALSE(b.session.desynced());
    CHECK_FALSE(a.session.aborted());
    CHECK_FALSE(b.session.aborted());
    CHECK(a.session.confirmed_tick() > confirmed_at_kill_a + 200);
    CHECK(b.session.confirmed_tick() > confirmed_at_kill_a + 200);
    const std::uint32_t ca = a.session.confirmed_tick();
    const std::uint32_t cb = b.session.confirmed_tick();
    CHECK((ca > cb ? ca - cb : cb - ca) <= 8);

    const std::string ta = a.engine.log_token();
    std::printf("  A token: %s\n  B token: %s\n", ta.c_str(), b.engine.log_token().c_str());
    CHECK(ta.rfind("switched(starved-with-dups@", 0) == 0);
    CHECK(b.engine.log_token().rfind("switched(starved-silent@", 0) == 0);
}

TEST_CASE("failover: the jitter corpus — bad but alive — never trips the detector") {
    // test_jitter_absorb.cpp's live-calibrated conditions, swept over its own
    // seeds: the worst honest path in the corpus (jitter 85-99 ms, RTT spikes
    // to ~850 ms over an ~100 ms floor) must not reach even the widen step.
    constexpr unsigned kSeeds[] = {0x1234567U, 0xA5A5A5A5U, 0x0BADF00DU, 0x51EED00DU, 0xC0FFEE11U};
    struct Cond {
        const char* label;
        int spread_ms;
        int persist_pct;
    };
    for (const Cond c : {Cond{"clean", 0, 0}, Cond{"low (live 5ms)", 16, 0},
                         Cond{"high (live 90ms)", 420, 35}}) {
        for (const unsigned seed : kSeeds) {
            std::int64_t now = 0;
            MsLink direct(kOneWayMs, kOneWayMs, c.spread_ms, c.persist_pct, seed);
            TestRelayNet relay_net(now);
            Peer a(0, direct, now, relay_net, TestAllocator::Mode::Grant);
            Peer b(1, direct, now, relay_net, TestAllocator::Mode::Grant);
            for (now = 0; now <= 30000; ++now) {
                if (now >= a.driver.next_frame) a.frame(now);
                if (now >= b.driver.next_frame) b.frame(now);
            }
            CAPTURE(c.label);
            CAPTURE(seed);
            CHECK(a.engine.state() == FState::Monitoring);
            CHECK(b.engine.state() == FState::Monitoring);
            CHECK_FALSE(fired(a.engine));
            CHECK_FALSE(fired(b.engine));
            CHECK(a.engine.widen_heals() == 0);  // not even the in-place cure engaged
            CHECK(b.engine.widen_heals() == 0);
            CHECK(a.engine.log_token().empty());
            CHECK_FALSE(a.session.desynced());
            CHECK_FALSE(b.session.desynced());
        }
    }
}

TEST_CASE("failover: a one-way blip that heals is cured IN PLACE — no relay spent") {
    // Kill side 0's outbound for 2 s, then heal it. The direct path carries
    // again, but the SESSION stays wedged (rollback_session.cpp's
    // widen_resend_window comment: the hearing side finalised the deaf side's
    // hole and resends from above it) — the shape of the three 2026-08-01 lines
    // that stalled forever at rx=20/s, loss~0%. The traffic arm's widen must
    // cure it on the spot, and neither allocator may be touched.
    std::int64_t now = 0;
    MsLink direct(kOneWayMs, kOneWayMs, /*jitter=*/0);
    TestRelayNet relay_net(now);
    Peer a(0, direct, now, relay_net, TestAllocator::Mode::Grant);
    Peer b(1, direct, now, relay_net, TestAllocator::Mode::Grant);

    std::uint32_t confirmed_at_kill = 0;
    for (now = 0; now <= 15000; ++now) {
        if (now == 5000) {
            direct.set_blackhole(0, true);
            confirmed_at_kill = a.session.confirmed_tick();
        }
        if (now == 7000) direct.set_blackhole(0, false);
        if (now >= a.driver.next_frame) a.frame(now);
        if (now >= b.driver.next_frame) b.frame(now);
    }

    // Cured, in place: no trigger latched on either side, at least one widen
    // did the curing, the wires never left the direct path, and the control
    // plane was never asked for anything.
    CHECK_FALSE(fired(a.engine));
    CHECK_FALSE(fired(b.engine));
    CHECK(a.engine.state() == FState::Monitoring);
    CHECK(b.engine.state() == FState::Monitoring);
    CHECK(a.engine.widen_heals() + b.engine.widen_heals() >= 1);
    CHECK(a.wire.path() == net::NetPath::Direct);
    CHECK(b.wire.path() == net::NetPath::Direct);
    CHECK_FALSE(a.alloc.requested());
    CHECK_FALSE(b.alloc.requested());
    // ...and genuinely cured: both frontiers rolled on well past the freeze.
    CHECK(a.session.confirmed_tick() > confirmed_at_kill + 80);
    CHECK(b.session.confirmed_tick() > confirmed_at_kill + 80);
    CHECK_FALSE(a.session.desynced());
    CHECK_FALSE(b.session.desynced());
    const std::string token = a.engine.widen_heals() > 0 ? a.engine.log_token()
                                                         : b.engine.log_token();
    CHECK(token.rfind("healed-in-place(", 0) == 0);
}

TEST_CASE("failover: a short SYMMETRIC blip needs neither widen nor relay") {
    // Both directions die for 2 s. The frontiers freeze TOGETHER, so each
    // peer's redundant window still covers everything the other missed — the
    // self-healing UDP already had. Nothing may engage.
    std::int64_t now = 0;
    MsLink direct(kOneWayMs, kOneWayMs, /*jitter=*/0);
    TestRelayNet relay_net(now);
    Peer a(0, direct, now, relay_net, TestAllocator::Mode::Grant);
    Peer b(1, direct, now, relay_net, TestAllocator::Mode::Grant);

    for (now = 0; now <= 15000; ++now) {
        if (now == 5000) {
            direct.set_blackhole(0, true);
            direct.set_blackhole(1, true);
        }
        if (now == 7000) {
            direct.set_blackhole(0, false);
            direct.set_blackhole(1, false);
        }
        if (now >= a.driver.next_frame) a.frame(now);
        if (now >= b.driver.next_frame) b.frame(now);
    }

    CHECK_FALSE(fired(a.engine));
    CHECK_FALSE(fired(b.engine));
    CHECK(a.engine.widen_heals() == 0);
    CHECK(b.engine.widen_heals() == 0);
    CHECK_FALSE(a.alloc.requested());
    CHECK_FALSE(b.alloc.requested());
    CHECK(a.session.confirmed_tick() > 250);  // ~15 s at 20 Hz minus the blip
    CHECK_FALSE(a.session.desynced());
    CHECK_FALSE(b.session.desynced());
}

TEST_CASE("failover: a refused allocation degrades to the existing drop path — an end, "
          "not a hang") {
    // The server says no (cap, budget, or a lapsed membership). The engines
    // latch Failed and the 600-pump drop policy must still be REACHABLE: the
    // traffic-arm side stays detached precisely so the peer's duplicate flood
    // cannot keep resetting the silence timer forever — re-attach there and
    // this case's abort assertions go red with the session live=1 for good,
    // which is tonight's netdiag hang re-created.
    std::int64_t now = 0;
    MsLink direct(kOneWayMs, kOneWayMs, /*jitter=*/0);
    TestRelayNet relay_net(now);
    const net::DropPolicy drop_a{/*revert_to_ai=*/false, /*is_host=*/true, kDropPumps};
    const net::DropPolicy drop_b{/*revert_to_ai=*/false, /*is_host=*/false, kDropPumps};
    Peer a(0, direct, now, relay_net, TestAllocator::Mode::Refuse, drop_a);
    Peer b(1, direct, now, relay_net, TestAllocator::Mode::Refuse, drop_b);

    FirstAt a_abort, b_abort;
    for (now = 0; now <= 50000; ++now) {
        if (now == 5000) direct.set_blackhole(0, true);
        if (now >= a.driver.next_frame) a.frame(now);
        if (now >= b.driver.next_frame) b.frame(now);
        a_abort.sample(a.session.aborted(), now);
        b_abort.sample(b.session.aborted(), now);
    }

    CHECK(a.engine.state() == FState::Failed);
    CHECK(a.engine.fail_reason() == FReason::RelayRefused);
    CHECK(b.engine.state() == FState::Failed);
    CHECK(b.engine.fail_reason() == FReason::RelayRefused);
    // Both sessions ENDED via the drop policy, inside its own window: the dup
    // side's silence starts at its detach (~10 s), the silent side's at the
    // kill, so both aborts land near (start + 30 s) — never never-land.
    CHECK(a.session.aborted());
    CHECK(b.session.aborted());
    CHECK(b_abort.at >= 33000);
    CHECK(b_abort.at <= 38000);
    CHECK(a_abort.at >= 38000);
    CHECK(a_abort.at <= 43000);
    CHECK(a.engine.log_token().rfind("failed(relay-refused,starved-with-dups@", 0) == 0);
}

TEST_CASE("failover: a peer that never joins the relay — an unpatched build — expires the "
          "probe and the drop path ends it") {
    // Side 1 models an OLD build: same session code, no engine, no allocator.
    // Side 0 fires, is granted, and probes into a relay whose far seat never
    // allocates — drop_unknown_dst eats every probe, the deadline expires, and
    // the existing drop policy ends both sides.
    std::int64_t now = 0;
    MsLink direct(kOneWayMs, kOneWayMs, /*jitter=*/0);
    TestRelayNet relay_net(now);
    const net::DropPolicy drop_a{/*revert_to_ai=*/false, /*is_host=*/true, kDropPumps};
    const net::DropPolicy drop_b{/*revert_to_ai=*/false, /*is_host=*/false, kDropPumps};
    Peer a(0, direct, now, relay_net, TestAllocator::Mode::Grant, drop_a);

    // The engine-less peer, hand-built: session straight over the direct leg.
    MsTransport b_direct(direct, 1, now);
    sim::Simulation b_sim(open_config());
    net::RollbackSession b_session(b_sim, kSeat1, kBoth, /*max_prediction=*/8, b_direct, drop_b);
    Driver b_driver;
    b_driver.seat = 1;

    FirstAt a_expired;
    for (now = 0; now <= 50000; ++now) {
        if (now == 5000) direct.set_blackhole(0, true);
        if (now >= a.driver.next_frame) a.frame(now);
        if (now >= b_driver.next_frame) b_driver.frame(b_session, now);
        a_expired.sample(a.engine.state() == FState::Failed, now);
    }

    CHECK(a.engine.fail_reason() == FReason::PeerNeverJoined);
    // The probe waited its full generous deadline (fire ~10 s + 20 s) before
    // giving up — early expiry would strand a slow-but-coming peer.
    CHECK(a_expired.at >= 29000);
    CHECK(a_expired.at <= 33000);
    CHECK(a.session.aborted());  // detached since the fire: silence ran honestly
    CHECK(b_session.aborted());  // heard nothing since the kill
}

TEST_CASE("failover: a peer whose membership is positively gone is not chased") {
    // The roster oracle (design §4.2): the seat VANISHED — it quit, or it is an
    // old build the reaper collected. Its AllocateRelay could only be refused,
    // so no allocation is attempted at all; detaching still lets the silence
    // timer run honestly under the residual duplicate flood.
    std::int64_t now = 0;
    MsLink direct(kOneWayMs, kOneWayMs, /*jitter=*/0);
    TestRelayNet relay_net(now);
    const net::DropPolicy drop_a{/*revert_to_ai=*/false, /*is_host=*/true, kDropPumps};
    Peer a(0, direct, now, relay_net, TestAllocator::Mode::Grant, drop_a);
    a.alloc.peer_present = false;
    Peer b(1, direct, now, relay_net, TestAllocator::Mode::Grant);

    for (now = 0; now <= 45000; ++now) {
        if (now == 5000) direct.set_blackhole(0, true);
        if (now >= a.driver.next_frame) a.frame(now);
        if (now >= b.driver.next_frame) b.frame(now);
    }

    CHECK(a.engine.state() == FState::Failed);
    CHECK(a.engine.fail_reason() == FReason::PeerGone);
    CHECK_FALSE(a.alloc.requested());
    CHECK_FALSE(a.wire.attached());  // detached, so the dup flood cannot feed live=1 forever
    CHECK(a.session.aborted());
    CHECK(a.engine.log_token().rfind("failed(peer-gone,", 0) == 0);
}

TEST_CASE("failover: the netdiag line carries the verdict") {
    net::SessionSummary s;
    s.timestamp = "2026-08-01 18:24:47";
    std::string line = format_session_log_line(s);
    CHECK(line.find("failover=") == std::string::npos);  // never fired: no key at all

    s.failover = "switched(starved-with-dups@41s,alloc=210ms,probe=580ms)";
    line = format_session_log_line(s);
    CHECK(line.find(" failover=switched(starved-with-dups@41s,alloc=210ms,probe=580ms)") !=
          std::string::npos);
}
