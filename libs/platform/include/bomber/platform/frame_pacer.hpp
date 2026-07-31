#pragma once

#include <cstdint>

namespace bomber::platform {

// The frame-pacing DECISION, lifted out of the SDL loops so it can be pinned
// headlessly: no SDL here, and no clock of its own — the caller passes the
// instant it measured after its present and gets back what to wait for.
//
// Two rules, because the two front-end modes are paced by different things:
// `plan_resync` follows the display, `plan_subframe` follows the sim's own
// interpolation quantum. Measured evidence for both, the sizing of kSpinTailNs,
// and the 2026-07-29 amendment that reattributed the present cost to the D3D11
// backend: docs/frame-pacing.md.
class FramePacer {
public:
    // What the caller should do before its next present.
    struct Wait {
        std::uint64_t sleep_ns = 0;       // hand to a coarse sleep (it will overshoot)
        std::uint64_t spin_until_ns = 0;  // then busy-wait until this instant (0 = don't)
    };

    // Tail of a SubFrame wait spent busy-waiting instead of sleeping: SDL3's
    // Win11 sleep has a ~0.5 ms floor and overshoots, so sleeping the whole gap
    // lands late every frame. 1 ms rather than 0.5 — see docs/frame-pacing.md
    // "Sizing kSpinTailNs". Only the uncapped path pays it.
    static constexpr std::uint64_t kSpinTailNs = 1'000'000;

    constexpr FramePacer(std::uint64_t period_ns, std::uint64_t now_ns)
        : period_ns_(period_ns ? period_ns : 1),
          target_ns_(now_ns + period_ns_),
          anchor_ns_(now_ns) {}

    constexpr std::uint64_t period_ns() const { return period_ns_; }
    constexpr std::uint64_t target_ns() const { return target_ns_; }
    constexpr std::uint64_t anchor_ns() const { return anchor_ns_; }

    // Live-settable so an F8 toggle takes effect on the very next frame.
    constexpr void set_period(std::uint64_t period_ns) { period_ns_ = period_ns ? period_ns : 1; }

    // Origin of the SubFrame lattice — for a match loop, the wall instant the
    // current sim tick began. Re-stating it every frame is what keeps the
    // presents welded to the interpolator's own boundaries instead of drifting
    // against them.
    constexpr void set_anchor(std::uint64_t anchor_ns) { anchor_ns_ = anchor_ns; }

    // Which lattice cell `t_ns` falls in. Shared with the callers/tests so
    // "one present per sub-frame index" is checkable against the same rule the
    // pacer uses.
    static constexpr std::uint64_t subframe_index(std::uint64_t t_ns, std::uint64_t anchor_ns,
                                                  std::uint64_t period_ns) {
        return (t_ns <= anchor_ns || period_ns == 0) ? 0 : (t_ns - anchor_ns) / period_ns;
    }

    // Refresh-boundary rule: a present that overran re-phases the target onto
    // itself rather than chasing the overrun, which would fight vblank.
    constexpr Wait plan_resync(std::uint64_t after_present_ns) {
        if (after_present_ns < target_ns_) {
            const std::uint64_t sleep_ns = target_ns_ - after_present_ns;
            target_ns_ += period_ns_;
            return {sleep_ns, 0};
        }
        target_ns_ = after_present_ns + period_ns_;
        return {0, 0};
    }

    // Lattice rule: the next present lands on the first lattice point STRICTLY
    // after this one. Boundaries an overrun skipped are NOT made up — showing
    // them late is the duplicate present this exists to avoid — yet phase
    // cannot drift, because the target is read off the lattice, not off "now".
    constexpr Wait plan_subframe(std::uint64_t after_present_ns) {
        target_ns_ = anchor_ns_ +
                     (subframe_index(after_present_ns, anchor_ns_, period_ns_) + 1) * period_ns_;
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
