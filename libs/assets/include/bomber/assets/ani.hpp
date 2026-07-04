#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "bomber/assets/image.hpp"

namespace bomber::assets::ani {

// Loader for the game's custom CHFILEANI animation container.
// See docs/formats/ani.md for the reverse-engineered format description.

struct Frame {
    std::string name;  // original artist-side TGA name, e.g. "WLKN0000.TGA"
    int hotspot_x = 0;
    int hotspot_y = 0;
    std::uint16_t key_color = 0;  // transparent value (kept for reference; alpha already applied)
    std::uint16_t cimg_type = 0;  // 4 = 16bpp, 11 = 8bpp paletted
    Image image;                  // RGBA8 with key color made transparent
};

struct SeqStep {
    int frame = -1;  // index into AniFile::frames
    int dx = 0;      // per-step blit offset
    int dy = 0;
    // STAT HEAD first u16 (only ever 0x001E or 0xFFFF). CONFIRMED INERT: the
    // original engine parses but never reads it; pacing is counter % statecnt.
    // Kept for fidelity/inspection only — must not drive rendering. See
    // docs/re/facts.md "ANI per-step timing" and game/anim_pace.hpp.
    std::uint16_t head0 = 0;
};

struct Sequence {
    std::string name;  // e.g. "walk north"
    std::vector<SeqStep> steps;
};

struct AniFile {
    int cell_width = 0;  // CBOX
    int cell_height = 0;
    std::vector<Frame> frames;
    std::vector<Sequence> sequences;
    // Non-fatal oddities found while loading (e.g. POWERS1.ANI/POWERZ.ANI ship
    // with a 'power jelly' sequence referencing a frame that doesn't exist --
    // a bug in the original 1997 assets; such refs are reset to -1).
    std::vector<std::string> warnings;
};

// Throws std::runtime_error / std::out_of_range on malformed files.
AniFile load(const std::filesystem::path& path);

}  // namespace bomber::assets::ani
