// ARRIVAL VARIANCE (network JITTER) — the regression behind the second live
// report, and the one variable that separated the good sessions from the bad.
//
// THE MEASUREMENTS THIS FILE IS WRITTEN AGAINST. netdiag.log holds 13 real
// Turkey<->Lithuania sessions from one day. Discarding every reading flagged
// [OFFSET-BOUND,NOT-PATH] (those measure the peers' wall-clock phase offset, not
// the wire, so they cannot be compared), what is left is:
//
//     time   rtt min/max     jitter   loss   stalls   rollbacks
//     17:00   80 / 151 ms      5 ms     0%      0          0
//     18:47  130 / 250 ms      5 ms     0%      0         80
//     18:48   99 / 150 ms      0 ms     0%      0          0
//     19:39  156 / 350 ms      6 ms     0%      0         76
//     21:36   83 / 845 ms     99 ms     0%      0         23
//     21:40   80 / 762 ms     85 ms    10%      0          3
//     21:42   83 / 706 ms     87 ms     0%      0        123
//
// The owner independently called the early sessions silky and the late ones badly
// laggy, which matches the split exactly. BASE RTT BARELY MOVED — 80-156 ms in
// both groups — so anything keyed to mean latency would be aimed at the wrong
// number. JITTER moved 15-20x, and it is the only thing that did.
//
// WHAT THIS SUITE FOUND, and therefore pins. Jitter's damage was not the packets:
// it was the FRAME-ADVANTAGE RE-PHASE CONTROLLER reading them. That controller
// (rollback_session.hpp's re-phasing note) compares our prediction depth against
// the peer's own and holds a displayed tick when the difference exceeds two,
// which is right for a standing clock skew and wrong for a burst of late
// arrivals — both halves of the comparison are instantaneous samples and under
// real jitter both are noise. Measured here at the live conditions, BEFORE the
// change: the pair spent 102 of 600 pumps holding and ran the match at 16.6 ticks
// a second instead of 20, ON BOTH MACHINES. Not a stutter — the whole game in
// slow motion, self-inflicted, with `stalls` reading near zero throughout
// because a re-phase hold returns before the cap check and so hides the stall it
// replaces. (That is also why the live table's stalls column proves less than it
// looks.)
//
// The two cures, neither of which touches the wire (see rollback_session.hpp):
//   * FILTER the controller — act on the minimum of the advantage across a
//     window, so a permanent skew still moves it and a burst never does;
//   * the LOCAL LEAD — file and send our own input a tick or two ahead of our own
//     head when, and only when, the peer's own depth is measurably bouncing, so
//     that much arrival variance costs the peer no prediction at all.
//
// Both peers are driven over the in-memory MsLink for every case, on INDEPENDENT
// wall clocks, because a one-sided test says nothing about arrival timing; the
// last case repeats the guarantee over the coarser LoopbackLink the older suites
// use. NOTHING HERE HAS MET A REAL NAT OR A REAL CONGESTED PATH — the table above
// came from live play, which is the only place this will really be judged.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>
#include <cstdio>

#include "bomber/net/migrating_transport.hpp"
#include "bomber/net/rollback_session.hpp"
#include "bomber/net/transport.hpp"
#include "fanout_bus.hpp"
#include "helpers.hpp"
#include "ms_clock_harness.hpp"

using namespace bomber;  // NOLINT(google-build-using-namespace) — test-local
using bomber::net::test::Driver;
using bomber::net::test::MsLink;
using bomber::net::test::MsTransport;
using bomber::sim::test::open_config;

namespace {

constexpr std::uint16_t kSeat0 = 0x1;
constexpr std::uint16_t kSeat1 = 0x2;
constexpr std::uint16_t kBoth = 0x3;

// The live path's floor: ~100 ms RTT, where every one of the 13 sessions sat
// (min 80-156 ms) whether it felt silky or awful.
constexpr int kBaseOneWayMs = 50;

struct Scenario {
    const char* label = "";
    int one_way_ms = kBaseOneWayMs;
    int spread_ms = 0;    // peak extra delay a packet can draw
    int persist_pct = 0;  // chance it inherits the previous packet's delay
    int run_ms = 30000;
    int frame_ms = 6;
    // A path that only bursts for part of the round: the variance is switched on
    // at `burst_from_ms` and off again at `burst_to_ms`, so the absorber has to
    // engage AND retreat inside one session.
    int burst_from_ms = -1;
    int burst_to_ms = -1;
    // WHICH REALISATION of the delay process. A jittery run is chaotic: the
    // arrival schedule is consumed in send order, so one extra held tick
    // reshuffles every later delay and the two builds walk into different
    // histories. Any assertion about a jittery path therefore has to be made over
    // a SWEEP of these, never over one (see kSeeds).
    unsigned seed = 0x1234567U;
};

// Enough realisations that a single unlucky one cannot carry a verdict, few
// enough that the suite stays a couple of seconds. Arbitrary constants, chosen
// once and then left alone — re-picking them after seeing a result would be
// fitting the test to the answer.
constexpr unsigned kSeeds[] = {0x1234567U, 0xA5A5A5A5U, 0x0BADF00DU, 0x51EED00DU, 0xC0FFEE11U};

struct Side {
    int ticks = 0;
    std::uint32_t stalls = 0, rephase = 0, absorbed = 0, lead_pumps = 0;
    std::uint32_t rollbacks = 0, resim = 0;
    int depth_max = 0, lag = 0, lag_max = 0, spread = 0, lead = 0;
    int jitter_ms = 0, rtt_min = -1, rtt_max = -1;

    // What the player actually feels: pumps on which the display could not be
    // advanced, whichever mechanism stopped it. A re-phase hold returns before the
    // cap check, so the two counters never overlap and adding them is exact.
    std::uint32_t held() const { return stalls + rephase; }
};

struct Result {
    Side a, b;
    int modelled_delta_ab = 0, modelled_delta_ba = 0;
    bool desynced = false;
    std::uint32_t confirmed_a = 0, confirmed_b = 0;
};

Side read(const net::RollbackSession& s, int ticks, int peer_seat) {
    const net::NetStats& n = s.stats();
    const net::PeerStats& p = n.peers[static_cast<std::size_t>(peer_seat)];
    Side v;
    v.ticks = ticks;
    v.stalls = n.stall_pumps;
    v.rephase = n.rephase_holds;
    v.absorbed = n.rephase_suppressed;
    v.lead_pumps = n.lead_pumps;
    v.lead = n.local_lead;
    v.spread = n.peer_depth_spread;
    v.rollbacks = n.rollbacks;
    v.resim = n.resim_ticks;
    v.depth_max = n.worst_prediction_depth;
    v.lag = p.lag_ticks;
    v.lag_max = p.worst_lag_ticks;
    v.jitter_ms = p.jitter_ms;
    v.rtt_min = p.rtt_min_ms;
    v.rtt_max = p.rtt_max_ms;
    return v;
}

Result measure(const Scenario& sc) {
    std::int64_t now = 0;
    const bool windowed = sc.burst_from_ms >= 0;
    MsLink link(sc.one_way_ms, sc.one_way_ms, windowed ? 0 : sc.spread_ms,
                windowed ? 0 : sc.persist_pct, sc.seed);
    MsTransport ta(link, 0, now);
    MsTransport tb(link, 1, now);
    sim::Simulation sa(open_config());
    sim::Simulation sb(open_config());
    net::RollbackSession a(sa, kSeat0, kBoth, /*max_prediction=*/8, ta);
    net::RollbackSession b(sb, kSeat1, kBoth, /*max_prediction=*/8, tb);

    Driver da;
    da.seat = 0;
    da.frame_ms = sc.frame_ms;
    Driver db;
    db.seat = 1;
    db.frame_ms = sc.frame_ms;
    const std::uint32_t t0a = a.predicted_tick();
    const std::uint32_t t0b = b.predicted_tick();

    for (now = 0; now <= sc.run_ms; ++now) {
        if (windowed && now == sc.burst_from_ms) link.set_jitter(sc.spread_ms, sc.persist_pct);
        if (windowed && now == sc.burst_to_ms) link.set_jitter(0, 0);
        if (now >= da.next_frame) da.frame(a, now);
        if (now >= db.next_frame) db.frame(b, now);
    }

    Result r;
    r.a = read(a, static_cast<int>(a.predicted_tick() - t0a), 1);
    r.b = read(b, static_cast<int>(b.predicted_tick() - t0b), 0);
    r.modelled_delta_ab = link.mean_abs_delta_ms(0);
    r.modelled_delta_ba = link.mean_abs_delta_ms(1);
    // The session's OWN per-tick confirmed-hash exchange is the parity check: it
    // compares every confirmed tick on both machines and latches on the first
    // disagreement. Nothing a test could assert afterwards is stronger.
    r.desynced = a.desynced() || b.desynced();
    r.confirmed_a = a.confirmed_tick();
    r.confirmed_b = b.confirmed_tick();
    return r;
}

void trace(const Scenario& sc, const Result& r) {
    std::printf("  %-26s modelled |dd|=%d/%d ms\n", sc.label, r.modelled_delta_ab,
                r.modelled_delta_ba);
    const auto row = [&](const char* who, const Side& v) {
        std::printf(
            "    %s %5.2f t/s  jitter=%3dms rtt=%d..%d | HELD %3u (rephase %u + stall %u), "
            "absorbed %3u | lead %dt for %3u pumps, spread %dt | rb %3u resim %4u depth_max %d\n",
            who, v.ticks * 1000.0 / sc.run_ms, v.jitter_ms, v.rtt_min, v.rtt_max, v.held(),
            v.rephase, v.stalls, v.absorbed, v.lead, v.lead_pumps, v.spread, v.rollbacks, v.resim,
            v.depth_max);
    };
    row("A", r.a);
    row("B", r.b);
}

Scenario clean_path() {
    Scenario sc;
    sc.label = "clean (0 ms jitter)";
    return sc;
}

// The silky group: <=6 ms reported jitter over an ~100 ms path.
Scenario low_jitter() {
    Scenario sc;
    sc.label = "low jitter (live: 5 ms)";
    sc.spread_ms = 16;
    return sc;
}

// The laggy group. `spread`/`persist` are calibrated against TWO live observables
// at once — the reported jitter (85-99 ms) and the reported max RTT (706-845 ms
// over an ~80 ms floor) — which is what pins them to one plausible pair rather
// than to a knob nobody can check. Correlated, not white: a datagram delayed on
// its own is superseded by the next one, which carries its ticks too, so it is
// RUNS of late packets that stall a confirmation frontier.
Scenario high_jitter() {
    Scenario sc;
    sc.label = "high jitter (live: 90 ms)";
    sc.spread_ms = 420;
    sc.persist_pct = 35;
    return sc;
}

}  // namespace

TEST_CASE("jitter: the modelled conditions reproduce the live readings") {
    // Calibration, ASSERTED rather than assumed. Everything below is only evidence
    // about the live table if the harness actually lands on the live table's
    // numbers, so that is checked first and on its own.
    const Scenario lo = low_jitter();
    const Result rl = measure(lo);
    trace(lo, rl);
    const Scenario hi = high_jitter();
    const Result rh = measure(hi);
    trace(hi, rh);

    CHECK(rl.a.jitter_ms <= 8);  // the silky sessions reported 0-6 ms
    CHECK(rl.a.rtt_min >= 100);  // ...over a path whose floor is ~100 ms RTT
    CHECK(rl.a.rtt_max <= 260);  // ...and which never spiked (live: 151-350 ms)
    // The laggy sessions: an ~80 ms floor with 700-850 ms excursions on it. The
    // reported jitter is checked only for ORDER OF MAGNITUDE, because with a lead
    // in force the ack-RTT stops measuring the wire and says so — see the
    // [OFFSET-BOUND,NOT-PATH] note in net_stats.hpp, and `spread` below, which is
    // the arrival-variance reading that survives.
    CHECK(rh.a.rtt_max >= 600);
    CHECK(rh.b.rtt_max >= 600);
    // The lead-invariant proof that the variance was SEEN: the absorber only ever
    // spends input lag against a measured spread in the peer's own depth, so a
    // non-zero count here is that measurement, cumulative rather than an
    // end-of-run instant.
    CHECK(rh.a.lead_pumps > 0);
    CHECK(rh.b.lead_pumps > 0);
    CHECK(rl.a.lead_pumps == 0);  // and the silky condition never triggered it
    CHECK(rl.b.lead_pumps == 0);
    CHECK_FALSE(rl.desynced);
    CHECK_FALSE(rh.desynced);
}

TEST_CASE("jitter: a clean path is BYTE-IDENTICAL to the build before the absorber") {
    // THE OWNER'S NON-NEGOTIABLE CONDITION. At the jitter he is happy with, the
    // behaviour must be indistinguishable from what he already has — so these are
    // not tolerances, they are the exact counters MEASURED ON THE PRE-CHANGE BUILD
    // (main at f9a45bf, this same scenario), re-asserted against the new one. Any
    // divergence at all, in either direction, fails.
    //
    // It is not a coincidence that they hold; both mechanisms are inert here by
    // construction, and each has its own witness in the numbers below:
    //   * the filter can only ever hold LESS than the raw controller (a minimum is
    //     never above the current sample) and the raw controller never fires on a
    //     clean path — so `rephase` and `absorbed` are both 0 and every branch is
    //     the branch it was;
    //   * the lead is driven by the SPREAD of the peer's own prediction depth,
    //     which on a steady path is inside the deadband — so `lead_pumps` is 0 and
    //     `local_next_` tracks `tick_` exactly, which is the old code.
    for (const Scenario& sc : {clean_path(), low_jitter()}) {
        const Result r = measure(sc);
        trace(sc, r);
        const bool clean = sc.spread_ms == 0;

        CHECK(r.a.lead_pumps == 0);  // no input lag was spent
        CHECK(r.b.lead_pumps == 0);
        CHECK(r.a.absorbed == 0);  // the filter never had to refuse anything
        CHECK(r.b.absorbed == 0);
        CHECK(r.a.rephase == 0);  // ...because nothing ever asked
        CHECK(r.b.rephase == 0);
        CHECK(r.a.stalls == 0);
        CHECK(r.b.stalls == 0);
        CHECK(r.a.ticks == 600);  // 30 s at a full 20 Hz, both machines
        CHECK(r.b.ticks == 600);
        CHECK(r.a.depth_max == 2);  // the path in ticks, and nothing else
        CHECK(r.b.depth_max == 2);
        CHECK(r.a.rollbacks == (clean ? 399u : 544u));
        CHECK(r.b.rollbacks == (clean ? 399u : 533u));
        CHECK(r.a.resim == (clean ? 798u : 1088u));
        CHECK(r.b.resim == (clean ? 798u : 1066u));
        CHECK_FALSE(r.desynced);
    }
}

TEST_CASE("jitter: high arrival variance costs measurably less than it did") {
    // THE POINT OF THE WHOLE CHANGE, at the live laggy condition. All figures here
    // are MEANS over kSeeds x both peers, measured on the tree at the host
    // migration merge; the baseline row is this same build with both mechanisms
    // ablated, so it is what main does today rather than a remembered number.
    //
    //     neither (= main)   16.57 t/s | HELD 102 (rephase 98) | resim 1182
    //     filter only        19.07 t/s | HELD  28 (rephase  6) | resim 1397
    //     lead only          17.30 t/s | HELD  80 (rephase 80) | resim  778
    //     both               19.57 t/s | HELD  12 (rephase  9) | resim  977
    //
    // 102 of 600 pumps is 17% of the round the display was not allowed to advance,
    // and it is the slow motion: 16.6 ticks a second against a wall clock that
    // delivered 20. Nearly all of it is the re-phase controller firing on arrival
    // noise rather than on any real clock skew.
    //
    // THE TWO MECHANISMS DO DIFFERENT JOBS, which is why neither may be dropped as
    // "the part that was not doing much": the FILTER is what recovers the frame
    // rate (16.6 -> 19.1) and on its own makes the correction work WORSE (1182 ->
    // 1397), because the holds it removes had been suppressing re-simulation with
    // the player's frame rate. The LEAD is what recovers the correction work
    // (1397 -> 977 on top of the filter) and on its own barely touches the rate.
    //
    // AN EARLIER VERSION OF THIS COMMENT CLAIMED THE LEAD ALONE WAS WORSE THAN
    // NEITHER. That was one realisation of a chaotic system read as a trend; over
    // the seed sweep it is not true (17.30 against 16.57). Left recorded because
    // the mistake is the reason this case sweeps at all.
    //
    // ROLLBACK COUNT IS DELIBERATELY NOT ASSERTED DOWN. Bursts COALESCE
    // corrections — a run of late inputs lands together and is fixed by one deep
    // rollback — so the jittery condition shows FEWER rollbacks than the clean one
    // while doing far more work per rollback. `resim_ticks` is the honest measure
    // of correction cost, and the held-pump count is the honest measure of what
    // the player feels.
    // ASSERTED OVER A SEED SWEEP, NOT OVER ONE RUN, and that is not belt-and-
    // braces. A jittery path is CHAOTIC with respect to the build under test: the
    // delay schedule is consumed in send order, so a single extra held tick
    // reshuffles every later arrival and the two builds walk into genuinely
    // different histories. A per-seed threshold would be fitted to noise — the
    // first version of this case was, and it broke on a change that improved
    // every mean. So each figure below is a MEAN over kSeeds, and the bounds are
    // set well clear of the spread rather than snug against one realisation.
    long long held = 0, rephase = 0, absorbed = 0, lead_pumps = 0, resim = 0, ticks = 0, rb = 0;
    int runs = 0;
    for (const unsigned seed : kSeeds) {
        Scenario sc = high_jitter();
        sc.seed = seed;
        const Result r = measure(sc);
        trace(sc, r);
        CHECK_FALSE(r.desynced);
        for (const Side& v : {r.a, r.b}) {
            held += v.held();
            rephase += v.rephase;
            absorbed += v.absorbed;
            lead_pumps += v.lead_pumps;
            resim += v.resim;
            ticks += v.ticks;
            rb += v.rollbacks;
            ++runs;
        }
    }
    const auto mean = [runs](long long total) { return total / runs; };
    std::printf(
        "  MEAN over %d peer-runs: %.2f t/s | HELD %lld (rephase %lld) | absorbed %lld | "
        "lead_pumps %lld | rb %lld resim %lld\n",
        runs, static_cast<double>(mean(ticks)) / 30.0, mean(held), mean(rephase), mean(absorbed),
        mean(lead_pumps), mean(rb), mean(resim));

    CHECK(mean(held) <= 30);          // was 71
    CHECK(mean(rephase) <= 25);       // was 63
    CHECK(mean(absorbed) >= 40);      // holds the filter refused: the absorber's work
    CHECK(mean(lead_pumps) >= 200);   // the lead was earned and spent
    CHECK(mean(resim) <= 1150);       // was 1288
    CHECK(mean(ticks) >= 575);        // 19.2 t/s; was 529, i.e. 17.63 t/s
}

TEST_CASE("jitter: the lead may be raised and lowered inside a live round") {
    // THE RISKY PATH, pinned. The lead changes the tick a local sample is filed
    // against, and the peer may already hold — and have CONFIRMED AND HASHED — an
    // input we filed. So the invariant is that a filed tick is never re-decided:
    // raising the lead files the current sample twice and lowering it files
    // nothing for one pump. This drives the path a steady scenario never touches —
    // a clean opening, a burst in the middle, a clean close — so the lead ratchets
    // up and back down while the round is running.
    //
    // The assertion that matters is the desync check, which is the session's own
    // per-tick confirmed-hash exchange between the two peers, not something this
    // file computes afterwards.
    Scenario sc;
    sc.label = "burst mid-round, then clean";
    sc.spread_ms = 420;
    sc.persist_pct = 35;
    sc.burst_from_ms = 8000;
    sc.burst_to_ms = 20000;
    sc.run_ms = 30000;
    const Result r = measure(sc);
    trace(sc, r);

    CHECK_FALSE(r.desynced);
    CHECK(r.a.lead_pumps > 0);  // it engaged during the burst...
    CHECK(r.b.lead_pumps > 0);
    CHECK(r.a.lead == 0);  // ...and gave the input lag back afterwards
    CHECK(r.b.lead == 0);
    // Both peers covered the whole round: neither was left behind by the other's
    // lead changing under it.
    CHECK(r.a.ticks >= 580);
    CHECK(r.b.ticks >= 580);
    CHECK(r.confirmed_a > 550);
    CHECK(r.confirmed_b > 550);
}

namespace {

// A 3-seat star peer, the same shape tests/net/test_host_migration.cpp uses:
// its own sim, its own session, and the MigratingTransport indirection that lets
// the far end be re-pointed without the session learning that it moved.
constexpr std::uint16_t kAll3 = 0x7;
constexpr int kStarTimeout = 40;  // pumps of silence before a seat is declared dropped

sim::MatchConfig three_config() {
    sim::MatchConfig cfg = open_config();
    cfg.spawns = {{0, 0}, {14, 10}, {14, 0}};
    cfg.player_count = 3;
    return cfg;
}

struct StarPeer {
    StarPeer(std::size_t endpoint, int seat, test::StarBus& bus, int host_seat)
        : sim(three_config()),
          link(bus, endpoint),
          wire(&link),
          session(sim, static_cast<std::uint16_t>(1U << seat), kAll3, /*max_prediction=*/8, wire,
                  net::DropPolicy{/*revert_to_ai=*/true, /*is_host=*/seat == host_seat,
                                  kStarTimeout, /*host_seat=*/host_seat}),
          seat_index(seat) {}

    void pump(std::uint32_t tick) {
        session.advance(net::test::seat_input(seat_index, net::test::scripted(seat_index, tick)));
    }

    sim::Simulation sim;
    test::StarTransport link;
    net::MigratingTransport wire;
    net::RollbackSession session;
    int seat_index;
};

}  // namespace

TEST_CASE("jitter: a hub dies while the absorber is holding a lead") {
    // THE SEAM BETWEEN THE TWO FEATURES, which neither suite covered on its own:
    // tests/net/test_host_migration.cpp runs on a flat-latency star, so the lead
    // never engages there and its migrations are all measured with the absorber
    // dormant; everything above this runs two peers with no hub to lose.
    //
    // The concern is concrete: a migration freezes `peer_depth_` at whatever the
    // dying hub last relayed, and the step back to live values after the heal
    // looks exactly like arrival variance — so an absorber reading it could buy
    // input lag for a peer whose real problem is that it is behind and needs to
    // catch up. `rephase_eligible()` refuses while `migration_healing()`, and the
    // advantage window is discarded rather than padded, so both mechanisms go
    // quiet together. What this case actually PROVES is the weaker, still
    // unproven-until-now claim that the two features coexist; see the coverage
    // note at the bottom for the part it does not reach.
    test::StarBus bus(3, /*latency=*/1, /*jitter_steps=*/6, /*persist_pct=*/35);
    StarPeer host(0, 0, bus, /*host_seat=*/0);
    StarPeer g1(1, 1, bus, /*host_seat=*/0);
    StarPeer g2(2, 2, bus, /*host_seat=*/0);

    std::uint32_t t = 0;
    for (; t < 120; ++t) {
        host.pump(t);
        g1.pump(t);
        g2.pump(t);
        bus.step();
    }
    // The premise: the link really is jittery enough that the absorber engaged.
    // Without this the rest of the case would pass vacuously on a dormant lead.
    REQUIRE(g1.session.stats().lead_pumps > 0);
    REQUIRE(g2.session.stats().lead_pumps > 0);

    const std::uint32_t holds_before_1 = g1.session.stats().rephase_holds;
    const std::uint32_t holds_before_2 = g2.session.stats().rephase_holds;

    bus.kill(0);  // the hub stops responding, and with it g1 <-> g2

    for (int i = 0; i < kStarTimeout + 120; ++i, ++t) {
        g1.pump(t);
        g2.pump(t);
        if (g1.session.current_hub() == 1 && bus.hub() != 1) bus.set_hub(1);
        bus.step();
    }

    // The migration still happens, and still converges, with the absorber live.
    CHECK(g1.session.current_hub() == 1);
    CHECK(g2.session.current_hub() == 1);
    CHECK_FALSE(g1.session.desynced());
    CHECK_FALSE(g2.session.desynced());
    CHECK(g1.session.confirmed_tick() > 120);
    CHECK(g2.session.confirmed_tick() > 120);
    // ...and they agree about the history, which is what `desynced()` above
    // already proves tick by tick. Their FRONTIERS are deliberately only required
    // to be close, not equal: on a jittery link two peers' confirmation frontiers
    // legitimately sit a tick or two apart at any given instant — that is the
    // subject of this whole file — so pinning them equal at an arbitrary stopping
    // point would be asserting the absence of the thing being studied.
    const std::uint32_t c1 = g1.session.confirmed_tick();
    const std::uint32_t c2 = g2.session.confirmed_tick();
    CHECK((c1 > c2 ? c1 - c2 : c2 - c1) <= 4);

    // The controller does not run away across the outage: a hold storm would show
    // here as a count approaching the window length. Bounded, not pinned — see the
    // coverage note below for what this does and does not establish.
    const std::uint32_t holds_1 = g1.session.stats().rephase_holds - holds_before_1;
    const std::uint32_t holds_2 = g2.session.stats().rephase_holds - holds_before_2;
    std::printf("  migration under a lead: holds during outage+heal = %u / %u of %d pumps\n",
                holds_1, holds_2, kStarTimeout + 120);
    CHECK(holds_1 <= 60);
    CHECK(holds_2 <= 60);

    // HONEST COVERAGE NOTE, in the spirit of the one resend_from() carries in
    // rollback_session.cpp. This case establishes that the two features COEXIST —
    // a hub dies while the absorber is holding a lead, and the survivors still
    // elect, converge and agree. It does NOT isolate the `migration_healing()`
    // guard in rephase_eligible(): deleting that guard leaves this case green, and
    // leaves the hold counts above statistically unchanged (measured 21/16 with
    // the guard against 22/15 without it).
    //
    // The reason is worth writing down rather than rediscovering. The holds that
    // happen here are in the window BEFORE the drop is declared, where the
    // frontier is already pinned but `host_lost_` is still zero, so the guard does
    // not apply — and there a hold is indistinguishable from the cap stall it
    // replaces, because the display cannot advance either way. The guard covers
    // the HEAL that follows, and reaching a state where it changes the outcome
    // needs a survivor still making progress while healing, which this topology
    // does not produce. Kept because the reasoning behind it is sound and the cost
    // is a suppressed hold; treat it as defensive, and if it ever needs
    // justifying, build that case first.
}

TEST_CASE("jitter: the older synchronous LoopbackLink rig sees no change at all") {
    // The coarser rig every other RollbackSession suite uses: both peers pumped
    // inside one step(), latency counted in whole pumps, no arrival variance
    // expressible at all. It is here as the second witness for "a clean path is
    // untouched" — on a link with no variance to measure, neither mechanism can
    // find a reason to act, and this is the harness in which the rest of tests/net
    // asserts its behaviour.
    net::LoopbackLink link(/*latency=*/2);
    net::LoopbackTransport ta(link, 0);
    net::LoopbackTransport tb(link, 1);
    sim::Simulation sa(open_config());
    sim::Simulation sb(open_config());
    net::RollbackSession a(sa, kSeat0, kBoth, /*max_prediction=*/8, ta);
    net::RollbackSession b(sb, kSeat1, kBoth, /*max_prediction=*/8, tb);

    for (int i = 0; i < 400; ++i) {
        a.advance(net::test::seat_input(0, net::test::scripted(0, a.predicted_tick())));
        b.advance(net::test::seat_input(1, net::test::scripted(1, b.predicted_tick())));
        link.step();
    }

    CHECK_FALSE(a.desynced());
    CHECK_FALSE(b.desynced());
    CHECK(a.stats().lead_pumps == 0);
    CHECK(b.stats().lead_pumps == 0);
    CHECK(a.stats().rephase_holds == 0);
    CHECK(b.stats().rephase_holds == 0);
    CHECK(a.stats().rephase_suppressed == 0);
    CHECK(b.stats().rephase_suppressed == 0);
    CHECK(a.predicted_tick() == 400);
    CHECK(b.predicted_tick() == 400);
}
