#include "bomber/assets/ani.hpp"

#include <array>
#include <cstring>
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
    auto tag = r.bytes(4);
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

// TGA type-10 style RLE. Pixel is u16 (type 4) or u8 (type 11).
template <typename Pixel>
std::vector<Pixel> decode_rle(BinaryReader& r, std::size_t data_end, std::size_t pixel_count) {
    std::vector<Pixel> out;
    out.reserve(pixel_count);
    while (out.size() < pixel_count) {
        if (r.pos() >= data_end) throw std::runtime_error("RLE: data ended early");
        std::uint8_t header = r.u8();
        std::size_t count = (header & 0x7F) + 1;
        if (count > pixel_count - out.size())
            throw std::runtime_error("RLE: packet overruns image");
        if (header & 0x80) {
            Pixel v{};
            if constexpr (sizeof(Pixel) == 2) v = r.u16(); else v = r.u8();
            out.insert(out.end(), count, v);
        } else {
            for (std::size_t i = 0; i < count; ++i) {
                Pixel v{};
                if constexpr (sizeof(Pixel) == 2) v = r.u16(); else v = r.u8();
                out.push_back(v);
            }
        }
    }
    return out;
}

inline std::uint8_t expand5(unsigned v) {
    return static_cast<std::uint8_t>((v << 3) | (v >> 2));
}

Frame parse_cimg(BinaryReader& r, const Item& cimg, const std::filesystem::path& path) {
    Frame f;
    f.cimg_type = r.u16();
    r.u16();  // unknown
    std::uint32_t additional_size = r.u32();
    r.u32();  // unknown
    f.image.width = r.u16();
    f.image.height = r.u16();
    f.hotspot_x = r.u16();
    f.hotspot_y = r.u16();
    f.key_color = r.u16();
    r.u16();  // unknown

    std::size_t palette_size = 0;
    std::array<std::uint8_t, 1024> palette{};
    if (additional_size >= 32) {
        palette_size = additional_size - 32;
        r.u32();  // unknown
        r.u32();  // unknown
        if (palette_size > palette.size()) fail(path, "CIMG palette too large");
        auto pal = r.bytes(palette_size);
        std::memcpy(palette.data(), pal.data(), palette_size);
    } else if (additional_size != 24) {
        fail(path, "CIMG unexpected additional_size " + std::to_string(additional_size));
    }

    r.u16();  // unknown
    r.u16();  // unknown
    std::uint32_t compressed_size = r.u32();
    std::uint32_t uncompressed_size = r.u32();
    if (compressed_size < 12) fail(path, "CIMG compressed_size < 12");
    std::size_t data_end = r.pos() + (compressed_size - 12);
    if (data_end > cimg.end) fail(path, "CIMG data overruns item");

    const std::size_t pixel_count =
        static_cast<std::size_t>(f.image.width) * static_cast<std::size_t>(f.image.height);
    f.image.rgba.resize(pixel_count * 4);

    if (f.cimg_type == 4) {
        if (uncompressed_size != pixel_count * 2) fail(path, "CIMG type 4 size mismatch");
        auto px = decode_rle<std::uint16_t>(r, data_end, pixel_count);
        for (std::size_t i = 0; i < pixel_count; ++i) {
            std::uint16_t v = px[i];
            f.image.rgba[i * 4 + 0] = expand5((v >> 10) & 31);
            f.image.rgba[i * 4 + 1] = expand5((v >> 5) & 31);
            f.image.rgba[i * 4 + 2] = expand5(v & 31);
            f.image.rgba[i * 4 + 3] = (v == f.key_color) ? 0 : 255;
        }
    } else if (f.cimg_type == 11) {
        if (palette_size != 1024) fail(path, "CIMG type 11 without 1024-byte palette");
        if (uncompressed_size != pixel_count) fail(path, "CIMG type 11 size mismatch");
        auto px = decode_rle<std::uint8_t>(r, data_end, pixel_count);
        for (std::size_t i = 0; i < pixel_count; ++i) {
            std::uint8_t idx = px[i];
            f.image.rgba[i * 4 + 0] = palette[idx * 4 + 0];
            f.image.rgba[i * 4 + 1] = palette[idx * 4 + 1];
            f.image.rgba[i * 4 + 2] = palette[idx * 4 + 2];
            f.image.rgba[i * 4 + 3] = (idx == (f.key_color & 0xFF)) ? 0 : 255;
        }
    } else {
        fail(path, "CIMG unknown type " + std::to_string(f.cimg_type));
    }

    r.seek(cimg.end);  // skip optional terminator/padding
    return f;
}

}  // namespace

AniFile load(const std::filesystem::path& path) {
    auto buf = read_file(path);
    BinaryReader r(buf);

    static constexpr char kMagic[] = "CHFILEANI ";
    if (buf.size() < 16 || std::memcmp(buf.data(), kMagic, 10) != 0)
        fail(path, "bad magic");
    r.seek(10);
    std::uint32_t payload_len = r.u32();
    r.u16();  // file id
    std::size_t file_end = r.pos() + payload_len;
    if (file_end > buf.size()) fail(path, "declared length overruns file");

    AniFile ani;
    while (r.pos() + 10 <= file_end) {
        Item item = read_item(r);
        if (item.is("CBOX")) {
            ani.cell_width = r.u16();
            ani.cell_height = r.u16();
        } else if (item.is("FRAM")) {
            Frame frame;
            while (r.pos() + 10 <= item.end) {
                Item sub = read_item(r);
                if (sub.is("FNAM")) {
                    frame.name = r.cstr_field(sub.length);
                } else if (sub.is("CIMG")) {
                    auto name = std::move(frame.name);
                    frame = parse_cimg(r, sub, path);
                    frame.name = std::move(name);
                }
                r.seek(sub.end);
            }
            ani.frames.push_back(std::move(frame));
        } else if (item.is("SEQ ")) {
            Sequence seq;
            while (r.pos() + 10 <= item.end) {
                Item sub = read_item(r);
                if (sub.is("HEAD")) {
                    seq.name = r.cstr_field(sub.length);
                } else if (sub.is("STAT")) {
                    SeqStep step;
                    while (r.pos() + 10 <= sub.end) {
                        Item leaf = read_item(r);
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
                    seq.steps.push_back(step);
                }
                r.seek(sub.end);
            }
            ani.sequences.push_back(std::move(seq));
        }
        r.seek(item.end);
    }

    for (auto& seq : ani.sequences) {
        for (auto& st : seq.steps) {
            if (st.frame < -1 || st.frame >= static_cast<int>(ani.frames.size())) {
                ani.warnings.push_back("sequence '" + seq.name + "' references frame " +
                                       std::to_string(st.frame) + " (only " +
                                       std::to_string(ani.frames.size()) + " exist), dropped");
                st.frame = -1;
            }
        }
    }

    return ani;
}

}  // namespace bomber::assets::ani
