#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>
#include <vector>

#include "bomber/assets/bmfont.hpp"
#include "bomber/assets/bmtext.hpp"
#include "bomber/audio/audio_engine.hpp"
#include "bomber/render/asset_store.hpp"
#include "bomber/render/sdl.hpp"
#include "bomber/ui/list_picker.hpp"

// The front-end `.BM` text-screen viewer — sub_41302D (BM95.EXE @ 0x41302D): a
// parsed BmDocument in the active bitmap font (FONT6.FON, pinned by graphics
// init via sub_431E9C(6)), one screen row per line, inline <IMG> PCXs blitted at
// their line, scrolled by the arrow/page keys. There is NO auto or timed scroll
// in the original; Enter or Escape dismisses it. Presentation only (ADR-0004).

namespace bomber::game {

// One colour triple. Every draw call used to spell a colour as three adjacent
// Uint8s — and an outlined draw as SIX — which is a transposition hazard nothing
// warns about here (.clang-tidy disables bugprone-easily-swappable-parameters
// repo-wide). The struct is the guard rail.
struct Rgb {
    Uint8 r = 0, g = 0, b = 0;
};

// FontTextures::draw's per-call style: the ink, and the dst-rect-only scale.
struct TextStyle {
    Rgb ink;
    float scale = 1.0f;
};

// FontTextures::draw_outlined's per-call style: ink over its outline colour
// (sub_41696C's a6/a7 — black at every dialog site but the quit confirm), plus
// the routine's max_w clip (its a4; <= 0 disables the clip, and only the [WAIT]
// prompt passes one).
struct OutlinedTextStyle {
    Rgb ink;
    Rgb outline{};
    float max_w = 0.0f;
};

// One glyph uploaded as an alpha texture (white ink, transparent ground); the
// on-screen colour is applied per-draw with SDL_SetTextureColorMod.
class FontTextures {
public:
    // Safe to call on an empty font: loaded() then returns false and draw is a
    // no-op.
    void build(SDL_Renderer* ren, const assets::bmfont::Font& font);
    void reset();
    ~FontTextures() { reset(); }
    FontTextures() = default;
    FontTextures(const FontTextures&) = delete;
    FontTextures& operator=(const FontTextures&) = delete;

    bool loaded() const { return !glyphs_.empty(); }
    int line_height() const { return line_height_; }
    int spacing() const { return spacing_; }

    // Advance width of one character, 0 for a code the font does not cover —
    // mirroring sub_432120, which only advances below the glyph count.
    int advance(unsigned char c) const;
    // Pixel width of the string laid left to right (sub_432120).
    int measure(const std::string& s) const;

    // Returns the x just past the run, so a caller can continue the same line
    // (e.g. after an inline image).
    float draw(SDL_Renderer* ren, const std::string& s, SDL_FPoint at,
               const TextStyle& style) const;
    // COMPAT overload of the above, kept ONLY while goldman_screen.cpp (frozen
    // under another agent's edit) still spells the colour as three Uint8s.
    // Delete it and convert that one call site once the freeze lifts.
    float draw(SDL_Renderer* ren, const std::string& s, float x, float y, Uint8 r, Uint8 g, Uint8 b,
               float scale = 1.0f) const {
        return draw(ren, s, SDL_FPoint{x, y}, TextStyle{{r, g, b}, scale});
    }

    // sub_41696C (pseudo.c 18516-18572) renders every front-end string FIVE
    // times — four outline passes and one ink pass — into a (w+2)-wide scratch,
    // and CLIPS the run to `style.max_w` pixels (its 4th argument; VALUELST rows
    // 705/710/715/720/790 column 3).
    // The four outline passes are the ink's DIAGONAL neighbours, not its
    // cardinal ones (facts.md). THE port of that routine: draw_dialog_text
    // delegates here, so there is one place for the offsets to be wrong in.
    float draw_outlined(SDL_Renderer* ren, const std::string& s, SDL_FPoint at,
                        const OutlinedTextStyle& style) const;

private:
    struct GlyphTex {
        SDL_Texture* tex = nullptr;  // owned via owners_ below; null for blanks
        int w = 0;                   // advance width
    };
    std::vector<GlyphTex> glyphs_;         // indexed by char code
    std::vector<sdl::TexturePtr> owners_;  // keep the textures alive
    int line_height_ = 0;
    int spacing_ = 0;
};

// A running `.BM` screen. The shell polls done(), feeds keys via on_key(), and
// paints with draw(); the Back-vs-Advance result is the shell's business, since
// a leaf routes to the menu either way.
class BmScreen {
public:
    BmScreen(const AssetStore& assets, const FontTextures& font)
        : assets_(&assets), font_(&font) {}

    // (Re)load and lay out the named `.BM` (install-root file, e.g. "CREDITS").
    // A missing or broken file yields an empty doc that still dismisses.
    void enter(const std::string& bm_name);

    // Enter/Escape finish; the arrow/page keys scroll (sub_41302D: up/down one
    // line, PgUp/PgDn one page).
    void on_key(SDL_Keycode key);

    void draw(SDL_Renderer* ren) const;

    bool done() const { return done_; }

private:
    int visible_rows() const;  // 344 / line_height (sub_41302D's row count)
    int max_scroll() const;    // clamp target for the top line

    const AssetStore* assets_ = nullptr;
    const FontTextures* font_ = nullptr;
    assets::bmtext::BmDocument doc_;
    int top_ = 0;  // index of the first visible line (sub_41302D's scroll top)
    bool done_ = false;
};

// The generic help-file browser — sub_41431C -> sub_414235 (docs/re/
// results-and-options.md §4, PINNED from the body). Globs every `*.BM` in the
// install ROOT (sub_41404B, the same helper SchemeFilePicker uses for `*.SCH`),
// lists the filenames through the generic list dialog at (100, 100) with header
// getstring(600), and opens the pick through the SAME `.BM` viewer. Dismissing a
// topic re-shows the list: sub_414235 re-runs the dialog in a do/while ending
// only on selection index -1, indexing the one glob result array rather than
// re-scanning. The SAME routine serves the main menu's row 5 (§4) and the
// in-round F1 key (docs/re/in-match-shell.md §1); the caller owns the loop and,
// for the in-round case, brackets it with sub_42A16F(1)/(0).
//
// Like BmScreen it paints NO backdrop: sub_41485A's list is a floating panel
// composited over whatever the caller already has on screen, never a cut.
class HelpBrowser {
public:
    HelpBrowser(const AssetStore& assets, const FontTextures& font)
        : assets_(&assets), font_(&font), bm_(assets, font), list_(font) {}

    // `manual_enabled` is the caller's getvalue(15) reading ("is the online
    // manual enabled?", default 1). sub_414235 checks it BEFORE globbing at all,
    // so a false here skips the scan and shows the pinned getstring(5)/
    // getstring(95) "disabled" pair instead of a topic list.
    void enter(bool manual_enabled = true);

    // While the list shows: Up/Down move, Enter opens the highlighted topic
    // (viewing() becomes true), Esc cancels the browser. While a topic is open
    // the caller should check viewing() and route keys to viewer() instead.
    void on_key(SDL_Keycode key, AudioEngine& audio);

    // sub_42DBCC is MOUSE-FIRST (list_dialog_geometry.hpp's "INPUT MODEL"), in
    // 640x480 logical coordinates. `buttons_held` is the motion event's own
    // button mask — the original only re-homes the highlight while no button is
    // down (@0x4330A0).
    void on_mouse_move(float x, float y, bool buttons_held);
    void on_mouse_down(float x, float y);
    void on_mouse_up(float x, float y);

    void draw(SDL_Renderer* ren) const;

    bool viewing() const { return viewing_; }
    BmScreen& viewer() { return bm_; }
    // Called once viewer().done(): closes the topic and re-shows the list (the
    // loop-back sub_414235 performs).
    void close_viewer() { viewing_ = false; }

    bool done() const { return done_; }
    // sub_41404B found zero `*.BM`, or the install root is missing — §4's
    // getstring(4)/getstring(95) error case rather than an empty list.
    bool empty() const { return list_.empty(); }
    // getvalue(15)==0 at enter() time — §4's getstring(5)/getstring(95) case.
    bool disabled() const { return disabled_; }

    static constexpr int kVisibleRows = ListPicker::kVisibleRows;

private:
    std::string header() const;  // getstring(600)
    std::string row_text(int i) const;
    void open_selected();
    // True while the list itself is the thing on screen taking input.
    bool list_active() const;
    void draw_error_dialog(SDL_Renderer* ren) const;

    const AssetStore* assets_ = nullptr;
    const FontTextures* font_ = nullptr;
    BmScreen bm_;
    // The shared (100,100) glob/list widget — sub_41404B + sub_42DBCC.
    ListPicker list_;
    bool viewing_ = false;
    bool done_ = false;
    bool disabled_ = false;
};

}  // namespace bomber::game
