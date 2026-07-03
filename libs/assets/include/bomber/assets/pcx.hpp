#pragma once

#include <filesystem>

#include "bomber/assets/image.hpp"

namespace bomber::assets::pcx {

// Loads an 8-bit palettized PCX (the only variant the game ships) as RGBA8.
Image load(const std::filesystem::path& path);

}  // namespace bomber::assets::pcx
