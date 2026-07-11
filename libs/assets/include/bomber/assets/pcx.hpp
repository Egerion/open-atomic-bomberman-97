#pragma once

#include <filesystem>

#include "bomber/assets/image.hpp"

namespace bomber::assets::pcx {

// Loads an 8-bit palettized or 24-bit RGB PCX as RGBA8. Original assets are
// 8-bit; DATA_HD may use standard 24-bit PCX artwork.
Image load(const std::filesystem::path& path);

}  // namespace bomber::assets::pcx
