#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "bomber/net/migrating_transport.hpp"
#include "bomber/net/protocol.hpp"
#include "bomber/net/rollback_session.hpp"
#include "bomber/sim/simulation.hpp"
#include "fanout_bus.hpp"
#include "helpers.hpp"  // bomber::sim::test::open_config

// HOST MIGRATION (wire v9, ADR-0011 decision 5, docs/online-multiplayer-design
// .md §8). What is under test is the half that lives in libs/net: detecting that
// the HUB itself died, agreeing a handoff tick without being able to ask the one
// machine that used to decide such things, re-electing a hub with no coordinator,
// and converging afterwards without reporting a desync that never happened.
//
// THE HOST HERE IS GENUINELY DEAD, never politely absent. StarBus::kill() stops
// its datagrams leaving, stops anything reaching it, and — because it was the
// reflector — severs guest<->guest traffic entirely. A test whose "dead" peer
// merely stops calling advance() would still have a live reflector and would
// prove almost nothing, since the survivors could still hear each other.
//
// EVERY PEER IS DRIVEN FOR REAL. There are no scripted stand-ins and no injected
// packets: each surviving seat runs its own RollbackSession over its own
// transport, so anything the peers agree on they agreed on by exchanging real
// datagrams. That is the only way the "no coordinator" claim means anything.

using namespace bomber;  // NOLINT(google-build-using-namespace) — test-local
using bomber::sim::test::open_config;

namespace {

constexpr std::uint16_t kSeat0 = 0x1;
constexpr std::uint16_t kSeat1 = 0x2;
constexpr std::uint16_t kSeat2 = 0x4;
constexpr std::uint16_t kAll3 = 0x7;
constexpr int kTimeout = 40;  // pumps of silence before a seat is declared dropped

sim::MatchConfig three_config() {
    sim::MatchConfig cfg = open_config();
    cfg.spawns = {{0, 0}, {14, 10}, {14, 0}};
    cfg.player_count = 3;
    return cfg;
}

// A pure function of (seat, tick) so every peer's re-simulation derives the same
// input, phase-shifted per seat so prediction genuinely mispredicts.
sim::PlayerInput scripted(int seat, std::uint32_t tick) {
    sim::PlayerInput in;
    const std::uint32_t phase = (tick + static_cast<std::uint32_t>(seat) * 3) % 8;
    in.left = phase == 1 || phase == 2;
    in.right = phase == 5 || phase == 6;
    in.up = phase == 3;
    in.down = phase == 7;
    in.action1 = phase == 4;
    return in;
}

sim::TickInputs seat_input(int seat, const sim::PlayerInput& in) {
    sim::TickInputs t;
    t.players[static_cast<std::size_t>(seat)] = in;
    return t;
}

// A LOSSY INBOUND LEG for one peer. While deaf it drains and DISCARDS whatever
// has arrived — genuinely losing it, not merely delaying it into a later burst —
// so a peer can be made to hold strictly less of the dying hub's final output
// than its neighbour does. That asymmetry is the whole subject of the
// divergent-frontier case below, and it has to be produced rather than injected:
// every seat still runs its own real session over its own real transport.
class DeafGate : public net::Transport {
public:
    explicit DeafGate(net::Transport& inner) : inner_(&inner) {}
    void send(const std::uint8_t* data, std::size_t size) override { inner_->send(data, size); }

    bool poll(std::vector<std::uint8_t>* out) override {
        while (inner_->poll(out)) {
            if (!drop_frame(*out)) return true;
        }
        return false;
    }
    net::NetPath path() const override { return inner_->path(); }

    // Lose EVERYTHING inbound.
    void set_deaf(bool deaf) { deaf_ = deaf; }
    // Lose only input frames carrying `seat`, and keep the rest — including the
    // hashes and inputs the OTHER survivor's traffic is made of. That models a
    // hub whose game loop has wedged (it stops producing its own input) while its
    // network layer keeps REFLECTING for a while, which is the only shape in
    // which one survivor ends up holding the other's confirmed hashes for ticks
    // above the handoff it is about to adopt. Blunt deafness cannot produce it,
    // because it throws away those hashes too.
    void mute_seat(int seat) { muted_ = seat; }
    void unmute() { muted_ = -1; }

private:
    bool drop_frame(const std::vector<std::uint8_t>& pkt) const {
        if (deaf_) return true;
        if (muted_ < 0) return false;
        net::Message m;
        if (!net::decode(pkt.data(), pkt.size(), &m)) return false;
        const std::uint16_t bit = static_cast<std::uint16_t>(1U << muted_);
        if (m.type == net::MsgType::InputRange) return (m.range.seat_mask & bit) != 0;
        if (m.type == net::MsgType::Input) return (m.input.seat_mask & bit) != 0;
        return false;
    }

    net::Transport* inner_;
    int muted_ = -1;
    bool deaf_ = false;
};

// One surviving peer: its own sim, its own session, and the MigratingTransport
// indirection the production path uses so the far end can be re-pointed without
// the session learning that it moved.
struct Peer {
    Peer(std::size_t endpoint, int seat, std::uint16_t all, test::StarBus& bus, int host_seat)
        : sim(three_config()),
          link(bus, endpoint),
          ear(link),
          wire(&ear),
          session(sim, static_cast<std::uint16_t>(1U << seat), all, /*max_prediction=*/8, wire,
                  net::DropPolicy{/*revert_to_ai=*/true, /*is_host=*/seat == host_seat, kTimeout,
                                  /*host_seat=*/host_seat}),
          seat_index(seat) {}

    void pump(std::uint32_t tick) {
        session.advance(seat_input(seat_index, scripted(seat_index, tick)));
    }

    sim::Simulation sim;
    test::StarTransport link;
    DeafGate ear;
    net::MigratingTransport wire;
    net::RollbackSession session;
    int seat_index;
};

}  // namespace

TEST_CASE("host migration: a dead hub is detected, re-elected around, and the match continues") {
    // A 3-SEAT STAR, which is where the hub role actually matters: seat 0 is the
    // hub and the only reason seats 1 and 2 can hear each other at all.
    test::StarBus bus(3, /*latency=*/1);
    Peer host(0, 0, kAll3, bus, /*host_seat=*/0);
    Peer g1(1, 1, kAll3, bus, /*host_seat=*/0);
    Peer g2(2, 2, kAll3, bus, /*host_seat=*/0);

    std::uint32_t t = 0;
    for (; t < 40; ++t) {
        host.pump(t);
        g1.pump(t);
        g2.pump(t);
        bus.step();
    }
    REQUIRE(host.session.confirmed_tick() > 20);  // a healthy match first
    CHECK(g1.session.current_hub() == 0);
    CHECK(g2.session.current_hub() == 0);
    CHECK(host.session.hosting());
    CHECK_FALSE(g1.session.hosting());

    // THE HUB DIES. Not "leaves" — it stops responding, and with it the
    // reflection that carried g1<->g2.
    bus.kill(0);

    const std::uint32_t frozen = g1.session.confirmed_tick();
    for (int i = 0; i < kTimeout + 30; ++i, ++t) {
        g1.pump(t);
        g2.pump(t);
        bus.step();
    }

    // Both survivors independently concluded the hub is gone. Neither could have
    // been told: the machine that would have decreed it is the corpse.
    CHECK(g1.session.host_lost_seats() == kSeat0);
    CHECK(g2.session.host_lost_seats() == kSeat0);

    // THE ELECTION, with no coordinator and no vote exchanged: seat 0 is gone, so
    // the lowest surviving seat takes the role. Both agree, and exactly one of
    // them believes it is hosting.
    CHECK(g1.session.current_hub() == 1);
    CHECK(g2.session.current_hub() == 1);
    CHECK(g1.session.hosting());
    CHECK_FALSE(g2.session.hosting());

    // The seat was handed to the AI at the same tick on both, which is what keeps
    // the deterministic sim agreeing.
    CHECK(g1.session.handoff_tick(0) != net::RollbackSession::kNoHandoff);
    CHECK(g1.session.handoff_tick(0) == g2.session.handoff_tick(0));

    // THE REWIRE: the promoted seat becomes the reflector, and every survivor's
    // MigratingTransport is re-pointed at it. In production this is a fresh punch
    // plus a rebuilt StarHubTransport; here it is the same move, modelled.
    bus.set_hub(1);
    for (int i = 0; i < 120; ++i, ++t) {
        g1.pump(t);
        g2.pump(t);
        bus.step();
    }

    // The match is RUNNING again — the frontier moved well past where the hub's
    // death pinned it — and nobody reported a divergence.
    CHECK(g1.session.confirmed_tick() > frozen + 40);
    CHECK(g2.session.confirmed_tick() > frozen + 40);
    CHECK_FALSE(g1.session.desynced());
    CHECK_FALSE(g2.session.desynced());
    CHECK_FALSE(g1.session.aborted());
    CHECK_FALSE(g2.session.aborted());
    CHECK(g1.sim.state().players[0].ai);  // the dead hub's seat is the AI's now
    CHECK(g2.sim.state().players[0].ai);
}

TEST_CASE("host migration: survivors that adopt DIFFERENT ticks still converge") {
    // THE HARD CASE, and the one that only appears with two survivors. They hold
    // different amounts of the dying hub's final output, so they propose
    // different handoff ticks; the peer that received MORE can already have
    // CONFIRMED past the tick it must adopt. Adopting therefore has to un-confirm
    // (rewind_for_migration) and purge hashes on both sides, or the lagging peer
    // compares its correct post-handoff hash against the other's stale
    // pre-handoff one and reports a divergence that never happened.
    //
    // The asymmetry is produced by making ONE survivor DEAF for a stretch before
    // the death — it keeps sending, so its neighbour's frontier climbs normally,
    // but it loses everything inbound, so it ends up holding strictly less of the
    // hub's final output. That is what puts g1's confirmed frontier ABOVE the
    // tick g2 will go on to propose, which is the only configuration in which
    // adopting forces an un-confirm.
    test::StarBus bus(3, /*latency=*/1);
    Peer host(0, 0, kAll3, bus, /*host_seat=*/0);
    Peer g1(1, 1, kAll3, bus, /*host_seat=*/0);
    Peer g2(2, 2, kAll3, bus, /*host_seat=*/0);

    std::uint32_t t = 0;
    for (; t < 40; ++t) {
        host.pump(t);
        g1.pump(t);
        g2.pump(t);
        bus.step();
    }
    REQUIRE(host.session.confirmed_tick() > 20);

    // The hub WEDGES before it dies: g2 stops hearing seat 0's own input while
    // the hub still reflects, so g1 — which still hears everything — confirms on
    // past the tick g2 will end up proposing, AND BROADCASTS ITS HASHES for those
    // ticks, which g2 files away. Those stale hashes describe a history that
    // included the hub's input, and they are what makes the naive convergence
    // report a divergence that never happened.
    //
    // Muting only seat 0's frames (rather than deafening g2 outright) is what
    // produces that: blunt deafness would discard g1's hashes too, and then there
    // would be nothing stale left to compare against.
    g2.ear.mute_seat(0);
    for (int i = 0; i < 14; ++i, ++t) {
        host.pump(t);
        g1.pump(t);
        g2.pump(t);
        bus.step();
    }
    bus.kill(0);
    // The mute stays on a few pumps past the death so g2 also loses the hub's
    // still-in-flight backlog; otherwise that backlog arrives a moment later and
    // silently restores exactly the knowledge the case is trying to withhold.
    // Selective, so g1's traffic — and its stale hashes — keep flowing.
    for (int i = 0; i < 4; ++i, ++t) {
        g1.pump(t);
        g2.pump(t);
        bus.step();
    }
    g2.ear.unmute();

    const std::uint32_t g1_conf = g1.session.confirmed_tick();
    const std::uint32_t g2_conf = g2.session.confirmed_tick();
    // The asymmetry the case needs actually exists, and in the direction that
    // makes the adoption move a frontier BACKWARDS rather than merely forwards.
    REQUIRE(g1_conf > g2_conf);

    bus.set_hub(1);
    for (int i = 0; i < kTimeout + 200; ++i, ++t) {
        g1.pump(t);
        g2.pump(t);
        bus.step();
    }

    // They converged on ONE tick — the lower of the two proposals — with no
    // agreement protocol, only "lowest wins" applied independently on each.
    CHECK(g1.session.handoff_tick(0) == g2.session.handoff_tick(0));
    // Deliberately NOT asserted against either peer's confirmed frontier: a peer
    // can HOLD input from seat 0 well above its own confirmed tick (confirming
    // needs EVERY seat), so the handoff legitimately lands above it. The claim
    // that matters is the agreement above, plus the absence of a phantom desync.
    // And, the point of the case: no phantom desync from the stale hashes.
    CHECK_FALSE(g1.session.desynced());
    CHECK_FALSE(g2.session.desynced());
    CHECK(g1.session.confirmed_tick() > g1_conf + 40);
    CHECK(g2.session.confirmed_tick() > g2_conf + 40);
}

TEST_CASE("host migration: releasing the hold does not cascade the surviving table") {
    // By the time a migration begins, EVERY remote seat has been silent for
    // longer than the drop timeout — that is how the hub's loss was detected. So
    // releasing the stall without re-arming the silence counters declares the
    // whole surviving table dropped one pump later, and the survivors then hand
    // each other's seats to the AI at different ticks and desync.
    test::StarBus bus(3, /*latency=*/1);
    Peer host(0, 0, kAll3, bus, /*host_seat=*/0);
    Peer g1(1, 1, kAll3, bus, /*host_seat=*/0);
    Peer g2(2, 2, kAll3, bus, /*host_seat=*/0);

    std::uint32_t t = 0;
    for (; t < 40; ++t) {
        host.pump(t);
        g1.pump(t);
        g2.pump(t);
        bus.step();
    }
    bus.kill(0);

    // The driver's real order, which matters: DETECT first, then hold. The hold
    // suppresses drop detection, so arming it before the loss is noticed would
    // simply stop the migration from ever being decided.
    for (int i = 0; i < kTimeout + 10; ++i, ++t) {
        g1.pump(t);
        g2.pump(t);
        bus.step();
    }
    REQUIRE(g1.session.host_lost_seats() == kSeat0);
    REQUIRE(g2.session.host_lost_seats() == kSeat0);

    // The outage is held OPEN for ten drop timeouts — far longer than the rewire
    // would really take — with the sim stalled. Nothing may rot in that window.
    g1.session.set_migration_hold(true);
    g2.session.set_migration_hold(true);
    for (int i = 0; i < kTimeout * 10; ++i, ++t) {
        g1.pump(t);
        g2.pump(t);
        bus.step();
    }
    CHECK(g1.session.migration_held());
    // Only the HUB's seat is dropped. The survivors did not accuse each other,
    // even though they were mutually inaudible for the whole outage.
    CHECK(g1.session.dropped_seats() == kSeat0);
    CHECK(g2.session.dropped_seats() == kSeat0);

    bus.set_hub(1);
    g1.session.set_migration_hold(false);
    g2.session.set_migration_hold(false);
    for (int i = 0; i < 150; ++i, ++t) {
        g1.pump(t);
        g2.pump(t);
        bus.step();
    }

    // Still only the hub. No cascade, no desync, and the match advanced.
    CHECK(g1.session.dropped_seats() == kSeat0);
    CHECK(g2.session.dropped_seats() == kSeat0);
    CHECK_FALSE(g1.session.desynced());
    CHECK_FALSE(g2.session.desynced());
    CHECK_FALSE(g1.session.aborted());
    CHECK_FALSE(g2.session.aborted());
}

TEST_CASE("host migration: the confirmed event stream never drains a tick twice") {
    // THE COLLISION BETWEEN THIS FEATURE AND THE CONFIRMED EVENT STREAM.
    // advance_confirmed() pushes each tick's events into the stream as the
    // frontier rises, on the reasonable assumption that the frontier only rises.
    // rewind_for_migration() breaks that assumption — it is the one place in the
    // class that moves confirmed_ BACKWARDS — so the re-walk would push the
    // rewound window's events a second time and an accumulator fed from the
    // stream would double-count every kill in it. That is the very bug the
    // confirmed stream exists to prevent, arriving through a door its author
    // could not have known about, and NOTHING would catch it: events are
    // excluded from state_hash by design, so the desync check, the goldens and
    // build_hash are all blind to it.
    //
    // The pin expresses the property DIRECTLY rather than counting: events for
    // tick t can only enter the stream as the frontier passes t, so a drain that
    // yields events while the frontier sits at or below one we have already
    // drained past is, by definition, a tick being emitted a second time. A
    // count-based pin is too weak here — this scenario produces few deaths, so a
    // duplicated window hides comfortably under any plausible bound.
    test::StarBus bus(3, /*latency=*/1);
    Peer host(0, 0, kAll3, bus, /*host_seat=*/0);
    Peer g1(1, 1, kAll3, bus, /*host_seat=*/0);
    Peer g2(2, 2, kAll3, bus, /*host_seat=*/0);

    std::vector<sim::Event> drained;
    std::uint32_t drained_through = 0;
    bool re_emitted = false;
    std::size_t total = 0;
    const auto drain = [&]() {
        std::vector<sim::Event> chunk;
        g1.session.drain_confirmed_events(chunk);
        const std::uint32_t c = g1.session.confirmed_tick();
        if (!chunk.empty() && c <= drained_through) re_emitted = true;
        if (c > drained_through) drained_through = c;
        total += chunk.size();
        drained.insert(drained.end(), chunk.begin(), chunk.end());
    };

    std::uint32_t t = 0;
    for (; t < 40; ++t) {
        host.pump(t);
        g1.pump(t);
        g2.pump(t);
        drain();
        bus.step();
    }

    // Reproduce the divergent-frontier setup, which is the shape that forces the
    // un-confirm: g2 loses only seat 0's frames, so g1 confirms past the tick g2
    // will propose and must later be rewound below its own frontier.
    g2.ear.mute_seat(0);
    for (int i = 0; i < 14; ++i, ++t) {
        host.pump(t);
        g1.pump(t);
        g2.pump(t);
        drain();
        bus.step();
    }
    bus.kill(0);
    for (int i = 0; i < 4; ++i, ++t) {
        g1.pump(t);
        g2.pump(t);
        drain();
        bus.step();
    }
    g2.ear.unmute();

    const std::uint32_t before_rewind = g1.session.confirmed_tick();
    bus.set_hub(1);
    bool frontier_moved_back = false;
    for (int i = 0; i < kTimeout + 200; ++i, ++t) {
        const std::uint32_t pre = g1.session.confirmed_tick();
        g1.pump(t);
        if (g1.session.confirmed_tick() < pre) frontier_moved_back = true;
        g2.pump(t);
        drain();
        bus.step();
    }

    // MEASURED, AND RECORDED RATHER THAN ASSERTED: this scenario does NOT drive
    // the frontier backwards, so rewind_for_migration()'s un-confirm branch is
    // NOT reached here — and the suite has no other case that reaches it either.
    // The guard below is therefore DEFENSIVE, not proven: reverting
    // `events_through_` leaves this suite green. It is kept because the hazard it
    // closes is real and silent (events are excluded from state_hash, so a
    // double-drained window would show up only as a wrong kill tally on one
    // machine), but do not read a passing run as evidence for it.
    //
    // Why the branch is hard to reach: confirming a tick needs EVERY awaited
    // seat, so no peer can confirm past the tick the hub's input stops at, and
    // the adopted tick is the LOWEST such tick across peers. Getting above it
    // requires one survivor to hold strictly more of the hub's tail AND to have
    // every other seat's input for that span — which the surviving peer, stalled
    // at its own cap, stops supplying at almost the same moment.
    CHECK_FALSE(frontier_moved_back);

    // The migration really did move g1's frontier backwards — otherwise this
    // case proves nothing about the interaction it is named for.
    REQUIRE(g1.session.handoff_tick(0) < before_rewind);
    REQUIRE(g1.session.host_lost_seats() == kSeat0);

    // Every event drained belongs to a distinct confirmed tick. A tick whose
    // events entered the stream twice would push the total above the number of
    // ticks that were ever confirmed.
    CHECK(total > 0);           // the pin would be vacuous on an event-free run
    CHECK_FALSE(re_emitted);    // no confirmed tick entered the stream twice
    CHECK_FALSE(g1.session.desynced());
}

TEST_CASE("host migration: the election chains when the successor dies too") {
    // "Lowest surviving seat" is a rule, not a single step. If the elected hub
    // dies as well, the next-lowest survivor takes over by the same rule, with
    // the same absence of any coordinator.
    test::StarBus bus(3, /*latency=*/1);
    Peer host(0, 0, kAll3, bus, /*host_seat=*/0);
    Peer g1(1, 1, kAll3, bus, /*host_seat=*/0);
    Peer g2(2, 2, kAll3, bus, /*host_seat=*/0);

    std::uint32_t t = 0;
    for (; t < 40; ++t) {
        host.pump(t);
        g1.pump(t);
        g2.pump(t);
        bus.step();
    }
    bus.kill(0);
    bus.set_hub(1);
    for (int i = 0; i < kTimeout + 60; ++i, ++t) {
        g1.pump(t);
        g2.pump(t);
        bus.step();
    }
    REQUIRE(g2.session.current_hub() == 1);  // the first migration landed

    // Now the SUCCESSOR dies. Seat 2 is the only one left.
    bus.kill(1);
    bus.set_hub(2);
    for (int i = 0; i < kTimeout + 60; ++i, ++t) {
        g2.pump(t);
        bus.step();
    }

    CHECK(g2.session.current_hub() == 2);
    CHECK(g2.session.hosting());  // it now drives the game, having started a guest
    CHECK(g2.session.handoff_tick(1) != net::RollbackSession::kNoHandoff);
    CHECK_FALSE(g2.session.aborted());
}

TEST_CASE("host migration: a 2-seat host death is left alone even when REALLY dead") {
    // The companion to the path-death case below, and the reason the gate is on
    // SEAT COUNT rather than on some liveness judgement: here the host really is
    // dead (killed, not merely unreachable), and the survivor STILL does not
    // elect. That is deliberate, and it is the whole point — from inside the
    // guest, this run and the severed-path run below are byte-for-byte
    // indistinguishable. Acting on one means acting on the other.
    //
    // So the 2-seat host death keeps its pre-migration behaviour: with Options
    // row 12 ON the guest stalls (the shell's double-Esc bail-out is the way
    // out); with it OFF, detect_drops() still ends the match loudly, as it
    // always did. Closing this properly needs §4.2's dead-peer/dead-path oracle
    // — the lobby's RosterUpdate — not a better guess on the data plane.
    test::StarBus bus(2, /*latency=*/1);
    Peer host(0, 0, kSeat0 | kSeat1, bus, /*host_seat=*/0);
    Peer guest(1, 1, kSeat0 | kSeat1, bus, /*host_seat=*/0);

    std::uint32_t t = 0;
    for (; t < 40; ++t) {
        host.pump(t);
        guest.pump(t);
        bus.step();
    }
    REQUIRE(guest.session.confirmed_tick() > 20);
    REQUIRE_FALSE(guest.session.hosting());

    bus.kill(0);
    bus.set_hub(1);
    for (int i = 0; i < kTimeout + 120; ++i, ++t) {
        guest.pump(t);
        bus.step();
    }

    CHECK(guest.session.host_lost_seats() == 0);
    CHECK_FALSE(guest.session.hosting());
    CHECK(guest.session.current_hub() == -1);  // migration never armed at 2 seats
    CHECK(guest.session.handoff_tick(0) == net::RollbackSession::kNoHandoff);
    CHECK_FALSE(guest.sim.state().players[0].ai);
}

TEST_CASE("host migration: a TWO-PEER path death must NOT trigger an election") {
    // THE OWNER'S ACTUAL CONFIGURATION, and the failure he captured: 2 seats,
    // path=direct, RX 0/s, ~100% LOSS, BAD 0 — nothing arriving at all, deep
    // into a long session, with the peer almost certainly still alive and still
    // playing. The data plane cannot tell that apart from a dead peer, and with
    // only two seats there is no third party who could.
    //
    // Both peers are ALIVE here and neither is killed: the PATH dies, in both
    // directions, while both keep simulating and keep sending. Nobody has
    // "dropped" in any sense the sim can observe — they simply cannot hear each
    // other any more.
    //
    // Migration must stay out of this. Electing here means the guest AIs the
    // host while the host AIs the guest, and both play on inside private,
    // divergent games that neither player can distinguish from a real one. That
    // is strictly worse than the freeze it would replace, because a freeze is at
    // least visible. Until §4.2's dead-peer/dead-path oracle exists, the 2-seat
    // case is left exactly as it was.
    test::StarBus bus(2, /*latency=*/1);
    Peer host(0, 0, kSeat0 | kSeat1, bus, /*host_seat=*/0);
    Peer guest(1, 1, kSeat0 | kSeat1, bus, /*host_seat=*/0);

    std::uint32_t t = 0;
    for (; t < 40; ++t) {
        host.pump(t);
        guest.pump(t);
        bus.step();
    }
    REQUIRE(guest.session.confirmed_tick() > 20);  // a healthy direct match first

    // The path dies both ways. Neither peer is dead; neither stops pumping.
    host.ear.set_deaf(true);
    guest.ear.set_deaf(true);
    for (int i = 0; i < kTimeout + 120; ++i, ++t) {
        host.pump(t);
        guest.pump(t);
        bus.step();
    }

    // NO ELECTION. The guest must not promote itself, and the hub must still be
    // the seat that started as hub.
    CHECK(guest.session.host_lost_seats() == 0);
    CHECK_FALSE(guest.session.hosting());
    CHECK(guest.session.handoff_tick(0) == net::RollbackSession::kNoHandoff);
    CHECK_FALSE(guest.sim.state().players[0].ai);
}

TEST_CASE("host migration: a HostLost naming a NON-hub seat is refused") {
    // MsgType::HostLost is accepted from ANY peer, because with the hub gone
    // there is no authority left to check it against. What keeps that safe is
    // that it may only name the seat we currently believe IS the hub — otherwise
    // it would be a lever letting any guest hand any other guest's seat to the
    // AI, which is exactly the decree MsgType::Drop still reserves to the hub.
    test::StarBus bus(3, /*latency=*/1);
    Peer host(0, 0, kAll3, bus, /*host_seat=*/0);
    Peer g1(1, 1, kAll3, bus, /*host_seat=*/0);
    Peer g2(2, 2, kAll3, bus, /*host_seat=*/0);

    std::uint32_t t = 0;
    for (; t < 30; ++t) {
        host.pump(t);
        g1.pump(t);
        g2.pump(t);
        bus.step();
    }

    // g1 forges a HostLost naming seat 2, which is a plain guest.
    const std::vector<std::uint8_t> forged = net::encode_host_lost(2, 5);
    g1.wire.send(forged.data(), forged.size());
    for (int i = 0; i < 20; ++i, ++t) {
        host.pump(t);
        g1.pump(t);
        g2.pump(t);
        bus.step();
    }

    // Nobody handed seat 2 to the AI, and the real hub is untouched.
    CHECK(host.session.handoff_tick(2) == net::RollbackSession::kNoHandoff);
    CHECK(g2.session.handoff_tick(2) == net::RollbackSession::kNoHandoff);
    CHECK(host.session.current_hub() == 0);
    CHECK_FALSE(host.sim.state().players[2].ai);
}

TEST_CASE("host migration: off by default leaves every pre-existing session unchanged") {
    // DropPolicy::host_seat defaults to -1, and that has to mean "behave exactly
    // as before wire v9": the role stays whatever is_host said, and a HostLost on
    // the wire is inert. Every existing caller and test depends on this.
    test::StarBus bus(2, /*latency=*/1);
    sim::Simulation s0(three_config()), s1(three_config());
    test::StarTransport t0(bus, 0), t1(bus, 1);
    const net::DropPolicy host{/*revert_to_ai=*/true, /*is_host=*/true, kTimeout};
    const net::DropPolicy guest{/*revert_to_ai=*/true, /*is_host=*/false, kTimeout};
    net::RollbackSession a(s0, kSeat0, kSeat0 | kSeat1, 8, t0, host);
    net::RollbackSession b(s1, kSeat1, kSeat0 | kSeat1, 8, t1, guest);

    CHECK(a.current_hub() == -1);  // migration off
    CHECK(a.hosting());            // the fixed role, straight from is_host
    CHECK_FALSE(b.hosting());

    for (std::uint32_t t = 0; t < 30; ++t) {
        a.advance(seat_input(0, scripted(0, t)));
        b.advance(seat_input(1, scripted(1, t)));
        bus.step();
    }
    // An inbound HostLost is ignored outright rather than scheduling anything.
    const std::vector<std::uint8_t> pkt = net::encode_host_lost(0, 5);
    t1.send(pkt.data(), pkt.size());
    for (std::uint32_t t = 30; t < 50; ++t) {
        a.advance(seat_input(0, scripted(0, t)));
        b.advance(seat_input(1, scripted(1, t)));
        bus.step();
    }
    CHECK(b.host_lost_seats() == 0);
    CHECK(b.handoff_tick(0) == net::RollbackSession::kNoHandoff);
    CHECK_FALSE(b.desynced());
}
