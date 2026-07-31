// bomber::net tests: the input codec (pack/serialize/deserialize) and the
// LOOPBACK LOCKSTEP HARNESS — two independent Simulations seeded from one
// MatchConfig, each fed the OTHER peer's input only through the wire codec,
// asserted hash-identical every tick. This is docs/re/multiplayer.md §3.3
// step 2 promoted from "same seed+inputs -> same hash" (the golden determinism
// property) to "same seed + wire-exchanged inputs -> same hash": the
// single-process proof that the cross-machine lockstep invariant holds over the
// codec. No sockets yet (ADR-0010); an in-memory hand-off stands in for UDP.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "bomber/net/input_codec.hpp"
#include "helpers.hpp"        // bomber::sim::test::open_config (+ pulls in simulation.hpp)
#include "input_scripts.hpp"  // the SPARSER walk; see that header on the three scripts

using namespace bomber;                  // NOLINT(google-build-using-namespace) — test-local
using bomber::sim::test::open_config;
using bomber::test::scripted_cycle8;

namespace {

std::uint16_t seat_bit(int seat) { return static_cast<std::uint16_t>(1U << seat); }

}  // namespace

TEST_CASE("pack/unpack roundtrips every 6-bit combination; the top two bits carry nothing") {
    for (unsigned v = 0; v < 64; ++v) {
        const sim::PlayerInput in = net::unpack_input(static_cast<std::uint8_t>(v));
        CHECK(net::pack_input(in) == v);
    }
    CHECK(net::pack_input(net::unpack_input(0xFF)) == 0x3F);
}

TEST_CASE("serialize/deserialize roundtrips a one-seat frame exactly") {
    sim::TickInputs in;
    in.players[0].right = true;
    in.players[0].action1 = true;
    const std::vector<std::uint8_t> bytes = net::serialize(1234, seat_bit(0), in);
    CHECK(bytes.size() == 7);  // tick(4) + mask(2) + one packed seat(1)

    net::InputFrame f;
    REQUIRE(net::deserialize(bytes.data(), bytes.size(), &f));
    CHECK(f.tick_index == 1234);
    CHECK(f.seat_mask == seat_bit(0));
    CHECK(f.inputs.players[0].right);
    CHECK(f.inputs.players[0].action1);
    CHECK_FALSE(f.inputs.players[0].up);
}

TEST_CASE("deserialize rejects every malformation (untrusted packets)") {
    net::InputFrame f;

    const std::uint8_t tiny[3] = {0, 0, 0};  // shorter than the 6-byte header
    CHECK_FALSE(net::deserialize(tiny, sizeof(tiny), &f));

    // A seat bit at/beyond kMaxPlayers (here seat 10 => mask 0x0400).
    std::vector<std::uint8_t> past = net::serialize(0, seat_bit(0), sim::TickInputs{});
    past[4] = 0x00;
    past[5] = 0x04;
    CHECK_FALSE(net::deserialize(past.data(), past.size(), &f));

    // Payload too short for the mask (mask names seat 1 but the byte is gone).
    std::vector<std::uint8_t> short_pay = net::serialize(0, seat_bit(1), sim::TickInputs{});
    short_pay.pop_back();
    CHECK_FALSE(net::deserialize(short_pay.data(), short_pay.size(), &f));

    // Trailing junk (exact framing required).
    std::vector<std::uint8_t> extra = net::serialize(0, seat_bit(1), sim::TickInputs{});
    extra.push_back(0);
    CHECK_FALSE(net::deserialize(extra.data(), extra.size(), &f));
}

TEST_CASE("loopback lockstep: two peers exchanging packed inputs stay hash-identical") {
    // Host advertises one config+seed; the guest adopts it wholesale
    // (docs/re/multiplayer.md §3.1.3), so both sims START byte-identical.
    sim::Simulation peer_a{open_config()};
    sim::Simulation peer_b{open_config()};
    REQUIRE(peer_a.hash() == peer_b.hash());

    // Peer A owns seat 0, peer B owns seat 1; neither knows the other's input
    // except through the wire codec.
    constexpr int kTicks = 600;  // 30 s at 20 Hz — long enough to run bombs/flames/deaths
    for (int t = 0; t < kTicks; ++t) {
        const auto tick = static_cast<std::uint32_t>(t);
        const sim::PlayerInput a_local = scripted_cycle8(0, tick);
        const sim::PlayerInput b_local = scripted_cycle8(1, tick);

        // Each peer serializes ONLY its own seat and "sends" it.
        sim::TickInputs a_only;
        a_only.players[0] = a_local;
        const std::vector<std::uint8_t> a_wire =
            net::serialize(static_cast<std::uint32_t>(t), seat_bit(0), a_only);

        sim::TickInputs b_only;
        b_only.players[1] = b_local;
        const std::vector<std::uint8_t> b_wire =
            net::serialize(static_cast<std::uint32_t>(t), seat_bit(1), b_only);

        // Each peer assembles the full frame: its own local seat + the remote
        // seat decoded off the wire (merge_seats overwrites only masked seats).
        net::InputFrame from_b;
        REQUIRE(net::deserialize(b_wire.data(), b_wire.size(), &from_b));
        REQUIRE(from_b.tick_index == static_cast<std::uint32_t>(t));
        sim::TickInputs a_full;
        a_full.players[0] = a_local;
        net::merge_seats(from_b, &a_full);

        net::InputFrame from_a;
        REQUIRE(net::deserialize(a_wire.data(), a_wire.size(), &from_a));
        sim::TickInputs b_full;
        b_full.players[1] = b_local;
        net::merge_seats(from_a, &b_full);

        peer_a.tick(a_full);
        peer_b.tick(b_full);

        // A single divergent bit — a mis-packed input, a dropped seat, an endian
        // slip — trips this the tick it happens.
        REQUIRE(peer_a.hash() == peer_b.hash());
    }
}
