#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "bomber/assets/bmfont.hpp"
#include "bomber/assets/bmtext.hpp"
#include "bomber/game/asset_store.hpp"
#include "bomber/game/audio_engine.hpp"
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

// The generic help-file browser — sub_41431C -> sub_414235 (docs/re/
// results-and-options.md §4, PINNED from the body): globs every `*.BM` in
// the install ROOT (sub_41404B, the same findfirst/findnext/qsort helper
// SchemeFilePicker uses for `*.SCH`, editor_screen.hpp), lists the matched
// filenames through the generic list dialog at (100, 100) with header
// getstring(600) ("Available help files:"), and opens the selected topic
// through the SAME `.BM` viewer (BmScreen) main-menu row 4/present_bm_screen
// already use. Selecting a topic and dismissing its viewer re-shows the SAME
// list (sub_414235's `do { list } while (v14 != -1)` loop indexes the one
// glob result array rather than re-scanning the directory) until the list
// itself is cancelled with Esc. This is the SAME routine both the main
// menu's row 5 (§4) and the in-round F1 key (docs/re/in-match-shell.md §1)
// invoke; the caller (GameApp) owns the loop and, for the in-round case,
// brackets it with the sim-tick suspension sub_42A16F(1)/(0) documents.
//
// Like BmScreen, this widget paints NO backdrop of its own — sub_41485A's
// list dialog (sub_42DB80) is a floating panel composited over whatever the
// caller already has on screen (the menu at row 5, the live match field at
// the in-round F1 site), never a full-screen cut. The caller draws the
// current screen first, then this on top (present_bm_screen's own
// convention, extended here).
//
// getvalue(15) gate: sub_414235 checks "is the online manual enabled?"
// BEFORE the glob; enter(false) reproduces that (§4), and both gated error
// paths (disabled here, or an empty glob) draw the SAME getstring(95)-suffixed
// two-line dialog in the SAME ink (byte_49D0DA, RGB (252,80,80) — PINNED in
// results-and-options.md §1, confirmed identical to sub_4141F8's team-1 ink).
class HelpBrowser {
public:
    HelpBrowser(const AssetStore& assets, const FontTextures& font)
        : assets_(&assets), font_(&font), bm_(assets, font) {}

    // Globs `*.BM` in the install root and resets the list cursor.
    // `manual_enabled` is the caller's getvalue(15) reading ("is the online
    // manual enabled?", default 1, docs/re/results-and-options.md §4):
    // sub_414235 checks this BEFORE globbing at all, so when false the
    // browser skips the scan entirely and shows the pinned getstring(5)/
    // getstring(95) "disabled" error pair instead of a topic list.
    void enter(bool manual_enabled = true);

    // Feed one SDL keycode. While the list is showing: Up/Down move,
    // Enter opens the highlighted topic (switches into the nested BmScreen
    // loop), Esc cancels the whole browser (done()==true). While a topic is
    // open, keys route to the BmScreen viewer instead (viewing()==true) —
    // the caller should check viewing() and dispatch there, mirroring
    // present_options_screen's own F1 sub-loop pattern.
    void on_key(SDL_Keycode key, AudioEngine& audio);
    void draw(SDL_Renderer* ren) const;

    // True once a `.BM` viewer is open on top of the list (route on_key/
    // draw's per-frame SDL_Delay pacing exactly like present_bm_screen).
    bool viewing() const { return viewing_; }
    BmScreen& viewer() { return bm_; }
    // Called by the caller once viewer().done() returns true: closes the
    // topic and re-shows the list (the loop-back sub_414235 performs).
    void close_viewer() { viewing_ = false; }

    bool done() const { return done_; }
    // sub_41404B found zero `*.BM` files, or the install root is missing —
    // the caller should show the getstring(4)/getstring(95) error case
    // instead of an empty list (§4's "no .BM files found" branch).
    bool empty() const { return entries_.empty(); }
    // getvalue(15)==0 at enter() time — the "manual disabled" error case
    // (getstring(5)/getstring(95)), checked ahead of the glob (§4).
    bool disabled() const { return disabled_; }

    static constexpr int kVisibleRows = 13;  // sub_42DBCC's 13-row dialog

private:
    const AssetStore* assets_ = nullptr;
    const FontTextures* font_ = nullptr;
    BmScreen bm_;
    std::vector<std::filesystem::path> entries_;
    int row_ = 0;
    int top_ = 0;
    bool viewing_ = false;
    bool done_ = false;
    bool disabled_ = false;
};

}  // namespace bomber::game
