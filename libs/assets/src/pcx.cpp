#include "bomber/assets/pcx.hpp"

#include <stdexcept>
#include <string>
#include <vector>

#include "bomber/assets/binary_reader.hpp"

namespace bomber::assets::pcx {

Image parse(std::span<const std::uint8_t> buf, const char* what) {
    const std::string name = what ? what : "<memory>";
    if (buf.size() < 128) throw std::runtime_error("PCX too small: " + name);
    BinaryReader r(buf);

    std::uint8_t manufacturer = r.u8();
    r.u8();  // version
    std::uint8_t encoding = r.u8();
    std::uint8_t bpp = r.u8();
    int xmin = r.u16(), ymin = r.u16(), xmax = r.u16(), ymax = r.u16();
    r.skip(4 + 48 + 1);  // dpi, ega palette, reserved
    std::uint8_t planes = r.u8();
    int bytes_per_line = r.u16();
    const bool indexed = bpp == 8 && planes == 1;
    const bool rgb24 = bpp == 8 && planes == 3;
    if (manufacturer != 0x0A || encoding != 1 || (!indexed && !rgb24))
        throw std::runtime_error("PCX unsupported variant: " + name);

    Image img;
    img.width = xmax - xmin + 1;
    img.height = ymax - ymin + 1;
    if (img.width <= 0 || img.height <= 0 || bytes_per_line < img.width)
        throw std::runtime_error("PCX bad dimensions: " + name);

    // 8-bit indexed PCX has a VGA palette in its final 769 bytes. Truecolour
    // PCX stores three RLE planes per row and has no trailing palette. Supporting
    // the latter lets the modern renderer load high-colour DATA_HD artwork while
    // retaining exact compatibility with the user's original 8-bit assets.
    std::size_t data_end = buf.size();
    const std::uint8_t* pal = nullptr;
    if (indexed) {
        if (buf.size() < 128 + 769) throw std::runtime_error("PCX palette missing: " + name);
        data_end = buf.size() - 769;
        if (buf[data_end] != 0x0C)
            throw std::runtime_error("PCX palette marker missing: " + name);
        pal = buf.data() + data_end + 1;
    }

    const std::size_t px = static_cast<std::size_t>(img.width) * img.height;
    r.seek(128);
    img.rgba.resize(px * 4);
    if (indexed) {
        // Kept so a caller can key on the palette INDEX (pcx.hpp): the original's
        // transparency is index 0, and several images store real black elsewhere.
        img.indices.resize(px);
        img.palette.assign(std::size_t{256} * 4, 255);
        for (int i = 0; i < 256; ++i) {
            img.palette[static_cast<std::size_t>(i) * 4 + 0] = pal[i * 3 + 0];
            img.palette[static_cast<std::size_t>(i) * 4 + 1] = pal[i * 3 + 1];
            img.palette[static_cast<std::size_t>(i) * 4 + 2] = pal[i * 3 + 2];
        }
    }
    std::vector<std::uint8_t> row(static_cast<std::size_t>(bytes_per_line) * planes);
    for (int y = 0; y < img.height; ++y) {
        int filled = 0;
        const int row_bytes = bytes_per_line * planes;
        while (filled < row_bytes) {
            if (r.pos() >= data_end) throw std::runtime_error("PCX data ended early: " + name);
            std::uint8_t b = r.u8();
            if ((b & 0xC0) == 0xC0) {
                if (r.pos() >= data_end)
                    throw std::runtime_error("PCX RLE data ended early: " + name);
                int count = b & 0x3F;
                std::uint8_t v = r.u8();
                for (int i = 0; i < count && filled < row_bytes; ++i) row[filled++] = v;
            } else {
                row[filled++] = b;
            }
        }
        for (int x = 0; x < img.width; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * img.width + x;
            const std::size_t o = i * 4;
            if (indexed) {
                std::uint8_t idx = row[x];
                img.indices[i] = idx;
                img.rgba[o + 0] = pal[idx * 3 + 0];
                img.rgba[o + 1] = pal[idx * 3 + 1];
                img.rgba[o + 2] = pal[idx * 3 + 2];
            } else {
                img.rgba[o + 0] = row[x];
                img.rgba[o + 1] = row[bytes_per_line + x];
                img.rgba[o + 2] = row[bytes_per_line * 2 + x];
            }
            img.rgba[o + 3] = 255;
        }
    }
    return img;
}

Image load(const std::filesystem::path& path) {
    auto buf = read_file(path);
    const std::string where = path.string();
    return parse(std::span<const std::uint8_t>(buf.data(), buf.size()), where.c_str());
}

}  // namespace bomber::assets::pcx
