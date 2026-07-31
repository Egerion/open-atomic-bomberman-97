// CHFILEANI (.ANI) loader contract. This parser reads 1997 files off an
// untrusted disk and had no suite at all until now (docs/coding-standards.md
// §12), which is the wrong gap to leave in the component whose entire job is
// hostile input.
//
// Everything here drives the REAL loader end-to-end — ani::load() on a file on
// disk — rather than hand-building an AniFile, so a regression anywhere in the
// container walk, the CIMG header, or the RLE decoder is caught. Every fixture
// is SYNTHESISED byte by byte from docs/formats/ani.md; no shipped asset is
// committed, and none is needed.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "bomber/assets/ani.hpp"

using namespace bomber::assets;

namespace {

using Bytes = std::vector<std::uint8_t>;

void put_u16(Bytes& b, unsigned v) {
    b.push_back(static_cast<std::uint8_t>(v & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}

void put_u32(Bytes& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}

void put_bytes(Bytes& b, const Bytes& more) { b.insert(b.end(), more.begin(), more.end()); }

// One container item: 4-byte tag, u32 payload length, u16 id, then the payload.
// `length` counts the payload ONLY -- the loader takes body = pos-after-header
// and end = body + length.
Bytes item(const char* tag, const Bytes& payload) {
    Bytes out;
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(tag[i]));
    put_u32(out, static_cast<std::uint32_t>(payload.size()));
    put_u16(out, 0);
    put_bytes(out, payload);
    return out;
}

// "CHFILEANI " + u32 payload length + u16 file id, then the item stream.
Bytes ani_file(const Bytes& items) {
    Bytes out;
    const char magic[] = "CHFILEANI ";
    for (int i = 0; i < 10; ++i) out.push_back(static_cast<std::uint8_t>(magic[i]));
    put_u32(out, static_cast<std::uint32_t>(items.size()));
    put_u16(out, 0);
    put_bytes(out, items);
    return out;
}

// TGA type-10 run packet: high bit set, count-1 in the low seven bits, then one
// pixel. `count` may exceed the image on purpose (the overrun cases below).
Bytes rle_run16(unsigned count, std::uint16_t value) {
    Bytes b{static_cast<std::uint8_t>(0x80 | ((count - 1) & 0x7F))};
    put_u16(b, value);
    return b;
}

Bytes rle_run8(unsigned count, std::uint8_t value) {
    return {static_cast<std::uint8_t>(0x80 | ((count - 1) & 0x7F)), value};
}

// A literal packet: high bit clear, then `values.size()` pixels verbatim.
Bytes rle_literal16(const std::vector<std::uint16_t>& values) {
    Bytes b{static_cast<std::uint8_t>((values.size() - 1) & 0x7F)};
    for (std::uint16_t v : values) put_u16(b, v);
    return b;
}

// CIMG payload, 16bpp (type 4): no palette, so additional_size is the bare 24.
Bytes cimg16(unsigned w, unsigned h, std::uint16_t key, const Bytes& rle,
             std::uint32_t uncompressed_override = 0) {
    Bytes p;
    put_u16(p, 4);                              // cimg_type
    put_u16(p, 0);                              // unknown
    put_u32(p, 24);                             // additional_size: no palette block
    put_u32(p, 0);                              // unknown
    put_u16(p, w);
    put_u16(p, h);
    put_u16(p, 0);                              // hotspot x
    put_u16(p, 0);                              // hotspot y
    put_u16(p, key);
    put_u16(p, 0);                              // unknown
    put_u16(p, 0);                              // unknown
    put_u16(p, 0);                              // unknown
    put_u32(p, static_cast<std::uint32_t>(12 + rle.size()));  // compressed_size
    put_u32(p, uncompressed_override != 0 ? uncompressed_override
                                          : static_cast<std::uint32_t>(w * h * 2));
    put_bytes(p, rle);
    return p;
}

// CIMG payload, 8bpp paletted (type 11): additional_size = 32 + the 1024-byte
// RGBA palette, and the loader insists on exactly that width.
Bytes cimg8(unsigned w, unsigned h, std::uint16_t key, const Bytes& palette, const Bytes& rle) {
    Bytes p;
    put_u16(p, 11);
    put_u16(p, 0);
    put_u32(p, static_cast<std::uint32_t>(32 + palette.size()));
    put_u32(p, 0);
    put_u16(p, w);
    put_u16(p, h);
    put_u16(p, 0);
    put_u16(p, 0);
    put_u16(p, key);
    put_u16(p, 0);
    put_u32(p, 0);  // unknown (only present when additional_size >= 32)
    put_u32(p, 0);  // unknown
    put_bytes(p, palette);
    put_u16(p, 0);
    put_u16(p, 0);
    put_u32(p, static_cast<std::uint32_t>(12 + rle.size()));
    put_u32(p, static_cast<std::uint32_t>(w * h));
    put_bytes(p, rle);
    return p;
}

// 256 RGBA entries; index i is (i, i*2, i*3, 255) so a lookup is checkable.
Bytes ramp_palette() {
    Bytes pal;
    for (int i = 0; i < 256; ++i) {
        pal.push_back(static_cast<std::uint8_t>(i));
        pal.push_back(static_cast<std::uint8_t>((i * 2) & 0xFF));
        pal.push_back(static_cast<std::uint8_t>((i * 3) & 0xFF));
        pal.push_back(255);
    }
    return pal;
}

struct TempFile {
    std::filesystem::path path;

    explicit TempFile(const char* name, const Bytes& data)
        : path(std::filesystem::temp_directory_path() / name) {
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size()));
    }
    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
};

// RGB555 pure red: expand5(31) == 255, expand5(0) == 0.
constexpr std::uint16_t kRed555 = 31u << 10;
constexpr std::uint16_t kBlue555 = 31u;

}  // namespace

TEST_CASE("a 16bpp (type 4) frame decodes to RGBA with the key colour punched out") {
    Bytes items;
    put_bytes(items, item("CBOX", [] {
                  Bytes b;
                  put_u16(b, 24);
                  put_u16(b, 32);
                  return b;
              }()));
    // 2x2: three red pixels then one blue, with blue as the key colour.
    Bytes rle = rle_run16(3, kRed555);
    put_bytes(rle, rle_literal16({kBlue555}));
    Bytes fram;
    put_bytes(fram, item("FNAM", [] {
                  Bytes b;
                  const char n[] = "WLKN0000.TGA";
                  for (char c : n) b.push_back(static_cast<std::uint8_t>(c));
                  return b;
              }()));
    put_bytes(fram, item("CIMG", cimg16(2, 2, kBlue555, rle)));
    put_bytes(items, item("FRAM", fram));

    TempFile tf("obm_ani_type4.ani", ani_file(items));
    ani::AniFile a = ani::load(tf.path);

    CHECK(a.cell_width == 24);
    CHECK(a.cell_height == 32);
    REQUIRE(a.frames.size() == 1);
    const ani::Frame& f = a.frames[0];
    CHECK(f.name == "WLKN0000.TGA");
    CHECK(f.cimg_type == 4);
    CHECK(f.image.width == 2);
    CHECK(f.image.height == 2);
    REQUIRE(f.image.rgba.size() == 2 * 2 * 4);
    // Pixel 0: opaque red.
    CHECK(f.image.rgba[0] == 255);
    CHECK(f.image.rgba[1] == 0);
    CHECK(f.image.rgba[2] == 0);
    CHECK(f.image.rgba[3] == 255);
    // Pixel 3 is the key colour: alpha 0.
    CHECK(f.image.rgba[3 * 4 + 2] == 255);  // blue channel still expanded
    CHECK(f.image.rgba[3 * 4 + 3] == 0);
    // Type 4 keeps no source indices -- recolouring goes through COLOR.PAL.
    CHECK(f.image.indices.empty());
    CHECK_FALSE(f.image.paletted());
}

TEST_CASE("an 8bpp (type 11) frame keeps its indices and palette for .RMP recolour") {
    // Four pixels: index 5 three times, then index 200 (the key).
    Bytes rle = rle_run8(3, 5);
    put_bytes(rle, rle_run8(1, 200));
    Bytes fram = item("CIMG", cimg8(2, 2, 200, ramp_palette(), rle));
    TempFile tf("obm_ani_type11.ani", ani_file(item("FRAM", fram)));

    ani::AniFile a = ani::load(tf.path);
    REQUIRE(a.frames.size() == 1);
    const ani::Frame& f = a.frames[0];
    CHECK(f.cimg_type == 11);
    REQUIRE(f.image.indices.size() == 4);
    CHECK(f.image.indices[0] == 5);
    CHECK(f.image.indices[3] == 200);
    REQUIRE(f.image.palette.size() == 1024);
    CHECK(f.image.paletted());
    // Palette lookup: index 5 -> (5, 10, 15), opaque.
    CHECK(f.image.rgba[0] == 5);
    CHECK(f.image.rgba[1] == 10);
    CHECK(f.image.rgba[2] == 15);
    CHECK(f.image.rgba[3] == 255);
    // The key index is transparent even though the palette gives it a colour.
    CHECK(f.image.rgba[3 * 4 + 3] == 0);
}

TEST_CASE("a SEQ item yields ordered steps with their per-step blit offsets") {
    Bytes items = item("FRAM", item("CIMG", cimg16(1, 1, 0, rle_run16(1, kRed555))));

    auto stat = [](unsigned frame, int dx, int dy) {
        Bytes head;
        put_u16(head, 0x001E);
        Bytes fr;
        put_u16(fr, 1);  // unknown, always 1
        put_u16(fr, frame);
        put_u16(fr, static_cast<unsigned>(dx) & 0xFFFF);
        put_u16(fr, static_cast<unsigned>(dy) & 0xFFFF);
        Bytes payload = item("HEAD", head);
        put_bytes(payload, item("FRAM", fr));
        return item("STAT", payload);
    };

    Bytes seq;
    put_bytes(seq, item("HEAD", [] {
                  Bytes b;
                  const char n[] = "walk north";
                  for (char c : n) b.push_back(static_cast<std::uint8_t>(c));
                  return b;
              }()));
    put_bytes(seq, stat(0, 3, -4));
    put_bytes(seq, stat(0, -1, 2));
    put_bytes(items, item("SEQ ", seq));

    TempFile tf("obm_ani_seq.ani", ani_file(items));
    ani::AniFile a = ani::load(tf.path);

    REQUIRE(a.sequences.size() == 1);
    CHECK(a.sequences[0].name == "walk north");
    REQUIRE(a.sequences[0].steps.size() == 2);
    CHECK(a.sequences[0].steps[0].frame == 0);
    CHECK(a.sequences[0].steps[0].dx == 3);
    CHECK(a.sequences[0].steps[0].dy == -4);
    CHECK(a.sequences[0].steps[0].head0 == 0x001E);
    CHECK(a.sequences[0].steps[1].dx == -1);
    CHECK(a.sequences[0].steps[1].dy == 2);
    CHECK(a.warnings.empty());
}

TEST_CASE("a sequence step naming a frame that does not exist is warned and dropped") {
    // The shipped POWERS1.ANI/POWERZ.ANI really do this (ani.hpp's comment); the
    // loader must keep the file usable rather than throw.
    Bytes items = item("FRAM", item("CIMG", cimg16(1, 1, 0, rle_run16(1, kRed555))));
    Bytes fr;
    put_u16(fr, 1);
    put_u16(fr, 9);  // only frame 0 exists
    put_u16(fr, 0);
    put_u16(fr, 0);
    Bytes seq = item("HEAD", Bytes{'p', 'j', 0});
    put_bytes(seq, item("STAT", item("FRAM", fr)));
    put_bytes(items, item("SEQ ", seq));

    TempFile tf("obm_ani_badref.ani", ani_file(items));
    ani::AniFile a = ani::load(tf.path);

    REQUIRE(a.sequences.size() == 1);
    REQUIRE(a.sequences[0].steps.size() == 1);
    CHECK(a.sequences[0].steps[0].frame == -1);
    REQUIRE(a.warnings.size() == 1);
    CHECK(a.warnings[0].find("references frame 9") != std::string::npos);
}

TEST_CASE("a file that is not an ANI is rejected") {
    TempFile tf("obm_ani_magic.ani", Bytes(64, 'x'));
    CHECK_THROWS_AS(ani::load(tf.path), std::runtime_error);
}

TEST_CASE("a declared payload length past the end of the file is rejected") {
    Bytes f = ani_file(item("CBOX", Bytes(4, 0)));
    f[10] = 0xFF;  // payload_len = 0x0000FFFF, far past the real size
    f[11] = 0xFF;
    TempFile tf("obm_ani_longlen.ani", f);
    CHECK_THROWS_AS(ani::load(tf.path), std::runtime_error);
}

TEST_CASE("an item whose length runs past the end of the file is rejected") {
    Bytes it = item("CBOX", Bytes(4, 0));
    it[4] = 0xFF;  // item length = 0x0000FFFF
    it[5] = 0xFF;
    Bytes f = ani_file(it);
    // Keep the file-level length honest so the ITEM check is what fires.
    f[10] = static_cast<std::uint8_t>(it.size() & 0xFF);
    f[11] = static_cast<std::uint8_t>((it.size() >> 8) & 0xFF);
    TempFile tf("obm_ani_longitem.ani", f);
    CHECK_THROWS_AS(ani::load(tf.path), std::runtime_error);
}

TEST_CASE("a CIMG whose uncompressed size disagrees with its dimensions is rejected") {
    Bytes fram = item("CIMG", cimg16(2, 2, 0, rle_run16(4, kRed555), /*uncompressed=*/99));
    TempFile tf("obm_ani_sizemismatch.ani", ani_file(item("FRAM", fram)));
    CHECK_THROWS_AS(ani::load(tf.path), std::runtime_error);
}

TEST_CASE("an unknown CIMG type is rejected rather than silently skipped") {
    Bytes p = cimg16(2, 2, 0, rle_run16(4, kRed555));
    p[0] = 7;  // cimg_type: neither 4 nor 11
    TempFile tf("obm_ani_badtype.ani", ani_file(item("FRAM", item("CIMG", p))));
    CHECK_THROWS_AS(ani::load(tf.path), std::runtime_error);
}

TEST_CASE("an RLE packet that would write past the image is rejected") {
    // 2x2 = 4 pixels, but the packet claims 8.
    Bytes fram = item("CIMG", cimg16(2, 2, 0, rle_run16(8, kRed555)));
    TempFile tf("obm_ani_rleoverrun.ani", ani_file(item("FRAM", fram)));
    CHECK_THROWS_AS(ani::load(tf.path), std::runtime_error);
}

TEST_CASE("an RLE stream that ends before the image is full is rejected") {
    // 4x4 = 16 pixels; the stream supplies 2.
    Bytes fram = item("CIMG", cimg16(4, 4, 0, rle_run16(2, kRed555)));
    TempFile tf("obm_ani_rleshort.ani", ani_file(item("FRAM", fram)));
    CHECK_THROWS_AS(ani::load(tf.path), std::runtime_error);
}

// SECURITY. width and height are two u16s read straight out of the file, and
// their product used to reach `rgba.resize(pixel_count * 4)` BEFORE anything
// compared them against how much compressed data actually follows. 65535x65535
// therefore asked the allocator for 17 GB on a 40-byte file. The allocation
// fails, but it fails as std::bad_alloc / std::length_error -- neither is
// std::runtime_error nor std::out_of_range, so it escapes the contract every
// caller of this module catches on (docs/coding-standards.md §6), and on a box
// with generous overcommit it does not fail at all, it thrashes.
//
// Reverting the bound in parse_cimg turns this case red (bad_alloc, not
// runtime_error), which is what makes it a test rather than a comment.
TEST_CASE("SECURITY: giant CIMG dimensions are rejected against the compressed span") {
    Bytes fram = item("CIMG", cimg16(0xFFFF, 0xFFFF, 0, rle_run16(4, kRed555)));
    TempFile tf("obm_ani_hugedims.ani", ani_file(item("FRAM", fram)));
    CHECK_THROWS_AS(ani::load(tf.path), std::runtime_error);
}

TEST_CASE("SECURITY: a type-11 CIMG cannot claim more pixels than its palette-sized stream") {
    Bytes fram = item("CIMG", cimg8(0xFFFF, 0xFFFF, 0, ramp_palette(), rle_run8(4, 1)));
    TempFile tf("obm_ani_hugedims8.ani", ani_file(item("FRAM", fram)));
    CHECK_THROWS_AS(ani::load(tf.path), std::runtime_error);
}

TEST_CASE("a type-11 CIMG without the full 1024-byte palette is rejected") {
    Bytes pal(512, 0);
    Bytes fram = item("CIMG", cimg8(2, 2, 0, pal, rle_run8(4, 1)));
    TempFile tf("obm_ani_shortpal.ani", ani_file(item("FRAM", fram)));
    CHECK_THROWS_AS(ani::load(tf.path), std::runtime_error);
}

TEST_CASE("a CIMG palette larger than the loader's buffer is rejected, not truncated") {
    Bytes pal(4096, 0);
    Bytes fram = item("CIMG", cimg8(2, 2, 0, pal, rle_run8(4, 1)));
    TempFile tf("obm_ani_bigpal.ani", ani_file(item("FRAM", fram)));
    CHECK_THROWS_AS(ani::load(tf.path), std::runtime_error);
}

TEST_CASE("a missing file throws rather than yielding an empty AniFile") {
    CHECK_THROWS_AS(ani::load(std::filesystem::temp_directory_path() / "obm_ani_absent.ani"),
                    std::runtime_error);
}
