// bomber::net peer-drop -> AI handoff (ADR-0011 Risks, "Dropped/late peers").
// A peer that goes silent forever would otherwise stall every other peer past
// the prediction cap. Past a hard timeout the HOST announces MsgType::Drop and
// every peer, at the SAME tick, hands that seat to the deterministic AISystem —
// so all peers keep computing identical inputs for it and the confirmed-hash
// exchange keeps agreeing. Gated on the RE'd Options row 12 "Lost net players
// revert to AI"; with the option OFF the drop must instead END the match
// loudly, never hang.
//
// No sockets: the many-endpoint FanoutBus in tests/common/fanout_bus.hpp models
// the star's effect (each peer's datagram reaches every other peer) with
// per-endpoint latency, so a third peer can simply stop pumping and be genuinely
// dead. It lives in tests/common now because the N-peer setup suite needs it too.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "bomber/net/protocol.hpp"
#include "bomber/net/rollback_session.hpp"
#include "bomber/net/transport.hpp"
#include "fanout_bus.hpp"     // bomber::test::FanoutBus / BusTransport
#include "helpers.hpp"        // bomber::sim::test::open_config
#include "input_scripts.hpp"  // the antiphase walk; see that header on the three scripts

using namespace bomber;  // NOLINT(google-build-using-namespace) — test-local
using bomber::sim::test::open_config;
using bomber::test::BusTransport;
using bomber::test::FanoutBus;
using bomber::test::scripted_cycle6;
using bomber::test::seat_input;

namespace {

constexpr std::uint16_t kSeat0 = 0x1;
constexpr std::uint16_t kSeat1 = 0x2;
constexpr std::uint16_t kSeat2 = 0x4;
constexpr std::uint16_t kAll3 = 0x7;

// Three players in the same open arena helpers.hpp builds for two.
sim::MatchConfig three_config() {
    sim::MatchConfig cfg = open_config();
    cfg.spawns = {{0, 0}, {14, 10}, {14, 0}};
    cfg.player_count = 3;
    return cfg;
}

}  // namespace

TEST_CASE("peer drop: the host hands a silent seat to the AI and nobody stalls") {
    constexpr int kTimeout = 40;  // 2 s at 20 Hz
    FanoutBus bus(3, /*latency=*/2);
    BusTransport t0(bus, 0);
    BusTransport t1(bus, 1);
    BusTransport t2(bus, 2);
    sim::Simulation s0(three_config());
    sim::Simulation s1(three_config());
    sim::Simulation s2(three_config());

    net::DropPolicy host{/*revert_to_ai=*/true, /*is_host=*/true, kTimeout};
    net::DropPolicy guest{/*revert_to_ai=*/true, /*is_host=*/false, kTimeout};
    net::RollbackSession a(s0, kSeat0, kAll3, /*max_prediction=*/8, t0, host);
    net::RollbackSession b(s1, kSeat1, kAll3, /*max_prediction=*/8, t1, guest);
    net::RollbackSession c(s2, kSeat2, kAll3, /*max_prediction=*/8, t2, guest);

    // Phase 1: all three alive.
    for (int i = 0; i < 60; ++i) {
        a.advance(seat_input(0, scripted_cycle6(0, a.predicted_tick())));
        b.advance(seat_input(1, scripted_cycle6(1, b.predicted_tick())));
        c.advance(seat_input(2, scripted_cycle6(2, c.predicted_tick())));
        bus.step();
    }
    const std::uint32_t alive_frontier = a.confirmed_tick();
    CHECK(alive_frontier > 40);  // three live peers confirm normally
    CHECK(c.handoff_tick(2) == net::RollbackSession::kNoHandoff);

    // Phase 2: peer C dies outright — it stops pumping, so it neither sends nor
    // receives. A and B keep going.
    const std::uint32_t stall_tick = a.predicted_tick();
    for (int i = 0; i < 20; ++i) {
        a.advance(seat_input(0, scripted_cycle6(0, a.predicted_tick())));
        b.advance(seat_input(1, scripted_cycle6(1, b.predicted_tick())));
        bus.step();
    }
    // The old behaviour, still intact BEFORE the timeout: the display runs at
    // most max_prediction ticks ahead and then holds.
    CHECK(a.predicted_tick() - a.confirmed_tick() == 8);
    CHECK(a.predicted_tick() < stall_tick + 20);  // it really did stall, not sail on

    // Phase 3: past the hard timeout the host schedules + broadcasts the handoff.
    for (int i = 0; i < 120; ++i) {
        a.advance(seat_input(0, scripted_cycle6(0, a.predicted_tick())));
        b.advance(seat_input(1, scripted_cycle6(1, b.predicted_tick())));
        bus.step();
    }

    CHECK(a.dropped_seats() == kSeat2);
    CHECK(b.dropped_seats() == kSeat2);      // the guest obeyed, it did not decide
    CHECK_FALSE(a.aborted());
    CHECK_FALSE(b.aborted());
    // Both peers agreed on the SAME tick — the whole point of the message.
    CHECK(a.handoff_tick(2) == b.handoff_tick(2));
    CHECK(a.handoff_tick(2) != net::RollbackSession::kNoHandoff);
    // Retroactive by design: at or below the frontier the drop froze.
    CHECK(a.handoff_tick(2) <= a.confirmed_tick());
    // The seat is now the AI's on both peers, and both hashed States agree.
    CHECK(s0.state().players[2].ai);
    CHECK(s1.state().players[2].ai);
    CHECK_FALSE(a.desynced());
    CHECK_FALSE(b.desynced());
    // And the match is MOVING again: the confirmed frontier passed the point the
    // dead peer had frozen it at, and keeps pace with the display.
    CHECK(a.confirmed_tick() > stall_tick + 60);
    CHECK(b.confirmed_tick() > stall_tick + 60);
    CHECK(a.predicted_tick() - a.confirmed_tick() < 8);
}

TEST_CASE("peer drop: a rollback ACROSS the handoff tick keeps the peers identical") {
    // The snapshot trap. `Player::ai` is hashed State, and a rollback restores a
    // snapshot taken BEFORE the handoff — so a session that merely SET the flag
    // when the message arrived would silently lose it on any re-simulation that
    // starts below the handoff tick, and only on the peer that rolled back.
    //
    // Constructed so that happens for real: seat 2's peer keeps sending input
    // (the test injects it), but a Drop for a FUTURE tick T is announced. Seat 2
    // therefore stops being awaited at T while ticks below T are still being
    // confirmed and corrected, so mispredictions below T re-simulate straight
    // through T.
    //
    // The two things that make this an actual trap detector:
    //  * the latency must be worth several ticks — at one tick of delay the
    //    confirmed frontier trails the display by exactly one tick and no
    //    window ever spans T (the `straddled` assert guards that), and
    //  * it must be ASYMMETRIC. Under equal latency both peers roll back over
    //    the same window and would lose the flag in exactly the same ticks, so
    //    they would stay (wrongly) identical and the bug would hide. Here A
    //    hears everything 6 pumps late and re-simulates constantly, while B
    //    hears everything the same pump and never mispredicts at all — so a
    //    flag lost in A's re-simulation shows up as a hash mismatch.
    FanoutBus bus(std::vector<int>{6, 0, 0});
    BusTransport t0(bus, 0);
    BusTransport t1(bus, 1);
    sim::Simulation s0(three_config());
    sim::Simulation s1(three_config());

    // Detection off: this test drives the handoff purely from the wire, so the
    // tick under test is the one WE choose.
    net::RollbackSession a(s0, kSeat0, kAll3, /*max_prediction=*/12, t0);
    net::RollbackSession b(s1, kSeat1, kAll3, /*max_prediction=*/12, t1);

    constexpr std::uint32_t kHandoff = 60;
    bool straddled = false;  // did a peer really speculate past T with T unconfirmed?

    for (int i = 0; i < 400; ++i) {
        // Seat 2's peer: one packet per pump carrying a window of its inputs, so
        // both A and B confirm it exactly as they would a real third peer. It
        // keeps talking straight through the handoff — the session must ignore
        // it from T on rather than let it re-open the seat.
        const std::uint32_t base = (a.predicted_tick() > 4) ? a.predicted_tick() - 4 : 0;
        std::vector<sim::TickInputs> window;
        for (std::uint32_t t = base; t < base + 8; ++t)
            window.push_back(seat_input(2, scripted_cycle6(2, t)));
        const std::vector<std::uint8_t> pkt = net::encode_input_range(base, kSeat2, window);
        bus.inject(0, pkt);
        bus.inject(1, pkt);

        // The host's Drop announcement, re-sent redundantly like the real one,
        // and reaching the two peers at different times.
        if (i >= 20 && i < 40) {
            const std::vector<std::uint8_t> drop = net::encode_drop(2, kHandoff);
            bus.inject(0, drop, /*extra_latency=*/0);
            bus.inject(1, drop, /*extra_latency=*/7);
        }

        a.advance(seat_input(0, scripted_cycle6(0, a.predicted_tick())));
        b.advance(seat_input(1, scripted_cycle6(1, b.predicted_tick())));
        if (a.confirmed_tick() < kHandoff && a.predicted_tick() > kHandoff) straddled = true;
        if (b.confirmed_tick() < kHandoff && b.predicted_tick() > kHandoff) straddled = true;
        bus.step();
    }

    CHECK(straddled);  // the re-simulation window really did span the handoff tick
    CHECK(a.handoff_tick(2) == kHandoff);
    CHECK(b.handoff_tick(2) == kHandoff);
    CHECK(s0.state().players[2].ai);
    CHECK(s1.state().players[2].ai);
    // The confirmed-hash exchange covers every tick either peer finalised,
    // including the whole re-simulated window around the handoff.
    CHECK_FALSE(a.desynced());
    CHECK_FALSE(b.desynced());
    CHECK(a.confirmed_tick() > kHandoff + 100);
    CHECK(b.confirmed_tick() > kHandoff + 100);
}

TEST_CASE("peer drop: the handoff message is idempotent, duplicated and out of order") {
    FanoutBus bus(3, /*latency=*/1);
    BusTransport t0(bus, 0);
    BusTransport t1(bus, 1);
    sim::Simulation s0(three_config());
    sim::Simulation s1(three_config());
    net::RollbackSession a(s0, kSeat0, kAll3, /*max_prediction=*/8, t0);
    net::RollbackSession b(s1, kSeat1, kAll3, /*max_prediction=*/8, t1);

    constexpr std::uint32_t kHandoff = 30;

    auto pump = [&](int rounds, bool announce) {
        for (int i = 0; i < rounds; ++i) {
            const std::uint32_t base = (a.predicted_tick() > 4) ? a.predicted_tick() - 4 : 0;
            std::vector<sim::TickInputs> window;
            for (std::uint32_t t = base; t < base + 8; ++t)
                window.push_back(seat_input(2, scripted_cycle6(2, t)));
            const std::vector<std::uint8_t> pkt = net::encode_input_range(base, kSeat2, window);
            bus.inject(0, pkt);
            bus.inject(1, pkt);
            if (announce) {
                // The same handoff four times over, and a LATER duplicate that
                // must lose to the one already held (earliest wins, so arrival
                // order cannot change the outcome).
                for (int k = 0; k < 4; ++k) {
                    const std::vector<std::uint8_t> d = net::encode_drop(2, kHandoff);
                    bus.inject(0, d, k);
                    bus.inject(1, d, 3 - k);
                }
                const std::vector<std::uint8_t> late = net::encode_drop(2, kHandoff + 25);
                bus.inject(0, late, 0);
                bus.inject(1, late, 2);
            }
            a.advance(seat_input(0, scripted_cycle6(0, a.predicted_tick())));
            b.advance(seat_input(1, scripted_cycle6(1, b.predicted_tick())));
            bus.step();
        }
    };

    pump(10, /*announce=*/false);
    pump(30, /*announce=*/true);   // the storm of duplicates
    pump(160, /*announce=*/false);

    CHECK(a.handoff_tick(2) == kHandoff);  // never moved by a duplicate
    CHECK(b.handoff_tick(2) == kHandoff);
    CHECK(a.dropped_seats() == kSeat2);
    CHECK(b.dropped_seats() == kSeat2);
    CHECK_FALSE(a.desynced());
    CHECK_FALSE(b.desynced());
    CHECK(a.confirmed_tick() > kHandoff + 100);
}

TEST_CASE("peer drop: a malformed Drop frame is rejected, never acted on") {
    net::Message m;
    const std::vector<std::uint8_t> good = net::encode_drop(2, 0x01020304U);
    CHECK(good.size() == 6);
    REQUIRE(net::decode(good.data(), good.size(), &m));
    CHECK(m.type == net::MsgType::Drop);
    CHECK(m.drop.seat == 2);
    CHECK(m.drop.at_tick == 0x01020304U);

    // Short: every truncation of a valid frame.
    for (std::size_t n = 1; n < good.size(); ++n)
        CHECK_FALSE(net::decode(good.data(), n, &m));
    // Long: a trailing byte is not "extra data to ignore".
    std::vector<std::uint8_t> longer = good;
    longer.push_back(0);
    CHECK_FALSE(net::decode(longer.data(), longer.size(), &m));
    // Seat index out of range (kMaxPlayers == 10) — the field indexes a State
    // array, so it is bounds-checked at the wire, not at the use site.
    for (int seat = sim::kMaxPlayers; seat < 256; ++seat) {
        std::vector<std::uint8_t> bad = good;
        bad[1] = static_cast<std::uint8_t>(seat);
        CHECK_FALSE(net::decode(bad.data(), bad.size(), &m));
    }

    // And a session fed the malformed frames changes nothing.
    FanoutBus bus(2, /*latency=*/0);
    BusTransport t0(bus, 0);
    sim::Simulation s0(three_config());
    net::RollbackSession a(s0, kSeat0, kSeat0, /*max_prediction=*/8, t0);
    for (int i = 0; i < 20; ++i) {
        std::vector<std::uint8_t> bad = good;
        bad[1] = 200;  // impossible seat
        bus.inject(0, bad);
        bus.inject(0, {static_cast<std::uint8_t>(net::MsgType::Drop)});  // tag only
        a.advance(seat_input(0, scripted_cycle6(0, a.predicted_tick())));
        bus.step();
    }
    CHECK(a.dropped_seats() == 0);
    CHECK_FALSE(a.aborted());
    CHECK_FALSE(a.desynced());
    CHECK(a.predicted_tick() == 20);

    // A Drop naming a seat this match does not exchange over the network (an AI
    // or empty slot) is equally inert: there is nothing to hand over.
    for (int i = 0; i < 5; ++i) {
        bus.inject(0, net::encode_drop(1, 0));
        a.advance(seat_input(0, scripted_cycle6(0, a.predicted_tick())));
        bus.step();
    }
    CHECK(a.dropped_seats() == 0);
    CHECK(a.handoff_tick(1) == net::RollbackSession::kNoHandoff);
}

TEST_CASE("peer drop: with 'lost net players revert to AI' OFF the match ends, it does not hang") {
    constexpr int kTimeout = 40;
    FanoutBus bus(3, /*latency=*/2);
    BusTransport t0(bus, 0);
    BusTransport t1(bus, 1);
    BusTransport t2(bus, 2);
    sim::Simulation s0(three_config());
    sim::Simulation s1(three_config());
    sim::Simulation s2(three_config());

    // Options row 12 OFF on both peers. Detection is local, so neither needs the
    // other's permission to end the match.
    net::DropPolicy host{/*revert_to_ai=*/false, /*is_host=*/true, kTimeout};
    net::DropPolicy guest{/*revert_to_ai=*/false, /*is_host=*/false, kTimeout};
    net::RollbackSession a(s0, kSeat0, kAll3, /*max_prediction=*/8, t0, host);
    net::RollbackSession b(s1, kSeat1, kAll3, /*max_prediction=*/8, t1, guest);
    net::RollbackSession c(s2, kSeat2, kAll3, /*max_prediction=*/8, t2, guest);

    for (int i = 0; i < 60; ++i) {
        a.advance(seat_input(0, scripted_cycle6(0, a.predicted_tick())));
        b.advance(seat_input(1, scripted_cycle6(1, b.predicted_tick())));
        c.advance(seat_input(2, scripted_cycle6(2, c.predicted_tick())));
        bus.step();
    }
    CHECK_FALSE(a.aborted());

    for (int i = 0; i < 200; ++i) {  // C dies
        a.advance(seat_input(0, scripted_cycle6(0, a.predicted_tick())));
        b.advance(seat_input(1, scripted_cycle6(1, b.predicted_tick())));
        bus.step();
    }

    CHECK(a.aborted());  // LOUD and readable, on the host...
    CHECK(b.aborted());  // ...and on the guest
    CHECK(a.dropped_seats() == kSeat2);
    CHECK(b.dropped_seats() == kSeat2);
    // No handoff was scheduled and no State was touched: the option was off.
    CHECK(a.handoff_tick(2) == net::RollbackSession::kNoHandoff);
    CHECK_FALSE(s0.state().players[2].ai);
    CHECK_FALSE(s1.state().players[2].ai);

    // advance() is inert from here — the caller polling aborted() gets a stable
    // answer instead of a session that quietly keeps pumping.
    const std::uint32_t frozen = a.predicted_tick();
    for (int i = 0; i < 50; ++i) {
        a.advance(seat_input(0, scripted_cycle6(0, a.predicted_tick())));
        bus.step();
    }
    CHECK(a.predicted_tick() == frozen);
}
