#pragma once

#include <cstdint>

namespace bomber::platform {

// The frame-pacing DECISION, lifted out of the SDL loops so it can be pinned
// headlessly: no SDL here, and no clock of its own — the caller passes the
// instant it measured after its present and gets back what to wait for.
//
// Two rules, because the two front-end modes are paced by different things.
//
//  * Resync — the DEFAULT (vsync) path, and what FrameClock has always done.
//    The cadence comes from the display; the pacer only supplies the block
//    SDL_RenderPresent does not reliably do on Windows windowed mode (see
//    FrameClock's own comment). A present that ran past the target means the
//    hardware train moved, so the target RE-PHASES onto it and the overrun is
//    deliberately dropped — chasing it would fight vblank.
//
//  * SubFrame — F8's uncapped path. Nothing in hardware paces this one: the
//    cadence is the sim's own interpolation quantum. renderer.cpp floors
//    `interp_alpha * kSubFrames`, so a 50 ms tick has exactly kSubFrames
//    DISTINCT on-screen positions and a present is only worth making at the
//    instant that index changes. The target is therefore not "now + period" but
//    the next point on a LATTICE anchored to the tick clock. One present per
//    index: no duplicates, and no phase to lose in the first place.
//
// Why the lattice rather than the conventional deficit catch-up (carry the
// overrun, run the next frames early to amortise it): measured 2026-07-28 on
// the reference Win11 box, 640x480 windowed, direct3d11, 60 Hz panel, 6 s
// samples of a live match, target 180 Hz —
//
//                                 presents/s  DISTINCT/s  frame p99   worst
//     shipped Resync rule            171.4      167.9      16.3 ms   19.5 ms
//     bounded catch-up + spin        179.5      135.5       8.5 ms   22.1 ms
//     SubFrame lattice               179.3      179.3       8.5 ms   11.8 ms
//
// The middle row is the trap: catch-up reaches the target RATE by re-presenting
// sub-frames it had already shown (265 of its 1081 presents were byte-identical
// repeats), so the number on the overlay improves and the motion does not. The
// lattice presents 179.3 distinct images for 179.3 presents.
//
// The dominant cost this is pacing around is SDL_RenderPresent itself, not the
// frame's CPU work: same rig measured 0.11 ms of work before the present and
// 0.3 ms (p50) / 4-10 ms (p90) / 11-18 ms (p99) inside it, the swapchain
// blocking as it backs up against the 60 Hz display.
class FramePacer {
public:
    // What the caller should do before its next present.
    struct Wait {
        std::uint64_t sleep_ns = 0;       // hand to a coarse sleep (it will overshoot)
        std::uint64_t spin_until_ns = 0;  // then busy-wait until this instant (0 = don't)
    };

    // Tail of a SubFrame wait spent busy-waiting instead of sleeping. Sized
    // from the measured error of SDL3's high-resolution waitable-timer sleep on
    // Win11: a 5.556 ms request returns after 6.008 ms mean (+0.45), and ANY
    // request has a ~0.5 ms floor (a 200 us one takes 0.54 ms). Sleeping the
    // whole gap therefore lands late every single frame. Spinning the last
    // millisecond also keeps the core out of the deep idle states that made the
    // present call itself measurably slower (p90 4.2 ms -> 1.5 ms on the rig
    // above) — the reason the number is this large rather than a bare 0.5 ms.
    // Costs at most ~1 ms of one core per frame (~18% of a core at 180 Hz),
    // which the DEFAULT vsync path deliberately does not pay: plan_resync never
    // asks for a spin.
    static constexpr std::uint64_t kSpinTailNs = 1'000'000;

    FramePacer(std::uint64_t period_ns, std::uint64_t now_ns)
        : period_ns_(period_ns ? period_ns : 1), target_ns_(now_ns + period_ns_), anchor_ns_(now_ns) {}

    std::uint64_t period_ns() const { return period_ns_; }
    std::uint64_t target_ns() const { return target_ns_; }
    std::uint64_t anchor_ns() const { return anchor_ns_; }

    // Live-settable so an F8 toggle takes effect on the very next frame.
    void set_period(std::uint64_t period_ns) { period_ns_ = period_ns ? period_ns : 1; }

    // Origin of the SubFrame lattice — for a match loop, the wall instant the
    // current sim tick began. Re-stating it every frame is what keeps the
    // presents welded to the interpolator's own boundaries instead of drifting
    // against them.
    void set_anchor(std::uint64_t anchor_ns) { anchor_ns_ = anchor_ns; }

    // Which lattice cell `t_ns` falls in. Shared with the callers/tests so
    // "one present per sub-frame index" is checkable against the same rule the
    // pacer uses.
    static constexpr std::uint64_t subframe_index(std::uint64_t t_ns, std::uint64_t anchor_ns,
                                                  std::uint64_t period_ns) {
        return (t_ns <= anchor_ns || period_ns == 0) ? 0 : (t_ns - anchor_ns) / period_ns;
    }

    // Refresh-boundary rule (unchanged behaviour — this is FrameClock's pace()
    // and match_runner's vsync path, verbatim).
    Wait plan_resync(std::uint64_t after_present_ns) {
        if (after_present_ns < target_ns_) {
            const std::uint64_t sleep_ns = target_ns_ - after_present_ns;
            target_ns_ += period_ns_;
            return {sleep_ns, 0};
        }
        target_ns_ = after_present_ns + period_ns_;
        return {0, 0};
    }

    // Lattice rule: the next present lands on the first lattice point STRICTLY
    // after this one. A present that overran one or more boundaries does not
    // get them back — those sub-frames are in the past, and showing them late
    // is the duplicate-present failure this exists to avoid — but it also
    // cannot lose phase, because the next target is read off the lattice rather
    // than off "now".
    Wait plan_subframe(std::uint64_t after_present_ns) {
        target_ns_ =
            anchor_ns_ + (subframe_index(after_present_ns, anchor_ns_, period_ns_) + 1) * period_ns_;
        // Guard, not a rule: an anchor ahead of the present (only reachable if a
        // caller hands one in from the future) would otherwise stall a whole
        // extra period. Never wait longer than one period, whatever the anchor.
        if (target_ns_ > after_present_ns + period_ns_) target_ns_ = after_present_ns + period_ns_;
        const std::uint64_t gap_ns = target_ns_ - after_present_ns;
        return {gap_ns > kSpinTailNs ? gap_ns - kSpinTailNs : 0, target_ns_};
    }

private:
    std::uint64_t period_ns_;
    std::uint64_t target_ns_;
    std::uint64_t anchor_ns_;
};

}  // namespace bomber::platform
