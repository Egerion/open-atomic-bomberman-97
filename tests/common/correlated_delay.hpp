#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// THE ARRIVAL-VARIANCE MODEL the socket-free links share, in the layer both of
// them sit under.
//
// WHY CORRELATED AND NOT WHITE NOISE. An independent extra delay per packet is
// absorbed almost entirely by the protocol's redundant input window: a datagram
// delayed on its own is superseded by the next one, which carries its ticks as
// well. What actually stalls a peer's confirmation frontier is a RUN of late
// packets — the correlated delay a filling queue produces, and also what a path
// reports as high jitter. `persist_pct` is the chance a packet INHERITS the
// previous one's extra delay, turning the same spread into bursts.
//
// AT jitter <= 0 NOT ONE RNG STEP IS TAKEN, so a link built without variance is
// bit-identical to one that never had the knob. That property is load-bearing:
// the arrival schedule is consumed in send order, so any change to how many
// datagrams a peer sends reshuffles every later delay, and a jittery run is
// therefore CHAOTIC. Suites asserting on a jittery path should sweep seeds and
// assert on the aggregate.
//
// LANES are independent directions sharing ONE rng stream. tests/net's MsLink
// gives each direction its own lane (per-direction delay is the whole point);
// fanout_bus.hpp's StarBus has a single shared queue and uses lane 0. The stream
// is deliberately shared across lanes — that is what both callers did before this
// was extracted, and it is what keeps their existing schedules byte-for-byte.

namespace bomber::test {

class CorrelatedDelay {
public:
    // `seed` picks WHICH realisation of the delay process a run gets, which
    // matters more than it looks — see the chaos note above.
    explicit CorrelatedDelay(std::size_t lanes, int jitter, int persist_pct, unsigned seed)
        : last_(lanes, 0), seen_(lanes, false), sum_(lanes, 0), n_(lanes, 0),
          jitter_(jitter), persist_pct_(persist_pct), rng_(seed) {}

    // Turn the variance on or off part-way through a run. A path does not stay
    // jittery for a whole round — a queue fills, drains and fills again — and a
    // suite that only measures a steady condition never exercises anything that
    // ADAPTS to one. Deliberately leaves the RNG stream and the accumulated
    // statistics alone, so a scenario that never calls it is unchanged.
    void set(int jitter, int persist_pct) {
        jitter_ = jitter;
        persist_pct_ = persist_pct;
    }

    std::int64_t draw(std::size_t lane) {
        if (jitter_ <= 0) return 0;
        const std::int64_t extra = inherit(lane) ? last_[lane] : fresh();
        record(lane, extra);
        return extra;
    }

    // What the model ACTUALLY produced on one lane: the mean |d_i - d_{i-1}| over
    // consecutive packets. That is the same quantity net_stats' `jitter_ms`
    // estimates from ack samples, so a suite can print the path it MODELLED beside
    // the jitter the SESSION derived, rather than asserting against a knob whose
    // relation to the live numbers nobody can check.
    int mean_abs_delta(std::size_t lane) const {
        return n_[lane] == 0 ? 0 : static_cast<int>(sum_[lane] / n_[lane]);
    }

private:
    bool inherit(std::size_t lane) {
        if (persist_pct_ <= 0) return false;
        step();  // test-local stream, never the sim's
        return static_cast<int>((rng_ >> 16) % 100U) < persist_pct_ && seen_[lane];
    }

    std::int64_t fresh() {
        step();
        return static_cast<std::int64_t>((rng_ >> 16) % static_cast<unsigned>(jitter_ + 1));
    }

    void record(std::size_t lane, std::int64_t extra) {
        if (seen_[lane]) {
            sum_[lane] += extra > last_[lane] ? extra - last_[lane] : last_[lane] - extra;
            ++n_[lane];
        }
        last_[lane] = extra;
        seen_[lane] = true;
    }

    void step() { rng_ = rng_ * 1103515245U + 12345U; }

    std::vector<std::int64_t> last_;
    std::vector<bool> seen_;
    std::vector<std::int64_t> sum_;
    std::vector<std::int64_t> n_;
    int jitter_;
    int persist_pct_;
    unsigned rng_;
};

}  // namespace bomber::test
