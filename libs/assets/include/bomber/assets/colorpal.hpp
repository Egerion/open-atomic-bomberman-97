#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>

#include "bomber/assets/image.hpp"

namespace bomber::assets::colorpal {

// The in-match shared-palette SNAP — a faithful port of the original's
// load-time colour quantization (docs/re/facts.md "In-match colour
// quantization", sub_41BBBD / sub_41C837:21309). During a match the whole
// screen runs on ONE 8-bit hardware palette = COLOR.PAL's 256 master colours;
// every decoded asset pixel is snapped to the nearest master entry through the
// file's RGB555->index reverse LUT, then displayed as the 6-bit master value
// times 4 (sub_443608's `4 * value` DirectDraw upload — so the brightest
// channel is 63*4 = 252, never 255).
//
// Most fields and tiles are AUTHORED in this master palette, so the snap is
// an exact identity for them (FIELD0/2..10, and the tile/brick art to within
// ~2%). The visible exception is FIELD1 ("Classic Green Acres"), whose floor
// is a vivid blue/green dither NOT in the master palette: the original snaps
// its (23,27,139)/(19,143,19) pair to (20,40,108)/(4,132,0) — the muted look
// the user compared against, which a raw per-asset decode misses. Verified
// pixel-exact against a live capture (2026-07-13).
//
// SDL-free and clean-room: COLOR.PAL is shipped data, not exe-derived.
class Palette {
public:
    // Loads COLOR.PAL (install ROOT, 33536 bytes = 768 master + 32768 LUT).
    // Throws std::runtime_error on a malformed/short file.
    static Palette load(const std::filesystem::path& path);

    // Default-constructed = inert: ok() is false and remap() is a no-op, so a
    // missing COLOR.PAL degrades to the raw per-asset decode.
    Palette() = default;
    bool ok() const { return ok_; }

    // Snap one 8-bit RGB in place to its master-palette colour: the exact
    // sub_41C837:21309 chain (RGB555 truncate -> LUT -> master*4).
    void snap(std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) const {
        const std::size_t off = (static_cast<std::size_t>(r >> 3) << 10) |
                                (static_cast<std::size_t>(g >> 3) << 5) |
                                static_cast<std::size_t>(b >> 3);
        const std::uint8_t idx = lut_[off];
        r = master_[static_cast<std::size_t>(idx) * 3 + 0];
        g = master_[static_cast<std::size_t>(idx) * 3 + 1];
        b = master_[static_cast<std::size_t>(idx) * 3 + 2];
    }

    // Remap every OPAQUE pixel of a decoded match image in place. Transparent
    // pixels (alpha 0, the key colour) are left untouched so their transparency
    // survives. A no-op when !ok(). Apply to CLASSIC match art only (field
    // PCX, tile/brick ANI cels) — never the DATA_HD truecolour overrides or the
    // front-end screens, which the original loads through the non-snapping path.
    void remap(Image& img) const;

private:
    bool ok_ = false;
    // 256 master colours, 6-bit VGA values already scaled *4 to 8-bit; entry 0
    // is a white sentinel in the file, forced to black at load (the original's
    // load-time fixup).
    std::array<std::uint8_t, std::size_t{256} * 3> master_{};
    std::array<std::uint8_t, 32768> lut_{};  // RGB555 -> master index
};

}  // namespace bomber::assets::colorpal
