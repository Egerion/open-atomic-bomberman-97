#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "bomber/net/transport.hpp"

// THE SEAM HOST MIGRATION MOVES BEHIND (ADR-0011 decision 5, design §8.3).
//
// A >2-seat match runs over a STAR: every guest's datagrams go to the hub and
// reach the other guests only because the hub reflects them
// (star_hub_transport.hpp). When the hub is the machine that dies, the survivors
// elect a new one (§8.2) — and then EVERY peer's transport has to change
// underneath a session that must not notice:
//
//   * the new hub needs a StarHubTransport over its own socket, fanning out to
//     the surviving guests, where it previously had a plain point-to-point
//     socket aimed at the old hub;
//   * every other survivor needs its socket re-aimed at the new hub (a fresh
//     hole punch — in a star a guest punched ONLY to the hub, so it has no path
//     to its new one), or a RelayedTransport if that punch fails.
//
// `Transport` is already the abstraction that makes the session indifferent to
// which of those it is talking through. This class is the INDIRECTION that lets
// the answer change mid-match: the RollbackSession borrows one of these for the
// whole round and the migration re-points it, so nothing above ever learns that
// the far end moved. It owns nothing — the concrete transports stay owned by
// whoever built them (LobbyFlow), exactly as they were before.
//
// DETACHED IS A REAL STATE, NOT AN ERROR. Between the old hub's death and the
// new star being punched there is genuinely nowhere to send: a guest's socket
// still points at a corpse. Rather than let those datagrams leave for an address
// that will never answer, a detached transport DROPS them and polls empty. The
// session above is expected to be held (RollbackSession::set_migration_hold) for
// exactly this window.
//
// It used to COUNT those drops as well, "so the caller can say how long the
// outage really was". Nothing ever read the counter — the caller that would have
// is the unbuilt re-anchoring half (design §8.3) — so it went on 2026-07-31. A
// future driver wanting the outage length should measure it where the decision
// lives (RollbackSession::host_lost_seats + the hold it drives), not out of a
// side effect of send().
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
    // rather than this wrapper (net_stats.hpp). It is asked once, in the
    // RollbackSession ctor, so what it reports is the path the round STARTED on
    // — after a migration the overlay's label is stale by design. Recording the
    // pre-migration path is the honest answer for a per-round statistic.
    NetPath path() const override { return target_ != nullptr ? target_->path() : NetPath::Unknown; }

    // The far end moved: from here on everything goes through `t`. Idempotent —
    // re-attaching the same object is how the caller re-reads LobbyFlow's answer
    // without tracking which of the three shapes it got.
    void attach(Transport& t) { target_ = &t; }
    // There is no path at all (the hub died; nothing has replaced it yet).
    void detach() { target_ = nullptr; }
    bool attached() const { return target_ != nullptr; }

private:
    Transport* target_;  // BORROWED; null = detached
};

}  // namespace bomber::net
