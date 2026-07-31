#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#include "bomber/assets/image.hpp"

namespace bomber::assets::bmfont {

// Loader for the original's bitmap fonts, `FONT<n>.FON` (install root) — the
// fonts the engine draws every string with, pinned to FONT6 by the graphics init
// (`sub_431E9C(6)`). Layout, offsets and bit order: docs/formats/fon.md.
//
// The one rule worth stating here: a character's glyph is indexed DIRECTLY by
// its byte value, so glyph index == char code and the file covers 0..count-1
// (128 for FONT6, 256 for FONT1). Untrusted input, bounds-checked throughout.

// One glyph: advance width plus rasterized rows, one byte per pixel (0 or 255),
// row-major, ready for the presentation layer to expand into RGBA. A blank glyph
// has width > 0 and all-zero pixels.
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
    // sub_432120: `spacing + glyph[c].width`. An unknown code contributes
    // nothing, matching the original's `if (c < count)` guard.
    int advance(int code) const {
        return has(code) ? glyphs[static_cast<std::size_t>(code)].width + spacing : 0;
    }
    int measure(std::span<const char> text) const {
        int w = 0;
        for (char ch : text) w += advance(static_cast<unsigned char>(ch));
        return w;
    }
};

// Throws std::out_of_range / std::runtime_error on a truncated or
// self-inconsistent file (a glyph whose bitmap runs past the data).
Font parse(std::span<const std::uint8_t> data);
Font load(const std::filesystem::path& path);

}  // namespace bomber::assets::bmfont
