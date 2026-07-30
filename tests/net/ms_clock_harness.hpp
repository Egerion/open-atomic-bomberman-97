#pragma once

// TWO PEERS ON INDEPENDENT WALL CLOCKS — the shared harness for every net suite
// whose subject is a DISAGREEMENT BETWEEN THE PEERS rather than the session's own
// arithmetic.
//
// The older suites pump both peers inside one synchronous step(), which can
// express neither a per-machine clock skew nor the frame loop the pump is nested
// inside — so both peers always see the same history, always predict the same
// things, and any bug that needs them to DIVERGE is invisible. This harness gives
// each peer its own frame loop and each direction its own millisecond delay, so
// one peer genuinely predicts and re-simulates where the other does not.
//
// Extracted from test_rollback_pacing.cpp (which measured standing prediction
// lag) when test_kill_tally.cpp needed exactly the same rig for a different
// question. Deliberately holds NO assertions and no scenario knowledge: what to
// measure, and what counts as correct, belongs to the suite.

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

#include "bomber/net/rollback_session.hpp"
#include "bomber/net/transport.hpp"
#include "bomber/sim/constants.hpp"
#include "bomber/sim/types.hpp"

namespace bomber::net::test {

inline constexpr int kTickMs = 50;
inline constexpr int kMaxCatchupTicks = 4;  // match_runner.cpp's own long-stall clamp

inline sim::TickInputs seat_input(int seat, const sim::PlayerInput& in) {
    sim::TickInputs t;
    t.players[static_cast<std::size_t>(seat)] = in;
    return t;
}

// Changes most ticks and is phase-shifted per seat, so "repeat the last remote
// input" is frequently wrong and the rollback path is genuinely exercised.
inline sim::PlayerInput scripted(int seat, std::uint32_t tick) {
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

// A link whose delay is in MILLISECONDS against a clock the driver owns, with
// independent per-direction delay and deterministic jitter. LoopbackLink cannot
// express any of the three: its latency is a single count of synchronous pumps.
//
// ARRIVAL VARIANCE, not merely extra delay (2026-07-30). `jitter_ms` on its own
// draws an INDEPENDENT extra delay per packet — white noise, and the protocol's
// redundant input window absorbs nearly all of it: a datagram delayed on its own
// is superseded by the next one, which carries its ticks as well. What actually
// stalls a peer's confirmation frontier is a RUN of late packets, i.e. the
// correlated delay a filling queue produces, which is also what a path reports as
// high jitter. `persist_pct` is the chance that a packet INHERITS the previous
// packet's extra delay, turning the same spread into bursts.
//
// It defaults to 0, and at 0 the draw is the original single RNG step per packet,
// bit for bit — so every pre-existing scenario in tests/net keeps its exact
// arrival schedule.
class MsLink {
public:
    MsLink(int a_to_b_ms, int b_to_a_ms, int jitter_ms, int persist_pct = 0)
        : delay_{a_to_b_ms, b_to_a_ms}, jitter_(jitter_ms), persist_pct_(persist_pct) {}

    void send(int from, const std::uint8_t* d, std::size_t n, std::int64_t now) {
        const std::size_t f = static_cast<std::size_t>(from);
        std::int64_t extra = 0;
        if (jitter_ > 0) {
            bool inherit = false;
            if (persist_pct_ > 0) {
                rng_ = rng_ * 1103515245U + 12345U;  // test-local, never the sim's stream
                inherit = static_cast<int>((rng_ >> 16) % 100U) < persist_pct_ && seen_[f];
            }
            if (inherit) {
                extra = last_extra_[f];  // the queue is still full: stay late
            } else {
                rng_ = rng_ * 1103515245U + 12345U;
                extra = static_cast<std::int64_t>((rng_ >> 16) % static_cast<unsigned>(jitter_ + 1));
            }
            if (seen_[f]) {
                delta_sum_[f] += extra > last_extra_[f] ? extra - last_extra_[f]
                                                        : last_extra_[f] - extra;
                ++delta_n_[f];
            }
            last_extra_[f] = extra;
            seen_[f] = true;
        }
        q_[static_cast<std::size_t>(1 - from)].push_back(
            {now + delay_[f] + extra, std::vector<std::uint8_t>(d, d + n)});
    }

    bool poll(int to, std::vector<std::uint8_t>* out, std::int64_t now) {
        auto& q = q_[static_cast<std::size_t>(to)];
        for (auto it = q.begin(); it != q.end(); ++it) {
            if (it->at <= now) {
                *out = std::move(it->packet);
                q.erase(it);
                return true;
            }
        }
        return false;
    }

    // Turn the variance on or off part-way through a run. A path does not stay
    // jittery for a whole round — a queue fills, drains, and fills again — and a
    // suite that only ever measures a steady condition never exercises anything
    // that ADAPTS to one. Deliberately leaves the RNG stream and the accumulated
    // per-direction statistics alone, so a scenario that never calls it is
    // bit-for-bit the scenario it was.
    void set_jitter(int jitter_ms, int persist_pct) {
        jitter_ = jitter_ms;
        persist_pct_ = persist_pct;
    }

    // What the model ACTUALLY produced in one direction: the mean |d_i - d_{i-1}|
    // over consecutive packets. That is the same quantity net_stats' `jitter_ms`
    // estimates from ack samples, so a suite can print the path it MODELLED beside
    // the jitter the SESSION derived — rather than asserting against a knob whose
    // relation to the live numbers nobody can check.
    int mean_abs_delta_ms(int from) const {
        const std::size_t f = static_cast<std::size_t>(from);
        return delta_n_[f] == 0 ? 0 : static_cast<int>(delta_sum_[f] / delta_n_[f]);
    }

private:
    struct P {
        std::int64_t at;
        std::vector<std::uint8_t> packet;
    };
    std::array<std::int64_t, 2> delay_;
    int jitter_;
    int persist_pct_;
    unsigned rng_ = 0x1234567U;
    std::array<std::int64_t, 2> last_extra_{};
    std::array<bool, 2> seen_{};
    std::array<std::int64_t, 2> delta_sum_{};
    std::array<std::int64_t, 2> delta_n_{};
    std::array<std::deque<P>, 2> q_{};
};

class MsTransport : public Transport {
public:
    MsTransport(MsLink& link, int side, const std::int64_t& now)
        : link_(&link), side_(side), now_(&now) {}
    void send(const std::uint8_t* data, std::size_t size) override {
        link_->send(side_, data, size, *now_);
    }
    bool poll(std::vector<std::uint8_t>* out) override { return link_->poll(side_, out, *now_); }
    NetPath path() const override { return NetPath::Direct; }

private:
    MsLink* link_;
    int side_;
    const std::int64_t* now_;
};

// MatchRunner::run()'s netplay driver, copied in STRUCTURE: one frame every
// `frame_ms`, the accumulator clamped to kMaxCatchupTicks, one session pump per
// 50 ms crossing, and the accumulator spent whether or not the pump advanced.
// The clamp is what makes a long freeze permanent, so it has to be here.
struct Driver {
    int seat = 0;
    int frame_ms = 6;  // uncapped sub-frame lattice: ~166 fps
    int wobble = 0;    // erratic frame pacing, deterministically generated
    std::int64_t last = 0;
    std::int64_t acc = 0;
    std::int64_t next_frame = 0;
    int pumps = 0;
    unsigned rng = 0x9E3779B9U;

    // `after` runs immediately after each pump, where MatchRunner does its own
    // per-tick bookkeeping (the kill tally among it).
    template <class AfterPump>
    void frame(RollbackSession& s, std::int64_t now, AfterPump&& after) {
        acc += now - last;
        last = now;
        if (acc > kMaxCatchupTicks * kTickMs) acc = kMaxCatchupTicks * kTickMs;
        while (acc >= kTickMs) {
            s.advance(seat_input(seat, scripted(seat, s.predicted_tick())), now);
            ++pumps;
            after(s);
            acc -= kTickMs;
        }
        std::int64_t extra = 0;
        if (wobble > 0) {
            rng = rng * 1103515245U + 12345U;
            extra = static_cast<std::int64_t>((rng >> 16) % static_cast<unsigned>(wobble + 1));
        }
        next_frame = now + frame_ms + extra;
    }

    void frame(RollbackSession& s, std::int64_t now) {
        frame(s, now, [](RollbackSession&) {});
    }
};

}  // namespace bomber::net::test
