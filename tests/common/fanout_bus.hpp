#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

#include "bomber/net/transport.hpp"
#include "correlated_delay.hpp"

// SOCKET-FREE MULTI-ENDPOINT TOPOLOGIES for the bomber::net suites. Two peers fit
// in a LoopbackLink; three or more need a shape, and the shape is the thing under
// test — so it lives here rather than in one suite's anonymous namespace.
//
//   FanoutBus — a BROADCAST bus: everyone hears everyone, one step-clock tick of
//               latency per destination. It models the star's EFFECT without its
//               mechanism, which is what a session-level test wants (does the
//               per-seat logic hold when N peers all talk?).
//
//   StarBus   — the production topology's MECHANISM (star_hub_transport.hpp):
//               endpoint 0 is the HUB, 1..N-1 are guests. A guest's datagram
//               reaches ONLY the hub; the other guests receive it because the hub
//               REFLECTS it — and, exactly as in StarHubTransport, only when the
//               hub POLLS. A test that stops pumping the hub therefore severs
//               guest↔guest traffic, which is the real failure mode of forgetting
//               setup_session.hpp's one-pump-at-a-time obligation.
//
// Both are deterministic: nothing sleeps, nothing reads a clock, and delivery is
// driven by an explicit step() the caller advances.

namespace bomber::test {

// A broadcast bus with N endpoints: send() from one endpoint queues the datagram
// for every OTHER endpoint, deliverable `latency[to]` steps later. Latency is per
// DESTINATION so a test can make one peer speculate (and roll back) heavily while
// another never mispredicts at all.
class FanoutBus {
public:
    FanoutBus(std::size_t endpoints, int latency)
        : queues_(endpoints), latency_(endpoints, latency) {}
    explicit FanoutBus(const std::vector<int>& latency)
        : queues_(latency.size()), latency_(latency) {}

    void send(std::size_t from, const std::uint8_t* data, std::size_t size) {
        for (std::size_t to = 0; to < queues_.size(); ++to) {
            if (to == from) continue;
            queues_[to].push_back(
                {step_ + latency_[to], std::vector<std::uint8_t>(data, data + size)});
        }
    }

    // Inject a datagram addressed to ONE endpoint (the test playing the part of a
    // peer whose own session we do not run).
    void inject(std::size_t to, const std::vector<std::uint8_t>& pkt, int extra_latency = 0) {
        queues_[to].push_back({step_ + latency_[to] + extra_latency, pkt});
    }

    bool poll(std::size_t to, std::vector<std::uint8_t>* out) {
        auto& q = queues_[to];
        for (auto it = q.begin(); it != q.end(); ++it) {
            if (it->deliver_at > step_) continue;
            *out = std::move(it->packet);
            q.erase(it);
            return true;
        }
        return false;
    }

    void step() { ++step_; }

private:
    struct Pending {
        std::int64_t deliver_at = 0;
        std::vector<std::uint8_t> packet;
    };
    std::vector<std::deque<Pending>> queues_;
    std::vector<int> latency_;  // indexed by DESTINATION endpoint
    std::int64_t step_ = 0;
};

class BusTransport : public net::Transport {
public:
    BusTransport(FanoutBus& bus, std::size_t id) : bus_(&bus), id_(id) {}
    void send(const std::uint8_t* data, std::size_t size) override { bus_->send(id_, data, size); }
    bool poll(std::vector<std::uint8_t>* out) override { return bus_->poll(id_, out); }

private:
    FanoutBus* bus_;
    std::size_t id_;
};

// The HOST-RELAY STAR, modelled the way StarHubTransport actually behaves.
// Endpoint 0 is the hub. Reflection happens inside the hub's poll(), so a guest
// only ever hears another guest because the hub was pumped.
class StarBus {
public:
    // `jitter_steps` / `persist_pct` add ARRIVAL VARIANCE on top of the flat
    // latency; correlated_delay.hpp owns the model and says why it is correlated
    // rather than white noise. Both default to 0, and at 0 the model draws
    // nothing at all, so every pre-existing star scenario keeps its exact arrival
    // schedule. The knob exists so host migration can be exercised on a link
    // jittery enough for the arrival-variance absorber (rollback_session.hpp) to
    // be ACTIVE when the hub dies — the seam between the two features, which
    // neither suite covered.
    explicit StarBus(std::size_t endpoints, int latency = 0, int jitter_steps = 0,
                     int persist_pct = 0, unsigned seed = 0x2468ACEU)
        : queues_(endpoints),
          delay_(/*lanes=*/1, jitter_steps, persist_pct, seed),
          endpoints_(endpoints),
          latency_(latency) {}

    void send(std::size_t from, const std::uint8_t* data, std::size_t size) {
        if (dead(from)) return;  // a corpse's socket sends nothing
        const std::vector<std::uint8_t> pkt(data, data + size);
        if (from == hub_) {
            for (std::size_t to = 0; to < endpoints_; ++to)
                if (to != hub_) enqueue(to, pkt, hub_);  // fan-out
        } else {
            enqueue(hub_, pkt, from);  // a guest talks to the hub and nobody else
        }
    }

    bool poll(std::size_t at, std::vector<std::uint8_t>* out) {
        auto& q = queues_[at];
        for (auto it = q.begin(); it != q.end(); ++it) {
            if (it->deliver_at > step_) continue;
            const std::size_t from = it->from;
            std::vector<std::uint8_t> pkt = std::move(it->packet);
            q.erase(it);
            // The reflection, verbatim and only on the CURRENT hub's own poll.
            if (at == hub_ && from != hub_)
                for (std::size_t to = 0; to < endpoints_; ++to)
                    if (to != from && to != hub_) enqueue(to, pkt, hub_);
            *out = std::move(pkt);
            return true;
        }
        return false;
    }

    void step() { ++step_; }
    std::size_t endpoints() const { return endpoints_; }

    // --- host migration support (tests/net/test_host_migration.cpp) -----------

    // THE REWIRE. Moves the reflector, exactly as a migration rebuilds a
    // StarHubTransport on the promoted guest's own socket. Datagrams already in
    // flight toward the old hub are deliberately NOT re-routed: they were
    // addressed to a machine that is gone, and losing them is the real outage.
    void set_hub(std::size_t hub) { hub_ = hub; }
    std::size_t hub() const { return hub_; }

    // A GENUINELY DEAD endpoint — it stops responding, it does not leave
    // politely. Nothing it sends leaves, nothing addressed to it is ever
    // delivered, and (when it is the hub) nothing it would have reflected is
    // reflected. This is the distinction the task cares about: a peer that
    // announces its exit is the easy case and is not what kills a real match.
    void kill(std::size_t at) {
        dead_.push_back(at);
        queues_[at].clear();
    }
    bool dead(std::size_t at) const {
        for (const std::size_t d : dead_)
            if (d == at) return true;
        return false;
    }

    static constexpr std::size_t kHub = 0;

private:
    struct Pending {
        std::int64_t deliver_at = 0;
        std::size_t from = 0;
        std::vector<std::uint8_t> packet;
    };

    void enqueue(std::size_t to, const std::vector<std::uint8_t>& pkt, std::size_t from) {
        if (dead(to)) return;  // nothing is ever delivered to a corpse
        queues_[to].push_back({step_ + latency_ + delay_.draw(0), from, pkt});
    }

    // Grouped by alignment, as everything in this codebase is
    // (clang-analyzer-optin.performance.Padding).
    std::vector<std::deque<Pending>> queues_;
    std::vector<std::size_t> dead_;
    CorrelatedDelay delay_;  // one lane: the star has a single shared schedule
    std::int64_t step_ = 0;
    std::size_t endpoints_;
    std::size_t hub_ = kHub;
    int latency_;
};

class StarTransport : public net::Transport {
public:
    StarTransport(StarBus& bus, std::size_t id) : bus_(&bus), id_(id) {}
    void send(const std::uint8_t* data, std::size_t size) override { bus_->send(id_, data, size); }
    bool poll(std::vector<std::uint8_t>* out) override { return bus_->poll(id_, out); }

private:
    StarBus* bus_;
    std::size_t id_;
};

}  // namespace bomber::test
