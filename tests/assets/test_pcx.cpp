// PCX decoder contract. Like the .ANI loader this parser eats 1997 files from
// an untrusted disk and had no suite at all (docs/coding-standards.md §12).
//
// Every case drives the REAL decoder -- pcx::parse() over a byte buffer this
// file synthesises from the format spec -- rather than asserting on a
// hand-built Image, so a regression in the header walk, the RLE loop or the
// plane de-interleave is caught. No shipped asset is committed.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <vector>

#include "bomber/assets/pcx.hpp"
#include "fixture.hpp"

using namespace bomber::assets;
using bomber::test::append;
using bomber::test::Bytes;

namespace {

// The fixed 128-byte PCX header, laid out at the offsets pcx::parse reads:
// 0 manufacturer, 2 encoding, 3 bpp, 4..11 the window, 65 planes, 66 stride.
struct HeaderSpec {
    std::uint8_t manufacturer = 0x0A;
    std::uint8_t encoding = 1;
    std::uint8_t bpp = 8;
    unsigned xmin = 0, ymin = 0, xmax = 1, ymax = 1;
    std::uint8_t planes = 1;
    unsigned bytes_per_line = 2;
};

// The header is fixed-width with fields at known offsets, so it is patched in
// place rather than appended -- put_u16 (which grows) is the wrong tool here.
void patch_u16(Bytes& b, std::size_t at, unsigned v) {
    b[at] = static_cast<std::uint8_t>(v & 0xFF);
    b[at + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
}

Bytes header(const HeaderSpec& h) {
    Bytes b(128, 0);
    b[0] = h.manufacturer;
    b[1] = 5;  // version
    b[2] = h.encoding;
    b[3] = h.bpp;
    patch_u16(b, 4, h.xmin);
    patch_u16(b, 6, h.ymin);
    patch_u16(b, 8, h.xmax);
    patch_u16(b, 10, h.ymax);
    b[65] = h.planes;
    patch_u16(b, 66, h.bytes_per_line);
    return b;
}

// 256 VGA triples; index i is (i*10, i*20, i*30) so a lookup is checkable.
// Preceded by the 0x0C marker the decoder requires.
Bytes vga_palette() {
    Bytes tail{0x0C};
    for (int i = 0; i < 256; ++i) {
        tail.push_back(static_cast<std::uint8_t>((i * 10) & 0xFF));
        tail.push_back(static_cast<std::uint8_t>((i * 20) & 0xFF));
        tail.push_back(static_cast<std::uint8_t>((i * 30) & 0xFF));
    }
    return tail;
}

// A byte with the top two bits set starts a run; anything below 0xC0 is a
// literal. Fixtures below stay under 0xC0 wherever a literal is intended.
Bytes rle_run(unsigned count, std::uint8_t value) {
    return {static_cast<std::uint8_t>(0xC0 | (count & 0x3F)), value};
}

Image parse(const Bytes& file) {
    return pcx::parse(std::span<const std::uint8_t>(file.data(), file.size()), "test.pcx");
}

}  // namespace

TEST_CASE("an 8-bit indexed PCX decodes to RGBA and keeps its indices and palette") {
    // 2x2, stride 2. Four literal bytes: indices 1,2 then 3,4.
    Bytes f = header({});
    append(f, Bytes{1, 2, 3, 4});
    append(f, vga_palette());

    Image img = parse(f);
    CHECK(img.width == 2);
    CHECK(img.height == 2);
    REQUIRE(img.rgba.size() == 2 * 2 * 4);
    // The original's transparency is an INDEX, not a colour (pcx.hpp), so both
    // extra vectors must survive.
    REQUIRE(img.indices.size() == 4);
    CHECK(img.indices[0] == 1);
    CHECK(img.indices[3] == 4);
    REQUIRE(img.palette.size() == 256 * 4);
    CHECK(img.paletted());
    // Index 1 -> (10, 20, 30); index 4 -> (40, 80, 120). Alpha is always opaque.
    CHECK(img.rgba[0] == 10);
    CHECK(img.rgba[1] == 20);
    CHECK(img.rgba[2] == 30);
    CHECK(img.rgba[3] == 255);
    CHECK(img.rgba[3 * 4 + 0] == 40);
    CHECK(img.rgba[3 * 4 + 1] == 80);
    CHECK(img.rgba[3 * 4 + 2] == 120);
    // The kept palette is RGBA, and matches the file's triples.
    CHECK(img.palette[1 * 4 + 0] == 10);
    CHECK(img.palette[1 * 4 + 3] == 255);
}

TEST_CASE("an RLE run expands to repeated pixels") {
    // 4x1, stride 4: one run of four copies of index 7.
    Bytes f = header({.xmax = 3, .ymax = 0, .bytes_per_line = 4});
    append(f, rle_run(4, 7));
    append(f, vga_palette());

    Image img = parse(f);
    CHECK(img.width == 4);
    CHECK(img.height == 1);
    REQUIRE(img.indices.size() == 4);
    for (std::size_t i = 0; i < 4; ++i) {
        CHECK(img.indices[i] == 7);
        CHECK(img.rgba[i * 4 + 0] == 70);
        CHECK(img.rgba[i * 4 + 1] == 140);
        CHECK(img.rgba[i * 4 + 2] == 210);
    }
}

TEST_CASE("stride padding beyond the image width is decoded and discarded") {
    // 2x1 image in a 4-byte scanline: the last two bytes are padding and must
    // not leak into the output.
    Bytes f = header({.xmax = 1, .ymax = 0, .bytes_per_line = 4});
    append(f, Bytes{1, 2, 99, 99});
    append(f, vga_palette());

    Image img = parse(f);
    CHECK(img.width == 2);
    REQUIRE(img.indices.size() == 2);
    CHECK(img.indices[0] == 1);
    CHECK(img.indices[1] == 2);
}

TEST_CASE("a 24-bit PCX de-interleaves its three planes and carries no palette") {
    // 2x2, three planes, stride 2: each row is R,R,G,G,B,B.
    Bytes f = header({.xmax = 1, .ymax = 1, .planes = 3, .bytes_per_line = 2});
    append(f, Bytes{10, 20, 30, 40, 50, 60});  // row 0
    append(f, Bytes{11, 21, 31, 41, 51, 61});  // row 1
    // No trailing palette: truecolour PCX has none, and the decoder must not
    // go looking for one.

    Image img = parse(f);
    CHECK(img.width == 2);
    CHECK(img.height == 2);
    CHECK(img.indices.empty());
    CHECK(img.palette.empty());
    CHECK_FALSE(img.paletted());
    CHECK(img.rgba[0] == 10);
    CHECK(img.rgba[1] == 30);
    CHECK(img.rgba[2] == 50);
    CHECK(img.rgba[3] == 255);
    CHECK(img.rgba[1 * 4 + 0] == 20);
    CHECK(img.rgba[1 * 4 + 1] == 40);
    CHECK(img.rgba[1 * 4 + 2] == 60);
    // Second row starts at pixel 2.
    CHECK(img.rgba[2 * 4 + 0] == 11);
    CHECK(img.rgba[2 * 4 + 2] == 51);
}

TEST_CASE("a buffer too small to hold a header is rejected") {
    CHECK_THROWS_AS(parse(Bytes(64, 0)), std::runtime_error);
}

TEST_CASE("a non-PCX or unsupported variant is rejected") {
    Bytes bad_magic = header({});
    bad_magic[0] = 0x00;
    append(bad_magic, vga_palette());
    CHECK_THROWS_AS(parse(bad_magic), std::runtime_error);

    Bytes uncompressed = header({});
    uncompressed[2] = 0;  // encoding != 1
    append(uncompressed, vga_palette());
    CHECK_THROWS_AS(parse(uncompressed), std::runtime_error);

    Bytes four_bit = header({.bpp = 4});
    append(four_bit, vga_palette());
    CHECK_THROWS_AS(parse(four_bit), std::runtime_error);
}

TEST_CASE("an inverted or empty window is rejected") {
    Bytes f = header({.xmin = 5, .xmax = 2});
    append(f, vga_palette());
    CHECK_THROWS_AS(parse(f), std::runtime_error);
}

TEST_CASE("a stride narrower than the image is rejected") {
    // bytes_per_line < width would make the scanline buffer too short for the
    // per-pixel reads below it.
    Bytes f = header({.xmax = 7, .bytes_per_line = 2});
    append(f, vga_palette());
    CHECK_THROWS_AS(parse(f), std::runtime_error);
}

TEST_CASE("an 8-bit PCX with no palette block is rejected") {
    Bytes f = header({});
    append(f, Bytes{1, 2, 3, 4});
    CHECK_THROWS_AS(parse(f), std::runtime_error);
}

TEST_CASE("an 8-bit PCX whose palette marker is wrong is rejected") {
    Bytes f = header({});
    append(f, Bytes{1, 2, 3, 4});
    Bytes pal = vga_palette();
    pal[0] = 0x00;  // marker must be 0x0C
    append(f, pal);
    CHECK_THROWS_AS(parse(f), std::runtime_error);
}

TEST_CASE("a scanline that runs out of data mid-row is rejected") {
    // 4x4 declared, two bytes of pixel data supplied.
    Bytes f = header({.xmax = 3, .ymax = 3, .bytes_per_line = 4});
    append(f, Bytes{1, 2});
    append(f, vga_palette());
    CHECK_THROWS_AS(parse(f), std::runtime_error);
}

TEST_CASE("a run header at the very end of the data with no value byte is rejected") {
    Bytes f = header({.xmax = 3, .ymax = 0, .bytes_per_line = 4});
    append(f, Bytes{0xC4});  // "four copies of..." and then nothing
    append(f, vga_palette());
    CHECK_THROWS_AS(parse(f), std::runtime_error);
}

// SECURITY. width and height come from four u16s in the header and their
// product used to reach `rgba.resize(px * 4)` BEFORE anything checked that the
// file could possibly hold that many decoded bytes. A 900-byte file declaring
// 65535x65536 therefore asked the allocator for 17 GB. A PCX run packet is two
// input bytes for at most 63 output bytes, so the remaining data section is a
// hard cap on the decoded size, and check_decodable is that cap.
//
// HOW THIS CASE FAILS, measured 2026-08-01 by disabling check_decodable -- the
// same finding as the .ANI suite's twin case, and this comment carried the same
// wrong claim ("turns this case red -- bad_alloc, not runtime_error"). On the
// reference Win11 box the allocation SUCCEEDS against the page file, the row
// decoder then runs out of input and throws the same std::runtime_error, and
// the case stays GREEN at 8.1 s instead of 0.0003 s. The ctest TIMEOUT
// (tests/assets/CMakeLists.txt) is what makes it red. A machine that refuses
// the allocation instead throws std::bad_alloc, which escapes the module's
// error contract (docs/coding-standards.md §6) and fails on type. Both paths
// are covered; neither alone is.
TEST_CASE("SECURITY: header dimensions are rejected against the size of the data section") {
    Bytes f = header({.xmax = 0xFFFE, .ymax = 0xFFFF, .bytes_per_line = 0xFFFF});
    append(f, Bytes{1, 2, 3, 4});
    append(f, vga_palette());
    CHECK_THROWS_AS(parse(f), std::runtime_error);
}

TEST_CASE("SECURITY: a 24-bit PCX cannot claim more rows than its data section can fill") {
    Bytes f = header({.xmax = 0xFFFE, .ymax = 0xFFFF, .planes = 3, .bytes_per_line = 0xFFFF});
    append(f, Bytes{1, 2, 3, 4});
    CHECK_THROWS_AS(parse(f), std::runtime_error);
}

TEST_CASE("a missing file throws rather than yielding an empty image") {
    // absent_path, not a bare temp name: a leftover from an earlier run would
    // still throw (it is not a PCX), and the case would pass for the wrong
    // reason while covering nothing.
    CHECK_THROWS_AS(pcx::load(bomber::test::absent_path("obm_pcx_absent.pcx")), std::runtime_error);
}
