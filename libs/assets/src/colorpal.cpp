#include "bomber/assets/colorpal.hpp"

#include <stdexcept>

#include "bomber/assets/binary_reader.hpp"

namespace bomber::assets::colorpal {

Palette Palette::load(const std::filesystem::path& path) {
    auto buf = read_file(path);
    if (buf.size() < 768 + 32768)
        throw std::runtime_error("COLOR.PAL too small: " + path.string());

    Palette p;
    // Master palette: 256 * 3 six-bit VGA entries, uploaded as `4 * value`
    // (sub_443608), i.e. an 8-bit value with the low two bits zero and a 252
    // ceiling. Clamp defensively though a valid 6-bit entry never exceeds 63.
    for (std::size_t i = 0; i < 256 * 3; ++i) {
        const int v = buf[i] * 4;
        p.master_[i] = static_cast<std::uint8_t>(v > 255 ? 255 : v);
    }
    // Entry 0 is (255,255,255) in the file (a sentinel), forced to black at
    // load — matching the original's fixup so a stray snap-to-0 reads black,
    // not white.
    p.master_[0] = p.master_[1] = p.master_[2] = 0;

    // RGB555 -> nearest-master-index reverse LUT (one byte per 15-bit colour).
    for (std::size_t i = 0; i < 32768; ++i) p.lut_[i] = buf[768 + i];

    p.ok_ = true;
    return p;
}

void Palette::remap(Image& img) const {
    if (!ok_ || img.rgba.empty()) return;
    for (std::size_t i = 0; i + 3 < img.rgba.size(); i += 4) {
        if (img.rgba[i + 3] == 0) continue;  // transparent key colour: leave as-is
        snap(img.rgba[i + 0], img.rgba[i + 1], img.rgba[i + 2]);
    }
}

}  // namespace bomber::assets::colorpal
