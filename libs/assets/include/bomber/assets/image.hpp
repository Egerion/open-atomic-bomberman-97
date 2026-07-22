#pragma once

#include <cstdint>
#include <vector>

namespace bomber::assets {

// Simple RGBA8 image (row-major, top-left origin).
struct Image {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;  // width * height * 4

    // Faithful player recolouring (the original's .RMP remap, docs/re/
    // player-colour.md) works at the PALETTE-INDEX level: the blit remaps each
    // pixel's index through the colour's table (sub_415A1C) before the palette
    // lookup. For 8bpp paletted CIMG (type 11) we keep the source indices + the
    // frame's own palette so recolor_image_rmp can remap them directly. These
    // stay EMPTY for 16bpp CIMG (type 4) — but type-4 IS still player-coloured
    // (in fact ALL player art in this install is type-4): the native decodes a
    // type-4 pixel straight to a MASTER-palette index via the COLOR.PAL LUT
    // (colorpal.hpp index_of), so its recolour needs no per-frame palette —
    // sprites.cpp recolor_image_master snaps then remaps through the same .RMP.
    std::vector<std::uint8_t> indices;  // width * height palette indices, or empty
    std::vector<std::uint8_t> palette;  // 256 * 4 RGBA source palette, or empty

    bool empty() const { return width == 0 || height == 0; }
    // True when indices + palette are present so a .RMP remap can be applied.
    bool paletted() const { return !indices.empty() && !palette.empty(); }
};

}  // namespace bomber::assets
