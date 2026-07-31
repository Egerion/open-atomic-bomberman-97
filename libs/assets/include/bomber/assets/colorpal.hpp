#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>

#include "bomber/assets/image.hpp"

namespace bomber::assets::colorpal {

// The in-match shared-palette SNAP — a faithful port of the original's load-time
// colour quantization (docs/re/facts.md "In-match colour quantization",
// sub_41BBBD / sub_41C837:21309). A match runs on ONE 8-bit hardware palette, so
// every decoded pixel is snapped to the nearest COLOR.PAL master entry through
// the file's RGB555->index reverse LUT and displayed as the 6-bit value times 4
// — which is why the brightest channel is 252, never 255.
//
// Most art is AUTHORED in that palette, making the snap an identity for it. The
// visible exception is FIELD1 ("Classic Green Acres"), whose vivid blue/green
// dither is not in the master palette and snaps to a muted pair a raw per-asset
// decode misses. Verified pixel-exact against a live capture (2026-07-13).
class Palette {
public:
    // Loads COLOR.PAL (install ROOT, 33536 bytes = 768 master + 32768 LUT).
    // Throws std::runtime_error on a malformed/short file.
    static Palette load(const std::filesystem::path& path);

    // Default-constructed = inert: ok() is false and remap() is a no-op, so a
    // missing COLOR.PAL degrades to the raw per-asset decode.
    Palette() = default;
    bool ok() const { return ok_; }

    // sub_41C837:21309's chain (RGB555 truncate -> LUT -> master*4), spelled as
    // its two halves because the faithful recolour splits between them.
    void snap(std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) const {
        master_rgb(index_of(r, g, b), r, g, b);
    }

    // The LUT lookup alone, without snap()'s master-RGB write-back: the recolour
    // needs the bare index so it can rewrite it through the colour's .RMP table
    // first. `r>>3` inverts the loader's expand5, so index_of(expand5(rgb555))
    // is exactly `byte_495390[rgb555]`.
    std::uint8_t index_of(std::uint8_t r, std::uint8_t g, std::uint8_t b) const {
        const std::size_t off = (static_cast<std::size_t>(r >> 3) << 10) |
                                (static_cast<std::size_t>(g >> 3) << 5) |
                                static_cast<std::size_t>(b >> 3);
        return lut_[off];
    }

    // The 8-bit RGB of master entry `idx` (already *4-scaled at load) — the
    // other half of the recolour, applied after the .RMP remap.
    void master_rgb(std::uint8_t idx, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) const {
        r = master_[static_cast<std::size_t>(idx) * 3 + 0];
        g = master_[static_cast<std::size_t>(idx) * 3 + 1];
        b = master_[static_cast<std::size_t>(idx) * 3 + 2];
    }

    // Remap every OPAQUE pixel of a decoded match image in place; transparent
    // (key-colour) pixels are left alone so their transparency survives. No-op
    // when !ok().
    //
    // CLASSIC match art ONLY — field PCX and tile/brick ANI cels. Never the
    // DATA_HD truecolour overrides or the front-end screens, which the original
    // loads through the non-snapping path.
    void remap(Image& img) const;

private:
    bool ok_ = false;
    // 256 master colours, already *4-scaled to 8-bit. Entry 0 is a white
    // sentinel in the file, forced to black at load as the original does.
    std::array<std::uint8_t, std::size_t{256} * 3> master_{};
    std::array<std::uint8_t, 32768> lut_{};  // RGB555 -> master index
};

}  // namespace bomber::assets::colorpal
