#include "bomber/assets/pcx.hpp"

#include <stdexcept>
#include <vector>

#include "bomber/assets/binary_reader.hpp"

namespace bomber::assets::pcx {

Image load(const std::filesystem::path& path) {
    auto buf = read_file(path);
    if (buf.size() < 128 + 769) throw std::runtime_error("PCX too small: " + path.string());
    BinaryReader r(buf);

    std::uint8_t manufacturer = r.u8();
    r.u8();  // version
    std::uint8_t encoding = r.u8();
    std::uint8_t bpp = r.u8();
    int xmin = r.u16(), ymin = r.u16(), xmax = r.u16(), ymax = r.u16();
    r.skip(4 + 48 + 1);  // dpi, ega palette, reserved
    std::uint8_t planes = r.u8();
    int bytes_per_line = r.u16();
    if (manufacturer != 0x0A || encoding != 1 || bpp != 8 || planes != 1)
        throw std::runtime_error("PCX unsupported variant: " + path.string());

    Image img;
    img.width = xmax - xmin + 1;
    img.height = ymax - ymin + 1;
    if (img.width <= 0 || img.height <= 0)
        throw std::runtime_error("PCX bad dimensions: " + path.string());

    // VGA palette: last 769 bytes = 0x0C marker + 256*3.
    std::size_t pal_off = buf.size() - 769;
    if (buf[pal_off] != 0x0C)
        throw std::runtime_error("PCX palette marker missing: " + path.string());
    const std::uint8_t* pal = buf.data() + pal_off + 1;

    r.seek(128);
    img.rgba.resize(static_cast<std::size_t>(img.width) * img.height * 4);
    std::vector<std::uint8_t> row(bytes_per_line);
    for (int y = 0; y < img.height; ++y) {
        int filled = 0;
        while (filled < bytes_per_line) {
            if (r.pos() >= pal_off)
                throw std::runtime_error("PCX data ended early: " + path.string());
            std::uint8_t b = r.u8();
            if ((b & 0xC0) == 0xC0) {
                int count = b & 0x3F;
                std::uint8_t v = r.u8();
                for (int i = 0; i < count && filled < bytes_per_line; ++i) row[filled++] = v;
            } else {
                row[filled++] = b;
            }
        }
        for (int x = 0; x < img.width; ++x) {
            std::uint8_t idx = row[x];
            std::size_t o = (static_cast<std::size_t>(y) * img.width + x) * 4;
            img.rgba[o + 0] = pal[idx * 3 + 0];
            img.rgba[o + 1] = pal[idx * 3 + 1];
            img.rgba[o + 2] = pal[idx * 3 + 2];
            img.rgba[o + 3] = 255;
        }
    }
    return img;
}

}  // namespace bomber::assets::pcx
