#pragma once

#include <cstddef>
#include <cstdint>

#include "bomber/assets/image.hpp"

// Makes a decoded image SAFE TO SAMPLE LINEARLY (the SOFT SCALING toggle,
// scale_filter.hpp; and the DATA_HD overrides, which are always linear).
//
// WHY IT IS NEEDED. Linear filtering samples a texel's RGB even where its ALPHA
// is zero, and SDL's default blend mode is non-premultiplied, so the blend at a
// cel's edge mixes in whatever colour sits behind the transparency. The 1997 art
// keeps the KEY COLOUR there: the ANI decode (libs/assets/src/ani.cpp) zeroes
// only the alpha — `rgba[i*4+3] = (v == key_color) ? 0 : 255` — which is free for
// the nearest-neighbour path it was written for and a magenta halo around every
// sprite the moment anything samples it linearly. The fix is the standard alpha
// dilate: give each fully-transparent texel the mean RGB of its opaque
// 8-neighbours, so an edge blend mixes the cel's own colour with itself.

namespace bomber::game {
namespace detail {

// The three numbers every texel address needs, so the helpers below stay inside
// the ≤3-parameter rule (docs/coding-standards.md §3).
struct BleedBitmap {
    std::uint8_t* px = nullptr;
    int w = 0;
    int h = 0;
};

inline std::uint8_t* bleed_texel(const BleedBitmap& bm, int x, int y) {
    return bm.px + (static_cast<std::size_t>(y) * static_cast<std::size_t>(bm.w) +
                    static_cast<std::size_t>(x)) *
                       4;
}

struct RgbSum {
    int r = 0, g = 0, b = 0, n = 0;
};

inline RgbSum opaque_neighbours(const BleedBitmap& bm, int x, int y) {
    static constexpr int kDx[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
    static constexpr int kDy[8] = {-1, -1, -1, 0, 0, 1, 1, 1};
    RgbSum s;
    for (int k = 0; k < 8; ++k) {
        const int nx = x + kDx[k];
        const int ny = y + kDy[k];
        if (nx < 0 || nx >= bm.w || ny < 0 || ny >= bm.h) continue;
        const std::uint8_t* q = bleed_texel(bm, nx, ny);
        if (q[3] == 0) continue;
        s.r += q[0];
        s.g += q[1];
        s.b += q[2];
        ++s.n;
    }
    return s;
}

// One texel. A texel with no opaque neighbour at all (deep inside a transparent
// region) is left alone: nothing samples it in any filter mode, and inventing a
// colour for it would only spread the key colour further.
inline void bleed_one(const BleedBitmap& bm, int x, int y) {
    std::uint8_t* p = bleed_texel(bm, x, y);
    if (p[3] != 0) return;
    const RgbSum s = opaque_neighbours(bm, x, y);
    if (s.n == 0) return;
    p[0] = static_cast<std::uint8_t>(s.r / s.n);
    p[1] = static_cast<std::uint8_t>(s.g / s.n);
    p[2] = static_cast<std::uint8_t>(s.b / s.n);
}

}  // namespace detail

// Bleeds opaque colour outward into the fully-transparent texels, IN PLACE.
//
// In-place is safe by construction: the pass only WRITES texels whose alpha is 0
// and only READS texels whose alpha is non-0, so a texel it has already written
// can never become a source for a later one and the result does not cascade —
// it is exactly one dilation step, whatever the iteration order.
//
// Alpha is never touched, so the nearest-neighbour render is bit-identical
// before and after, which is what lets this run unconditionally on every upload
// including the frames tests/visual pins.
inline void bleed_transparent_rgb(assets::Image& img) {
    const detail::BleedBitmap bm{img.rgba.data(), img.width, img.height};
    if (bm.w <= 0 || bm.h <= 0) return;
    const std::size_t need = static_cast<std::size_t>(bm.w) * static_cast<std::size_t>(bm.h) * 4;
    if (img.rgba.size() < need) return;  // 1997 files are untrusted input
    for (int y = 0; y < bm.h; ++y)
        for (int x = 0; x < bm.w; ++x) detail::bleed_one(bm, x, y);
}

}  // namespace bomber::game
