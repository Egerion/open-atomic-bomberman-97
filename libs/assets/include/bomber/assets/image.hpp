#pragma once

#include <cstdint>
#include <vector>

namespace bomber::assets {

// Simple RGBA8 image (row-major, top-left origin).
struct Image {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;  // width * height * 4

    bool empty() const { return width == 0 || height == 0; }
};

}  // namespace bomber::assets
