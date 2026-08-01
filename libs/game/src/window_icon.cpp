#include "window_icon.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace bomber::game {

namespace {

// A classic .ICO is a stack of bottom-up BMP DIBs behind an ICONDIR index. This
// reader takes only the 8-bit palette entries — the simple path, and what
// BM95.ICO ships — and treats the file as untrusted input: every offset is
// bounds-checked against the buffer before it is read.
class IcoFile {
public:
    IcoFile(const std::uint8_t* data, std::size_t size) : d_(data), sz_(size) {}

    // The ICONDIR entry to use, or -1 if the file holds none we can decode.
    int best_entry() const {
        if (sz_ < 6 || u16(2) != 1) return -1;  // reserved0, type == 1
        const int count = u16(4);
        int best = -1;
        int best_score = -1;
        for (int i = 0; i < count; ++i) {
            const int score = entry_score(i);
            if (score <= best_score) continue;
            best_score = score;
            best = i;
        }
        return best;
    }

    SDL_Surface* decode(int entry) const {
        const std::size_t off = u32(entry_at(entry) + 12);
        const int w = static_cast<int>(u32(off + 4));
        // A DIB's stored height covers the XOR (colour) bitmap AND the AND mask.
        const int h = static_cast<int>(u32(off + 8)) / 2;
        if (w <= 0 || h <= 0 || w > 256 || h > 256) return nullptr;
        Bitmaps bm{};
        bm.w = w;
        bm.h = h;
        bm.palette = off + 40;                              // 256 BGRA entries
        bm.colour = bm.palette + std::size_t{256} * 4;      // XOR (colour) bitmap
        bm.colour_row = ((w + 3) / 4) * 4;                  // 8-bpp row, 4-aligned
        bm.mask_row = ((w + 31) / 32) * 4;                  // 1-bpp AND row, 4-aligned
        bm.mask = bm.colour + row_bytes(bm.colour_row, h);  // AND (mask) bitmap
        if (bm.mask + row_bytes(bm.mask_row, h) > sz_) return nullptr;
        SDL_Surface* surf = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
        if (surf) paint(surf, bm);
        return surf;
    }

private:
    struct Bitmaps {
        int w;
        int h;
        std::size_t palette;
        std::size_t colour;
        int colour_row;
        int mask_row;
        std::size_t mask;
    };

    std::uint16_t u16(std::size_t o) const {
        return static_cast<std::uint16_t>(d_[o] | (d_[o + 1] << 8));
    }
    std::uint32_t u32(std::size_t o) const {
        return static_cast<std::uint32_t>(d_[o] | (d_[o + 1] << 8) | (d_[o + 2] << 16) |
                                          (static_cast<std::uint32_t>(d_[o + 3]) << 24));
    }
    static std::size_t entry_at(int i) { return 6 + static_cast<std::size_t>(i) * 16; }
    static std::size_t row_bytes(int stride, int rows) {
        return static_cast<std::size_t>(stride) * static_cast<std::size_t>(rows);
    }

    // Prefer 32x32, else the largest; -1 for an entry we cannot read at all.
    int entry_score(int i) const {
        const std::size_t e = entry_at(i);
        if (e + 16 > sz_) return -1;
        const std::uint32_t off = u32(e + 12);
        if (static_cast<std::size_t>(off) + 40 > sz_) return -1;
        if (u16(off + 14) != 8) return -1;  // 8-bpp DIB entries only
        const int w = d_[e] ? d_[e] : 256;
        return w == 32 ? 10000 : w;
    }

    void paint(SDL_Surface* surf, const Bitmaps& bm) const {
        auto* px = static_cast<std::uint8_t*>(surf->pixels);
        for (int y = 0; y < bm.h; ++y) {
            const int sy = bm.h - 1 - y;  // DIB rows are bottom-up
            for (int x = 0; x < bm.w; ++x) {
                const std::uint8_t idx =
                    d_[bm.colour + static_cast<std::size_t>(sy) * bm.colour_row + x];
                const std::size_t p = bm.palette + static_cast<std::size_t>(idx) * 4;
                const std::uint8_t m =
                    d_[bm.mask + static_cast<std::size_t>(sy) * bm.mask_row + (x / 8)];
                const bool clear = ((m >> (7 - (x & 7))) & 1) != 0;  // set == transparent
                std::uint8_t* o = px + static_cast<std::size_t>(y) * surf->pitch +
                                  static_cast<std::size_t>(x) * 4;
                o[0] = d_[p + 2];  // R (the palette is BGRA)
                o[1] = d_[p + 1];  // G
                o[2] = d_[p + 0];  // B
                o[3] = clear ? 0 : 255;
            }
        }
    }

    const std::uint8_t* d_;
    std::size_t sz_;
};

struct SdlFree {
    void operator()(void* p) const { SDL_free(p); }
};

}  // namespace

SDL_Surface* load_window_icon(const std::filesystem::path& ico_path) {
    std::size_t sz = 0;
    const std::unique_ptr<void, SdlFree> raw{SDL_LoadFile(ico_path.string().c_str(), &sz)};
    if (!raw) return nullptr;
    const IcoFile ico(static_cast<const std::uint8_t*>(raw.get()), sz);
    const int entry = ico.best_entry();
    return entry < 0 ? nullptr : ico.decode(entry);
}

}  // namespace bomber::game
