// ROLLBACK PACING on INDEPENDENT wall clocks — the standing-lag regression.
//
// Every other RollbackSession suite pumps both peers inside one synchronous
// step(), which can express neither a per-machine wall-clock skew nor the frame
// loop the pump is nested inside. Both turned out to matter. A live
// Turkey<->Lithuania record showed a match pinned at the prediction cap for a
// whole round — depth=8/8, lag=lag_max=8, rollbacks=0 (the peer was never WRONG,
// only late) — on a ~100 ms path whose healthy depth is 2, and nothing in the
// synchronous harness could reproduce it.
//
// What this file pins, measured 2026-07-30:
//
//  * A freeze of one peer's frame loop longer than MatchRunner's 200 ms catch-up
//    clamp has its excess wall time DISCARDED, so that peer falls PERMANENTLY
//    behind its partner. A 400 ms window drag cost a standing 4 ticks of
//    prediction depth for the rest of the round.
//  * The prediction cap used to be the only thing that ever gave that skew back,
//    and it engages only once the entire budget is spent — so the steady state
//    after a few hitches was "parked at the cap", where ordinary packet-timing
//    noise turns into visible stalls.
//  * With frame-advantage re-phasing (rollback_session.hpp's note) the skew is
//    shed while there is still budget in hand, and the depth returns to the path
//    baseline.
//
// Deliberately NOT asserted: exact tick counts. The point of these scenarios is
// the STANDING depth a transient event leaves behind, and pinning throughput to
// the millisecond would make the suite fragile without measuring anything more.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "bomber/net/rollback_session.hpp"
#include "helpers.hpp"
#include "ms_clock_harness.hpp"

using namespace bomber;  // NOLINT(google-build-using-namespace) — test-local
using bomber::sim::test::open_config;

// The two-peer, independent-clocks rig this file introduced now lives in
// ms_clock_harness.hpp, so tests/net/test_kill_tally.cpp can drive the SAME rig
// (it needs peers with genuinely different rollback histories for a different
// reason). Pure extract-header: MsLink/MsTransport/Driver/scripted are verbatim
// apart from Driver::frame gaining an optional post-pump callback, which this
// file does not pass.
using bomber::net::test::Driver;
using bomber::net::test::MsLink;
using bomber::net::test::MsTransport;

namespace {

constexpr std::uint16_t kSeat0 = 0x1;
constexpr std::uint16_t kSeat1 = 0x2;
constexpr std::uint16_t kBoth = 0x3;

struct Scenario {
    int one_way_ms = 25;    // A -> B
    int back_way_ms = -1;   // B -> A; -1 = symmetric
    int run_ms = 20000;
    int frame_ms = 6;      // A's frame period
    int frame_b_ms = -1;   // B's; -1 = same as A
    int frame_b_wobble = 0;  // add 0..n ms of erratic variation to B's frames
    int hitch_at_ms = -1;   // B's frame loop freezes (a window drag, an asset load)
    int hitch_ms = 0;
    int hitch_every_ms = 0;  // repeat on this period (0 = once)
    int jitter_ms = 0;
};

struct Result {
    int ticks_a = 0, ticks_b = 0;
    std::uint32_t stalls_a = 0, rephase_a = 0;
    int depth_a = 0, depth_max_a = 0, depth_max_b = 0;
    int lag_a = 0, lag_b = 0, lag_max_a = 0, lag_max_b = 0;
    int pumps_a = 0;
    bool desynced = false;
    std::uint64_t hash_a = 0, hash_b = 0;
    std::uint32_t confirmed_a = 0, confirmed_b = 0;
};

Result measure(const Scenario& sc) {
    std::int64_t now = 0;
    MsLink link(sc.one_way_ms, sc.back_way_ms < 0 ? sc.one_way_ms : sc.back_way_ms, sc.jitter_ms);
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
    db.frame_ms = sc.frame_b_ms < 0 ? sc.frame_ms : sc.frame_b_ms;
    db.wobble = sc.frame_b_wobble;
    const std::uint32_t t0a = a.predicted_tick();
    const std::uint32_t t0b = b.predicted_tick();

    for (now = 0; now <= sc.run_ms; ++now) {
        if (now >= da.next_frame) da.frame(a, now);
        bool frozen = false;
        if (sc.hitch_at_ms >= 0 && now >= sc.hitch_at_ms) {
            const std::int64_t since = now - sc.hitch_at_ms;
            frozen = sc.hitch_every_ms > 0 ? (since % sc.hitch_every_ms) < sc.hitch_ms
                                           : since < sc.hitch_ms;
        }
        if (frozen) {
            db.next_frame = now + 1;  // the loop is wedged: no frames, and `last` stays put
            continue;
        }
        if (now >= db.next_frame) db.frame(b, now);
    }

    Result r;
    r.ticks_a = static_cast<int>(a.predicted_tick() - t0a);
    r.ticks_b = static_cast<int>(b.predicted_tick() - t0b);
    r.stalls_a = a.stats().stall_pumps;
    r.rephase_a = a.stats().rephase_holds;
    r.depth_a = a.stats().prediction_depth;
    r.depth_max_a = a.stats().worst_prediction_depth;
    r.depth_max_b = b.stats().worst_prediction_depth;
    r.lag_a = a.stats().peers[1].lag_ticks;
    r.lag_b = b.stats().peers[0].lag_ticks;
    r.lag_max_a = a.stats().peers[1].worst_lag_ticks;
    r.lag_max_b = b.stats().peers[0].worst_lag_ticks;
    r.pumps_a = da.pumps;
    r.desynced = a.desynced() || b.desynced();
    r.confirmed_a = a.confirmed_tick();
    r.confirmed_b = b.confirmed_tick();
    return r;
}

void trace(const char* label, const Scenario& sc, const Result& r) {
    std::printf(
        "  %-38s A: %4.1f t/s stalls=%u rephase=%u depth=%d(max %d) lag=%d | B: %4.1f t/s lag=%d\n",
        label, r.ticks_a * 1000.0 / sc.run_ms, r.stalls_a, r.rephase_a, r.depth_a, r.depth_max_a,
        r.lag_a, r.ticks_b * 1000.0 / sc.run_ms, r.lag_b);
}

}  // namespace

TEST_CASE("pacing: a healthy path costs nothing and re-phases nothing") {
    // The baseline the other cases are measured against, and the guard that the
    // re-phasing never fires when there is no skew to shed: a symmetric path with
    // both loops running freely settles at a depth of the path delay in ticks, and
    // the session never holds a tick it did not have to.
    for (const int d : {5, 25, 50}) {
        Scenario sc;
        sc.one_way_ms = d;
        const Result r = measure(sc);
        char label[48];
        std::snprintf(label, sizeof(label), "rtt=%dms", 2 * d);
        trace(label, sc, r);

        CHECK(r.rephase_a == 0);       // nothing to shed
        CHECK(r.stalls_a == 0);        // never near the cap
        CHECK(r.depth_max_a <= 3);     // the path, and nothing else
        CHECK_FALSE(r.desynced);
        CHECK(r.ticks_a > 19 * sc.run_ms / 1000);  // real time, not slow motion
    }
}

TEST_CASE("pacing: a transient freeze does not leave a PERMANENT standing lag") {
    // THE REGRESSION. B's frame loop freezes for 400 ms — a window drag, an asset
    // stall, a Windows notification. MatchRunner's catch-up clamp discards
    // everything past 200 ms of it, so B is permanently 4 ticks behind in wall
    // clock and A is 4 ticks ahead. Before frame-advantage re-phasing that landed
    // as standing prediction depth for the whole rest of the round (measured: a
    // steady lag of 5 with the cap touched); the peers must instead give the skew
    // back and return to the path baseline.
    Scenario sc;
    sc.hitch_at_ms = 5000;
    sc.hitch_ms = 400;
    sc.run_ms = 20000;  // 15 s of running AFTER the freeze — plenty to recover in
    const Result r = measure(sc);
    trace("400ms freeze", sc, r);

    CHECK_FALSE(r.desynced);
    CHECK(r.rephase_a > 0);      // the skew was actively shed, not absorbed
    CHECK(r.lag_a <= 3);         // and the standing lag came back down (was 5)
    CHECK(r.depth_max_a < 8);    // the cap was never reached (was exactly 8)
    CHECK(r.ticks_a > 19 * sc.run_ms / 1000);
}

TEST_CASE("pacing: repeated freezes over a full round do not park the pair at the cap") {
    // The closest analogue of the live record: a 38 s round on a machine that
    // hitches every few seconds. Each freeze past the clamp used to add standing
    // depth that nothing removed, so the pair ended up living at the cap where
    // every arrival wobble is a stall (measured: lag 5, depth_max 8, 32 stalls).
    Scenario sc;
    sc.run_ms = 38000;
    sc.hitch_at_ms = 2000;
    sc.hitch_ms = 300;
    sc.hitch_every_ms = 3000;
    sc.jitter_ms = 20;
    const Result r = measure(sc);
    trace("300ms freeze every 3s + jitter", sc, r);

    CHECK_FALSE(r.desynced);
    CHECK(r.rephase_a > 0);
    CHECK(r.stalls_a == 0);     // was 32 — the stalls ARE the stutter the owner saw
    CHECK(r.lag_a <= 3);        // was 5
    CHECK(r.depth_max_a < 8);   // was exactly 8, i.e. pinned at the cap
}

TEST_CASE("pacing: the pump rate is DECOUPLED from the frame rate") {
    // The question a field report raised: the stalling player's partner was in F9
    // native cadence, so was the session being pumped once per RENDERED FRAME? If
    // it were, everything that changes the frame rate would change the send rate,
    // and a peer whose frame rate fell near 20 Hz would starve its partner.
    //
    // It is not. MatchRunner drives the pump off the 50 ms accumulator, so the pump
    // rate is 20 Hz at any frame rate — pinned here across a 27x span of frame
    // periods, from the uncapped sub-frame lattice to 10 fps. Anything that makes
    // one peer's PRESENTATION heavier therefore cannot starve the other by itself;
    // it can only contribute by making single frames long enough to trip the
    // catch-up clamp, which is the skew the case above covers.
    for (const int f : {6, 16, 33, 50, 100, 160}) {
        Scenario sc;
        sc.frame_ms = f;
        sc.frame_b_ms = f;
        const Result r = measure(sc);
        char label[48];
        std::snprintf(label, sizeof(label), "both at frame=%dms", f);
        trace(label, sc, r);

        // 20 pumps a second, whatever the frame rate — within one pump of it.
        CHECK(r.pumps_a >= 19 * sc.run_ms / 1000);
        CHECK(r.pumps_a <= 21 * sc.run_ms / 1000);
        CHECK(r.stalls_a == 0);
        CHECK_FALSE(r.desynced);
    }
}

TEST_CASE("pacing: one peer on a much slower and more erratic frame loop") {
    // The asymmetry every suite in tests/net used to be blind to: both peers were
    // always pumped at the same rate in the same loop. Here B renders at ~10 fps
    // with heavily erratic pacing while A runs uncapped — the shape of the field
    // report, whatever its cause. Neither peer may drift into the prediction cap.
    Scenario sc;
    sc.frame_ms = 6;
    sc.frame_b_ms = 100;
    sc.frame_b_wobble = 90;  // 100-190 ms frames: worse than the 4-tick clamp allows
    sc.jitter_ms = 15;
    sc.run_ms = 38000;
    const Result r = measure(sc);
    trace("A uncapped, B ~6-10 fps erratic", sc, r);

    CHECK_FALSE(r.desynced);
    CHECK(r.lag_max_a < 8);    // neither peer reaches the cap...
    CHECK(r.depth_max_b < 8);  // ...from either side
    CHECK(r.ticks_a > 18 * sc.run_ms / 1000);
    CHECK(r.ticks_b > 18 * sc.run_ms / 1000);
}

TEST_CASE("pacing: an asymmetric path is SHARED rather than borne by one peer") {
    // A path that is far slower in one direction gives the two peers unequal lag
    // through no fault of either. Re-phasing equalises it: the peer with the
    // shorter lag waits, so neither sits close to the cap alone. Nothing here is a
    // fault to fix — it is pinned because the equaliser must converge and STOP,
    // not throttle one side forever.
    Scenario sc;
    sc.one_way_ms = 10;
    sc.back_way_ms = 200;
    const Result r = measure(sc);
    trace("A->B 10ms, B->A 200ms", sc, r);

    CHECK_FALSE(r.desynced);
    CHECK(r.lag_a <= 4);
    CHECK(r.lag_b <= 4);
    CHECK(r.stalls_a == 0);
    CHECK(r.ticks_a > 19 * sc.run_ms / 1000);  // converged, not permanently throttled
}
