#include "bomber/assets/ani.hpp"

#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <stdexcept>

#include "bomber/assets/binary_reader.hpp"

namespace bomber::assets::ani {
namespace {

struct Item {
    std::array<char, 4> tag{};
    std::uint32_t length = 0;
    std::uint16_t id = 0;
    std::size_t body = 0;  // absolute offset of payload start
    std::size_t end = 0;   // absolute offset of payload end

    bool is(const char* t) const { return std::memcmp(tag.data(), t, 4) == 0; }
};

Item read_item(BinaryReader& r) {
    Item it;
    const auto tag = r.bytes(4);
    std::memcpy(it.tag.data(), tag.data(), 4);
    it.length = r.u32();
    it.id = r.u16();
    it.body = r.pos();
    it.end = it.body + it.length;
    if (it.end > r.size()) throw std::runtime_error("ANI: item overruns file");
    return it;
}

[[noreturn]] void fail(const std::filesystem::path& p, const std::string& msg) {
    throw std::runtime_error("ANI '" + p.string() + "': " + msg);
}

// The `else` is load-bearing rather than redundant: `if constexpr` only discards
// the branch it is attached to, and without it both reads would be instantiated
// for both pixel widths.
template <typename Pixel>
Pixel read_pixel(BinaryReader& r) {
    if constexpr (sizeof(Pixel) == 2) {
        return static_cast<Pixel>(r.u16());
    } else {
        return static_cast<Pixel>(r.u8());
    }
}

// TGA type-10 style RLE. Pixel is u16 (type 4) or u8 (type 11). `pixel_count`
// must already have been checked against the compressed span — see
// max_decodable_pixels; the reserve below is otherwise file-controlled.
template <typename Pixel>
std::vector<Pixel> decode_rle(BinaryReader& r, std::size_t data_end, std::size_t pixel_count) {
    std::vector<Pixel> out;
    out.reserve(pixel_count);
    while (out.size() < pixel_count) {
        if (r.pos() >= data_end) throw std::runtime_error("RLE: data ended early");
        const std::uint8_t header = r.u8();
        const std::size_t count = (header & 0x7F) + 1;
        if (count > pixel_count - out.size())
            throw std::runtime_error("RLE: packet overruns image");
        if (header & 0x80) {
            out.insert(out.end(), count, read_pixel<Pixel>(r));
            continue;
        }
        for (std::size_t i = 0; i < count; ++i) out.push_back(read_pixel<Pixel>(r));
    }
    return out;
}

constexpr std::uint8_t expand5(unsigned v) {
    return static_cast<std::uint8_t>((v << 3) | (v >> 2));
}

// Where a CIMG lives. Both members exist only for bounds and error text, so
// they travel as one argument.
struct CimgContext {
    const std::filesystem::path& path;
    const Item& item;
};

// The CIMG header fields that steer decoding without surviving into Frame.
struct CimgData {
    std::size_t palette_size = 0;
    std::array<std::uint8_t, 1024> palette{};
    std::size_t data_end = 0;
    std::uint32_t uncompressed_size = 0;
};

// What a decoder needs beyond the reader; `palette` points into the CimgData
// the caller keeps alive for the duration.
struct CimgDecode {
    const std::filesystem::path& path;
    std::size_t data_end = 0;
    std::size_t pixel_count = 0;
    std::uint32_t uncompressed_size = 0;
    std::span<const std::uint8_t> palette;
};

CimgData read_cimg_header(BinaryReader& r, const CimgContext& ctx, Frame& f) {
    f.cimg_type = r.u16();
    r.u16();  // unknown
    const std::uint32_t additional_size = r.u32();
    r.u32();  // unknown
    f.image.width = r.u16();
    f.image.height = r.u16();
    f.hotspot_x = r.u16();
    f.hotspot_y = r.u16();
    f.key_color = r.u16();
    r.u16();  // unknown

    CimgData d;
    if (additional_size >= 32) {
        d.palette_size = additional_size - 32;
        r.u32();  // unknown
        r.u32();  // unknown
        if (d.palette_size > d.palette.size()) fail(ctx.path, "CIMG palette too large");
        const auto pal = r.bytes(d.palette_size);
        std::memcpy(d.palette.data(), pal.data(), d.palette_size);
    } else if (additional_size != 24) {
        fail(ctx.path, "CIMG unexpected additional_size " + std::to_string(additional_size));
    }

    r.u16();  // unknown
    r.u16();  // unknown
    const std::uint32_t compressed_size = r.u32();
    d.uncompressed_size = r.u32();
    if (compressed_size < 12) fail(ctx.path, "CIMG compressed_size < 12");
    d.data_end = r.pos() + (compressed_size - 12);
    if (d.data_end > ctx.item.end) fail(ctx.path, "CIMG data overruns item");
    return d;
}

// SECURITY. width and height are two u16s read straight out of the file, so
// their product alone asks the allocator for 17 GB — and both the rgba resize
// and decode_rle's reserve used to happen before anything compared them against
// how much compressed data actually follows. The failure then escaped as
// std::bad_alloc, which is neither std::runtime_error nor std::out_of_range and
// so slips through the contract every caller of this module catches on
// (docs/coding-standards.md §6); with generous overcommit it does not fail at
// all, it thrashes.
//
// An RLE packet is one header byte plus at least one pixel and yields at most
// 128 pixels, so the compressed span is a hard cap on how many pixels can
// legitimately follow. This rejects only images that could never have decoded.
std::size_t max_decodable_pixels(std::size_t avail, std::size_t pixel_bytes) {
    return (avail / (1 + pixel_bytes) + 1) * 128;
}

void decode_true_colour(BinaryReader& r, const CimgDecode& d, Frame& f) {
    if (d.uncompressed_size != d.pixel_count * 2) fail(d.path, "CIMG type 4 size mismatch");
    const auto px = decode_rle<std::uint16_t>(r, d.data_end, d.pixel_count);
    for (std::size_t i = 0; i < d.pixel_count; ++i) {
        const std::uint16_t v = px[i];
        f.image.rgba[i * 4 + 0] = expand5((v >> 10) & 31);
        f.image.rgba[i * 4 + 1] = expand5((v >> 5) & 31);
        f.image.rgba[i * 4 + 2] = expand5(v & 31);
        f.image.rgba[i * 4 + 3] = (v == f.key_color) ? 0 : 255;
    }
}

void decode_paletted(BinaryReader& r, const CimgDecode& d, Frame& f) {
    if (d.palette.size() != 1024) fail(d.path, "CIMG type 11 without 1024-byte palette");
    if (d.uncompressed_size != d.pixel_count) fail(d.path, "CIMG type 11 size mismatch");
    const auto px = decode_rle<std::uint8_t>(r, d.data_end, d.pixel_count);
    for (std::size_t i = 0; i < d.pixel_count; ++i) {
        const std::uint8_t idx = px[i];
        f.image.rgba[i * 4 + 0] = d.palette[idx * 4 + 0];
        f.image.rgba[i * 4 + 1] = d.palette[idx * 4 + 1];
        f.image.rgba[i * 4 + 2] = d.palette[idx * 4 + 2];
        f.image.rgba[i * 4 + 3] = (idx == (f.key_color & 0xFF)) ? 0 : 255;
    }
    // Retain the source indices + palette so player recolour can apply a .RMP
    // remap at index level, exactly like the original blit (sub_415A1C,
    // docs/re/player-colour.md). rgba's alpha already encodes the key-colour
    // transparency; the recolour preserves it.
    f.image.indices.assign(px.begin(), px.end());
    f.image.palette.assign(d.palette.begin(), d.palette.end());
}

Frame parse_cimg(BinaryReader& r, const CimgContext& ctx) {
    Frame f;
    const CimgData d = read_cimg_header(r, ctx, f);
    if (f.cimg_type != 4 && f.cimg_type != 11)
        fail(ctx.path, "CIMG unknown type " + std::to_string(f.cimg_type));

    const std::size_t pixel_bytes = f.cimg_type == 4 ? 2 : 1;
    const std::size_t pixel_count =
        static_cast<std::size_t>(f.image.width) * static_cast<std::size_t>(f.image.height);
    // Must precede every allocation below — see max_decodable_pixels.
    if (pixel_count > max_decodable_pixels(d.data_end - r.pos(), pixel_bytes))
        fail(ctx.path, "CIMG dimensions exceed the compressed data");

    f.image.rgba.resize(pixel_count * 4);
    const CimgDecode dec{ctx.path, d.data_end, pixel_count, d.uncompressed_size,
                         std::span<const std::uint8_t>(d.palette.data(), d.palette_size)};
    if (f.cimg_type == 4) {
        decode_true_colour(r, dec, f);
    } else {
        decode_paletted(r, dec, f);
    }

    r.seek(ctx.item.end);  // skip optional terminator/padding
    return f;
}

// A FRAM item is an FNAM (the artist-side TGA name) plus a CIMG. parse_cimg
// builds a fresh Frame, so the name is carried across it.
Frame parse_frame(BinaryReader& r, const Item& fram, const std::filesystem::path& path) {
    Frame frame;
    while (r.pos() + 10 <= fram.end) {
        const Item sub = read_item(r);
        if (sub.is("FNAM")) {
            frame.name = r.cstr_field(sub.length);
        } else if (sub.is("CIMG")) {
            auto name = std::move(frame.name);
            frame = parse_cimg(r, CimgContext{path, sub});
            frame.name = std::move(name);
        }
        r.seek(sub.end);
    }
    return frame;
}

SeqStep parse_stat(BinaryReader& r, const Item& stat) {
    SeqStep step;
    while (r.pos() + 10 <= stat.end) {
        const Item leaf = read_item(r);
        if (leaf.is("HEAD") && leaf.length >= 2) {
            step.head0 = r.u16();
        } else if (leaf.is("FRAM") && leaf.length >= 8) {
            r.u16();  // unknown, always 1
            step.frame = r.u16();
            step.dx = r.i16();
            step.dy = r.i16();
        }
        r.seek(leaf.end);
    }
    return step;
}

Sequence parse_sequence(BinaryReader& r, const Item& seq_item) {
    Sequence seq;
    while (r.pos() + 10 <= seq_item.end) {
        const Item sub = read_item(r);
        if (sub.is("HEAD")) {
            seq.name = r.cstr_field(sub.length);
        } else if (sub.is("STAT")) {
            seq.steps.push_back(parse_stat(r, sub));
        }
        r.seek(sub.end);
    }
    return seq;
}

// POWERS1.ANI/POWERZ.ANI ship a 'power jelly' sequence naming a frame that does
// not exist — a bug in the original 1997 assets, so the file stays usable and
// the dangling step is reset to -1 with a warning rather than throwing.
void drop_dangling_frame_refs(AniFile& ani) {
    const int frame_count = static_cast<int>(ani.frames.size());
    for (auto& seq : ani.sequences) {
        for (auto& st : seq.steps) {
            if (st.frame >= -1 && st.frame < frame_count) continue;
            ani.warnings.push_back("sequence '" + seq.name + "' references frame " +
                                   std::to_string(st.frame) + " (only " +
                                   std::to_string(frame_count) + " exist), dropped");
            st.frame = -1;
        }
    }
}

}  // namespace

AniFile load(const std::filesystem::path& path) {
    const auto buf = read_file(path);
    BinaryReader r(buf);

    static constexpr char kMagic[] = "CHFILEANI ";
    if (buf.size() < 16 || std::memcmp(buf.data(), kMagic, 10) != 0) fail(path, "bad magic");
    r.seek(10);
    const std::uint32_t payload_len = r.u32();
    r.u16();  // file id
    const std::size_t file_end = r.pos() + payload_len;
    if (file_end > buf.size()) fail(path, "declared length overruns file");

    AniFile ani;
    while (r.pos() + 10 <= file_end) {
        const Item item = read_item(r);
        if (item.is("CBOX")) {
            ani.cell_width = r.u16();
            ani.cell_height = r.u16();
        } else if (item.is("FRAM")) {
            ani.frames.push_back(parse_frame(r, item, path));
        } else if (item.is("SEQ ")) {
            ani.sequences.push_back(parse_sequence(r, item));
        }
        r.seek(item.end);
    }

    drop_dangling_frame_refs(ani);
    return ani;
}

}  // namespace bomber::assets::ani
