#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

// The datagram transport seam. The lockstep session (lockstep_session.hpp) talks
// only to the abstract Transport, so the SAME session logic runs over a real UDP
// socket (UdpTransport, a later increment) or over an in-memory LoopbackLink
// (headless tests, no sockets). UDP-shaped by design: unordered and unreliable,
// so the session must never assume a packet arrives or arrives in order.

namespace bomber::net {

class Transport {
public:
    Transport() = default;
    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;
    Transport(Transport&&) = delete;
    Transport& operator=(Transport&&) = delete;
    virtual ~Transport() = default;

    // Enqueue one datagram toward the peer (fire-and-forget, like sendto()).
    virtual void send(const std::uint8_t* data, std::size_t size) = 0;
    // Non-blocking receive: fills *out and returns true if a datagram had
    // arrived, else returns false (like a non-blocking recvfrom()).
    virtual bool poll(std::vector<std::uint8_t>* out) = 0;
};

// An in-memory link between two endpoints (sides 0 and 1) for headless tests.
// Models UDP's imperfections DETERMINISTICALLY so session behaviour is
// reproducible: `latency` pumps of delay before a packet is deliverable, and an
// optional `drop_every` (every Nth packet from a side is dropped). step()
// advances the shared delivery clock one pump.
class LoopbackLink {
public:
    explicit LoopbackLink(int latency = 0, int drop_every = 0)
        : latency_(latency), drop_every_(drop_every) {}

    void send(int from, const std::uint8_t* data, std::size_t size) {
        ++sent_[static_cast<std::size_t>(from)];
        if (drop_every_ > 0 && (sent_[static_cast<std::size_t>(from)] % drop_every_) == 0)
            return;  // deterministic drop
        queue_[static_cast<std::size_t>(1 - from)].push_back(
            {step_ + latency_, std::vector<std::uint8_t>(data, data + size)});
    }

    bool poll(int to, std::vector<std::uint8_t>* out) {
        auto& q = queue_[static_cast<std::size_t>(to)];
        for (auto it = q.begin(); it != q.end(); ++it) {
            if (it->deliver_at <= step_) {
                *out = std::move(it->packet);
                q.erase(it);
                return true;
            }
        }
        return false;
    }

    void step() { ++step_; }

private:
    struct Pending {
        std::int64_t deliver_at = 0;
        std::vector<std::uint8_t> packet;
    };
    int latency_;
    int drop_every_;
    std::int64_t step_ = 0;
    std::array<std::int64_t, 2> sent_{};
    std::array<std::deque<Pending>, 2> queue_{};
};

// A Transport endpoint bound to one side of a LoopbackLink.
class LoopbackTransport : public Transport {
public:
    LoopbackTransport(LoopbackLink& link, int side) : link_(&link), side_(side) {}
    void send(const std::uint8_t* data, std::size_t size) override {
        link_->send(side_, data, size);
    }
    bool poll(std::vector<std::uint8_t>* out) override { return link_->poll(side_, out); }

private:
    LoopbackLink* link_;
    int side_;
};

}  // namespace bomber::net
