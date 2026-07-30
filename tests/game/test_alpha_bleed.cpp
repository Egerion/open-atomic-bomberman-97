// The alpha-dilate pass every uploaded texture goes through (alpha_bleed.hpp),
// so a linear sample at a cel edge cannot drag the 1997 art's KEY COLOUR into
// the blend. The rules are easy to get subtly wrong — an in-place pass that
// cascades, a border that reads out of bounds, an alpha the dilate quietly
// rewrites (which would move every tests/visual pin) — so they are pinned here
// rather than eyeballed once on a screenshot.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "bomber/game/alpha_bleed.hpp"

using bomber::assets::Image;
using bomber::game::bleed_transparent_rgb;

namespace {

// w*h RGBA image; `spec` is one char per pixel: 'o' = opaque red-ish marker,
// '.' = fully transparent carrying the MAGENTA key colour (255,0,255), which is
// exactly the state ani.cpp's decode leaves behind.
Image make(int w, int h, const char* spec) {
    Image img;
    img.width = w;
    img.height = h;
    img.rgba.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
    for (int i = 0; i < w * h; ++i) {
        std::uint8_t* p = img.rgba.data() + static_cast<std::size_t>(i) * 4;
        if (spec[i] == 'o') {
            p[0] = 200;
            p[1] = 40;
            p[2] = 40;
            p[3] = 255;
        } else {
            p[0] = 255;
            p[1] = 0;
            p[2] = 255;  // the key colour, left in the buffer by the decode
            p[3] = 0;
        }
    }
    return img;
}

const std::uint8_t* px(const Image& img, int x, int y) {
    return img.rgba.data() +
           (static_cast<std::size_t>(y) * static_cast<std::size_t>(img.width) +
            static_cast<std::size_t>(x)) *
               4;
}

}  // namespace

TEST_CASE("a transparent texel next to opaque art loses the key colour") {
    // . o .
    // The two transparent neighbours take the opaque texel's colour, so a linear
    // sample across either edge blends red with red instead of red with magenta.
    Image img = make(3, 1, ".o.");
    bleed_transparent_rgb(img);
    for (int x : {0, 2}) {
        CHECK(px(img, x, 0)[0] == 200);
        CHECK(px(img, x, 0)[1] == 40);
        CHECK(px(img, x, 0)[2] == 40);
    }
}

TEST_CASE("ALPHA is never touched, which is what keeps the crisp render identical") {
    // The whole reason this can run unconditionally on every upload: nearest
    // sampling only ever shows a texel's RGB where alpha is non-zero, and the
    // pass changes neither the alphas nor any opaque texel's colour.
    Image img = make(3, 3,
                     ".o."
                     "oo."
                     "...");
    const std::vector<std::uint8_t> before = img.rgba;
    bleed_transparent_rgb(img);
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 3; ++x) {
            const std::uint8_t* a = before.data() + (static_cast<std::size_t>(y) * 3 + x) * 4;
            const std::uint8_t* b = px(img, x, y);
            CHECK(a[3] == b[3]);                    // alpha preserved everywhere
            if (a[3] != 0) {                        // opaque art is untouched
                CHECK(a[0] == b[0]);
                CHECK(a[1] == b[1]);
                CHECK(a[2] == b[2]);
            }
        }
    }
}

TEST_CASE("a transparent texel with no opaque neighbour is left alone") {
    // Deep inside a transparent region nothing samples it, and inventing a
    // colour would only spread the key colour one ring further out per call.
    Image img = make(5, 1, "o...o");
    bleed_transparent_rgb(img);
    CHECK(px(img, 2, 0)[0] == 255);  // still the untouched key colour
    CHECK(px(img, 2, 0)[1] == 0);
    CHECK(px(img, 2, 0)[2] == 255);
}

TEST_CASE("the pass does NOT cascade: exactly one dilation step, order-independent") {
    // The in-place safety argument. Pixel 1 is bled from pixel 0; if the pass
    // then used pixel 1 as a SOURCE for pixel 2, the colour would creep across
    // the whole row and the result would depend on iteration order.
    Image img = make(4, 1, "o...");
    bleed_transparent_rgb(img);
    CHECK(px(img, 1, 0)[0] == 200);  // one step out: bled
    CHECK(px(img, 2, 0)[0] == 255);  // two steps out: untouched key colour
    CHECK(px(img, 2, 0)[1] == 0);
    CHECK(px(img, 3, 0)[0] == 255);
}

TEST_CASE("a transparent texel takes the MEAN of its opaque neighbours") {
    Image img = make(3, 1, "oo.");
    // Make the two opaque texels differ so the average is observable.
    img.rgba[0] = 100;
    img.rgba[4] = 200;
    bleed_transparent_rgb(img);
    CHECK(px(img, 2, 0)[0] == 200);  // only texel 1 is a neighbour of texel 2
    Image img2 = make(3, 3,
                      "o.o"
                      "..."
                      "...");
    img2.rgba[0] = 100;                          // (0,0) red = 100
    img2.rgba[2 * 4 + 0] = 200;                  // (2,0) red = 200
    bleed_transparent_rgb(img2);
    CHECK(px(img2, 1, 0)[0] == 150);             // mean of both opaque neighbours
}

TEST_CASE("degenerate and hostile inputs are refused, not walked off the end") {
    // 1997 files are untrusted (CLAUDE.md): a header claiming a size the pixel
    // buffer does not have must bail rather than run past it.
    Image empty;
    bleed_transparent_rgb(empty);  // 0x0: no-op, no crash

    Image lying = make(2, 2, "o...");
    lying.width = 64;  // claims 64x2 but holds 4 texels
    lying.height = 2;
    const std::vector<std::uint8_t> before = lying.rgba;
    bleed_transparent_rgb(lying);
    CHECK(lying.rgba == before);  // short buffer: left untouched
}

TEST_CASE("a fully opaque image is unchanged (the common case pays nothing)") {
    Image img = make(2, 2, "oooo");
    const std::vector<std::uint8_t> before = img.rgba;
    bleed_transparent_rgb(img);
    CHECK(img.rgba == before);
}
