#pragma once

#include <cstdint>

// The PORT-ONLY scaling filter ("SOFT SCALING", the F10 Video Settings panel):
// which SDL sampling mode the upscale from the logical 640x480 frame uses. The
// SDL translation and the live re-application live in sprites.cpp, the one place
// textures are created and destroyed.
//
// NOT A FIDELITY FEATURE, and it must never be presented as one. The 1997 build
// renders 640x480 and scales NOTHING — on a period CRT there is no stretch and
// so no filtering step to reproduce. The blur players associate with running it
// today is an artifact of the MODERN path: a GPU or display scaler stretching
// 640x480 to a large panel, and hardware scalers are typically bilinear. Some
// people prefer that look, which is a fine reason to offer it and a bad reason
// to default to it — hence Crisp (nearest) is the default.

namespace bomber::game {

// How a texture is sampled when the logical frame is scaled up.
enum class ScaleFilter : std::uint8_t {
    Crisp,  // nearest-neighbour: the default, one source pixel per block
    Soft,   // linear: the smoothed look of a bilinear display scaler
};

// The two classes are authored for different sampling: classic 1x cels are
// pixel art, while the optional DATA_HD 4x truecolour overrides are de-dithered
// upscales that have ALWAYS been uploaded linear. Turning SOFT SCALING off must
// not make those crisp — that would change the HD look, which this option has
// nothing to do with.
enum class TextureArt : std::uint8_t {
    Classic,  // DATA/*.ANI, DATA/RES/*.PCX, FONT<n>.FON — 1x pixel art
    HighRes,  // DATA_HD overrides — always Soft
};

// The sampling mode one texture gets, given the frame-wide filter.
constexpr ScaleFilter art_filter(TextureArt art, ScaleFilter frame_filter) {
    return art == TextureArt::HighRes ? ScaleFilter::Soft : frame_filter;
}

// The frame-wide filter a run starts with. `capture_run` WINS: this option
// changes every scaled pixel, so a value read out of the mutable options.ini
// must not reach tests/visual's pinned frames. FOUR separate options have
// already broken those pins that exact way (tests/visual/README.md), which is
// why this is a decision with a test rather than an inline ternary.
constexpr ScaleFilter scale_filter_for(bool soft_scaling_option, bool capture_run) {
    return (soft_scaling_option && !capture_run) ? ScaleFilter::Soft : ScaleFilter::Crisp;
}

}  // namespace bomber::game
