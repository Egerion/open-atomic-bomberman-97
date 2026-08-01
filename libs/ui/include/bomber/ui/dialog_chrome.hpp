#pragma once

// The shared sub_43C734 window primitive + sub_432298 button widget — PINNED
// (docs/re/frontend-flow.md "The sub_43C734 dialog-chrome primitive",
// "sub_432298 — the button widget"). EVERY modal in the original front end opens
// through this one constructor and draws its buttons through this one widget:
// the boot LOADING dialog (sub_412E33), the menu quit confirm (sub_412987 ->
// sub_41456C), the campaign confirm (sub_414340), the editor's confirms
// (sub_41456C), the powerup sub-editor's prompts (sub_42EDE0) and every
// text-entry prompt (sub_42E938).
//
// THE VISIBLE CHROME IS A 9-PATCH TEXTURE, NOT A FLAT FILL — CORRECTED
// 2026-07-10 (frontend-flow.md "The WINZ.PCX 9-patch window skin").
// sub_43C734 lays a flat base coat (colormode 256 -> byte_495390[10570]), and
// then most callers overpaint it via sub_41726B -> sub_416B43: a 9-patch of
// DATA/RES/WINZ.PCX (72x72, loaded as "winz.plt" by sub_414DF4), corners pinned,
// edges and centre tiled, cell = 72/3 = 24 px. A caller with no sub_41726B call
// in its body — the editor's sub_42E938/sub_42EDE0 prompts — stays flat.

#include <SDL3/SDL.h>

#include <string>
#include <vector>

#include "bomber/game_util/list_dialog_geometry.hpp"  // sub_42DBCC's geometry + input model
#include "bomber/render/sprites.hpp"
#include "bomber/ui/bmscreen.hpp"

namespace bomber::game {

struct DialogRect {
    float x, y, w, h;
};

// CORRECTED 2026-07-26: sub_43C734 is SIX-arg — (x, y, width, height, colormode,
// flags), and a1 is X, a2 is Y. Proof: a1/a2 flow into sub_43D398 @0x43C8C2,
// which bounds-checks width + a1 against the right clip edge and height + a2
// against the bottom. The old "X is never an explicit parameter" note was a
// Hex-Rays artefact — it dropped the EAX argument at these call sites.
//
// This helper still CENTERS because the confirm/acknowledge callers do; it is
// not a claim about the primitive. Callers with a literal X (the LOADING
// dialog's 150, the list dialog's 100) pass it.
DialogRect dialog_rect(float y_px, float height_px, float width_px);
// sub_41456C's own callers' variant: y = (screenH - height) / 2.
DialogRect dialog_rect_vcentered(float height_px, float width_px);

// Chrome colours, decoded through the REAL LUT bytes and the shared reserved-UI
// palette region (frontend-flow.md "COLOR.PAL — byte_495390 decoded for real").
// These CORRECT the earlier nearest-colour approximations.
inline constexpr Uint8 kDialogFillR = 88, kDialogFillG = 84, kDialogFillB = 80;
// byte_49D38F (LUT offset 0x7FFF) -> idx 72: the general dialog text ink.
inline constexpr Uint8 kDialogInkR = 240, kDialogInkG = 248, kDialogInkB = 252;
// dword_45C478 = 21140 -> idx 178: the sub_432298 button LABEL ink, and the list
// dialog's title-strip grey. Exported so a screen can DIM an un-actionable item
// (the lobby browser's incompatible-build rows) in a colour the front end
// already uses for text rather than inventing a greyed-out ramp.
inline constexpr Uint8 kDialogDimR = 168, kDialogDimG = 168, kDialogDimB = 164;

// The WINZ 9-patch when `winz` is a loaded sprite, the flat base coat when it is
// not (sub_41726B/sub_416B43; partial tiles clip at the far edges exactly like
// the original's min(remaining, cell) loops).
void draw_dialog_chrome(SDL_Renderer* ren, const DialogRect& r, const Sprite* winz);

// sub_41696C — ink glyphs over a 4-pass 1-px outline. The outline colour is a
// per-call argument (a7), black for every dialog except the quit confirm (see
// draw_confirm_dialog). `x, y` is the top-left.
void draw_dialog_text(SDL_Renderer* ren, const FontTextures& font, const std::string& text,
                      float x, float y, Uint8 r, Uint8 g, Uint8 b, Uint8 outline_r = 0,
                      Uint8 outline_g = 0, Uint8 outline_b = 0);

// sub_432298 — size derived from the label (w = measure+16, h = fontheight+6),
// face = the base coat washed toward white by sub_442C28 -> (108,112,108), a
// two-ring inset bevel at insets 1 and 2, a 1-px black outline, and the
// dword_45C478 label ink. `x, y` are WINDOW-relative. `pressed` draws the "down"
// bitmap (pseudo.c 35238-35256): the same construction with the bevel pair
// SWAPPED and WITHOUT the wash, so its face is the raw base coat.
void draw_dialog_button(SDL_Renderer* ren, const FontTextures& font, float x, float y,
                        const std::string& label, bool pressed = false);

// sub_44240C — interior fill plus a light top/left and dark bottom/right 1-px
// strip (swapped for sunken), in the button widget's own dword_45C470/45C474.
void draw_bevel_rect(SDL_Renderer* ren, float x, float y, float w, float h, bool raised,
                     Uint8 face_r, Uint8 face_g, Uint8 face_b);

// The 4-space run separating two key hints. Exported so a caller measuring a
// hint block by hand uses the SAME gap pack_hint_lines does.
inline constexpr char kHintGap[] = "    ";

struct HintBlock {
    std::vector<std::string> lines;
    float width = 0.0f;  // the width the block was BUDGETED at (see pack_hint_lines)
};

// PORT-ONLY (ADR-0011: the original front end has no online lobby, so there is
// no hint chrome to pin). Packs `parts` into the fewest kHintGap-joined lines
// within `max_w`. Nothing is ever truncated: a part wider than `max_w` gets its
// own line and `width` reports it, so the window grows instead of clipping.
// `budget`, when non-empty, is parallel to `parts` and supplies the text each
// part is MEASURED as — a hint whose wording changes with state passes its
// longest variant, so flipping the state never resizes the dialog.
HintBlock pack_hint_lines(const FontTextures& font, const std::vector<std::string>& parts,
                          const std::vector<std::string>& budget, float max_w);

// Where draw_list_dialog put things, so a caller can place item text and the
// selection band on top of the chrome. Screen-space pixels.
struct ListDialogLayout {
    DialogRect win;   // full window rect
    float item_x;     // left edge of item text
    float item_y0;    // top y of the first visible item
    float item_h;     // per-row pitch (one font line)
    float item_w;     // item text column width (selection-band width)
    float done_x;     // "Done" button x
    float done_y;     // "Done" button y
    float footer_y0;  // top y of the first reserved footer line (see footer_lines)
};

// The generic bevel LIST dialog — sub_42DBCC, RE-PINNED 2026-07-26 from a full
// body read. Every offset and the draw order live in list_dialog_geometry.hpp
// with their per-address citations; the three facts a CALLER must know are:
//
//   * the scrollbar is drawn UNCONDITIONALLY (no branch guards it), so a list
//     that fits still shows a full-height bar with the thumb parked at the top,
//     and the thumb is a FIXED 15x15 that only slides in Y;
//   * the window is NOT centred — x_px/y_px go straight to sub_43C734, and both
//     RE'd callers pass the literal (100, 100);
//   * `item_text_w` is the widest ITEM's measured width (sub_42FEF0's max) and
//     must NOT be pre-maxed with the title, because the widget folds the title
//     in itself: win_w = max(item_text_w + 16, measure(title)) + 20.
//
// No WINZ 9-patch is painted. `footer_lines` reserves that many text lines
// INSIDE the window between the item area and "Done", reported as `footer_y0` —
// PORT-ONLY room for the online lobby's key hints (ADR-0011), with no home in
// sub_42DBCC's own chrome. It defaults to 0, so every RE'd caller keeps the
// pinned geometry exactly.
ListDialogLayout draw_list_dialog(SDL_Renderer* ren, const FontTextures& font,
                                  const std::string& title, float x_px, float y_px,
                                  float item_text_w, int visible_rows, int total_rows, int top_row,
                                  int footer_lines = 0);

// The selection highlight. The original does NOT invert: sub_442C28 @0x42DF80
// washes the row's rect toward white over a base coat that is exactly the
// sub_432298 button face (same wash, same coat, @0x4323DB), so the band colour
// is the button face by construction. The row's TEXT is washed too, but the
// white ink is already at the ramp ceiling and does not move — so CALLERS KEEP
// DRAWING SELECTED ROWS IN THEIR NORMAL INK.
void draw_list_selection(SDL_Renderer* ren, const ListDialogLayout& lay, int visible_index);

// --- the list dialog's INPUT side ----------------------------------------
//
// sub_42DBCC is mouse-first (list_dialog_geometry.hpp's "INPUT MODEL" carries
// the addresses). The DECISIONS live in that SDL-free header so the headless
// suite can drive them; these are only the font/SDL adapters.

// The same geometry draw_list_dialog builds for the identical arguments, so a
// screen can hit-test exactly what it last drew.
ListDialogGeometry list_dialog_layout_for(const FontTextures& font, const std::string& title,
                                          float x_px, float y_px, float item_text_w,
                                          int visible_rows, int total_rows, int top_row,
                                          int footer_lines = 0);

// Which hotspot a screen-space point lands on.
ListDialogHit list_dialog_hit_for(const FontTextures& font, const ListDialogGeometry& g,
                                  int visible_rows, float mx, float my);

// One SDL keycode as the code sub_42DBCC's own switch expects, or 0 for a key
// the widget does not handle. Only the keys the ORIGINAL binds are mapped —
// there is no W/S alias and no Space.
int list_dialog_key_code(SDL_Keycode key);

// The logical (640x480) point of a mouse event; false for a non-mouse event.
bool list_mouse_point(SDL_Renderer* ren, const SDL_Event& ev, float& x, float& y);

// Route one SDL event's mouse half into a list widget (anything exposing the
// on_mouse_move/down/up trio). True when the event WAS a mouse event, so a
// screen pump can `continue` past it.
template <class ListWidget>
inline bool dispatch_list_mouse(SDL_Renderer* ren, const SDL_Event& ev, ListWidget& w) {
    float lx = 0.0f, ly = 0.0f;
    if (!list_mouse_point(ren, ev, lx, ly)) return false;
    if (ev.type == SDL_EVENT_MOUSE_MOTION) {
        w.on_mouse_move(lx, ly, ev.motion.state != 0);
        return true;
    }
    // Past the motion case list_mouse_point only admits the two button events,
    // so ev.button is the live union member here. Only the LEFT button reaches
    // the widget: every id sub_42DBCC registers sits in the left-button slot
    // pair (+0x20/+0x24) and the right pair (+0x28/+0x2c) is -1 throughout, so a
    // right click is swallowed rather than falling through to the key path.
    if (ev.button.button != SDL_BUTTON_LEFT) return true;
    if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN) w.on_mouse_down(lx, ly);
    if (ev.type == SDL_EVENT_MOUSE_BUTTON_UP) w.on_mouse_up(lx, ly);
    return true;
}

// sub_414340 — the ACKNOWLEDGE modal (PINNED, pseudo.c 17003-17107): two centred
// lines (the halves of one packed 64-bit string argument; the top is
// getstring(95) "NOTE!" at every UI call site), width = max(the two widths, 80)
// + 64, height = 4*fontheight + 64 + linesHeight, centred both ways, WINZ
// 9-patch @17070, lines at window-relative y = fontheight+32 and +fontheight+2
// more, and ONE sub_432298 button labelled getstring(27) " Ok " at
// x = width/2 - 32, y = height - 32 - fontheight - 6, widget id 27. Its key loop
// blips (sub_427961(20)) for ANY real key and closes on Enter/Space/Esc — the
// CALLER owns that loop; these helpers only draw.
DialogRect acknowledge_dialog_rect(const FontTextures& font, const std::string& top,
                                   const std::string& bottom);
DialogRect acknowledge_ok_rect(const FontTextures& font, const DialogRect& win,
                               const std::string& ok_label);
void draw_acknowledge_dialog(SDL_Renderer* ren, const FontTextures& font, const Sprite* winz,
                             const std::string& top, const std::string& bottom,
                             const std::string& ok_label, Uint8 ink_r, Uint8 ink_g, Uint8 ink_b,
                             bool ok_pressed = false);

// The sub_41456C confirm family (PINNED geometry, pseudo.c 17128-17277): centred
// both ways, height = 4*fontheight + 64 + linesHeight, width = max(textwidth,
// 80) + 64, buttons bottom-anchored at y = height - 32 - fontheight - 6,
// x = width/2 - 80 ("Yes") and width/2 + 22 ("No"), all CONFIRMED literals. It
// paints the WINZ 9-patch @17200, so pass the loaded sprite. Empty `line2` gives
// the one-line shape (linesHeight = fontheight, CONFIRMED 17176/17190 — the "+2"
// at 17207 only offsets line2's DRAW position and is NOT part of the height).
//
// Ink and outline are both per-CALLER arguments: sub_412987 passes byte_49A390
// (idx 248 -> (164,0,0)) for the quit prompt where the editor confirms pass
// white, and that prompt also overrides the black outline with (252,248,88) =
// byte_49D37A. The RE records sub_412987's outline argument as decompiler-lost,
// so that gold is PHOTO-DERIVED pending a re-read of the a2/a7 slot.
//
// NOT pinned: which packed half lands on TOP — the register spill (pseudo.c
// 17167-17190) is ambiguous the same way sub_43C734's X-placement was. This
// primitive always draws `line1` first, a port convention.
void draw_confirm_dialog(SDL_Renderer* ren, const FontTextures& font, const Sprite* winz,
                         const std::string& line1, const std::string& line2,
                         const std::string& yes_label, const std::string& no_label, Uint8 ink_r,
                         Uint8 ink_g, Uint8 ink_b, Uint8 outline_r = 0, Uint8 outline_g = 0,
                         Uint8 outline_b = 0);

// The sub_42EDE0 family (PARTIALLY pinned, pseudo.c ~32897-32920) — the lighter
// Yes/No the powerup sub-editor's Forbidden/HasOverride prompts use.
// CONFIRMED: the flat base coat with NO sub_41726B call, the same sub_432298
// bevel, HARDCODED "Yes"/"No" literals (`aYes`/`aNo`, not getstring) at the
// offsets center-64/center+16, and a 3*fontheight + 16 height. The WIDTH
// baseline is decompiler-ambiguous (the dword_45C37C/45C380 font-metric chain is
// register-spilled), so it reuses the confirm family's content-width shape.
void draw_compact_confirm_dialog(SDL_Renderer* ren, const FontTextures& font,
                                 const std::string& line, const std::string& yes_label,
                                 const std::string& no_label);

// The sub_42E938 text-entry family (PARTIALLY pinned, pseudo.c 32748-32796) —
// the Done/Cancel prompt behind every editable field: the editor's
// density/name/filename prompts (y=180, CONFIRMED literal) and the powerup
// sub-editor's born-with/override prompts (y=400, CONFIRMED literal).
// CONFIRMED: flat base coat, height = 5*fontheight + 16, HARDCODED
// "Done"/"Cancel". The width has the same register-spilled ambiguity as above.
void draw_text_entry_dialog(SDL_Renderer* ren, const FontTextures& font, float y_px,
                            const std::string& label, const std::string& entry_text,
                            const std::string& done_label, const std::string& cancel_label);

}  // namespace bomber::game
