#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "bomber/net/transport.hpp"

// THE SEAM HOST MIGRATION MOVES BEHIND (ADR-0011 decision 5, design §8.3).
//
// When a star's hub dies, EVERY survivor's transport has to change underneath a
// session that must not notice: the new hub needs a StarHubTransport where it had
// a point-to-point socket, and every other survivor needs its socket re-aimed
// (in a star a guest punched ONLY to the hub, so it has no path to its new one)
// or relayed. `Transport` already makes the session indifferent to which; this
// is the INDIRECTION that lets the answer change MID-MATCH. It owns nothing —
// the concrete transports stay owned by whoever built them (LobbyFlow).
//
// DETACHED IS A REAL STATE, NOT AN ERROR. Between the old hub's death and the
// new star being punched there is genuinely nowhere to send, so a detached
// transport DROPS and polls empty rather than aiming datagrams at a corpse. The
// session above is expected to be held (RollbackSession::set_migration_hold) for
// exactly that window.
namespace bomber::net {

class MigratingTransport : public Transport {
public:
    // `initial` is the transport the match starts on — LobbyFlow::transport()'s
    // answer at connect time. BORROWED, like every later target. Passing nullptr
    // starts detached (a session built before any path exists).
    explicit MigratingTransport(Transport* initial = nullptr) : target_(initial) {}

    void send(const std::uint8_t* data, std::size_t size) override;
    bool poll(std::vector<std::uint8_t>* out) override;

    // Delegated so the diagnostics name the path actually carrying the match
    // rather than this wrapper. Asked ONCE, in the RollbackSession ctor, so it
    // reports the path the round STARTED on — for a per-round statistic the
    // pre-migration path is the honest answer.
    NetPath path() const override { return target_ != nullptr ? target_->path() : NetPath::Unknown; }

    // The far end moved: from here on everything goes through `t`. Idempotent —
    // re-attaching the same object is how the caller re-reads LobbyFlow's answer
    // without tracking which of the three shapes it got.
    void attach(Transport& t) { target_ = &t; }
    // There is no path at all (the hub died; nothing has replaced it yet).
    void detach() { target_ = nullptr; }
    bool attached() const { return target_ != nullptr; }

    // Datagrams poll() has DELIVERED, ever. The arrival evidence PathFailover's
    // traffic arm reads — counted here rather than taken from NetStats, because
    // the stats are instrumentation by contract ("consulted by no decision") and
    // a failover is very much a decision.
    std::uint64_t rx_polled() const { return rx_polled_; }

private:
    Transport* target_;  // BORROWED; null = detached
    std::uint64_t rx_polled_ = 0;
};

}  // namespace bomber::net
