#pragma once

#include <array>
#include <cstdint>
#include <filesystem>

namespace bomber::assets::res {

// A player-colour remap table loaded from a `.RMP` file (`0.RMP`..`9.RMP` in the
// install ROOT, one per player colour). The original engine stores every player
// sprite as an 8-bit paletted image and, at blit time, rewrites each pixel's
// palette INDEX through this table before the palette lookup (sub_415A1C, the
// per-colour table is dword_460564[colour]); that is how the "green" master art
// is retargeted to each player's colour. See docs/re/player-colour.md.
//
// File layout (259 bytes, CONFIRMED from the install):
//   [0..255]  256-byte index->index remap table (the colour band is non-zero,
//             every other entry is 0x00).
//   [256..258] three tail bytes = R, G, B a
//   s 0..100 PERCENT (e.g. 2.RMP = red
//             `64 00 0a` = 100,0,10). Mirrors VALUELST 200-247 / color_rgb; used
//             only as the slot's label tint on the setup screen, not for sprites.
//
// Apply convention (sub_414A65 backfill, decompile 17498-99): a 0 entry means
// "not remapped", so the loader BACKFILLS it to identity — `if (t[i]==0) t[i]=i`.
// After load the table is total: `dst = map[src]` is identity outside the colour
// band and the colour ramp inside it.
struct RemapTable {
    std::array<std::uint8_t, 256> map{};  // index -> index, identity outside the band
    std::array<std::uint8_t, 3> rgb{};    // tail R,G,B percent (0..100)
};

// Parses a `.RMP` file. Throws std::runtime_error on a short/unreadable file
// (1997 files are untrusted input — the read is bounds-checked). The 256-byte
// table is backfilled to identity for every 0 entry per sub_414A65.
RemapTable load_rmp(const std::filesystem::path& path);

}  // namespace bomber::assets::res
