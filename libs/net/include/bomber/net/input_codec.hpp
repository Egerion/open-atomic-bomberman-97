#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "bomber/sim/types.hpp"

// Wire (de)serialization for per-tick player inputs — the ONLY payload a
// deterministic-lockstep peer must exchange each tick (docs/re/multiplayer.md
// §3, ADR-0010). A pure codec: no sockets, no SDL. Untrusted input like the 1997
// asset files, so deserialize() bounds-checks rather than trusting a length.

namespace bomber::net {

// Six-bit packing of one seat's PlayerInput. THE BIT ORDER IS PART OF THE WIRE
// CONTRACT, as the RNG draw order is part of the determinism contract:
// up/down/left/right/action1/action2 = bits 0..5, so a seat rides in one byte.
std::uint8_t pack_input(const sim::PlayerInput& in);
sim::PlayerInput unpack_input(std::uint8_t bits);

// One peer's contribution to `tick_index`. Only the seats a peer OWNS travel;
// seats outside `seat_mask` are meaningless in `inputs`.
struct InputFrame {
    std::uint32_t tick_index = 0;
    std::uint16_t seat_mask = 0;
    sim::TickInputs inputs;
};

// [tick_index u32-LE][seat_mask u16-LE][one packed byte per set seat, ascending]
// — 7 bytes for one seat, 16 for ten. Little-endian is fixed by the contract.
std::vector<std::uint8_t> serialize(std::uint32_t tick_index, std::uint16_t seat_mask,
                                    const sim::TickInputs& inputs);

// False, leaving *out untouched, on a short buffer, a seat bit past kMaxPlayers,
// or a payload length disagreeing with the mask. EXACT-length framing: trailing
// bytes are a malformation, not padding.
bool deserialize(const std::uint8_t* data, std::size_t size, InputFrame* out);

// Overwrite exactly `frame.seat_mask`'s seats in `*dst`, so a peer keeps its own
// local seats and adopts only the remote ones.
void merge_seats(const InputFrame& frame, sim::TickInputs* dst);

}  // namespace bomber::net
