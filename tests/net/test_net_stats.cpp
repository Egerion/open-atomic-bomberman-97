// The IN-MATCH NETPLAY DIAGNOSTICS (net_stats.hpp): the numbers the F3 overlay
// draws and the line a dead session leaves in netdiag.log.
//
// Two things are being pinned here, and the second matters as much as the first:
//
//  1. THE DERIVATIONS ARE RIGHT. Every number comes from traffic the protocol
//     already carries — no message was added, so no wire version was taken. The
//     ack-RTT in particular is derived from a peer's InputRange first_tick (its
//     confirmed frontier), which is only an acknowledgement when it RISES; the
//     tests below prove that a stream of redundant re-sends at an unchanged
//     frontier produces no sample, which is the failure mode a naive
//     "timestamp every packet" version would have.
//
//  2. THE COUNTING DOES NOT PERTURB WHAT IT COUNTS. The clocked and unclocked
//     runs of the same scenario are asserted to produce byte-identical sim
//     hashes and identical tick/confirmed frontiers, so instrumentation cannot
//     have changed a single simulated tick.
//
// EVERY netplay case drives BOTH PEERS over a LoopbackLink. A one-sided test is
// what let a connection bug through earlier; a statistic that only makes sense
// from one end of a link is not a statistic worth shipping.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "bomber/net/net_stats.hpp"
#include "bomber/net/rollback_session.hpp"
#include "bomber/net/transport.hpp"
#include "helpers.hpp"  // bomber::sim::test::open_config

using namespace bomber;  // NOLINT(google-build-using-namespace) — test-local
using bomber::sim::test::open_config;

namespace {

constexpr std::uint16_t kSeat0 = 0x1;
constexpr std::uint16_t kSeat1 = 0x2;
constexpr std::uint16_t kBoth = 0x3;

// One pump is one sim tick: 50 ms at the fixed 20 Hz the sessions run at, which
// is also what a real match's frame loop hands in.
constexpr std::int64_t kPumpMs = 1000 / net::kPumpHz;

sim::TickInputs seat_input(int seat, const sim::PlayerInput& in) {
    sim::TickInputs t;
    t.players[static_cast<std::size_t>(seat)] = in;
    return t;
}

// Changes most ticks and is phase-shifted per seat, so "repeat the peer's last
// input" is frequently wrong and real rollbacks happen (mirrors
// test_rollback_session.cpp's script).
sim::PlayerInput scripted(int seat, std::uint32_t tick) {
    sim::PlayerInput in;
    switch ((tick + static_cast<std::uint32_t>(seat) * 3U) % 6U) {
        case 0: in.right = true; break;
        case 1: in.down = true; break;
        case 2: in.left = true; break;
        case 3: in.up = true; break;
        case 4: in.action1 = true; break;
        default: break;
    }
    return in;
}

// A two-peer harness over one LoopbackLink. `clocked` decides whether the
// sessions are handed a wall clock at all — the whole point of the
// perturbation test below.
struct Pair {
    net::LoopbackLink link;
    net::LoopbackTransport ta{link, 0};
    net::LoopbackTransport tb{link, 1};
    sim::Simulation sa{open_config()};
    sim::Simulation sb{open_config()};
    net::RollbackSession a;
    net::RollbackSession b;
    std::int64_t now = 1000;  // deliberately not 0: nothing may depend on the origin
    bool clocked = true;

    Pair(int latency, int max_prediction, bool with_clock)
        : link(latency),
          a(sa, kSeat0, kBoth, max_prediction, ta),
          b(sb, kSeat1, kBoth, max_prediction, tb),
          clocked(with_clock) {}

    void pump() {
        const std::int64_t t = clocked ? now : -1;
        a.advance(seat_input(0, scripted(0, a.predicted_tick())), t);
        b.advance(seat_input(1, scripted(1, b.predicted_tick())), t);
        link.step();
        now += kPumpMs;
    }

    void pump_n(int n) {
        for (int i = 0; i < n; ++i) pump();
    }
};

}  // namespace

TEST_CASE("net stats: the path label comes from the transport actually carrying the bytes") {
    // The single most valuable field on the overlay: a direct match does not
    // touch the matchmaker after tick 0, so knowing the path rules half the
    // causes in or out before anything else is measured.
    Pair p(/*latency=*/2, /*max_prediction=*/16, /*with_clock=*/true);
    p.pump_n(4);
    CHECK(p.a.stats().path == net::NetPath::Loopback);
    CHECK(p.b.stats().path == net::NetPath::Loopback);
    // The default on the base class is Unknown, so a transport that has not been
    // taught to answer never claims to be direct.
    CHECK(std::string(net::path_name(net::NetPath::Unknown)) == "unknown");
    CHECK(std::string(net::path_name(net::NetPath::Direct)) == "direct");
    CHECK(std::string(net::path_name(net::NetPath::Relayed)) == "relayed");
    CHECK(std::string(net::path_name(net::NetPath::StarHub)) == "starhub");
}

TEST_CASE("net stats: BOTH peers measure an ack-RTT that tracks the link's latency") {
    // The derivation under test: a peer's InputRange begins at ITS confirmed
    // frontier, and that frontier cannot pass a tick our input has not reached.
    // So a rise in first_tick is an acknowledgement of our own earlier tick, and
    // the interval between the two is a real round trip — measured with no new
    // message on the wire.
    constexpr int kLatency = 3;  // pumps each way
    Pair p(kLatency, /*max_prediction=*/16, /*with_clock=*/true);
    p.pump_n(200);

    const net::PeerStats& from_b = p.a.stats().peers[1];
    const net::PeerStats& from_a = p.b.stats().peers[0];
    REQUIRE(from_b.tracked);
    REQUIRE(from_a.tracked);
    CHECK(from_b.rtt_ms > 0);
    CHECK(from_a.rtt_ms > 0);

    // A round trip is at least two link crossings; the reading is an UPPER bound
    // (the peer answers only on a pump, and must itself have reached the tick),
    // so it sits at or above 2*latency and comfortably inside a few extra pumps.
    // Asserting the band rather than a literal is what makes this a statement
    // about the derivation instead of about this harness's arithmetic.
    const int floor_ms = 2 * kLatency * static_cast<int>(kPumpMs);
    CHECK(from_b.rtt_min_ms >= floor_ms);
    CHECK(from_a.rtt_min_ms >= floor_ms);
    CHECK(from_b.rtt_min_ms <= floor_ms + 6 * static_cast<int>(kPumpMs));
    CHECK(from_a.rtt_min_ms <= floor_ms + 6 * static_cast<int>(kPumpMs));

    // A steady link: the smoothed value sits in the same band and jitter is small.
    CHECK(from_b.rtt_smooth_ms >= floor_ms);
    CHECK(from_b.jitter_ms < 3 * static_cast<int>(kPumpMs));

    // The sparkline filled with real samples (a gap is stored as 0, so a
    // non-zero entry is a measurement that actually happened).
    CHECK(from_b.rtt_history_len > 8);
    bool any_sample = false;
    for (std::size_t i = 0; i < from_b.rtt_history_len; ++i)
        if (from_b.rtt_history[i] != 0) any_sample = true;
    CHECK(any_sample);
    CHECK(from_b.rtt_recent_max_ms >= from_b.rtt_min_ms);
}

TEST_CASE("net stats: an ack-RTT dominated by the peer's tick offset says so") {
    // MEASURED, not theorised. Two real peers over UDP loopback (a ~25 ms path)
    // reported 33 ms and 333 ms: rollback netcode lets the peers settle into a
    // wall-clock phase offset bounded only by the prediction cap, and the peer
    // that is AHEAD is measuring that offset rather than the wire. A 349 ms
    // reading on a LAN would send someone hunting a network fault that does not
    // exist, so the flag is not a nicety — it is the difference between a
    // diagnostic and a misleading one.
    //
    // Reproduced the way it happens for real: A gets a head start and then BOTH
    // peers run at the same rate, so the lead persists as a constant phase
    // offset (nothing in rollback pulls it back in below the prediction cap).
    constexpr int kLead = 8;
    net::LoopbackLink link(/*latency=*/1);
    net::LoopbackTransport ta(link, 0);
    net::LoopbackTransport tb(link, 1);
    sim::Simulation sa(open_config());
    sim::Simulation sb(open_config());
    net::RollbackSession a(sa, kSeat0, kBoth, /*max_prediction=*/16, ta);
    net::RollbackSession b(sb, kSeat1, kBoth, /*max_prediction=*/16, tb);
    std::int64_t now = 0;
    const auto pump_a = [&] { a.advance(seat_input(0, scripted(0, a.predicted_tick())), now); };
    const auto pump_b = [&] { b.advance(seat_input(1, scripted(1, b.predicted_tick())), now); };
    for (int i = 0; i < kLead; ++i) {
        pump_a();
        link.step();
        now += kPumpMs;
    }
    for (int i = 0; i < 300; ++i) {
        pump_a();
        pump_b();
        link.step();
        now += kPumpMs;
    }
    const net::PeerStats& ahead = a.stats().peers[1];   // A leads: its reading is contaminated
    const net::PeerStats& behind = b.stats().peers[0];  // B trails: its partner answers at once
    REQUIRE(ahead.rtt_ms > 0);
    REQUIRE(behind.rtt_ms > 0);
    CHECK(ahead.lag_ticks >= kLead);
    // The contaminated reading has collapsed onto the offset: it is ~1x the lag,
    // where a healthy one would be ~2x (net_stats.cpp derives both).
    CHECK(ahead.rtt_ms * 2 < ahead.lag_ticks * (1000 / net::kPumpHz) * 3);
    CHECK(ahead.rtt_offset_bound);
    // And it is many times what this one-pump link can actually cost, which is
    // precisely why an unflagged reading would send someone hunting a fault.
    CHECK(ahead.rtt_ms > 4 * static_cast<int>(kPumpMs));
    // The peer that is BEHIND is not flagged: its partner is always ready to
    // acknowledge, so its sample really is the round trip — and it reads it.
    CHECK_FALSE(behind.rtt_offset_bound);
    CHECK(behind.rtt_ms <= 4 * static_cast<int>(kPumpMs));

    // It reaches the log too, so a line pasted back to us cannot be misread
    // later either.
    net::SessionSummary sum;
    sum.stats = a.stats();
    CHECK(net::format_session_log_line(sum).find("[OFFSET-BOUND,NOT-PATH]") != std::string::npos);
}

TEST_CASE("net stats: a level pair is NOT flagged offset-bound") {
    // The flag has to be quiet in the ordinary case or it is noise.
    Pair p(/*latency=*/2, /*max_prediction=*/16, /*with_clock=*/true);
    p.pump_n(300);
    CHECK_FALSE(p.a.stats().peers[1].rtt_offset_bound);
    CHECK_FALSE(p.b.stats().peers[0].rtt_offset_bound);
}

TEST_CASE("net stats: a longer link reads as a longer RTT on both peers") {
    // The number has to MOVE with the thing it claims to measure — otherwise it
    // is a plausible-looking constant, which is worse than no number at all.
    Pair fast(/*latency=*/1, /*max_prediction=*/16, /*with_clock=*/true);
    Pair slow(/*latency=*/6, /*max_prediction=*/16, /*with_clock=*/true);
    fast.pump_n(200);
    slow.pump_n(200);
    CHECK(slow.a.stats().peers[1].rtt_min_ms > fast.a.stats().peers[1].rtt_min_ms);
    CHECK(slow.b.stats().peers[0].rtt_min_ms > fast.b.stats().peers[0].rtt_min_ms);
    // And so does the tick-space measure, which needs no clock at all.
    CHECK(slow.a.stats().peers[1].worst_lag_ticks > fast.a.stats().peers[1].worst_lag_ticks);
}

TEST_CASE("net stats: an unchanged confirmed frontier yields NO new RTT sample") {
    // The trap this design had to avoid. Every peer re-sends its whole
    // unconfirmed window every pump, so most arriving datagrams repeat a
    // frontier already seen. Timestamping those would report a round trip that
    // grows without bound the longer a peer is stalled — the opposite of the
    // truth. Only a RISE closes a round trip.
    net::NetStatsTracker t;
    t.begin(net::NetPath::Direct, /*remote_seats=*/kSeat1, /*start_tick=*/0,
            /*max_prediction=*/8);

    t.begin_pump(1000);
    t.on_local_tick(0);
    t.on_local_tick(1);
    t.begin_pump(1100);
    t.on_input_from(1, /*first_tick=*/2);  // acknowledges our tick 1, sent at 1000
    const int first = t.stats().peers[1].rtt_ms;
    CHECK(first == 100);

    // Ten more datagrams at the SAME frontier, 5 s later: no sample, so the
    // reading stays exactly what was last actually proven.
    for (int i = 0; i < 10; ++i) {
        t.begin_pump(1100 + 500 * (i + 1));
        t.on_input_from(1, /*first_tick=*/2);
    }
    CHECK(t.stats().peers[1].rtt_ms == first);
    CHECK(t.stats().peers[1].rtt_max_ms == first);
    // But the traffic itself was counted — that is the liveness signal.
    CHECK(t.stats().peers[1].input_packets == 11);
}

TEST_CASE("net stats: a single-tick Input frame carries no frontier and fabricates no RTT") {
    net::NetStatsTracker t;
    t.begin(net::NetPath::Direct, kSeat1, 0, 8);
    t.begin_pump(1000);
    t.on_local_tick(0);
    t.begin_pump(9000);
    t.on_input_from(1, /*first_tick=*/0);  // 0 = "no frontier in this frame"
    CHECK(t.stats().peers[1].rtt_ms == -1);
    CHECK(t.stats().peers[1].input_packets == 1);
}

TEST_CASE("net stats: rates and the loss ESTIMATE come off completed one-second windows") {
    net::NetStatsTracker t;
    t.begin(net::NetPath::Relayed, kSeat1, 0, 8);
    const std::array<std::uint32_t, sim::kMaxPlayers> next{};

    // A full second at the expected 20 datagrams/s: no shortfall.
    for (int i = 0; i < net::kPumpHz; ++i) {
        t.begin_pump(1000 + i * kPumpMs);
        t.on_datagram(true);
        t.on_input_from(1, static_cast<std::uint32_t>(i + 1));
        t.end_pump(static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(i), next, 0, false, 0,
                   false);
    }
    t.begin_pump(2000);  // closes the window
    CHECK(t.stats().peers[1].recv_per_sec == net::kPumpHz);
    CHECK(t.stats().peers[1].loss_pct_est == 0);
    CHECK(t.stats().rx_per_sec == net::kPumpHz);

    // The next second only half the datagrams arrive.
    for (int i = 0; i < net::kPumpHz / 2; ++i) {
        t.begin_pump(2000 + i * 2 * kPumpMs);
        t.on_datagram(true);
        t.on_input_from(1, static_cast<std::uint32_t>(net::kPumpHz + i + 1));
    }
    t.begin_pump(3000);
    CHECK(t.stats().peers[1].recv_per_sec == net::kPumpHz / 2);
    CHECK(t.stats().peers[1].loss_pct_est == 50);
}

TEST_CASE("net stats: a seat handed to the AI stops reading as packet loss") {
    // A dropped seat legitimately goes quiet. Reporting that as 100% loss would
    // point at the network for a peer that is simply gone — the exact wrong
    // conclusion when the owner is trying to work out what happened.
    net::NetStatsTracker t;
    t.begin(net::NetPath::Direct, kSeat1, 0, 8);
    const std::array<std::uint32_t, sim::kMaxPlayers> next{};
    t.begin_pump(1000);
    t.end_pump(0, 0, next, /*dropped=*/kSeat1, false, 0, false);
    t.begin_pump(2100);  // roll a window with no traffic at all
    CHECK_FALSE(t.stats().peers[1].live);
    CHECK(t.stats().peers[1].recv_per_sec == 0);
    CHECK(t.stats().peers[1].loss_pct_est == 0);
}

TEST_CASE("net stats: malformed datagrams are counted, on the peer that receives them") {
    // Nothing else here would reveal a path that corrupts or injects, and the
    // relay bug that had to be read out of server logs was exactly this shape.
    Pair p(/*latency=*/1, /*max_prediction=*/16, /*with_clock=*/true);
    p.pump_n(10);
    CHECK(p.a.stats().rx_malformed == 0);
    CHECK(p.b.stats().rx_malformed == 0);

    const std::vector<std::uint8_t> junk{0xEE, 0x01, 0x02, 0x03};
    p.link.send(/*from=*/0, junk.data(), junk.size());  // arrives at side 1
    p.pump_n(4);
    CHECK(p.b.stats().rx_malformed >= 1);
    CHECK(p.a.stats().rx_malformed == 0);  // and NOT on the sender
    CHECK(p.b.stats().rx_packets > p.b.stats().rx_malformed);
}

TEST_CASE("net stats: rollbacks, re-sim depth and the redundancy count are all exercised") {
    Pair p(/*latency=*/5, /*max_prediction=*/16, /*with_clock=*/true);
    p.pump_n(300);

    for (const net::NetStats* s : {&p.a.stats(), &p.b.stats()}) {
        CHECK(s->rollbacks > 0);                  // latency forced real mispredictions
        CHECK(s->resim_ticks >= s->rollbacks);    // each rollback replays >= 1 tick
        CHECK(s->prediction_depth >= 0);
        CHECK(s->worst_prediction_depth >= s->prediction_depth);
        CHECK(s->max_prediction == 16);
        CHECK(s->rx_packets > 0);
        // The redundancy window means most arriving input repeats what is held.
        // It is a liveness signal, explicitly NOT a loss signal.
        CHECK(s->peers[0].dup_inputs + s->peers[1].dup_inputs > 0);
    }
    // Neither peer diverged while all of this was being counted.
    CHECK_FALSE(p.a.desynced());
    CHECK_FALSE(p.b.desynced());
}

TEST_CASE("net stats: a stall is counted only when the prediction cap really holds a pump") {
    // "Stutter" is not a feeling once this number exists: a pump the session was
    // not allowed to simulate is exactly what the player sees as a hitch.
    net::LoopbackLink link(/*latency=*/0);
    net::LoopbackTransport ta(link, 0);
    net::LoopbackTransport tb(link, 1);
    sim::Simulation sa(open_config());
    sim::Simulation sb(open_config());
    net::RollbackSession a(sa, kSeat0, kBoth, /*max_prediction=*/4, ta);
    net::RollbackSession b(sb, kSeat1, kBoth, /*max_prediction=*/4, tb);

    // Peer B never pumps: A speculates to its cap and then cannot move.
    std::int64_t now = 0;
    for (int i = 0; i < 20; ++i) {
        a.advance(seat_input(0, scripted(0, a.predicted_tick())), now);
        link.step();
        now += kPumpMs;
    }
    CHECK(a.predicted_tick() - a.confirmed_tick() == 4);
    CHECK(a.stats().stall_pumps > 0);
    CHECK(a.stats().prediction_depth == 4);
    CHECK(a.stats().peers[1].lag_ticks > 0);

    // B catching up clears the stall: the counter is a record of pumps lost, and
    // the frontier moves again.
    const std::uint32_t stalled = a.stats().stall_pumps;
    for (int i = 0; i < 20; ++i) {
        b.advance(seat_input(1, scripted(1, b.predicted_tick())), now);
        a.advance(seat_input(0, scripted(0, a.predicted_tick())), now);
        link.step();
        now += kPumpMs;
    }
    CHECK(a.confirmed_tick() > 0);
    // NOT ONE further stall: the cap is tested BEFORE simulating, so a peer that
    // is keeping up always finds room. (The reported depth still sits AT the cap
    // afterwards — the pump raises the head by one after the test passes — which
    // is exactly why the stall COUNT, not the depth, is the stutter signal.)
    CHECK(a.stats().stall_pumps == stalled);
    CHECK(a.stats().prediction_depth <= 4);
}

TEST_CASE("net stats: instrumenting the session does not perturb the session") {
    // The hard requirement. Same scenario, once with a clock and once without;
    // if a single simulated tick differed the hashes would part company.
    Pair clocked(/*latency=*/4, /*max_prediction=*/12, /*with_clock=*/true);
    Pair blind(/*latency=*/4, /*max_prediction=*/12, /*with_clock=*/false);
    clocked.pump_n(400);
    blind.pump_n(400);

    CHECK(clocked.a.hash() == blind.a.hash());
    CHECK(clocked.b.hash() == blind.b.hash());
    CHECK(clocked.a.predicted_tick() == blind.a.predicted_tick());
    CHECK(clocked.b.predicted_tick() == blind.b.predicted_tick());
    CHECK(clocked.a.confirmed_tick() == blind.a.confirmed_tick());
    CHECK(clocked.b.confirmed_tick() == blind.b.confirmed_tick());
    // The tick-derived counters are identical too — they need no clock.
    CHECK(clocked.a.stats().rollbacks == blind.a.stats().rollbacks);
    CHECK(clocked.a.stats().resim_ticks == blind.a.stats().resim_ticks);
    CHECK(clocked.a.stats().peers[1].lag_ticks == blind.a.stats().peers[1].lag_ticks);
    // Without a clock the time-derived ones stay UNAVAILABLE rather than reading
    // as a confident zero, which is the whole reason for the -1 sentinel.
    CHECK_FALSE(blind.a.stats().clocked);
    CHECK(blind.a.stats().peers[1].rtt_ms == -1);
    CHECK(blind.a.stats().peers[1].rtt_min_ms == -1);
    CHECK(clocked.a.stats().clocked);
    CHECK(clocked.a.stats().peers[1].rtt_ms > 0);
}

TEST_CASE("net stats: the round tick base does not confuse the diagnostics") {
    // A multi-round match bases each round far up the tick space
    // (round_rotation.hpp). Lag and the ack-RTT are both relative, so nothing
    // may read the base itself as 400 000 ticks of lag on pump one.
    constexpr std::uint32_t kBase = 100u << 22;
    net::LoopbackLink link(/*latency=*/2);
    net::LoopbackTransport ta(link, 0);
    net::LoopbackTransport tb(link, 1);
    sim::Simulation sa(open_config());
    sim::Simulation sb(open_config());
    net::RollbackSession a(sa, kSeat0, kBoth, 16, ta, {}, kBase);
    net::RollbackSession b(sb, kSeat1, kBoth, 16, tb, {}, kBase);
    std::int64_t now = 500;
    for (int i = 0; i < 120; ++i) {
        a.advance(seat_input(0, scripted(0, a.predicted_tick())), now);
        b.advance(seat_input(1, scripted(1, b.predicted_tick())), now);
        link.step();
        now += kPumpMs;
    }
    CHECK(a.stats().tick >= kBase);
    CHECK(a.stats().peers[1].lag_ticks < 32);
    CHECK(b.stats().peers[0].lag_ticks < 32);
    CHECK(a.stats().peers[1].rtt_ms > 0);
    CHECK(b.stats().peers[0].rtt_ms > 0);
}

TEST_CASE("net stats: the end-of-session log line is one greppable record") {
    // What the owner actually pastes back to us after "it suddenly cut out".
    // Pinned literally, because a log format that drifts is a log nobody can
    // read two builds later.
    net::SessionSummary s;
    s.timestamp = "2026-07-29 14:03:11";
    s.reason = net::SessionEndReason::Desync;
    s.round = 2;
    s.is_host = true;
    s.local_seats = 0x1;
    s.all_seats = 0x3;
    s.stats.path = net::NetPath::Relayed;
    s.stats.clocked = true;
    s.stats.elapsed_ms = 92500;
    s.stats.tick = 1264;
    s.stats.confirmed = 1201;
    s.stats.prediction_depth = 63;
    s.stats.max_prediction = 64;
    s.stats.worst_prediction_depth = 64;
    s.stats.stall_pumps = 418;
    s.stats.rollbacks = 97;
    s.stats.resim_ticks = 612;
    s.stats.rx_packets = 24880;
    s.stats.rx_malformed = 3;
    s.stats.desynced = true;
    s.stats.desync_tick = 1201;
    net::PeerStats& p = s.stats.peers[1];
    p.tracked = true;
    p.live = true;
    p.lag_ticks = 63;
    p.worst_lag_ticks = 71;
    p.rtt_ms = 214;
    p.rtt_min_ms = 188;
    p.rtt_max_ms = 940;
    p.jitter_ms = 41;
    p.input_packets = 12440;
    p.recv_per_sec = 18;
    p.dup_inputs = 9412;
    p.loss_pct_est = 10;

    const std::string line = net::format_session_log_line(s);
    CHECK(line ==
          "2026-07-29 14:03:11 netdiag end=desync path=relayed host=1 round=2 "
          "local_seats=0x001 all_seats=0x003 elapsed=92s tick=1264 confirmed=1201 depth=63/64 "
          "depth_max=64 stalls=418 rollbacks=97 resim_ticks=612 rx=24880 bad=3 desync_tick=1201 "
          "| seat1 live=1 lag=63t lag_max=71t rtt=214/188/940(last/min/max)ms jitter=41ms "
          "rx=12440(18/s) dup=9412 loss~10%\n");
    // No [OFFSET-BOUND] marker on a peer that is not one: the marker has to mean
    // something when it does appear.
    CHECK(line.find("OFFSET-BOUND") == std::string::npos);

    // Exactly one record: a newline at the end and nowhere else, so a grep for
    // "netdiag end=" returns whole lines however the note was written.
    CHECK(line.find('\n') == line.size() - 1);
}

TEST_CASE("net stats: a note cannot split the log record, and a blind session says so") {
    net::SessionSummary s;
    s.timestamp = "2026-07-29 14:03:11";
    s.reason = net::SessionEndReason::WindowClosed;
    s.note = "peer said\nthis\r\nover two lines";
    const std::string line = net::format_session_log_line(s);
    CHECK(line.find('\n') == line.size() - 1);
    CHECK(line.find("note=peer said this  over two lines") != std::string::npos);
    // No clock was ever supplied, and the line admits it rather than reporting
    // a confident 0 ms RTT.
    CHECK(line.find("clock=none") != std::string::npos);
    CHECK(line.find("end=window-closed") != std::string::npos);
}

TEST_CASE("net stats: every end reason has a distinct, stable name") {
    // These strings are the grep keys; a collision would silently merge two
    // different failures in the log.
    const net::SessionEndReason all[] = {
        net::SessionEndReason::Unknown,        net::SessionEndReason::MatchCompleted,
        net::SessionEndReason::RoundAbandoned, net::SessionEndReason::Desync,
        net::SessionEndReason::PeerDropped,    net::SessionEndReason::PeerLostBetweenRounds,
        net::SessionEndReason::WindowClosed,   net::SessionEndReason::LeftSession};
    std::vector<std::string> seen;
    for (const net::SessionEndReason r : all) {
        const std::string n = net::end_reason_name(r);
        CHECK(!n.empty());
        for (const std::string& other : seen) CHECK(other != n);
        seen.push_back(n);
    }
}

TEST_CASE("net stats: a real session's numbers survive the trip into a log line") {
    // End to end: drive both peers, snapshot the loser's stats, and check the
    // formatted record carries the live values rather than a default-constructed
    // struct (the mistake that would make every log line useless and identical).
    Pair p(/*latency=*/3, /*max_prediction=*/16, /*with_clock=*/true);
    p.pump_n(200);

    net::SessionSummary s;
    s.stats = p.a.stats();
    s.timestamp = "2026-07-29 14:10:00";
    s.reason = net::SessionEndReason::MatchCompleted;
    s.local_seats = kSeat0;
    s.all_seats = kBoth;
    const std::string line = net::format_session_log_line(s);

    CHECK(line.find("end=match-completed") != std::string::npos);
    CHECK(line.find("path=loopback") != std::string::npos);
    CHECK(line.find("| seat1 ") != std::string::npos);
    CHECK(line.find("| seat0 ") == std::string::npos);  // seat 0 is ours, not a peer
    CHECK(line.find("clock=none") == std::string::npos);
    CHECK(line.find("tick=0 ") == std::string::npos);  // the session really ran
}
