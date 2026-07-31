// THE KILL TALLY UNDER ROLLBACK — two peers, one agreed simulation, one verdict.
//
// The front-end keeps a per-match kill counter (libs/game_util's results.hpp
// `tally_kills`, GameApp::kill_count_) that the RESULTS row shows and that, under
// Team Play + "win by kills", the MATCH-CLINCH predicate reads. It was summed
// straight from `sim.state().events`, once per session pump.
//
// That is an accumulator fed from a stream that REPLAYS. Under rollback a
// mispredicted tick is simulated, then simulated again with corrected input, and
// `State::events` is rebuilt each time; a pump that stalls at the prediction cap
// does not tick at all and re-presents the previous tick's events. So the total
// depended on the local machine's PREDICTION HISTORY, not on the agreed
// simulation — two peers could hold different kill counts, and therefore clinch
// the match for different players, from a run that never desynced.
//
// NOTHING in the project could catch it. `State::events` is excluded from
// state_hash by design (determinism rule 4: per-tick outputs, rebuilt every tick,
// never read back by the sim), so the goldens, the per-tick confirmed-hash
// exchange and `build_hash` are all blind to a counter derived from it. The only
// way to see it is to run BOTH peers with genuinely different rollback histories
// and compare something outside the hash — which is what this file does.
//
// The rig is ms_clock_harness.hpp (extracted from test_rollback_pacing.cpp):
// independent per-peer frame loops on independent wall clocks and per-direction
// millisecond latency, so one peer predicts deeply where the other barely
// predicts at all. Every case below asserts, in this order:
//
//   1. the two sims AGREED     — equal state_hash, no desync, kills actually happened
//   2. the FIXED tally agrees  — equal per-player counts and equal win-by-kills clinch
//   3. the NAIVE tally does NOT — the bug is still reachable by the old formulation
//
// (3) is the part that keeps this suite honest. A test in which both peers see the
// same history proves nothing, and a test that passes with and without the fix is
// worse than none: it is checked here rather than by a one-off manual revert, so
// it cannot rot into a green tautology.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "bomber/game_util/net_tally.hpp"  // tally_netplay_kills — the REAL wiring under test
#include "bomber/game_util/results.hpp"    // tally_kills / win_by_kills_clinch
#include "bomber/net/rollback_session.hpp"
#include "bomber/sim/simulation.hpp"
#include "helpers.hpp"
#include "ms_clock_harness.hpp"

using namespace bomber;  // NOLINT(google-build-using-namespace) — test-local
using bomber::net::test::Driver;
using bomber::net::test::MsLink;
using bomber::net::test::MsTransport;

namespace {

constexpr std::uint16_t kSeat0 = 0x1;
constexpr std::uint16_t kSeat1 = 0x2;
constexpr std::uint16_t kBoth = 0x3;
constexpr int kPlayers = 10;

using Kills = std::array<int, sim::kMaxPlayers>;

// A FULL ROSTER packed into two rows: seats 0 and 1 (spawning adjacent, well
// inside each other's blast reach) are the two network peers, and slots 2..9 are
// PRESENT BYSTANDERS — outside `all_seats`, so the session feeds them neutral
// input and they stand where they spawned.
//
// Both details are load-bearing. The bystanders produce ATTRIBUTED kills, which
// is the only kind that counts: `tally_kills` deliberately drops a self-kill, so
// a scenario where the peers only blow themselves up would tally zero and prove
// nothing. And a crowd keeps the round UNDECIDED for a few hundred ticks, since
// the sim freezes every fuse the moment one side remains — with two players the
// first death ends the round and there is almost nothing left to disagree about.
sim::MatchConfig duel_config() {
    sim::MatchConfig cfg = sim::test::open_config();
    cfg.spawns = {{0, 0}, {2, 0}, {4, 0}, {6, 0}, {8, 0},
                  {0, 2}, {2, 2}, {4, 2}, {6, 2}, {8, 2}};
    cfg.player_count = kPlayers;
    return cfg;
}

struct Run {
    Kills fixed_a{}, fixed_b{};  // the confirmed-stream tally (the fix)
    Kills naive_a{}, naive_b{};  // the old per-pump sum over sim.state().events
    std::uint64_t hash_a = 0, hash_b = 0;
    std::uint32_t tick_a = 0, tick_b = 0;
    std::uint32_t rollbacks_a = 0, rollbacks_b = 0;
    std::uint32_t stalls_a = 0, stalls_b = 0;
    bool desynced = false;
    bool ended = false;  // both peers reached the agreed end tick with nothing outstanding
};

int total(const Kills& k) {
    int n = 0;
    for (const int v : k) n += v;
    return n;
}

// The clinch the RESULTS screen would compute from a tally (results.hpp). This is
// the VERDICT the bug could split: same simulation, different winner.
int clinch(const Kills& k, int target) {
    std::array<bool, sim::kMaxPlayers> present{};
    for (int i = 0; i < kPlayers; ++i) present[static_cast<std::size_t>(i)] = true;
    return game::win_by_kills_clinch(k, present, target);
}

struct Scenario {
    int a_to_b_ms = 25;
    int b_to_a_ms = 25;
    int frame_a_ms = 6;
    int frame_b_ms = 6;
    int frame_b_wobble = 0;
    int jitter_ms = 0;
    int hitch_at_ms = -1;  // B's frame loop freezes (a window drag, an asset stall)
    int hitch_ms = 0;
    // The host stops the round once its head passes this tick — the wire v8
    // abandon. Used here as the SETTLING mechanism rather than for its own sake:
    // both peers stop simulating at one agreed tick and keep pumping past it, so
    // the confirmation frontier catches the head on both machines and the final
    // tally covers an identical, fully confirmed tick range. That is exactly the
    // shape the shell has at a real round end, where a 3 s linger runs long after
    // the last death.
    std::uint32_t stop_after_tick = 220;
    int limit_ms = 60000;
};

// MatchRunner's per-pump bookkeeping, both ways round, on one peer.
struct Tally {
    Kills fixed{};
    Kills naive{};
    std::vector<sim::Event> scratch;
    bool done = false;

    void operator()(net::RollbackSession& s) {
        if (done) return;  // MatchRunner has returned MatchOver by now
        // THE FIX: each tick's events handed over exactly once, from the
        // confirmed stream (libs/game_util's net_tally.hpp — the real call site).
        game::tally_netplay_kills(s, scratch, fixed);
        // THE BUG, kept alive alongside it: sum whatever the live state happens
        // to hold this pump. Verbatim what match_runner.cpp used to do.
        game::tally_kills(s.sim().state().events, naive);
        if (s.round_ended()) done = true;
    }

    void finish(net::RollbackSession& s) { game::tally_netplay_kills_final(s, scratch, fixed); }
};

Run measure(const Scenario& sc) {
    std::int64_t now = 0;
    MsLink link(sc.a_to_b_ms, sc.b_to_a_ms, sc.jitter_ms);
    MsTransport ta(link, 0, now);
    MsTransport tb(link, 1, now);
    sim::Simulation sa(duel_config());
    sim::Simulation sb(duel_config());
    // Seat 0 is the host: only a host may schedule the agreed end tick.
    net::DropPolicy host_policy;
    host_policy.is_host = true;
    net::RollbackSession a(sa, kSeat0, kBoth, /*max_prediction=*/8, ta, host_policy);
    net::RollbackSession b(sb, kSeat1, kBoth, /*max_prediction=*/8, tb);

    Driver da;
    da.seat = 0;
    da.frame_ms = sc.frame_a_ms;
    Driver db;
    db.seat = 1;
    db.frame_ms = sc.frame_b_ms;
    db.wobble = sc.frame_b_wobble;

    Tally tally_a;
    Tally tally_b;
    bool requested = false;
    Run r;

    for (now = 0; now <= sc.limit_ms; ++now) {
        if (now >= da.next_frame) da.frame(a, now, [&](net::RollbackSession& s) { tally_a(s); });
        bool frozen = sc.hitch_at_ms >= 0 && now >= sc.hitch_at_ms &&
                      now - sc.hitch_at_ms < sc.hitch_ms;
        if (frozen) {
            db.next_frame = now + 1;  // the loop is wedged: no frames, and `last` stays put
        } else if (now >= db.next_frame) {
            db.frame(b, now, [&](net::RollbackSession& s) { tally_b(s); });
        }
        if (!requested && a.predicted_tick() >= sc.stop_after_tick) {
            a.request_end_round();
            requested = true;
        }
        // Both peers have stopped at the agreed tick AND have nothing left
        // outstanding: every tick either of them simulated is confirmed on both.
        if (a.round_ended() && b.round_ended() && a.confirmed_tick() == a.predicted_tick() &&
            b.confirmed_tick() == b.predicted_tick()) {
            r.ended = true;
            break;
        }
    }

    // The shell's last act before it reads the tally (MatchRunner::finish_round).
    tally_a.finish(a);
    tally_b.finish(b);

    r.fixed_a = tally_a.fixed;
    r.fixed_b = tally_b.fixed;
    r.naive_a = tally_a.naive;
    r.naive_b = tally_b.naive;
    r.hash_a = a.hash();
    r.hash_b = b.hash();
    r.tick_a = a.predicted_tick();
    r.tick_b = b.predicted_tick();
    r.rollbacks_a = a.stats().rollbacks;
    r.rollbacks_b = b.stats().rollbacks;
    r.stalls_a = a.stats().stall_pumps;
    r.stalls_b = b.stats().stall_pumps;
    r.desynced = a.desynced() || b.desynced();
    return r;
}

void trace(const char* label, const Run& r) {
    std::printf("  %-26s tick=%u rb=%u/%u stall=%u/%u | fixed %d==%d %s | naive %d vs %d %s\n",
                label, r.tick_a, r.rollbacks_a, r.rollbacks_b, r.stalls_a, r.stalls_b,
                total(r.fixed_a), total(r.fixed_b), r.fixed_a == r.fixed_b ? "agree" : "SPLIT",
                total(r.naive_a), total(r.naive_b), r.naive_a == r.naive_b ? "agree" : "SPLIT");
}

// Everything that must hold whatever the path looks like.
void check_agreed(const Run& r) {
    CHECK(r.ended);                       // the run reached the agreed end tick
    CHECK_FALSE(r.desynced);              // ...having never disagreed
    CHECK(r.hash_a == r.hash_b);          // THE SIM AGREED — the premise of the whole file
    CHECK(r.tick_a == r.tick_b);          // both stopped at the same tick
    CHECK(total(r.fixed_a) > 0);          // and kills actually happened, so this is not vacuous
    CHECK(r.fixed_a == r.fixed_b);        // THE FIX: same tally on both machines
    CHECK(clinch(r.fixed_a, 1) == clinch(r.fixed_b, 1));  // ...and therefore the same verdict
}

}  // namespace

TEST_CASE("kill tally: an asymmetric path leaves the two peers with the SAME count") {
    // The plain case, and the one closest to a real match: a path far slower in
    // one direction, so the peer on the fast-receive side predicts several ticks
    // ahead while its partner barely predicts at all. Both rolled back, neither
    // rolled back the same ticks.
    //
    // These three delays are not arbitrary — each is a pairing the sweep at the
    // bottom of this file shows the OLD accumulator getting WRONG. So every
    // assertion below is being made on a run where the bug was live, not merely on
    // a run where rollback happened.
    //
    // The middle pairing was 200/60 until 2026-07-30, when the arrival-variance
    // absorber (rollback_session.hpp's jitter note) stopped it splitting: fewer
    // and shallower corrections mean the two peers' rollback histories diverge
    // less, so some pairings that used to expose the naive accumulator no longer
    // do. Re-picked from the sweep rather than loosened — 300/60 is the same shape
    // (A->B far slower than B->A) and still splits.
    for (const auto& sc : {Scenario{.a_to_b_ms = 300, .b_to_a_ms = 120},
                           Scenario{.a_to_b_ms = 300, .b_to_a_ms = 60},
                           Scenario{.a_to_b_ms = 120, .b_to_a_ms = 300, .jitter_ms = 30}}) {
        const Run r = measure(sc);
        char label[48];
        std::snprintf(label, sizeof(label), "%d/%d ms", sc.a_to_b_ms, sc.b_to_a_ms);
        trace(label, r);
        CHECK(r.rollbacks_a + r.rollbacks_b > 0);  // the rollback path WAS exercised
        CHECK(r.naive_a != r.naive_b);             // ...and it WOULD have split the count
        check_agreed(r);
    }
}

TEST_CASE("kill tally: a one-sided frame hitch does not move the count either") {
    // The other way to give two peers different histories: B's frame loop freezes
    // past MatchRunner's catch-up clamp, so it falls behind in wall clock, its
    // partner predicts through the gap, and the two spend the rest of the round
    // re-phasing. Nothing about who killed whom changed.
    Scenario sc;
    sc.a_to_b_ms = 200;
    sc.b_to_a_ms = 60;
    sc.frame_b_ms = 100;
    sc.frame_b_wobble = 90;
    sc.jitter_ms = 15;
    sc.hitch_at_ms = 3000;
    sc.hitch_ms = 500;
    const Run r = measure(sc);
    trace("B hitches + renders slowly", r);
    check_agreed(r);
}

TEST_CASE("kill tally: THE BUG is still reachable by the old formulation") {
    // THE DISCRIMINATOR. Everything above would pass just as happily if
    // tally_netplay_kills were a synonym for the old per-pump sum, so the same
    // runs also carry the naive accumulator, and at least one of them must show
    // it splitting the two peers apart. If this case ever goes green by itself,
    // the suite above has stopped measuring anything and the scenarios need to be
    // made harsher — NOT this assertion removed.
    //
    // Two distinct defects feed it, and either alone is enough: a re-simulated
    // tick's events are counted from whichever pass the local machine saw first
    // (and never from the corrected one), and a pump held at the prediction cap
    // re-counts the previous tick's events because the live state did not move.
    int cases = 0;
    int split = 0;
    int agreed = 0;
    for (const int fwd : {5, 20, 60, 120, 200, 300}) {
        for (const int back : {5, 20, 60, 120, 200, 300}) {
            for (const int jit : {0, 30}) {
                Scenario sc;
                sc.a_to_b_ms = fwd;
                sc.b_to_a_ms = back;
                sc.jitter_ms = jit;
                const Run r = measure(sc);
                ++cases;
                if (r.naive_a != r.naive_b) {
                    ++split;
                    char label[48];
                    std::snprintf(label, sizeof(label), "%d/%d ms j=%d", fwd, back, jit);
                    trace(label, r);  // only the splitting rows: the rest are quiet
                }
                if (r.fixed_a == r.fixed_b && total(r.fixed_a) > 0) ++agreed;
                // Whatever the naive counts do, the sim itself never disagreed —
                // which is precisely why no existing guard could have caught this.
                CHECK(r.hash_a == r.hash_b);
            }
        }
    }
    std::printf("  SWEEP over %d paths: naive split %d, confirmed-stream agreed %d\n", cases, split,
                agreed);
    // Measured 2026-07-30: 23 of 72 before the arrival-variance absorber landed
    // and 14 of 72 after it — the absorber leaves the two peers with less
    // divergent rollback histories, so it takes some of the naive accumulator's
    // failures away with it. Not a reason to relax anything: the defect is a
    // property of the OLD formulation, not of any particular path, and this stays
    // a floor of one so that a future change which makes it unreachable is caught
    // as a loss of coverage rather than celebrated as a pass.
    CHECK(split > 0);
    CHECK(agreed == cases);  // and the fix held on every single one
}
