#include "bomber/assets/bmfont.hpp"

#include <stdexcept>

#include "bomber/assets/binary_reader.hpp"

namespace bomber::assets::bmfont {

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

    // Glyph table: glyph_count * { u32 width, u32 bitmap_offset }. Offsets are
    // measured from the START OF THE BITMAP BLOCK, which begins right after the
    // table (sub_431BBC computes the block size from the LAST entry the same
    // way). BinaryReader validates every table read.
    struct Entry {
        std::uint32_t width;
        std::uint32_t offset;
    };
    std::vector<Entry> table(glyph_count);
    for (auto& e : table) {
        e.width = r.u32();
        e.offset = r.u32();
    }
    const std::size_t bitmap_base = r.pos();  // 20 + 8*count

    // Rasterize each glyph: `height` rows of `(width+7)/8` bytes, 1 bit/pixel,
    // MSB-first. A set bit is inked (255), clear is transparent (0). Bounds are
    // checked against the buffer so a lying offset/width throws rather than
    // reading OOB (the shipped fonts are self-consistent).
    for (std::size_t c = 0; c < glyph_count; ++c) {
        const int w = static_cast<int>(table[c].width);
        Glyph& g = font.glyphs[c];
        g.width = w;
        if (w <= 0) continue;  // zero-width glyph: advance only, no pixels
        const std::size_t stride = (static_cast<std::size_t>(w) + 7) / 8;
        const std::size_t need = stride * glyph_height;
        const std::size_t start = bitmap_base + table[c].offset;
        if (start + need > data.size())
            throw std::runtime_error("bmfont: glyph bitmap past end");
        g.pixels.assign(static_cast<std::size_t>(w) * glyph_height, 0);
        for (std::size_t row = 0; row < glyph_height; ++row) {
            const std::uint8_t* rowp = data.data() + start + row * stride;
            for (int x = 0; x < w; ++x) {
                const std::uint8_t byte = rowp[static_cast<std::size_t>(x) >> 3];
                const bool set = (byte >> (7 - (x & 7))) & 1;
                if (set) g.pixels[row * static_cast<std::size_t>(w) + x] = 255;
            }
        }
    }
    return font;
}

Font load(const std::filesystem::path& path) {
    auto buf = read_file(path);
    return parse(std::span<const std::uint8_t>(buf.data(), buf.size()));
}

}  // namespace bomber::assets::bmfont
