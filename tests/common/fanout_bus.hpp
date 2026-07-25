#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

#include "bomber/net/transport.hpp"

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
    explicit StarBus(std::size_t endpoints, int latency = 0)
        : queues_(endpoints), endpoints_(endpoints), latency_(latency) {}

    void send(std::size_t from, const std::uint8_t* data, std::size_t size) {
        const std::vector<std::uint8_t> pkt(data, data + size);
        if (from == kHub) {
            for (std::size_t to = 1; to < endpoints_; ++to) enqueue(to, pkt, kHub);  // fan-out
        } else {
            enqueue(kHub, pkt, from);  // a guest talks to the hub and nobody else
        }
    }

    bool poll(std::size_t at, std::vector<std::uint8_t>* out) {
        auto& q = queues_[at];
        for (auto it = q.begin(); it != q.end(); ++it) {
            if (it->deliver_at > step_) continue;
            const std::size_t from = it->from;
            std::vector<std::uint8_t> pkt = std::move(it->packet);
            q.erase(it);
            // The reflection, verbatim and only on the hub's own poll.
            if (at == kHub && from != kHub)
                for (std::size_t to = 1; to < endpoints_; ++to)
                    if (to != from) enqueue(to, pkt, kHub);
            *out = std::move(pkt);
            return true;
        }
        return false;
    }

    void step() { ++step_; }
    std::size_t endpoints() const { return endpoints_; }

    static constexpr std::size_t kHub = 0;

private:
    struct Pending {
        std::int64_t deliver_at = 0;
        std::size_t from = 0;
        std::vector<std::uint8_t> packet;
    };

    void enqueue(std::size_t to, const std::vector<std::uint8_t>& pkt, std::size_t from) {
        queues_[to].push_back({step_ + latency_, from, pkt});
    }

    // Grouped by alignment, as everything in this codebase is
    // (clang-analyzer-optin.performance.Padding).
    std::vector<std::deque<Pending>> queues_;
    std::int64_t step_ = 0;
    std::size_t endpoints_;
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
