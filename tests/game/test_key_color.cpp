// The transparent-blit key the `.BM` text-screen viewer applies to its inline
// `<IMGname>` art (key_color.hpp): sub_4428E4 -> sub_44AED5 skips source bytes
// equal to 0, so palette INDEX 0 is transparent — not the colour black, which
// several of the shipped images carry at a different index.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>

#include "bomber/assets/image.hpp"
#include "bomber/game_util/key_color.hpp"

using bomber::assets::Image;
using namespace bomber::game;

namespace {

// A 2x2 indexed image: index 0 top-left, and a DIFFERENT index that happens to
// be black top-right — the JERM.PCX/KURT.PCX shape, where the photo's real
// blacks live at index 255 and index 0 is never used.
Image two_by_two(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d) {
    Image img;
    img.width = 2;
    img.height = 2;
    img.indices = {a, b, c, d};
    img.palette.assign(std::size_t{256} * 4, 255);
    // palette[0] and palette[255] are both black; everything else is white.
    for (int i = 0; i < 256; ++i) {
        const bool black = (i == 0 || i == 255);
        for (int ch = 0; ch < 3; ++ch)
            img.palette[static_cast<std::size_t>(i) * 4 + ch] = black ? 0 : 255;
    }
    img.rgba.resize(16);
    const std::uint8_t idx[4] = {a, b, c, d};
    for (int p = 0; p < 4; ++p) {
        for (int ch = 0; ch < 3; ++ch)
            img.rgba[static_cast<std::size_t>(p) * 4 + ch] =
                img.palette[static_cast<std::size_t>(idx[p]) * 4 + ch];
        img.rgba[static_cast<std::size_t>(p) * 4 + 3] = 255;
    }
    return img;
}

std::uint8_t alpha(const Image& img, int p) {
    return img.rgba[static_cast<std::size_t>(p) * 4 + 3];
}

}  // namespace

TEST_CASE("index 0 becomes transparent and nothing else does") {
    Image img = two_by_two(0, 255, 7, 0);
    apply_key_index(img);
    CHECK(alpha(img, 0) == 0);    // index 0: keyed
    CHECK(alpha(img, 1) == 255);  // index 255: BLACK but opaque
    CHECK(alpha(img, 2) == 255);
    CHECK(alpha(img, 3) == 0);
}

TEST_CASE("RGB is left alone; only alpha moves") {
    // The upload's alpha dilate (alpha_bleed.hpp) owns the keyed texels' colour;
    // this pass must not pre-empt it, exactly as the ANI decode does not.
    Image img = two_by_two(0, 7, 7, 7);
    const auto before = img.rgba;
    apply_key_index(img);
    for (std::size_t o = 0; o + 3 < img.rgba.size(); o += 4)
        for (int ch = 0; ch < 3; ++ch) CHECK(img.rgba[o + ch] == before[o + ch]);
}

TEST_CASE("a non-zero key index can be requested") {
    Image img = two_by_two(0, 255, 7, 0);
    apply_key_index(img, 7);
    CHECK(alpha(img, 0) == 255);
    CHECK(alpha(img, 2) == 0);
}

TEST_CASE("an image with no indices is untouched") {
    // 24-bit PCX (DATA_HD) and anything already decoded to RGBA: calling the
    // index key on it must be a no-op, so callers need no guard.
    Image img;
    img.width = 1;
    img.height = 1;
    img.rgba = {0, 0, 0, 255};
    apply_key_index(img);
    CHECK(img.rgba[3] == 255);
}

TEST_CASE("uses_key_index reports whether the asset participates at all") {
    // CREDBAR/BOMBDUDE/QALOGO do; JERM/KURT contain no index 0 whatsoever, which
    // is what stops the DATA_HD black key from eating holes in the photographs.
    CHECK(uses_key_index(two_by_two(0, 1, 2, 3)));
    CHECK_FALSE(uses_key_index(two_by_two(1, 255, 2, 3)));
    CHECK_FALSE(uses_key_index(Image{}));
}

TEST_CASE("the DATA_HD variant keys pure black, and only pure black") {
    Image img;
    img.width = 3;
    img.height = 1;
    img.rgba = {0, 0, 0, 255,  // pure black: the HD pack's key paint
                0, 0, 1, 255,  // one channel off: kept
                9, 9, 9, 255};
    apply_key_black(img);
    CHECK(img.rgba[3] == 0);
    CHECK(img.rgba[7] == 255);
    CHECK(img.rgba[11] == 255);
}
