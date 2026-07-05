#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#include "bomber/assets/image.hpp"

namespace bomber::assets::bmfont {

// Loader for the original's bitmap fonts, `FONT<n>.FON` (install root). These
// are the fonts the engine draws every text string with — the front-end .BM
// help/credits screens (sub_41302D) render through the active font, which the
// graphics init pins to FONT6 (`sub_431E9C(6)`, BM95.EXE @ 0x417600). See
// docs/formats/fon.md.
//
// Format (little-endian, confirmed from the loader sub_431BBC @ 0x431BBC and
// the metric routines sub_432120/sub_432164):
//   Header (20 bytes): u32 glyph_count, u32 glyph_height, u32 spacing,
//                      u32 magic0, u32 magic1  (the two magics are integrity
//                      hashes the renderer never needs).
//   Glyph table: glyph_count * { u32 width, u32 bitmap_offset }.
//   Bitmaps: for each glyph, `glyph_height` rows of `(width+7)/8` bytes, 1 bit
//            per pixel, MSB-first within each byte; a set bit is an inked pixel.
// The glyph for a character is indexed DIRECTLY by its byte value (`8 * c` into
// the table), so glyph index == char code and the file covers codes
// 0..glyph_count-1 (128 for FONT6, 256 for FONT1). SDL-free; treats the 1997
// file as untrusted input (every field bounds-checked via BinaryReader).

// One glyph: its advance width and the rasterized rows. `pixels` is one byte
// per pixel (0 = transparent, 255 = inked), row-major, width*height, ready to be
// expanded into an RGBA texture by the presentation layer. Blank glyphs (space,
// control codes) simply have width>0 and all-zero pixels.
struct Glyph {
    int width = 0;                     // advance width in pixels (table column 0)
    std::vector<std::uint8_t> pixels;  // width * height coverage, 0 or 255
};

struct Font {
    int glyph_height = 0;   // common cell height (header)
    int spacing = 0;        // extra advance added between glyphs (header + sub_432120)
    std::vector<Glyph> glyphs;  // indexed by char code (0..glyph_count-1)

    bool has(int code) const {
        return code >= 0 && code < static_cast<int>(glyphs.size());
    }
    // Advance width for one character = glyph width + the font's inter-char
    // spacing (sub_432120: `spacing + glyph[c].width`). Unknown codes contribute
    // nothing, matching the original's `if (c < count)` guard.
    int advance(int code) const {
        return has(code) ? glyphs[static_cast<std::size_t>(code)].width + spacing : 0;
    }
    // Total pixel width a NUL-terminated run would occupy (sub_432120).
    int measure(std::span<const char> text) const {
        int w = 0;
        for (char ch : text) w += advance(static_cast<unsigned char>(ch));
        return w;
    }
};

// Parse an in-memory `.FON`. Throws std::out_of_range / std::runtime_error on a
// truncated or self-inconsistent file (a glyph whose bitmap runs past the data).
Font parse(std::span<const std::uint8_t> data);

// Convenience loader: read the file at `path` and parse it. Throws
// std::runtime_error when the file cannot be opened (assets read_file).
Font load(const std::filesystem::path& path);

}  // namespace bomber::assets::bmfont
