#pragma once

// The shared sub_43C734 window primitive + sub_432298 button widget — PINNED
// (docs/re/frontend-flow.md "The sub_43C734 dialog-chrome primitive",
// "sub_432298 — the button widget"). Every modal window in the original
// front-end opens through this ONE window constructor and draws its buttons
// through this ONE button widget: the boot LOADING dialog (sub_412E33), the
// main-menu quit confirm (sub_412987 -> sub_41456C), the campaign confirm
// (sub_414340), the scheme editor's Ctrl+B reset / Ctrl+F fill / Esc save
// confirms (also sub_41456C), the powerup sub-editor's Forbidden/HasOverride
// prompts (the lighter sub_42EDE0 variant), and every text-entry prompt in
// the game (density/name/filename/born-with/override, sub_42E938). One
// shared implementation here keeps every caller pixel-identical instead of
// re-deriving the same chrome per screen.
//
// THE VISIBLE CHROME IS A 9-PATCH TEXTURE, NOT A FLAT FILL — CORRECTED
// 2026-07-10 (docs/re/frontend-flow.md "The WINZ.PCX 9-patch window skin").
// sub_43C734's constructor lays down a flat base coat (colormode 256 ->
// byte_495390[10570]), but the boot LOADING dialog and both sub_41456C/
// sub_414340 confirm variants immediately overpaint the whole window via
// sub_41726B -> sub_416B43: a 9-patch of DATA/RES/WINZ.PCX (72x72, loaded
// as "winz.plt" at graphics init, sub_414DF4) — corner cells pinned, edge
// cells tiled, center cell tiled, cell size = 72/3 = 24 px. WINZ's art is a
// dark BLUE noise interior with a baked-in bevel border — the blue dialog
// the original shows at boot. Callers that do NOT paint WINZ (the editor's
// sub_42E938/sub_42EDE0 prompts — no sub_41726B call in their bodies) stay
// on the flat base coat.

#include <SDL3/SDL.h>

#include <string>

#include "bomber/game/bmscreen.hpp"
#include "bomber/game/sprites.hpp"

namespace bomber::game {

struct DialogRect {
    float x, y, w, h;
};

// sub_43C734's real signature is `(y, height, width, colormode, flags)`; X is
// never an explicit parameter anywhere in this family (docs/re/
// frontend-flow.md's X-placement TODO(RE)) — every caller's evident intent is
// a horizontally centered window, so this always centers against the 640-px
// screen width (renderer.hpp's kScreenW).
DialogRect dialog_rect(float y_px, float height_px, float width_px);
// The vertically-centered variant sub_41456C's own callers use, computing
// `(screenH - height) / 2` instead of passing a literal y.
DialogRect dialog_rect_vcentered(float height_px, float width_px);

// Chrome colours — all decoded through the REAL LUT bytes (COLOR.PAL: 768-
// byte 6-bit master palette + the 32768-byte RGB555->index LUT that IS
// byte_495390) and the shared "reserved UI colours" palette region, which is
// byte-identical across the master palette and MAINMENU.PCX's (docs/re/
// frontend-flow.md "COLOR.PAL — byte_495390 decoded for real"). These
// CORRECT the earlier nearest-colour-search approximations.
//
// dword_45C46C = 10570 -> LUT idx 205 -> RGB (88,84,80): the flat BASE COAT
// colour (visible only in the non-WINZ editor prompts; under a WINZ 9-patch
// it is fully overpainted).
inline constexpr Uint8 kDialogFillR = 88, kDialogFillG = 84, kDialogFillB = 80;
// byte_49D38F (LUT offset 0x7FFF) -> idx 72 -> (240,248,252): the general
// dialog text ink ("white").
inline constexpr Uint8 kDialogInkR = 240, kDialogInkG = 248, kDialogInkB = 252;

// The window paint: WINZ.PCX 9-patch when `winz` is a loaded sprite
// (sub_41726B/sub_416B43 — corners pinned, edges/center tiled, 24-px cells,
// partial tiles clipped at the far edges exactly like the original's
// min(remaining, cell) loops); the flat base coat when `winz` is null/empty
// (the constructor-only look the editor prompts keep).
void draw_dialog_chrome(SDL_Renderer* ren, const DialogRect& r, const Sprite* winz);

// sub_41696C — the shared dialog text primitive: the ink glyphs over a
// 4-pass 1-px outline in the outline ink (sub_41696C draws the string FOUR
// times in its a7 colour, then once in its a6 colour, into a colour-key-0
// scratch; every visible call site passes byte_495390[0] = black as a7).
// Draws at `x, y` (top-left).
void draw_dialog_text(SDL_Renderer* ren, const FontTextures& font, const std::string& text,
                      float x, float y, Uint8 r, Uint8 g, Uint8 b);

// sub_432298 — text-derived size (width = measure+16, height = fontheight+6),
// face = the window base coat washed toward white by sub_442C28's brightness
// ramp -> (108,112,108), two-ring inset bevel at insets 1 and 2 (light
// top/left (108,116,128), dark bottom/right (60,68,56) — the raised "up"
// state, the only one drawn here), 1-px black outline, label ink
// dword_45C478 -> idx 178 -> (168,168,164). `x, y` are WINDOW-relative
// top-left pixels.
void draw_dialog_button(SDL_Renderer* ren, const FontTextures& font, float x, float y,
                        const std::string& label);

// The sub_41456C confirm-dialog family (PINNED geometry, pseudo.c
// 17128-17277): vertically AND horizontally centered, height = `4*fontheight
// + 64 + linesHeight` (linesHeight = fontheight for one line, 2*fontheight
// for two — CONFIRMED pseudo.c 17176/17190; the "+2" at 17207 only offsets
// line2's DRAW position below line1, it is not part of the window height —
// sub_41456C packs up to two prompt lines into one `__int64` argument),
// width = `max(textwidth, 80) + 64`, buttons bottom-anchored at
// `y = height - 32 - fontheight - 6`, x = `width/2 - 80` (first/"Yes") and
// `width/2 + 22` (second/"No") — both CONFIRMED literal offsets read
// directly from the button-placement calls. Pass an empty `line2` for a
// single-line prompt (the quit-confirm's own shape). sub_41456C paints the
// WINZ 9-patch over its window (sub_41726B @ pseudo.c 17200) — pass the
// loaded WINZ sprite. The prompt INK is the CALLER's third argument:
// sub_412987 passes byte_49A390 (LUT offset 0x5000 -> idx 248 -> (164,0,0),
// a dark red) for the quit prompt; the editor confirms pass the general
// white — so the ink is a parameter here, always outlined black via
// draw_dialog_text (frontend-flow.md "sub_41696C — outlined dialog text").
// NOT pinned: which of the two packed lines (`LODWORD`/`HIDWORD` of the
// `__int64` arg) actually lands on TOP at the pixel level — the decompiler's
// register-spill for that packing (pseudo.c 17167-17190) is ambiguous the
// same way sub_43C734's own X-placement is (see that TODO(RE)); this
// primitive always draws `line1` first (top) as the more legible ordering,
// a port convention rather than a confirmed fact.
void draw_confirm_dialog(SDL_Renderer* ren, const FontTextures& font, const Sprite* winz,
                         const std::string& line1, const std::string& line2,
                         const std::string& yes_label, const std::string& no_label, Uint8 ink_r,
                         Uint8 ink_g, Uint8 ink_b);

// The sub_42EDE0 family (PARTIALLY pinned, pseudo.c ~32897-32920) — a
// lighter Yes/No variant used only by the powerup sub-editor's
// Forbidden/HasOverride prompts (docs/re/results-and-options.md #5b).
// CONFIRMED: same sub_43C734 colormode-256 base coat and NO sub_41726B call
// (this family really is the flat fill — one of the two dialog shapes the
// WINZ correction does NOT apply to), same sub_432298 button bevel, and —
// unlike sub_41456C's getstring(26)/(25) lookup — HARDCODED literal
// "Yes"/"No" labels (`aYes`/`aNo` in the decompile, not a message-table
// call) at the CONFIRMED tighter offsets `center-64`/`center+16`. The exact
// width/height baseline is decompiler-ambiguous (the intermediate
// `dword_45C37C`/`dword_45C380` font-metric chain is register-spilled,
// "possibly undefined" in the decompile — the same class of gap as
// `sub_43C734`'s own X-placement TODO(RE)), so this reuses the confirm
// family's content-driven-width-with-minimum-clamp shape at a shorter,
// CONFIRMED `3*fontheight + 16` height instead of guessing the precise
// register-spilled term.
void draw_compact_confirm_dialog(SDL_Renderer* ren, const FontTextures& font,
                                 const std::string& line, const std::string& yes_label,
                                 const std::string& no_label);

// The sub_42E938 text-entry family (PARTIALLY pinned, pseudo.c
// 32748-32796) — the Done/Cancel prompt behind every editable field in the
// game: the editor's density/name/filename prompts (`y=180`, CONFIRMED
// literal, sub_4028D2 cases 68/78/27) and the powerup sub-editor's
// born-with/override prompts (`y=400`, CONFIRMED literal, sub_4023A2).
// CONFIRMED: same chrome family (flat base coat — no sub_41726B call, like
// sub_42EDE0 above), height = `5*fontheight + 16`, HARDCODED literal
// "Done"/"Cancel" labels (`aDone_0`/`aCancel`, not message-table lookups).
// Width baseline has the same register-spilled ambiguity noted above for
// `draw_compact_confirm_dialog`; the port uses the same
// content-driven-width-with-minimum-160-clamp shape.
void draw_text_entry_dialog(SDL_Renderer* ren, const FontTextures& font, float y_px,
                            const std::string& label, const std::string& entry_text,
                            const std::string& done_label, const std::string& cancel_label);

}  // namespace bomber::game
