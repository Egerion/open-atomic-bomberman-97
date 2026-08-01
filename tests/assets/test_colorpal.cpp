// COLOR.PAL in-match palette snap (colorpal.hpp). Verifies the exact
// RGB555 -> reverse-LUT -> master*4 chain and the entry-0-black fixup against
// a SYNTHETIC COLOR.PAL, so the engine cases run without the copyrighted
// install; the final case additionally pins the SHIPPED file when an install
// is present, and SKIPs when it is not.
//
// The real FIELD1 "Classic Green Acres" dither mapping this reproduces
// ((23,27,139)->(20,40,108), (19,143,19)->(4,132,0)) was validated pixel-exact
// against a live capture of the original — see docs/re/facts.md "In-match
// colour quantization". Here we pin the ENGINE (indexing, 6-bit x4, entry 0).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

#include "bomber/assets/colorpal.hpp"
#include "bomber/assets/install.hpp"

using namespace bomber::assets;

namespace {

// Build a 33536-byte COLOR.PAL: [768 master (6-bit RGB)] + [32768 LUT (RGB555
// -> index)]. master[k] = (k, k/2, k/4) 6-bit; every RGB555 slot -> index 5
// except a couple we point elsewhere to prove the indexing.
std::filesystem::path write_synth_pal() {
    std::vector<std::uint8_t> buf(768 + 32768, 0);
    for (int k = 0; k < 256; ++k) {
        buf[static_cast<std::size_t>(k) * 3 + 0] = static_cast<std::uint8_t>(k & 63);
        buf[static_cast<std::size_t>(k) * 3 + 1] = static_cast<std::uint8_t>((k / 2) & 63);
        buf[static_cast<std::size_t>(k) * 3 + 2] = static_cast<std::uint8_t>((k / 4) & 63);
    }
    for (std::size_t i = 0; i < 32768; ++i) buf[768 + i] = 5;  // default -> master[5]
    // RGB555 for input (8,16,24): r5=1,g5=2,b5=3 -> off = 1<<10 | 2<<5 | 3 = 1091.
    buf[768 + 1091] = 7;   // point this slot at master[7]
    // RGB555 for input (0,0,0) -> off 0 -> keep index 0 (tests entry-0 fixup).
    buf[768 + 0] = 0;

    auto p = std::filesystem::temp_directory_path() / "bomber_synth_color.pal";
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    return p;
}

}  // namespace

TEST_CASE("colorpal snap follows RGB555 -> LUT -> master*4") {
    const auto path = write_synth_pal();
    auto pal = colorpal::Palette::load(path);
    REQUIRE(pal.ok());

    // (8,16,24) truncates to RGB555 (1,2,3) -> off 1091 -> LUT index 7 ->
    // master[7] = (7, 3, 1) 6-bit, displayed *4 = (28, 12, 4).
    std::uint8_t r = 8, g = 16, b = 24;
    pal.snap(r, g, b);
    CHECK(r == 28);
    CHECK(g == 12);
    CHECK(b == 4);

    // A colour whose slot maps to the default index 5 -> master[5] = (5,2,1)*4
    // = (20, 8, 4). Input (80,80,80): r5=g5=b5=10 -> off != 1091/0 -> index 5.
    std::uint8_t r2 = 80, g2 = 80, b2 = 80;
    pal.snap(r2, g2, b2);
    CHECK(r2 == 20);
    CHECK(g2 == 8);
    CHECK(b2 == 4);

    std::filesystem::remove(path);
}

TEST_CASE("colorpal entry 0 is forced black, not the file's white sentinel") {
    // Write a pal whose master[0] is the (255,255,255)-style sentinel (63,63,63
    // 6-bit) and whose RGB555(0,0,0) slot -> index 0.
    std::vector<std::uint8_t> buf(768 + 32768, 5);
    buf[0] = buf[1] = buf[2] = 63;  // master[0] = max 6-bit (would be 252 *4)
    for (std::size_t i = 0; i < 32768; ++i) buf[768 + i] = 0;  // everything -> index 0
    auto path = std::filesystem::temp_directory_path() / "bomber_synth_color0.pal";
    {
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(buf.data()),
                static_cast<std::streamsize>(buf.size()));
    }
    auto pal = colorpal::Palette::load(path);
    REQUIRE(pal.ok());
    std::uint8_t r = 200, g = 100, b = 50;
    pal.snap(r, g, b);  // -> index 0 -> forced black
    CHECK(r == 0);
    CHECK(g == 0);
    CHECK(b == 0);
    std::filesystem::remove(path);
}

TEST_CASE("colorpal remap leaves transparent pixels untouched") {
    const auto path = write_synth_pal();
    auto pal = colorpal::Palette::load(path);
    Image img;
    img.width = 2;
    img.height = 1;
    img.rgba = {8, 16, 24, 255, /*opaque -> snapped*/
                8, 16, 24, 0 /*transparent -> untouched*/};
    pal.remap(img);
    CHECK(img.rgba[0] == 28);  // opaque snapped to (28,12,4)
    CHECK(img.rgba[1] == 12);
    CHECK(img.rgba[2] == 4);
    CHECK(img.rgba[4] == 8);  // transparent kept raw
    CHECK(img.rgba[5] == 16);
    CHECK(img.rgba[6] == 24);
    std::filesystem::remove(path);
}

TEST_CASE("a default (unloaded) palette is an inert no-op") {
    colorpal::Palette pal;
    CHECK_FALSE(pal.ok());
    Image img;
    img.width = 1;
    img.height = 1;
    img.rgba = {8, 16, 24, 255};
    pal.remap(img);  // no-op
    CHECK(img.rgba[0] == 8);
    CHECK(img.rgba[1] == 16);
    CHECK(img.rgba[2] == 24);
}

// The cases above prove the ENGINE on synthetic data; this one pins the port
// against the SHIPPED COLOR.PAL when an install is present (located the same
// way the game locates it) and SKIPs otherwise, because two of colorpal.cpp's
// load-bearing claims are claims about the FILE, not the code:
//
//  * "a valid 6-bit entry never exceeds 63". The original's upload
//    (sub_443608) stores `4 * value` into a BYTE — it wraps mod 256 — while
//    the port clamps to 255; the two agree only while no reachable entry
//    exceeds 63. Measured 2026-08-01 (docs/re/facts.md, the palette-pass
//    audit): only entry 0 does, and that is the white sentinel BOTH sides
//    force to black before any multiply, so the divergence is unreachable.
//
//  * the 6-bit DAC ceiling. Every displayable channel is 4*(0..63): nothing
//    the snap emits may exceed 252 or leave the multiple-of-4 grid. This is
//    the "darker and deeper" palette pass — a decode that reached 255 or
//    landed off-grid would be the pre-2026-07-13 raw look coming back.
TEST_CASE("shipped COLOR.PAL: 6-bit master, DAC ceiling, facts.md examples") {
    namespace fs = std::filesystem;
    const fs::path dir = default_game_dir();
    const fs::path pal_path = dir.empty() ? fs::path{} : dir / "COLOR.PAL";
    if (pal_path.empty() || !fs::exists(pal_path)) {
        MESSAGE("SKIP: no original install found (COLOR.PAL absent)");
        return;
    }

    // Raw-file scan: entries 1..255 are 6-bit; entry 0 alone may exceed 63.
    std::ifstream f(pal_path, std::ios::binary);
    std::vector<char> raw(768);
    f.read(raw.data(), 768);
    REQUIRE(f.gcount() == 768);
    for (std::size_t i = 3; i < 768; ++i) CHECK(static_cast<unsigned char>(raw[i]) <= 63);

    auto pal = colorpal::Palette::load(pal_path);
    REQUIRE(pal.ok());

    // The three worked examples of docs/re/facts.md "In-match colour
    // quantization", validated there pixel-exact against a live capture of the
    // running original. Master bytes -> displayed = *4:
    //   FIELD1 blue dither  (23,27,139)   -> idx 57  = (5,10,27)*4  = (20,40,108)
    //   FIELD1 green dither (19,143,19)   -> idx 128 = (1,33,0)*4   = (4,132,0)
    //   border brick grey   (126,126,126) -> idx 60  = (27,29,32)*4 = (108,116,128)
    auto check_snap = [&](std::uint8_t r, std::uint8_t g, std::uint8_t b, int er, int eg, int eb) {
        pal.snap(r, g, b);
        CHECK(r == er);
        CHECK(g == eg);
        CHECK(b == eb);
    };
    check_snap(23, 27, 139, 20, 40, 108);
    check_snap(19, 143, 19, 4, 132, 0);
    check_snap(126, 126, 126, 108, 116, 128);

    // Ceiling sweep: stepping every channel by 8 visits each of the 32768
    // RGB555 LUT slots exactly once (index_of truncates with >>3), i.e. every
    // colour the snap can ever produce. All outputs on the 4-multiple grid,
    // none above 252.
    for (int r = 0; r < 256; r += 8)
        for (int g = 0; g < 256; g += 8)
            for (int b = 0; b < 256; b += 8) {
                std::uint8_t sr = static_cast<std::uint8_t>(r);
                std::uint8_t sg = static_cast<std::uint8_t>(g);
                std::uint8_t sb = static_cast<std::uint8_t>(b);
                pal.snap(sr, sg, sb);
                const bool on_grid = sr <= 252 && sg <= 252 && sb <= 252 && sr % 4 == 0 &&
                                     sg % 4 == 0 && sb % 4 == 0;
                if (!on_grid) {  // one CHECK per failure, not 32768 assertions
                    CHECK(on_grid);
                    return;
                }
            }
    CHECK(true);  // sweep completed clean
}
