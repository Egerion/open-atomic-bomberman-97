#include "bomber/assets/bmfont.hpp"

#include <cstddef>
#include <stdexcept>
#include <vector>

#include "bomber/assets/binary_reader.hpp"

namespace bomber::assets::bmfont {
namespace {

// glyph_count * { u32 width, u32 bitmap_offset }. Offsets are measured from the
// START OF THE BITMAP BLOCK, which begins right after the table (sub_431BBC
// computes the block size from the LAST entry the same way).
struct Entry {
    std::uint32_t width = 0;
    std::uint32_t offset = 0;
};

// `height` rows of `(width+7)/8` bytes, 1 bit/pixel, MSB-first. A set bit is
// inked (255), clear is transparent (0). The caller has already bounds-checked
// `start + stride * height` against the buffer, so a lying offset or width
// throws rather than reading OOB (the shipped fonts are self-consistent).
void rasterize(std::span<const std::uint8_t> bitmap, int width, Glyph& g) {
    const std::size_t w = static_cast<std::size_t>(width);
    const std::size_t stride = (w + 7) / 8;
    const std::size_t rows = bitmap.size() / stride;
    g.pixels.assign(w * rows, 0);
    for (std::size_t row = 0; row < rows; ++row) {
        for (std::size_t x = 0; x < w; ++x) {
            const std::uint8_t byte = bitmap[row * stride + (x >> 3)];
            if ((byte >> (7 - (x & 7))) & 1) g.pixels[row * w + x] = 255;
        }
    }
}

}  // namespace

Font parse(std::span<const std::uint8_t> data) {
    BinaryReader r(data);

    // Header (20 bytes), mirroring the loader sub_431BBC: it reads 20 bytes into
    // a 5-dword header (count, height, spacing, magic0, magic1) before the glyph
    // table. The two magics are file-integrity hashes; the renderer ignores them.
    std::uint32_t glyph_count = r.u32();
    std::uint32_t glyph_height = r.u32();
    std::uint32_t spacing = r.u32();
    r.u32();  // magic0 (unused)
    r.u32();  // magic1 (unused)

    // A font with no glyphs or no height is unusable; the shipped files are
    // 128/256 glyphs at 16 px. Guard so a corrupt header can't wedge later math.
    if (glyph_count == 0 || glyph_count > 0x10000 || glyph_height == 0 ||
        glyph_height > 0x1000)
        throw std::runtime_error("bmfont: implausible header");

    Font font;
    font.glyph_height = static_cast<int>(glyph_height);
    font.spacing = static_cast<int>(spacing);
    font.glyphs.resize(glyph_count);

    // BinaryReader validates every table read.
    std::vector<Entry> table(glyph_count);
    for (Entry& e : table) {
        e.width = r.u32();
        e.offset = r.u32();
    }
    const std::size_t bitmap_base = r.pos();  // 20 + 8*count

    for (std::size_t c = 0; c < glyph_count; ++c) {
        const int w = static_cast<int>(table[c].width);
        Glyph& g = font.glyphs[c];
        g.width = w;
        if (w <= 0) continue;  // zero-width glyph: advance only, no pixels
        const std::size_t stride = (static_cast<std::size_t>(w) + 7) / 8;
        const std::size_t need = stride * glyph_height;
        const std::size_t start = bitmap_base + table[c].offset;
        // `start` cannot wrap: bitmap_base is a validated file offset and
        // `offset` is a u32, both widened to size_t.
        if (start > data.size() || need > data.size() - start)
            throw std::runtime_error("bmfont: glyph bitmap past end");
        rasterize(data.subspan(start, need), w, g);
    }
    return font;
}

Font load(const std::filesystem::path& path) {
    auto buf = read_file(path);
    return parse(std::span<const std::uint8_t>(buf.data(), buf.size()));
}

}  // namespace bomber::assets::bmfont
