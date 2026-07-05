#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>
#include <vector>

#include "bomber/assets/bmfont.hpp"
#include "bomber/assets/bmtext.hpp"
#include "bomber/game/asset_store.hpp"
#include "bomber/game/sdl.hpp"

// The front-end `.BM` text-screen viewer — the SDL realisation of the original's
// sub_41302D (BM95.EXE @ 0x41302D). It renders a parsed BmDocument (bmtext) with
// the active bitmap font (FONT6.FON, the font graphics-init pins via
// sub_431E9C(6)) one screen row per line, blitting each inline <IMG> PCX at its
// line, and scrolls vertically one line at a time on the arrow/page keys — a
// faithful reproduction of sub_41302D's keyboard-driven scroll (there is NO
// auto/timed scroll in the original; Enter or Escape dismiss it). Presentation
// only: nothing here touches the sim or its RNG (ADR-0004).

namespace bomber::game {

// One glyph uploaded as an alpha texture (white ink, transparent ground); the
// on-screen colour is applied per-draw with SDL_SetTextureColorMod.
class FontTextures {
public:
    // Build glyph textures from a parsed FON. Safe to call on an empty font
    // (loaded() then returns false and draw is a no-op).
    void build(SDL_Renderer* ren, const assets::bmfont::Font& font);
    void reset();
    ~FontTextures() { reset(); }
    FontTextures() = default;
    FontTextures(const FontTextures&) = delete;
    FontTextures& operator=(const FontTextures&) = delete;

    bool loaded() const { return !glyphs_.empty(); }
    int line_height() const { return line_height_; }
    int spacing() const { return spacing_; }

    // Advance width of one character (glyph width + inter-char spacing), 0 for a
    // code the font does not cover — mirroring sub_432120's `if (c < count)`.
    int advance(unsigned char c) const;
    // Pixel width the string would occupy laid left-to-right (sub_432120).
    int measure(const std::string& s) const;

    // Draw `s` at (x, y) in colour (r,g,b); returns the x just past the run so
    // callers can continue the same line (e.g. after an inline image).
    float draw(SDL_Renderer* ren, const std::string& s, float x, float y, Uint8 r, Uint8 g,
               Uint8 b) const;

private:
    struct GlyphTex {
        SDL_Texture* tex = nullptr;  // owned via owners_ below; null for blanks
        int w = 0;                   // advance width
    };
    std::vector<GlyphTex> glyphs_;          // indexed by char code
    std::vector<sdl::TexturePtr> owners_;   // keep the textures alive
    int line_height_ = 0;
    int spacing_ = 0;
};

// A running `.BM` screen. The app shell polls done(), feeds keys via on_key(),
// and paints each frame with draw(); the Back-vs-Advance result is tracked by
// the shell (present_bm_screen), since a leaf routes to the menu either way.
class BmScreen {
public:
    BmScreen(const AssetStore& assets, const FontTextures& font)
        : assets_(&assets), font_(&font) {}

    // (Re)load and lay out the named `.BM` (install-root file, e.g. "CREDITS").
    // A missing/broken file yields an empty doc that still dismisses on a key.
    void enter(const std::string& bm_name);

    // Feed one SDL keycode. Enter/Escape finish the screen; the arrow/page keys
    // scroll (sub_41302D: up/down one line, PgUp/PgDn one page).
    void on_key(SDL_Keycode key);

    // Paint the current scroll position: the visible run of lines with their
    // text + inline images, over the .BM backdrop.
    void draw(SDL_Renderer* ren) const;

    bool done() const { return done_; }

private:
    int visible_rows() const;      // 344 / line_height (sub_41302D v60)
    int max_scroll() const;        // clamp target for the top line

    const AssetStore* assets_ = nullptr;
    const FontTextures* font_ = nullptr;
    assets::bmtext::BmDocument doc_;
    int top_ = 0;      // index of the first visible line (sub_41302D v54)
    bool done_ = false;
};

}  // namespace bomber::game
