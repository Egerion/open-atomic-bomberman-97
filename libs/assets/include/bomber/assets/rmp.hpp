#pragma once

#include <array>
#include <cstdint>
#include <filesystem>

namespace bomber::assets::res {

// A player-colour remap table (`0.RMP`..`9.RMP`, install root, one per colour).
// Player sprites are 8-bit paletted, and the blit rewrites each pixel's palette
// INDEX through this table before the palette lookup (sub_415A1C) — that is how
// one set of "green" master art becomes every player's colour. Layout and the
// 259-byte breakdown: docs/formats/rmp.md, docs/re/player-colour.md.
//
// A 0 entry means "not remapped", so the loader BACKFILLS it to identity
// (sub_414A65). After load the table is total: identity outside the colour band,
// the colour ramp inside it.
struct RemapTable {
    std::array<std::uint8_t, 256> map{};  // index -> index, identity outside the band
    // Tail R,G,B as 0..100 PERCENT (2.RMP red = 100,0,10). The setup screen's
    // slot label tint only — sprites go through `map`.
    std::array<std::uint8_t, 3> rgb{};
};

// Throws std::runtime_error on a short or unreadable file.
RemapTable load_rmp(const std::filesystem::path& path);

}  // namespace bomber::assets::res
