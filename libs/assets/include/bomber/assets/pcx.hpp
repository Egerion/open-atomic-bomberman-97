#pragma once

#include <cstdint>
#include <filesystem>
#include <span>

#include "bomber/assets/image.hpp"

namespace bomber::assets::pcx {

// Decodes an 8-bit palettized or 24-bit RGB PCX as RGBA8. Original assets are
// 8-bit; DATA_HD may use standard 24-bit PCX artwork.
//
// The 8-bit path also fills `Image::indices` + `Image::palette` with the source
// indices and the file's own VGA palette. That matters because the original's
// TRANSPARENCY is an INDEX, not a colour: the blit the .BM viewer uses
// (sub_4428E4 -> sub_44AED5) writes only source bytes that are non-zero, so
// palette index 0 is the key. Several front-end images have real black at some
// OTHER index (KURT.PCX and JERM.PCX both put the photo's blacks at 255 and
// never use 0 at all), so keying on RGB would punch holes in them. Callers that
// need the key use `apply_key_index` (libs/game key_color.hpp); everything else
// simply ignores the two extra vectors. 24-bit PCX has no indices and leaves
// both empty.
Image parse(std::span<const std::uint8_t> data, const char* what = "<memory>");

// Convenience loader: read the file at `path` and parse it.
Image load(const std::filesystem::path& path);

}  // namespace bomber::assets::pcx
