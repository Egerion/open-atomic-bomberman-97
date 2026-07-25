#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "bomber/sim/match_config.hpp"

// Wire (de)serialization of a fully RESOLVED sim::MatchConfig — the payload the
// host-authoritative setup layer (setup_session.hpp) broadcasts when it confirms
// a match.
//
// WHY THE WHOLE CONFIG AND NOT A LEVEL INDEX. This is the determinism boundary.
// A level index would only name a map; each peer would then BUILD a board from
// its own files, and two installs disagree constantly — a different .SCH, a
// different EXTRA<n>.RES, a custom map occupying that index, a hand-edited
// VALUELST. Any of those produces a different `cells` grid or a different
// Tuning, so the two Simulations diverge on tick 0 and the state_hash desync
// detector fires (at best) or the match silently drifts (at worst). Shipping the
// RESOLVED config makes that impossible: the grid, the stage actors, the warp
// destinations, the spawns, the per-slot roster, the seed and the whole Tuning
// travel as bytes, so every peer feeds Simulation byte-identical input and the
// only remaining input is the shared build (build_hash.hpp).
//
// Every gameplay-relevant field of sim::MatchConfig is covered. The encoder and
// the decoder are ONE templated visitor in match_config_codec.cpp (a Writer and
// a Reader implementing the same call interface), so a field added to
// MatchConfig cannot be serialized on one side and forgotten on the other —
// forgetting it means it is missing from both, which the full-field round-trip
// test in tests/net/test_match_config_codec.cpp catches immediately.
//
// Untrusted input, exactly like the 1997 asset files and the rest of libs/net:
// decode_match_config() bounds-checks every read, rejects out-of-range enums
// (Cell, ActorType) and an out-of-range spawn count, never reads past the
// buffer, never throws, and demands an EXACT length (trailing bytes are a
// malformation, not padding).
//
// LAYOUT (little-endian throughout, in this fixed order):
//   [layout u16]                                = kMatchConfigLayout
//   [cells        15*11 u8]  row-major, y-major; 0 Blank / 1 Brick / 2 Solid
//   [actor_type   15*11 u8]  0 DirArrow / 1 Warphole / 2 Conveyor /
//                            3 Trampoline / 255 None — any other byte rejected
//   [actor_dir    15*11 u8]
//   [warp_dest_x  15*11 u8]
//   [warp_dest_y  15*11 u8]
//   [spawn_count u8]  <= sim::kMaxPlayers
//   [spawn_count * (x i32, y i32)]
//   [player_count i32]
//   [ai       10 u8]  0/1
//   [active   10 u8]  0/1
//   [team     10 u8]
//   [seed u32]
//   [tuning: 157 i32 then 4 u8 flags, in declaration order]
//   [spawn_override 13 i32]
//   [forbidden      13 u8]
//   [born_with      13 u8]
//   [born_with_extra 10*13 u8]
//   [born_with_clogs 10 i32]
//   [campaign_rovers i32][campaign_rover_speed i32]
//   [campaign_ghosts i32][campaign_ghost_speed i32]
//
// SIZE: 1761 bytes fixed + 1 + 8 per spawn = 1842 bytes for a full 10-spawn
// config. That is well past a safe UDP payload, so setup_session.hpp CHUNKS it
// (see kSetupChunkPayloadBytes) rather than trusting IP fragmentation — a
// config that only fails to arrive on someone else's network is exactly the bug
// this avoids. The blob grows whenever MatchConfig does; the chunker absorbs
// that, which is why the size is documented here but not asserted anywhere.

namespace bomber::net {

// Bumped when the byte layout above changes. Decode rejects anything else, so a
// stale peer fails loudly instead of mis-parsing a shifted field. (Peers on
// different builds are normally already rejected by build_hash(); this is the
// last line of defence.)
inline constexpr std::uint16_t kMatchConfigLayout = 1;

// Upper bound the decoder enforces before allocating a reassembly buffer, so a
// hostile "total length" cannot make us reserve arbitrary memory. Roughly 4x the
// current blob, leaving room for MatchConfig to grow without a protocol change.
inline constexpr std::size_t kMaxMatchConfigBytes = 8192;

// Serialize `cfg` to the layout above. Never fails: every field has a wire form.
std::vector<std::uint8_t> encode_match_config(const sim::MatchConfig& cfg);

// Parse a blob produced by encode_match_config(). On success fills *out (which
// is fully overwritten, defaults included) and returns true. On ANY malformation
// — wrong layout tag, short buffer, trailing bytes, an out-of-range Cell /
// ActorType / spawn count — leaves *out untouched and returns false.
bool decode_match_config(const std::uint8_t* data, std::size_t size, sim::MatchConfig* out);

}  // namespace bomber::net
