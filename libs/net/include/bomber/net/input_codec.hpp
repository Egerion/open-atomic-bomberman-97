#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "bomber/sim/types.hpp"

// Wire (de)serialization for per-tick player inputs — the ONLY payload a
// deterministic-lockstep peer must exchange each tick (docs/re/multiplayer.md
// §3, ADR-0010). SDL-free; depends only on bomber::sim's trivially-copyable
// input value types. No sockets live here: this is the pure codec that the
// transport layer (a later increment) frames onto UDP, and that the loopback
// lockstep harness (tests/net) exercises end to end without a network.
//
// Network packets are untrusted input, exactly like the 1997 asset files
// (CLAUDE.md): deserialize() bounds-checks everything and rejects malformed
// frames rather than trusting a length or a seat mask.

namespace bomber::net {

// Six-bit packing of one seat's PlayerInput. The bit order is FIXED and part of
// the wire contract (like the sim's RNG draw order is part of the determinism
// contract): up/down/left/right/action1/action2 = bits 0..5. Bits 6-7 are
// always zero, so a whole seat rides in one byte.
std::uint8_t pack_input(const sim::PlayerInput& in);
sim::PlayerInput unpack_input(std::uint8_t bits);

// One peer's contribution to tick `tick_index`: the packed inputs for the seats
// named in `seat_mask` (bit s set => seat s carried). Only the seats a peer
// OWNS travel on the wire; the receiver merges them into its locally-assembled
// TickInputs (merge_seats) before Simulation::tick. Seats not in the mask are
// meaningless in `inputs`.
struct InputFrame {
    std::uint32_t tick_index = 0;
    std::uint16_t seat_mask = 0;
    sim::TickInputs inputs;
};

// Serialize the seats named in `seat_mask` of `inputs`, stamped with
// `tick_index`, to a compact frame:
//   [tick_index u32-LE][seat_mask u16-LE][one packed byte per set seat, ascending]
// A 1-seat frame is 7 bytes; a full 10-seat frame is 16. Little-endian is fixed
// by the contract so peers on different-endian hosts agree.
std::vector<std::uint8_t> serialize(std::uint32_t tick_index, std::uint16_t seat_mask,
                                    const sim::TickInputs& inputs);

// Parse a frame produced by serialize(). On success fills *out and returns true;
// on ANY malformation (short buffer, a seat bit past kMaxPlayers, or a payload
// length that disagrees with the mask's set-bit count) leaves *out untouched and
// returns false. Exact-length framing: trailing bytes are a malformation, not
// ignored.
bool deserialize(const std::uint8_t* data, std::size_t size, InputFrame* out);

// Receiver-side merge: overwrite exactly the seats named in `frame.seat_mask` of
// `*dst` with the frame's inputs, leaving every other seat untouched (so a peer
// keeps its own local seats and adopts only the remote ones).
void merge_seats(const InputFrame& frame, sim::TickInputs* dst);

}  // namespace bomber::net
