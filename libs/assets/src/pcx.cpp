#include "bomber/assets/pcx.hpp"

#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "bomber/assets/binary_reader.hpp"

namespace bomber::assets::pcx {
namespace {

// As much of the fixed 128-byte header as the decoder uses.
struct Header {
    int width = 0;
    int height = 0;
    int bytes_per_line = 0;  // the scanline stride, per plane
    int planes = 1;
    bool indexed = false;  // 8bpp, one plane: the original 1997 assets
};

// The RLE byte stream plus the offset it must not read past, so the row decoder
// takes one argument for its source rather than three.
struct RleSource {
    BinaryReader& r;
    std::size_t data_end = 0;
    const std::string& name;
};

// One decoded scanline plus what it needs to become pixels. `pal` is the file's
// 768-byte VGA table, empty for the truecolour path.
struct Scanline {
    std::span<const std::uint8_t> row;
    std::span<const std::uint8_t> pal;
    int bytes_per_line = 0;
};

Header read_header(BinaryReader& r, const std::string& name) {
    const std::uint8_t manufacturer = r.u8();
    r.u8();  // version
    const std::uint8_t encoding = r.u8();
    const std::uint8_t bpp = r.u8();
    const int xmin = r.u16(), ymin = r.u16(), xmax = r.u16(), ymax = r.u16();
    r.skip(4 + 48 + 1);  // dpi, ega palette, reserved

    Header h;
    h.planes = r.u8();
    h.bytes_per_line = r.u16();
    h.indexed = bpp == 8 && h.planes == 1;
    const bool rgb24 = bpp == 8 && h.planes == 3;
    if (manufacturer != 0x0A || encoding != 1 || (!h.indexed && !rgb24))
        throw std::runtime_error("PCX unsupported variant: " + name);

    h.width = xmax - xmin + 1;
    h.height = ymax - ymin + 1;
    if (h.width <= 0 || h.height <= 0 || h.bytes_per_line < h.width)
        throw std::runtime_error("PCX bad dimensions: " + name);
    return h;
}

// One scanline of PCX RLE: a byte with its top two bits set carries a 6-bit
// repeat count and is followed by the value; anything else is a literal.
void decode_row(RleSource& src, std::span<std::uint8_t> row) {
    std::size_t filled = 0;
    while (filled < row.size()) {
        if (src.r.pos() >= src.data_end)
            throw std::runtime_error("PCX data ended early: " + src.name);
        const std::uint8_t b = src.r.u8();
        if ((b & 0xC0) != 0xC0) {
            row[filled++] = b;
            continue;
        }
        if (src.r.pos() >= src.data_end)
            throw std::runtime_error("PCX RLE data ended early: " + src.name);
        const int count = b & 0x3F;
        const std::uint8_t v = src.r.u8();
        for (int i = 0; i < count && filled < row.size(); ++i) row[filled++] = v;
    }
}

// Kept so a caller can key on the palette INDEX (pcx.hpp): the original's
// transparency is index 0, and several images store real black elsewhere.
void store_palette(std::span<const std::uint8_t> pal, Image& img) {
    img.palette.assign(std::size_t{256} * 4, 255);
    for (std::size_t i = 0; i < 256; ++i) {
        img.palette[i * 4 + 0] = pal[i * 3 + 0];
        img.palette[i * 4 + 1] = pal[i * 3 + 1];
        img.palette[i * 4 + 2] = pal[i * 3 + 2];
    }
}

void emit_indexed_row(const Scanline& line, int y, Image& img) {
    for (int x = 0; x < img.width; ++x) {
        const std::size_t i = static_cast<std::size_t>(y) * img.width + x;
        const std::uint8_t idx = line.row[static_cast<std::size_t>(x)];
        img.indices[i] = idx;
        img.rgba[i * 4 + 0] = line.pal[idx * 3 + 0];
        img.rgba[i * 4 + 1] = line.pal[idx * 3 + 1];
        img.rgba[i * 4 + 2] = line.pal[idx * 3 + 2];
        img.rgba[i * 4 + 3] = 255;
    }
}

// Truecolour PCX stores the three channels as consecutive planes within one
// scanline, so the red byte of pixel x is at x and its blue at 2*stride + x.
void emit_rgb_row(const Scanline& line, int y, Image& img) {
    const std::size_t stride = static_cast<std::size_t>(line.bytes_per_line);
    for (int x = 0; x < img.width; ++x) {
        const std::size_t i = static_cast<std::size_t>(y) * img.width + x;
        const std::size_t sx = static_cast<std::size_t>(x);
        img.rgba[i * 4 + 0] = line.row[sx];
        img.rgba[i * 4 + 1] = line.row[stride + sx];
        img.rgba[i * 4 + 2] = line.row[stride * 2 + sx];
        img.rgba[i * 4 + 3] = 255;
    }
}

void emit_row(const Scanline& line, int y, Image& img) {
    if (line.pal.empty()) {
        emit_rgb_row(line, y, img);
        return;
    }
    emit_indexed_row(line, y, img);
}

// SECURITY. width, height and bytes_per_line are u16s straight out of the
// header, so their products reach ~17 GB on a 900-byte file — and the resize
// used to happen BEFORE anything compared them against how much data actually
// follows. The failure then escaped as std::bad_alloc, which is neither
// std::runtime_error nor std::out_of_range and so slips through the contract
// every caller of this module catches on (docs/coding-standards.md §6); with
// generous overcommit it does not fail at all, it thrashes.
//
// A run packet spends two input bytes for at most 63 output bytes, so
// ceil(avail/2) * 63 is an upper bound on the decoded size that no valid file
// can exceed — the check rejects only files that could never have decoded.
void check_decodable(std::size_t decoded_bytes, std::size_t avail, const std::string& name) {
    if (decoded_bytes > (avail / 2 + 1) * 63)
        throw std::runtime_error("PCX dimensions exceed the data section: " + name);
}

}  // namespace

Image parse(std::span<const std::uint8_t> buf, const char* what) {
    const std::string name = what ? what : "<memory>";
    if (buf.size() < 128) throw std::runtime_error("PCX too small: " + name);
    BinaryReader r(buf);
    const Header hdr = read_header(r, name);

    // 8-bit indexed PCX has a VGA palette in its final 769 bytes. Truecolour
    // PCX stores three RLE planes per row and has no trailing palette. Supporting
    // the latter lets the modern renderer load high-colour DATA_HD artwork while
    // retaining exact compatibility with the user's original 8-bit assets.
    std::size_t data_end = buf.size();
    std::span<const std::uint8_t> pal;
    if (hdr.indexed) {
        if (buf.size() < 128 + 769) throw std::runtime_error("PCX palette missing: " + name);
        data_end = buf.size() - 769;
        if (buf[data_end] != 0x0C)
            throw std::runtime_error("PCX palette marker missing: " + name);
        pal = buf.subspan(data_end + 1, 768);
    }

    const std::size_t stride = static_cast<std::size_t>(hdr.bytes_per_line) * hdr.planes;
    check_decodable(stride * static_cast<std::size_t>(hdr.height), data_end - 128, name);

    Image img;
    img.width = hdr.width;
    img.height = hdr.height;
    const std::size_t px = static_cast<std::size_t>(hdr.width) * hdr.height;
    img.rgba.resize(px * 4);
    if (hdr.indexed) {
        img.indices.resize(px);
        store_palette(pal, img);
    }

    r.seek(128);
    RleSource src{r, data_end, name};
    std::vector<std::uint8_t> row(stride);
    for (int y = 0; y < hdr.height; ++y) {
        decode_row(src, row);
        emit_row(Scanline{row, pal, hdr.bytes_per_line}, y, img);
    }
    return img;
}

Image load(const std::filesystem::path& path) {
    auto buf = read_file(path);
    const std::string where = path.string();
    return parse(std::span<const std::uint8_t>(buf.data(), buf.size()), where.c_str());
}

}  // namespace bomber::assets::pcx
