#pragma once

#include <cstddef>
#include <cstdint>

#include "bomber/assets/image.hpp"

// THE TRANSPARENT-BLIT KEY for front-end PCX art.
//
// `libs/assets/src/ani.cpp` already keys the ANI cels it decodes, but PCX
// decodes fully opaque — correct for the full-screen backdrops (MAINMENU,
// GLUE<n>, the WINZ border), which the original copies with an opaque row blit.
// It is NOT correct for the images the `.BM` text screens embed with `<IMGname>`:
// sub_41302D blits those through sub_4428E4 -> sub_44AED5, whose inner loop is
//
//     v25 = *src++;  if (v25) *dst = v25;          (BM95.EXE @ 0x44AED5)
//
// i.e. a source byte of ZERO is skipped. Palette index 0 is the key colour, and
// the shipped art relies on it: CREDBAR.PCX is 81% index 0, BOMBDUDE.PCX 88%,
// QALOGO.PCX 67%. Drawn opaque, each of those is a black rectangle on screen.
//
// The key is an INDEX, not a colour. JERM.PCX and KURT.PCX (the two credits
// photographs) contain no index 0 at all and put their real blacks at index 255,
// so keying on RGB(0,0,0) would eat holes out of them. In the three images that
// DO use the key, index 0 is the only source of RGB(0,0,0) — which is what makes
// the DATA_HD rule below exact.

namespace bomber::game {

// True when the decoded image has any pixel at the transparent key index.
// Used to decide whether the asset participates in keying at all.
inline bool uses_key_index(const assets::Image& img, std::uint8_t key = 0) {
    for (std::uint8_t i : img.indices)
        if (i == key) return true;
    return false;
}

// Zeroes the alpha of every pixel whose SOURCE PALETTE INDEX is `key`
// (sub_44AED5's skip test). A no-op on an image with no indices — 24-bit PCX
// and anything already decoded to RGBA — so it is safe to call unconditionally.
//
// RGB is left as decoded; `bleed_transparent_rgb` (alpha_bleed.hpp) runs at
// upload and replaces it, exactly as it does for the ANI path.
inline void apply_key_index(assets::Image& img, std::uint8_t key = 0) {
    const std::size_t px = img.indices.size();
    if (px == 0 || img.rgba.size() < px * 4) return;
    for (std::size_t i = 0; i < px; ++i)
        if (img.indices[i] == key) img.rgba[i * 4 + 3] = 0;
}

// The DATA_HD variant of a keyed asset. The HD pack ships 24-bit PCX upscales
// with NO palette, so there is no index to test — it paints the keyed region as
// literal black instead (HD CREDBAR.PCX is 73% pure black, BOMBDUDE 85%,
// QALOGO 61%). Keying RGB(0,0,0) reproduces the classic mask exactly for those
// three, because index 0 is their only source of black; the caller must only
// apply it to assets whose CLASSIC image actually used the key
// (`uses_key_index`), since HD JERM/KURT carry 1-2% genuine black in the
// photographs' hair and shadows.
inline void apply_key_black(assets::Image& img) {
    for (std::size_t o = 0; o + 3 < img.rgba.size(); o += 4)
        if (img.rgba[o] == 0 && img.rgba[o + 1] == 0 && img.rgba[o + 2] == 0)
            img.rgba[o + 3] = 0;
}

}  // namespace bomber::game
