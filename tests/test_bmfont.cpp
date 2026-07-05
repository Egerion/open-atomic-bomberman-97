// Checks for the front-end bitmap-font parser (FONT<n>.FON). Faithful to the
// loader sub_431BBC and the metric routine sub_432120 (BM95.EXE): a 20-byte
// header (count, height, spacing, magic0, magic1), a glyph table of
// {width, offset} pairs, and 1-bit-per-pixel MSB-first bitmaps indexed directly
// by char code. See docs/formats/fon.md.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "bomber/assets/bmfont.hpp"

using namespace bomber::assets::bmfont;

namespace {

// Little-endian u32 append.
void put32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    v.push_back(static_cast<std::uint8_t>(x));
    v.push_back(static_cast<std::uint8_t>(x >> 8));
    v.push_back(static_cast<std::uint8_t>(x >> 16));
    v.push_back(static_cast<std::uint8_t>(x >> 24));
}

// Build a tiny 3-glyph FON: code 0 = blank width-4, code 1 = width-8 with a
// single set row, code 2 = width-9 (2-byte stride) all clear. Height 3.
std::vector<std::uint8_t> make_font(std::uint32_t spacing) {
    const std::uint32_t count = 3, height = 3;
    // Bitmaps, contiguous; record each glyph's offset from the bitmap base.
    std::vector<std::uint8_t> bmp;
    // glyph 0: width 4, stride 1, 3 rows, all clear (offset 0).
    std::uint32_t off0 = static_cast<std::uint32_t>(bmp.size());
    for (int r = 0; r < 3; ++r) bmp.push_back(0x00);
    // glyph 1: width 8, stride 1, 3 rows; middle row = 0b10100000 (bits 0 and 2).
    std::uint32_t off1 = static_cast<std::uint32_t>(bmp.size());
    bmp.push_back(0x00);
    bmp.push_back(0xA0);  // MSB-first: pixels x=0 and x=2 set
    bmp.push_back(0x00);
    // glyph 2: width 9, stride 2, 3 rows, all clear.
    std::uint32_t off2 = static_cast<std::uint32_t>(bmp.size());
    for (int r = 0; r < 3 * 2; ++r) bmp.push_back(0x00);

    std::vector<std::uint8_t> f;
    put32(f, count);
    put32(f, height);
    put32(f, spacing);
    put32(f, 0xDEADBEEF);  // magic0 (ignored)
    put32(f, 0x12345678);  // magic1 (ignored)
    // Glyph table {width, offset}.
    put32(f, 4);
    put32(f, off0);
    put32(f, 8);
    put32(f, off1);
    put32(f, 9);
    put32(f, off2);
    f.insert(f.end(), bmp.begin(), bmp.end());
    return f;
}

}  // namespace

TEST_CASE("header, glyph metrics and advance widths") {
    Font font = parse(std::span<const std::uint8_t>(make_font(1)));
    CHECK(font.glyph_height == 3);
    CHECK(font.spacing == 1);
    REQUIRE(font.glyphs.size() == 3);
    CHECK(font.glyphs[0].width == 4);
    CHECK(font.glyphs[1].width == 8);
    CHECK(font.glyphs[2].width == 9);

    // advance = glyph width + spacing (sub_432120). Unknown codes contribute 0.
    CHECK(font.advance(0) == 5);
    CHECK(font.advance(1) == 9);
    CHECK(font.advance(2) == 10);
    CHECK(font.advance(99) == 0);  // past the table
    CHECK_FALSE(font.has(99));
}

TEST_CASE("1bpp bitmap rasterizes MSB-first") {
    Font font = parse(std::span<const std::uint8_t>(make_font(0)));
    const Glyph& g = font.glyphs[1];  // width 8, middle row 0xA0
    REQUIRE(g.pixels.size() == 8u * 3u);
    // Row 0 and row 2 are all clear.
    for (int x = 0; x < 8; ++x) {
        CHECK(g.pixels[0 * 8 + x] == 0);
        CHECK(g.pixels[2 * 8 + x] == 0);
    }
    // Middle row: 0xA0 = bits for x=0 and x=2 set (MSB-first), rest clear.
    CHECK(g.pixels[1 * 8 + 0] == 255);
    CHECK(g.pixels[1 * 8 + 1] == 0);
    CHECK(g.pixels[1 * 8 + 2] == 255);
    for (int x = 3; x < 8; ++x) CHECK(g.pixels[1 * 8 + x] == 0);
}

TEST_CASE("measure sums the run width like sub_432120") {
    Font font = parse(std::span<const std::uint8_t>(make_font(2)));
    // codes {0,1,2}: advances (4+2)+(8+2)+(9+2) = 6+10+11 = 27.
    const char run[] = {0, 1, 2};
    CHECK(font.measure(std::span<const char>(run, 3)) == 27);
}

TEST_CASE("a truncated or lying file throws rather than reading OOB") {
    // Header claims 3 glyphs but the buffer is cut before the table.
    std::vector<std::uint8_t> f = make_font(0);
    f.resize(24);  // header + 4 bytes of table only
    CHECK_THROWS(parse(std::span<const std::uint8_t>(f)));

    // A glyph whose offset points past the bitmap block must throw.
    std::vector<std::uint8_t> g = make_font(0);
    // The last glyph's offset field sits at header(20) + 2*8 + 4 = 40.
    g[40] = 0xFF;
    g[41] = 0xFF;  // absurd offset
    CHECK_THROWS(parse(std::span<const std::uint8_t>(g)));
}
