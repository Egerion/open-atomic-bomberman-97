#pragma once

#include <cstddef>
#include <cstdint>

#include "bomber/assets/image.hpp"

// Makes a decoded image SAFE TO SAMPLE LINEARLY (the SOFT SCALING toggle,
// scale_filter.hpp; and the DATA_HD overrides, which are always linear).
// SDL-free so the pixel rules below are doctest-pinnable on their own
// (tests/game/test_alpha_bleed.cpp) — the same reason editor_grid.hpp and
// list_dialog_geometry.hpp keep their models out of the SDL layer.
//
// WHY IT IS NEEDED. Linear filtering samples a texel's RGB even where its ALPHA
// is zero, and SDL's default blend mode is non-premultiplied, so the blend at a
// cel's edge mixes in whatever colour sits behind the transparency. The 1997 art
// keeps the KEY COLOUR there: the ANI decode (libs/assets/src/ani.cpp) zeroes
// only the alpha — `rgba[i*4+3] = (v == key_color) ? 0 : 255` — and leaves the
// key colour's RGB in the buffer, which is free for the nearest-neighbour path
// it was written for (a zero-alpha texel contributes nothing to the blend
// whatever its RGB) and a magenta halo around every sprite the moment anything
// samples it linearly.
//
// THE FIX is the standard alpha dilate: give each fully-transparent texel the
// mean RGB of its opaque 8-neighbours, so an edge blend mixes the cel's own
// colour with itself and only the alpha ramps.

namespace bomber::game {

// Bleeds opaque colour outward into the fully-transparent texels, IN PLACE.
//
// In-place is safe by construction: the pass only WRITES texels whose alpha is
// 0 and only READS texels whose alpha is non-0, so a texel it has already
// written can never become a source for a later one and the result does not
// cascade across the image (it is exactly one dilation step, whatever the
// iteration order).
//
// Alpha is never touched, so the nearest-neighbour render is bit-identical
// before and after — which is what lets this run unconditionally on every
// upload, including the frames tests/visual pins.
//
// A texel with no opaque neighbour at all (deep inside a transparent region) is
// left alone: nothing samples it in any filter mode, and inventing a colour for
// it would only spread the key colour further.
inline void bleed_transparent_rgb(assets::Image& img) {
    const int w = img.width, h = img.height;
    if (w <= 0 || h <= 0) return;
    const std::size_t need = static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4;
    if (img.rgba.size() < need) return;  // 1997 files are untrusted input
    std::uint8_t* px = img.rgba.data();
    const auto at = [px, w](int x, int y) {
        return px + (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                     static_cast<std::size_t>(x)) *
                        4;
    };
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            std::uint8_t* p = at(x, y);
            if (p[3] != 0) continue;
            int r = 0, g = 0, b = 0, n = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                const int ny = y + dy;
                if (ny < 0 || ny >= h) continue;
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = x + dx;
                    if ((dx == 0 && dy == 0) || nx < 0 || nx >= w) continue;
                    const std::uint8_t* q = at(nx, ny);
                    if (q[3] == 0) continue;
                    r += q[0];
                    g += q[1];
                    b += q[2];
                    ++n;
                }
            }
            if (n == 0) continue;
            p[0] = static_cast<std::uint8_t>(r / n);
            p[1] = static_cast<std::uint8_t>(g / n);
            p[2] = static_cast<std::uint8_t>(b / n);
        }
    }
}

}  // namespace bomber::game
