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
    // lookup. Truecolour rgba alone loses the indices, so for 8bpp paletted CIMG
    // (type 11) we also keep the source indices + the frame's palette. Empty for
    // 16bpp CIMG (type 4), which carries no palette and is never player-coloured.
    std::vector<std::uint8_t> indices;  // width * height palette indices, or empty
    std::vector<std::uint8_t> palette;  // 256 * 4 RGBA source palette, or empty

    bool empty() const { return width == 0 || height == 0; }
    // True when indices + palette are present so a .RMP remap can be applied.
    bool paletted() const { return !indices.empty() && !palette.empty(); }
};

}  // namespace bomber::assets
