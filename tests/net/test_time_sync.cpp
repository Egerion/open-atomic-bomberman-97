// THE PACING DECISION, pinned as a PURE FUNCTION (time_sync.hpp).
//
// Until TimeSyncController was extracted, every one of these rules was only
// observable by running two RollbackSessions at each other over a modelled path
// and reading the aggregate out the other end — so tests/net/test_rollback_pacing
// .cpp and tests/net/test_jitter_absorb.cpp can say "the pair stopped parking at
// the cap" and "the pair stopped holding 12% of its pumps", which is what those
// suites are for and what the live reports were about, but neither can say WHICH
// rule produced a given hold, or pin the boundary of one. A threshold fitted to a
// chaotic two-peer realisation is fitted to noise (ms_clock_harness.hpp says so
// itself about its own seeds); a threshold checked against injected numbers is
// not. Both kinds are needed and this is the missing kind.
//
// So every case here drives the controller DIRECTLY: a chosen peer depth and a
// chosen frame advantage go in, one Decision comes out, and no clock, no
// transport, no Simulation and no second peer are involved. Same reasoning as
// tests/platform's FramePacer suite, which is the model this extraction followed.
//
// WHAT THE CASES DELIBERATELY DO NOT CLAIM: nothing here is evidence about a real
// path. The numbers this controller is TUNED to came from 13 live
// Turkey<->Lithuania sessions (the table at the head of test_jitter_absorb.cpp),
// and that is still where a tuning change has to be judged. What is pinned here
// is that the rules are the rules the header describes.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>

#include "bomber/net/time_sync.hpp"

using namespace bomber;  // NOLINT(google-build-using-namespace) — test-local

namespace {

using Decision = net::TimeSyncController::Decision;

// ONE remote seat, at seat 1 — the two-peer shape both live reports came from.
// The reported frontier RISES on every note, so the staleness guard never fires
// except in the case that is about it.
constexpr std::uint16_t kPeer = 0x2;

struct Rig {
    net::TimeSyncController c;
    std::uint32_t frontier = 0;

    // One eligible pump at a chosen peer depth and a chosen frame advantage. The
    // local depth is derived, so the controller's own subtraction (ours minus
    // theirs) lands on exactly `advantage` — which is the quantity every rule in
    // the header is stated in terms of.
    Decision pump(int peer_depth, int advantage) {
        c.note_peer_range(kPeer, ++frontier, peer_depth);
        return c.pump(true, peer_depth + advantage, kPeer);
    }

    // Fill the whole window at a KNOWN spread and a known advantage. Alternating
    // two depths is the smallest thing that produces a chosen spread, and running
    // it for exactly kRephaseWindowPumps is what makes the next pump the first one
    // with a full window behind it.
    void fill_window(int spread, int advantage) {
        for (int i = 0; i < net::kRephaseWindowPumps; ++i) pump(i % 2 == 0 ? 0 : spread, advantage);
    }
};

}  // namespace

TEST_CASE("an ineligible pump decides nothing, however large the reading") {
    Rig r;
    r.c.note_peer_range(kPeer, 1, 0);
    // A local depth of 99 against a peer depth of 0 is an advantage no path could
    // explain — and it is still not acted on, because the session has said the
    // comparison is meaningless (no peer, round ending, migration healing).
    const Decision d = r.c.pump(false, 99, kPeer);
    CHECK(d.hold == false);
    CHECK(d.raw == 0);
    CHECK(d.sustained == 0);
    CHECK(d.suppressed == false);
    CHECK(r.c.lead() == 0);
}

TEST_CASE("a clean path keeps the UNFILTERED rule: it holds on alternate pumps") {
    // The header's key claim about the filter — "on a steady path the spread is 0
    // and the rule reduces to the unfiltered one this controller shipped with".
    // A steady peer depth means the spread arm (`raw >= spread + threshold`)
    // degenerates to the bare threshold, so the very first pump acts.
    Rig r;
    CHECK(r.pump(2, net::kRephaseAdvantageTicks).hold == true);
    CHECK(r.pump(2, net::kRephaseAdvantageTicks).hold == false);  // never two in a row
    CHECK(r.pump(2, net::kRephaseAdvantageTicks).hold == true);
    CHECK(r.pump(2, net::kRephaseAdvantageTicks).hold == false);

    int holds = 0;
    for (int i = 0; i < 40; ++i)
        if (r.pump(2, net::kRephaseAdvantageTicks).hold) ++holds;
    CHECK(holds == 20);  // exactly half: a skew is shed at half rate, not by freezing
}

TEST_CASE("an advantage below the threshold is never acted on") {
    Rig r;
    int holds = 0;
    for (int i = 0; i < 60; ++i)
        if (r.pump(2, net::kRephaseAdvantageTicks - 1).hold) ++holds;
    CHECK(holds == 0);
    CHECK(r.c.sustained_advantage() == net::kRephaseAdvantageTicks - 1);  // seen, just not acted on
}

TEST_CASE("ARRIVAL VARIANCE alone cannot buy a hold") {
    // The jitter regression, reduced to its arithmetic. A window's worth of
    // bouncing peer depth with no standing advantage, and then one pump where the
    // instantaneous reading crosses the threshold — which is exactly what 90 ms of
    // arrival variance manufactures, and what used to cost a displayed tick.
    Rig r;
    r.fill_window(5, 0);
    CHECK(r.c.peer_depth_spread() == 5);
    CHECK(r.c.sustained_advantage() == 0);

    const Decision d = r.pump(3, net::kRephaseAdvantageTicks);
    CHECK(d.raw == net::kRephaseAdvantageTicks);  // the raw reading did ask
    CHECK(d.spread == 5);
    CHECK(d.hold == false);       // and the filter refused it
    CHECK(d.suppressed == true);  // which is the absorber's own meter
}

TEST_CASE("a STANDING skew survives the minimum and is still shed") {
    // The other half of the same rule: the filter must not have bought its noise
    // immunity by going deaf. Same jittery path, but now a genuine clock skew on
    // top of it — which by construction cannot clear the spread arm, so the only
    // thing that can act on it is the window filling.
    Rig r;
    r.fill_window(5, 0);

    const int skew = net::kRephaseAdvantageTicks + 1;
    int holds = 0;
    for (int i = 0; i < net::kRephaseWindowPumps - 1; ++i)
        if (r.pump(i % 2 == 0 ? 0 : 5, skew).hold) ++holds;
    CHECK(holds == 0);  // one sample short of a full window: still refused

    const Decision d = r.pump(0, skew);  // the pump that completes the window
    CHECK(d.sustained == skew);
    CHECK(d.hold == true);
}

TEST_CASE("an advantage larger than the variance explains is acted on AT ONCE") {
    // The reaction-time escape hatch: a frozen frame loop hands the pair several
    // ticks of skew in one go, and waiting a full second for the window would let
    // that skew spend the whole prediction budget first.
    Rig r;
    r.fill_window(5, 0);
    CHECK(r.c.sustained_advantage() == 0);  // the window says nothing yet

    const Decision d = r.pump(0, 5 + net::kRephaseAdvantageTicks);
    CHECK(d.sustained == 0);  // and still says nothing
    CHECK(d.hold == true);    // but the sample is too big for jitter to have made it
    CHECK(d.suppressed == false);
}

TEST_CASE("EVERY hold implies the unfiltered predicate") {
    // The header's first safety property: "a pump this holds is a pump the
    // unfiltered controller would also have held. It can only ever hold LESS."
    // Swept rather than argued, because it is the property that makes the filter
    // safe to ship over a path nobody has modelled.
    for (int spread = 0; spread <= 8; ++spread) {
        for (int advantage = -4; advantage <= 8; ++advantage) {
            Rig r;
            r.fill_window(spread, advantage);
            for (int i = 0; i < 8; ++i) {
                const Decision d = r.pump(i % 2 == 0 ? 0 : spread, advantage);
                if (d.hold) CHECK(d.raw >= net::kRephaseAdvantageTicks);
                if (d.suppressed) CHECK(d.raw >= net::kRephaseAdvantageTicks);
            }
        }
    }
}

TEST_CASE("a REORDERED datagram does not report the depth the peer left behind") {
    // A sender's confirmed frontier only ever rises, so a frame carrying a lower
    // one overtook a newer one in flight. Its stale window reads as the peer
    // having suddenly caught up — precisely the reading that makes this peer
    // decide it is ahead — and reordering is routine on the jittery paths where
    // that matters most.
    net::TimeSyncController c;
    c.note_peer_range(kPeer, 100, 5);
    CHECK(c.peer_lag(kPeer) == 5);
    c.note_peer_range(kPeer, 90, 1);  // overtaken: ignore it
    CHECK(c.peer_lag(kPeer) == 5);
    c.note_peer_range(kPeer, 100, 1);  // same frontier, a legitimate re-send
    CHECK(c.peer_lag(kPeer) == 1);
    c.note_peer_range(kPeer, 101, 4);  // and the frontier moving on
    CHECK(c.peer_lag(kPeer) == 4);
    // A seat nobody is awaiting contributes nothing, whatever it reported.
    CHECK(c.peer_lag(0) == 0);
}

TEST_CASE("the LOCAL LEAD is never spent on a path that does not need it") {
    // "A 300 ms path with no jitter holds a constant depth and gets no lead."
    // The level of the peer's depth is the path's delay and is none of our
    // business; only its spread can be bought back.
    Rig r;
    for (int i = 0; i < 60; ++i) {
        r.pump(6, 0);  // a slow path, perfectly steady
        r.c.update_lead(true);
    }
    CHECK(r.c.peer_depth_spread() == 0);
    CHECK(r.c.lead() == 0);
}

TEST_CASE("the LEAD DEADBAND keeps ordinary slop off the wire") {
    // A pump boundary alone moves the peer's depth by one tick on any path, so
    // the first kLeadDeadbandTicks of spread buy nothing.
    for (int spread = 0; spread <= net::kLeadDeadbandTicks; ++spread) {
        Rig r;
        for (int i = 0; i < 60; ++i) {
            r.pump(i % 2 == 0 ? 0 : spread, 0);
            r.c.update_lead(true);
        }
        CHECK(r.c.peer_depth_spread() == spread);
        CHECK(r.c.lead() == 0);
    }
}

TEST_CASE("the LEAD ramps one tick per pump, is capped, and decays back to zero") {
    Rig r;
    const int spread = net::kLeadDeadbandTicks + net::kMaxLocalLeadTicks;
    r.fill_window(spread, 0);
    CHECK(r.c.lead() == 0);  // pump() alone never moves it

    // One tick per SIMULATING pump: raising it holds the local sample one extra
    // tick, and a multi-tick jump would be visible where 50 ms is not.
    for (int i = 1; i <= net::kMaxLocalLeadTicks; ++i) {
        r.c.update_lead(true);
        CHECK(r.c.lead() == i);
    }
    r.c.update_lead(true);
    CHECK(r.c.lead() == net::kMaxLocalLeadTicks);  // and no further, however bad it gets

    // Past the cap the answer does not change: the rest of the burst is what
    // rollback is FOR.
    Rig wild;
    wild.fill_window(20, 0);
    for (int i = 0; i < 10; ++i) wild.c.update_lead(true);
    CHECK(wild.c.lead() == net::kMaxLocalLeadTicks);

    // The variance stops. The lead is paid for in the player's own input lag, so
    // it must come back off — one tick per pump, the same rate it went on.
    r.fill_window(0, 0);
    for (int i = net::kMaxLocalLeadTicks - 1; i >= 0; --i) {
        r.c.update_lead(true);
        CHECK(r.c.lead() == i);
    }
    r.c.update_lead(true);
    CHECK(r.c.lead() == 0);  // and it does not go negative
}

TEST_CASE("an INELIGIBLE pump discards the window rather than padding it") {
    // A migration freezes the peer's reported depth at whatever the dead hub last
    // relayed, so the step back to live values would register as arrival variance
    // and buy a lead nobody asked for. Starting over costs one window of
    // inaction, which is the state a session opens in anyway.
    Rig r;
    r.fill_window(5, 3);
    CHECK(r.c.peer_depth_spread() == 5);
    CHECK(r.c.sustained_advantage() == 3);

    r.c.pump(false, 0, kPeer);
    CHECK(r.c.peer_depth_spread() == 0);
    CHECK(r.c.sustained_advantage() == 0);

    // And it is a full window, not one pump, before either speaks again.
    for (int i = 0; i < net::kRephaseWindowPumps - 1; ++i) r.pump(i % 2 == 0 ? 0 : 5, 3);
    CHECK(r.c.peer_depth_spread() == 0);
    r.pump(5, 3);
    CHECK(r.c.peer_depth_spread() == 5);
}

TEST_CASE("the LEAD OUTLIVES the spread that bought it — netdiag task #58") {
    // The reported symptom is one netdiag line reading `spread=0t` beside
    // `lead=2t`, which looks like the controller contradicting itself. It is not:
    // the two are read off DIFFERENT TIME BASES on purpose, and both are printed
    // exactly as the controller holds them.
    //
    //   * `spread` is this pump's instantaneous reading, and it reads 0 whenever
    //     the window is not full — including for a whole window after ANY
    //     ineligible pump, which discards it;
    //   * `lead` is a rate-limited follower that only moves on a pump that
    //     actually SIMULATES. A pump held for a re-phase, or stalled at the
    //     prediction cap, reports through on_timing and never reaches
    //     update_lead().
    //
    // So a peer parked at the cap prints a lead it earned earlier beside a spread
    // measured now, indefinitely. That is a display artefact of two honest
    // numbers, not a fault: the lead is INERT while it is not being consumed
    // (nothing is filed on a pump that does not simulate) and it decays on the
    // first pump that does. Both halves are pinned below.
    Rig r;
    r.fill_window(net::kLeadDeadbandTicks + net::kMaxLocalLeadTicks, 0);
    for (int i = 0; i < net::kMaxLocalLeadTicks; ++i) r.c.update_lead(true);
    CHECK(r.c.lead() == net::kMaxLocalLeadTicks);

    // One ineligible pump — a host migration beginning, say — and the reading the
    // lead was bought with is gone while the lead itself is not.
    const Decision d = r.c.pump(false, 0, kPeer);
    CHECK(d.spread == 0);
    CHECK(r.c.lead() == net::kMaxLocalLeadTicks);

    // The same disagreement, held INDEFINITELY: pump() every pump on a path that
    // is now perfectly steady, update_lead() on none of them — which is exactly
    // what a session stalled at the prediction cap does.
    bool disagreed_every_pump = true;
    for (int i = 0; i < 100; ++i) {
        const Decision steady = r.pump(4, 0);
        if (steady.spread != 0 || r.c.lead() != net::kMaxLocalLeadTicks)
            disagreed_every_pump = false;
    }
    CHECK(disagreed_every_pump);

    // And it is self-correcting the moment the session simulates again.
    r.fill_window(0, 0);
    for (int i = 0; i < net::kMaxLocalLeadTicks; ++i) r.c.update_lead(true);
    CHECK(r.c.lead() == 0);
}
