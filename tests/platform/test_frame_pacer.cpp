#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "bomber/platform/frame_pacer.hpp"

using bomber::platform::FramePacer;

namespace {

// 20 Hz tick / 9 interpolation sub-frames — the rate F8's uncapped mode paces
// at. Spelled out rather than pulled from libs/sim: this suite is about the
// pacing rule, and must not start failing because a sim constant moved.
constexpr std::uint64_t kTickNs = 1'000'000'000ull / 20;
constexpr int kSubFrames = 9;
constexpr std::uint64_t kSubNs = kTickNs / kSubFrames;  // 5'555'555 ns, ~180 Hz

// A virtual front-end loop: a fake clock the pacer never sees, a sleep that
// OVERSHOOTS the way Win11's does, and a scripted per-frame present cost.
// Everything the real MatchRunner tail does, minus SDL.
struct Loop {
    std::uint64_t now = 1'000'000'000ull;  // arbitrary non-zero origin
    std::uint64_t sleep_overshoot = 450'000;  // measured mean at a 5.556 ms request
    std::uint64_t sleep_floor = 500'000;      // any request costs at least this
    std::uint64_t anchor = 0;                 // tick-clock origin of the lattice
    std::vector<std::uint64_t> present_costs;  // cycled; per-frame cost of the present
    std::size_t frame = 0;
    std::uint64_t work_ns = 110'000;  // measured CPU cost of a frame before present
    std::vector<std::uint64_t> targets;  // every target the pacer handed back

    std::uint64_t present_cost() {
        return present_costs.empty() ? 300'000
                                     : present_costs[frame % present_costs.size()];
    }

    // Run one frame; returns the instant of the present (what the fps counter
    // and the sub-frame accounting key off).
    std::uint64_t run_frame(FramePacer& pacer, bool subframe) {
        now += work_ns + present_cost();
        const std::uint64_t present_at = now;
        FramePacer::Wait wait;
        if (subframe) {
            pacer.set_anchor(anchor);
            wait = pacer.plan_subframe(now);
        } else {
            wait = pacer.plan_resync(now);
        }
        if (wait.sleep_ns) now += wait.sleep_ns < sleep_floor ? sleep_floor
                                                             : wait.sleep_ns + sleep_overshoot;
        if (wait.spin_until_ns && now < wait.spin_until_ns) now = wait.spin_until_ns;  // the spin
        targets.push_back(pacer.target_ns());
        ++frame;
        return present_at;
    }
};

// The index renderer.cpp would floor interp_alpha onto for a present at `t`.
std::uint64_t sub_index(std::uint64_t t, std::uint64_t anchor) {
    return FramePacer::subframe_index(t, anchor, kSubNs);
}

}  // namespace

TEST_SUITE("frame_pacer") {
    // ---- the DEFAULT (vsync) path: behaviour must be exactly what shipped ----
    TEST_CASE("resync sleeps to the target and advances by one period") {
        FramePacer p(kSubNs, 0);
        CHECK(p.target_ns() == kSubNs);
        const FramePacer::Wait w = p.plan_resync(kSubNs - 1'000'000);
        CHECK(w.sleep_ns == 1'000'000);
        CHECK(w.spin_until_ns == 0);  // the default path never pays for a spin
        CHECK(p.target_ns() == 2 * kSubNs);
    }

    TEST_CASE("resync re-phases onto an overrunning present and drops the deficit") {
        // Deliberate: in vsync mode the cadence is the vblank train, so a
        // present that ran past the target means the train moved. Chasing the
        // lost time would fight it. This pins the rule so a future change to the
        // uncapped path cannot quietly alter the default one.
        FramePacer p(kSubNs, 0);
        const FramePacer::Wait w = p.plan_resync(kSubNs + 3'000'000);
        CHECK(w.sleep_ns == 0);
        CHECK(w.spin_until_ns == 0);
        CHECK(p.target_ns() == kSubNs + 3'000'000 + kSubNs);
    }

    // ---- F8's uncapped path ----
    TEST_CASE("a run of on-time frames holds the target rate exactly") {
        Loop loop;
        FramePacer pacer(kSubNs, loop.now);
        loop.anchor = loop.now;
        std::vector<std::uint64_t> presents;
        for (int i = 0; i < 400; ++i) presents.push_back(loop.run_frame(pacer, true));

        // Mean interval is the period, not the period plus the sleep's error:
        // the target is ABSOLUTE, so each frame's overshoot is paid back by the
        // next frame's shorter sleep. Worth pinning even though the shipped
        // rule also passes it (it was absolute too) — it is the property that
        // makes a 0.45 ms sleep overshoot cost nothing on a clean timeline, and
        // the one a naive "sleep(period)" rewrite would break, landing at
        // 6.0 ms/166 Hz instead of 5.556 ms/180 Hz.
        const std::uint64_t span = presents.back() - presents.front();
        const double mean = static_cast<double>(span) /
                            static_cast<double>(presents.size() - 1);
        CHECK(mean == doctest::Approx(static_cast<double>(kSubNs)).epsilon(0.005));

        // And no individual frame drifts: every present sits in its own cell.
        for (std::size_t i = 1; i < presents.size(); ++i)
            CHECK(sub_index(presents[i], loop.anchor) ==
                  sub_index(presents[i - 1], loop.anchor) + 1);
    }

    TEST_CASE("the sub-frame index advances exactly once per present in steady state") {
        // The point of the whole exercise: 9 distinct on-screen positions per
        // tick means a present is only worth making when the index changes.
        // Duplicates are wasted presents that inflate the fps number without
        // adding a frame of motion; skips are dropped motion.
        Loop loop;
        loop.present_costs = {300'000, 250'000, 400'000, 280'000, 1'400'000};
        FramePacer pacer(kSubNs, loop.now);
        loop.anchor = loop.now;
        std::uint64_t prev = sub_index(loop.run_frame(pacer, true), loop.anchor);
        int duplicates = 0, skips = 0;
        for (int i = 0; i < 500; ++i) {
            const std::uint64_t idx = sub_index(loop.run_frame(pacer, true), loop.anchor);
            if (idx == prev) ++duplicates;
            if (idx > prev + 1) skips += static_cast<int>(idx - prev - 1);
            prev = idx;
        }
        CHECK(duplicates == 0);
        CHECK(skips == 0);
    }

    TEST_CASE("no sub-frame is ever presented twice, however lumpy the present") {
        // THE discriminating pin. Costs are the measured shape of
        // SDL_RenderPresent on the reference rig: mostly ~0.3 ms, ~4-6 ms at
        // p90, 13 ms at p99 as the swapchain backs up against a 60 Hz panel —
        // i.e. frames that blow the 5.556 ms budget outright, several times a
        // second. Run through the OLD rule (plan_resync at this period, which
        // is what shipped) the same timeline produces 52 duplicate presents in
        // 400 frames: sub-frames re-shown because the target had been re-based
        // onto "now" and drifted back inside a cell already presented. Those
        // duplicates are the reason a deficit-catch-up pacer can hit 180 on the
        // overlay while the motion does not improve.
        Loop loop;
        loop.present_costs = {300'000, 250'000, 400'000,   280'000, 1'400'000,
                              320'000, 260'000, 4'800'000, 300'000, 350'000,
                              290'000, 310'000, 270'000,   330'000, 13'000'000};
        FramePacer pacer(kSubNs, loop.now);
        loop.anchor = loop.now;
        std::vector<std::uint64_t> presents;
        for (int i = 0; i < 400; ++i) presents.push_back(loop.run_frame(pacer, true));
        for (std::size_t i = 1; i < presents.size(); ++i)
            CHECK(sub_index(presents[i], loop.anchor) > sub_index(presents[i - 1], loop.anchor));
    }

    TEST_CASE("an overrunning frame loses that sub-frame but never the phase") {
        // One present blocks for four periods (the swapchain stall this pacer
        // exists to survive). The sub-frames it ran through are gone — showing
        // them late is the duplicate-present failure above — but the pacer must
        // land back on the lattice immediately rather than carry the error
        // forward. The pre-fix code re-based the target onto "now" here, which
        // shifted the phase by the frame's own cost permanently.
        //
        // Stated exactly: every target the pacer hands back is ON the lattice,
        // i.e. congruent to the anchor modulo the period. There is no state in
        // which it can be anything else, which is what "cannot lose phase"
        // means. plan_resync's target fails this the moment it re-bases.
        Loop loop;
        loop.present_costs = {300'000, 300'000, 300'000, 4 * kSubNs, 300'000, 300'000};
        FramePacer pacer(kSubNs, loop.now);
        loop.anchor = loop.now;
        std::vector<std::uint64_t> presents;
        for (int i = 0; i < 300; ++i) presents.push_back(loop.run_frame(pacer, true));

        for (std::uint64_t t : loop.targets) CHECK((t - loop.anchor) % kSubNs == 0);
        for (std::size_t i = 1; i < presents.size(); ++i) {
            // Never early: a present is at or after the boundary it belongs to,
            // and never presented into a cell already shown.
            const std::uint64_t idx = sub_index(presents[i], loop.anchor);
            CHECK(presents[i] >= loop.anchor + idx * kSubNs);
            CHECK(idx > sub_index(presents[i - 1], loop.anchor));
        }
        // The stall costs its own sub-frames and nothing more. Each 6-frame
        // cycle advances the index by exactly 10: five on-time frames take one
        // cell each, and the stalled one takes five (four periods of blocked
        // present, plus the cell its own tail lands in). 300 frames = 50 cycles
        // = 500 cells, less the first frame that opened the count. A pacer that
        // leaked phase would drift off this figure without ever duplicating.
        const std::uint64_t advanced = sub_index(presents.back(), loop.anchor) -
                                       sub_index(presents.front(), loop.anchor);
        CHECK(advanced == 50 * 10 - 1);
    }

    TEST_CASE("a stalled frame does not fire a catch-up burst of duplicates") {
        // The alternative shape (carry the deficit, run the next frames early to
        // amortise it) reaches the target rate by re-presenting sub-frames it
        // has already shown. Pin that this one does not: after a 4-period stall
        // the very next wait is a real one, not zero.
        FramePacer pacer(kSubNs, 0);
        pacer.set_anchor(0);
        const std::uint64_t present_at = 4 * kSubNs + kSubNs / 2;
        const FramePacer::Wait w = pacer.plan_subframe(present_at);
        CHECK(w.spin_until_ns == 5 * kSubNs);  // the NEXT cell, not cell 1 replayed
        CHECK(w.sleep_ns + FramePacer::kSpinTailNs == 5 * kSubNs - present_at);
    }

    TEST_CASE("the wait is split sleep-then-spin and never exceeds one period") {
        FramePacer pacer(kSubNs, 0);
        pacer.set_anchor(0);
        // A full period of slack: sleep all but the spin tail, then spin.
        FramePacer::Wait w = pacer.plan_subframe(kSubNs);
        CHECK(w.spin_until_ns == 2 * kSubNs);
        CHECK(w.sleep_ns == kSubNs - FramePacer::kSpinTailNs);
        // Less slack than the tail: pure spin, no sleep to overshoot with.
        w = pacer.plan_subframe(2 * kSubNs - 400'000);
        CHECK(w.sleep_ns == 0);
        CHECK(w.spin_until_ns == 2 * kSubNs);
        // An anchor in the future must not stall for more than one period.
        pacer.set_anchor(10 * kSubNs);
        w = pacer.plan_subframe(kSubNs);
        CHECK(w.spin_until_ns == 2 * kSubNs);
    }

    TEST_CASE("an F8 toggle takes effect on the next frame") {
        // uncap_fps flips mid-match; the pacer must switch rate without a
        // multi-frame stall or a burst either way.
        FramePacer pacer(1'000'000'000ull / 60, 0);
        pacer.plan_resync(1'000'000);
        pacer.set_period(kSubNs);
        pacer.set_anchor(0);
        const FramePacer::Wait w = pacer.plan_subframe(20'000'000);
        CHECK(w.spin_until_ns == 4 * kSubNs);  // 20 ms is inside cell 3
        CHECK(w.spin_until_ns > 20'000'000);
        CHECK(w.spin_until_ns - 20'000'000 <= kSubNs);
    }

    TEST_CASE("subframe_index partitions the tick the way the renderer does") {
        // renderer.cpp computes floor(interp_alpha * kSubFrames) with
        // interp_alpha = acc / tick_ns; the pacer's lattice is anchor + k *
        // (tick_ns / kSubFrames). "One present per index" is only worth
        // anything if those two partitions are the same one — pin that they
        // are, everywhere except within a hair of a boundary.
        //
        // The hair is real and deliberate: kSubNs is an INTEGER division
        // (50'000'000 / 9 = 5'555'555, remainder 5), so the pacer's cell f
        // opens up to f * 0.55 ns before the renderer's does, at most 5 ns
        // across a whole tick. Against the hundreds of microseconds of real
        // present/sleep jitter that is nothing, but it means the boundaries are
        // congruent, not identical, and a test must not claim otherwise.
        for (int f = 0; f < kSubFrames; ++f) {
            const std::uint64_t acc = f * kSubNs + 1'000;  // 1 us into cell f
            const auto alpha = static_cast<float>(acc) / static_cast<float>(kTickNs);
            CHECK(static_cast<int>(alpha * static_cast<float>(kSubFrames)) == f);
            CHECK(sub_index(acc, 0) == static_cast<std::uint64_t>(f));
        }
        // The same remainder leaves a 5 ns sliver at the end of the tick that
        // the lattice calls cell kSubFrames — a 10th cell the renderer has no
        // position for. Pinned rather than papered over: it is harmless because
        // the match loop re-anchors the lattice on every frame (anchor = the
        // instant the CURRENT tick began), so the sliver is re-based away long
        // before anything could present into it, and a present that did land in
        // it would simply target the next boundary 5.556 ms later like any
        // other. It is a 1-in-10-million window on a wait that already jitters
        // by hundreds of microseconds.
        CHECK(kSubFrames * kSubNs == kTickNs - 5);
        CHECK(sub_index(kSubFrames * kSubNs - 1, 0) ==
              static_cast<std::uint64_t>(kSubFrames - 1));
        CHECK(sub_index(kTickNs - 1, 0) == static_cast<std::uint64_t>(kSubFrames));
    }
}
