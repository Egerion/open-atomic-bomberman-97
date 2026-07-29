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
#include <vector>

#include "bomber/game/bmscreen.hpp"
#include "bomber/game/list_dialog_geometry.hpp"  // sub_42DBCC's geometry + input model
#include "bomber/game/sprites.hpp"

namespace bomber::game {

struct DialogRect {
    float x, y, w, h;
};

// CORRECTED 2026-07-26: sub_43C734 is SIX-arg — `(x, y, width, height,
// colormode, flags)`. Its return pops 8 bytes of stack arguments (two of them)
// on top of the four Watcom register args, and its a1/a2 flow into sub_43D398
// @0x43C8C2, which bounds-checks the window's +0x18 width plus the first of
// those two against the right clip edge, and the +0x1c height plus the second
// against the bottom — so a1 is X and a2 is Y.
// The old "X is never an explicit parameter anywhere in this family" note
// (and the X-placement TODO(RE) it came from) was a decompiler artefact:
// Hex-Rays dropped the EAX argument at these call sites. Confirms the rescued
// `worktree-dialog-chrome-todo-re` branch, commit 0c0b00d.
//
// This helper still CENTERS, because the callers that use it (the confirm /
// acknowledge families) do center themselves. It is no longer a claim about
// the primitive: callers with a literal X — the boot LOADING dialog's 150, the
// list dialog's 100 — must pass it, and draw_list_dialog now does.
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
// dword_45C478 = 21140 -> LUT idx 178 -> (168,168,164): the sub_432298 button
// LABEL ink, which is also the list dialog's title-strip grey. Exported so a
// screen can DIM an item it will not let you action (the lobby browser's
// incompatible-build rows) in a colour the original front end already uses for
// text — rather than inventing a greyed-out ramp of its own.
inline constexpr Uint8 kDialogDimR = 168, kDialogDimG = 168, kDialogDimB = 164;

// The window paint: WINZ.PCX 9-patch when `winz` is a loaded sprite
// (sub_41726B/sub_416B43 — corners pinned, edges/center tiled, 24-px cells,
// partial tiles clipped at the far edges exactly like the original's
// min(remaining, cell) loops); the flat base coat when `winz` is null/empty
// (the constructor-only look the editor prompts keep).
void draw_dialog_chrome(SDL_Renderer* ren, const DialogRect& r, const Sprite* winz);

// sub_41696C — the shared dialog text primitive: the ink glyphs over a
// 4-pass 1-px outline in the outline ink (sub_41696C draws the string FOUR
// times in its a7 colour, then once in its a6 colour, into a colour-key-0
// scratch). The outline colour is a genuine per-call argument (a7): it
// defaults to byte_495390[0] = black — what every dialog passes EXCEPT the
// quit-confirm, whose caller passes a gold (see draw_confirm_dialog).
// Draws at `x, y` (top-left).
void draw_dialog_text(SDL_Renderer* ren, const FontTextures& font, const std::string& text,
                      float x, float y, Uint8 r, Uint8 g, Uint8 b, Uint8 outline_r = 0,
                      Uint8 outline_g = 0, Uint8 outline_b = 0);

// sub_432298 — text-derived size (width = measure+16, height = fontheight+6),
// face = the window base coat washed toward white by sub_442C28's brightness
// ramp -> (108,112,108), two-ring inset bevel at insets 1 and 2 (light
// top/left (108,116,128), dark bottom/right (60,68,56) — the raised "up"
// state), 1-px black outline, label ink dword_45C478 -> idx 178 ->
// (168,168,164). `x, y` are WINDOW-relative top-left pixels. `pressed`
// draws the "down" bitmap instead (pseudo.c 35238-35256): the SAME
// construction with the bevel colour pair SWAPPED and WITHOUT the
// sub_442C28 wash — only the "up" bitmap gets the lightened face, so the
// pressed face is the raw base coat (88,84,80).
void draw_dialog_button(SDL_Renderer* ren, const FontTextures& font, float x, float y,
                        const std::string& label, bool pressed = false);

// A 1-px beveled rectangle in the shared UI palette (sub_44240C): the raised
// ("up" — light top/left, dark bottom/right) or sunken (swapped) frame the
// generic list dialog draws around its panel, title strip, scrollbar track
// and thumb. Fills the interior with (face_*) first, then the two 1-px bevel
// strips. Bevel colours are the button widget's own dword_45C470/45C474.
void draw_bevel_rect(SDL_Renderer* ren, float x, float y, float w, float h, bool raised,
                     Uint8 face_r, Uint8 face_g, Uint8 face_b);

// The 4-space run every key-hint line separates two hints with. Exported so a
// caller measuring a hint block by hand uses the SAME gap pack_hint_lines does.
inline constexpr char kHintGap[] = "    ";

// A packed block of key-hint text: the lines to draw, and the pixel width the
// block was BUDGETED at (see pack_hint_lines) — what a caller feeds into a
// content-sized window's width so the hints land INSIDE its border.
struct HintBlock {
    std::vector<std::string> lines;
    float width = 0.0f;
};

// PORT-ONLY (ADR-0011: the original front end has no online lobby, so there is
// no hint-line chrome to pin). Packs `parts` into the fewest kHintGap-joined
// lines whose MEASURED width stays within `max_w`, so a longer — or localised —
// hint wraps and widens its window instead of spilling past the border.
// Nothing is ever truncated: a single part wider than `max_w` still gets its
// own full line, and `width` reports it so the window grows to match.
//
// `budget`, when non-empty, must be parallel to `parts` and supplies the text
// each part is MEASURED as while packing. A hint whose wording changes with
// state ("SPACE = READY" / "SPACE = NOT READY") passes its longest variant
// there, so flipping the state never resizes the dialog under the player.
HintBlock pack_hint_lines(const FontTextures& font, const std::vector<std::string>& parts,
                          const std::vector<std::string>& budget, float max_w);

// Layout returned by draw_list_dialog so the caller can place its item text
// and the selection band on top of the chrome. Screen-space pixels.
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
// read of the body (docs/re/results-and-options.md §5c;
// list_dialog_geometry.hpp carries the per-offset citations). The chrome, in
// the order the original lays it down:
//   * a 1-px BLACK outer rect (0,0)..(w-1,h-1)          sub_442384 @0x42DCF9
//   * a RAISED bevel at inset 1                          sub_44240C @0x42DD39
//   * a base-coat title-strip fill at (5,5), then a SUNKEN bevel over it, then
//     the centred title in dword_45C478 grey (168,168,164) at y = 8
//   * a base-coat item-area fill and its own SUNKEN frame
//   * the scrollbar — arrow buttons "\x18"/"\x19", a sunken track, and a
//     15x15 raised thumb of FIXED SIZE (never proportional) but sliding Y.
//     UNCONDITIONAL: there is no branch around it, so a list that fits still
//     shows a full-height scrollbar, with the thumb parked at the track's top.
//   * a "Done" button at (w/2 - 32, h - fontheight - 14). The label is the
//     hardcoded literal at 0x45AAB4, NOT a getstring.
// The window is NOT centred — `x_px`/`y_px` go straight to sub_43C734, and
// both RE'd callers pass the literal (100, 100). No WINZ 9-patch is painted
// (no sub_41726B call in the body), so the panel is the flat (88,84,80) grey.
//
// `item_text_w` is the widest ITEM's measured width (sub_42FEF0's max), NOT
// pre-maxed with the title — the widget folds the title in itself:
// win_w = max(item_text_w + 16, measure(title)) + 20.
//
// `visible_rows` / `total_rows` / `top_row` are sub_42DBCC's own three list
// counters. Only the thumb's Y reads the last two, and only through
// list_dialog_thumb_y(), which carries the disassembly it mirrors — in short:
// the offset is proportional to `top_row` (the FIRST VISIBLE row, never the
// highlighted one) over `total_rows - visible_rows`, and the whole computation
// is skipped when the list fits, leaving the thumb at the top of the track.
//
// `footer_lines` reserves that many extra text lines INSIDE the window between
// the item area and the "Done" button, reported as `footer_y0` — the PORT-ONLY
// room for the online lobby's key hints (ADR-0011), which have no home in
// sub_42DBCC's own chrome. It defaults to 0, so every RE'd caller (the help
// browser, the *.SCH picker) keeps the pinned geometry exactly.
ListDialogLayout draw_list_dialog(SDL_Renderer* ren, const FontTextures& font,
                                  const std::string& title, float x_px, float y_px,
                                  float item_text_w, int visible_rows, int total_rows, int top_row,
                                  int footer_lines = 0);

// The selection highlight for a list row. The original does NOT invert: it
// runs sub_442C28 @0x42DF80 over the selected row's rectangle (item_w x
// fontheight), a per-pixel remap through byte_495390's runtime blend LUT at
// column 0x93 — a lerp of each 5-bit channel toward white. Applied to the
// base coat that is exactly the sub_432298 BUTTON FACE wash (sub_432298 calls
// the same sub_442C28 at 0x4323DB on the same base coat), so the band colour
// here is the button face by construction rather than a new constant.
//
// The row's already-drawn TEXT is washed too, but the general white ink
// (240,248,252) = 5-bit (30,31,31) is at the ramp ceiling and does not move,
// so callers keep drawing selected rows in their NORMAL ink — no dark-on-light
// inversion. `visible_index` is 0-based within the visible window.
void draw_list_selection(SDL_Renderer* ren, const ListDialogLayout& lay, int visible_index);

// --- the list dialog's INPUT side ----------------------------------------
//
// sub_42DBCC is mouse-first (list_dialog_geometry.hpp's "INPUT MODEL" block
// carries the addresses). The DECISIONS live in that SDL-free header so the
// headless suite can drive them; these two are only the font/SDL adapters
// the screens need to reach them.

// The same geometry draw_list_dialog builds, for the identical arguments —
// so a screen can hit-test exactly what it last drew.
ListDialogGeometry list_dialog_layout_for(const FontTextures& font, const std::string& title,
                                          float x_px, float y_px, float item_text_w,
                                          int visible_rows, int total_rows, int top_row,
                                          int footer_lines = 0);

// Which hotspot a screen-space point lands on. Measures the two arrow
// glyphs' and the "Done" label's button boxes, then defers to
// list_dialog_hit_test().
ListDialogHit list_dialog_hit_for(const FontTextures& font, const ListDialogGeometry& g,
                                  int visible_rows, float mx, float my);

// One SDL keycode translated to the code sub_42DBCC's own switch expects
// (kListKeyUp &c), or 0 for a key the widget does not handle. Only the keys
// the ORIGINAL binds are mapped — there is no W/S alias and no Space.
int list_dialog_key_code(SDL_Keycode key);

// The logical (640x480) point of a mouse event, for a screen that drives the
// list model inline rather than through a widget class. Returns false for a
// non-mouse event.
bool list_mouse_point(SDL_Renderer* ren, const SDL_Event& ev, float& x, float& y);

// Route one SDL event's mouse half into a list widget (anything exposing the
// on_mouse_move/down/up trio). Returns true when the event WAS a mouse event,
// so a screen pump can `continue` past it. The only thing living here is the
// window->logical coordinate conversion — the house pattern from
// keyremap_screen's runner, which SDL_SetRenderLogicalPresentation makes a
// one-liner.
template <class ListWidget>
inline bool dispatch_list_mouse(SDL_Renderer* ren, const SDL_Event& ev, ListWidget& w) {
    float lx = 0.0f, ly = 0.0f;
    if (!list_mouse_point(ren, ev, lx, ly)) return false;
    // Only the LEFT button reaches the widget: every id sub_42DBCC registers
    // sits in the left-button pair of slots (+0x20/+0x24), and the right pair
    // (+0x28/+0x2c) is -1 throughout. A right click is swallowed, not passed
    // on to the key path.
    if (ev.type == SDL_EVENT_MOUSE_MOTION)
        w.on_mouse_move(lx, ly, ev.motion.state != 0);
    else if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev.button.button == SDL_BUTTON_LEFT)
        w.on_mouse_down(lx, ly);
    else if (ev.type == SDL_EVENT_MOUSE_BUTTON_UP && ev.button.button == SDL_BUTTON_LEFT)
        w.on_mouse_up(lx, ly);
    return true;
}

// sub_414340 — the ACKNOWLEDGE modal (PINNED from the body, pseudo.c
// 17003-17107): two centered lines (top = the EAX half of the packed 64-bit
// string argument — getstring(95) "NOTE!" at every UI call site — bottom =
// its EDX half), width =
// max(measure(top), measure(bottom), 80) + 64, height = 4*fontheight + 64 +
// linesHeight (2*fontheight when both lines are non-empty), vertically AND
// horizontally centered, WINZ 9-patch (sub_41726B @ 17070), lines at
// window-relative y = fontheight+32 and +fontheight+2 more, centered via
// sub_4172BA's `cx - (w+2)/2`, and ONE sub_432298 button labelled
// getstring(27) " Ok " at x = width/2 - 32, y = height - 32 - fontheight - 6,
// widget id 27 (clicking posts the Esc code). Its key loop plays the nav
// blip (sub_427961(20)) for ANY real key and closes only on Enter(13)/
// Space(32)/Esc(27) — the CALLER owns that loop; these helpers only draw.
DialogRect acknowledge_dialog_rect(const FontTextures& font, const std::string& top,
                                   const std::string& bottom);
// The Ok button's screen-space rect inside `win` (sub_432298's text-derived
// size at sub_414340's pinned position) for callers that hit-test the click.
DialogRect acknowledge_ok_rect(const FontTextures& font, const DialogRect& win,
                               const std::string& ok_label);
void draw_acknowledge_dialog(SDL_Renderer* ren, const FontTextures& font, const Sprite* winz,
                             const std::string& top, const std::string& bottom,
                             const std::string& ok_label, Uint8 ink_r, Uint8 ink_g, Uint8 ink_b,
                             bool ok_pressed = false);

// The sub_41456C confirm-dialog family (PINNED geometry, pseudo.c
// 17128-17277): vertically AND horizontally centered, height = `4*fontheight
// + 64 + linesHeight` (linesHeight = fontheight for one line, 2*fontheight
// for two — CONFIRMED pseudo.c 17176/17190; the "+2" at 17207 only offsets
// line2's DRAW position below line1, it is not part of the window height —
// sub_41456C packs up to two prompt lines into one 64-bit argument, one
// pointer per half),
// width = `max(textwidth, 80) + 64`, buttons bottom-anchored at
// `y = height - 32 - fontheight - 6`, x = `width/2 - 80` (first/"Yes") and
// `width/2 + 22` (second/"No") — both CONFIRMED literal offsets read
// directly from the button-placement calls. Pass an empty `line2` for a
// single-line prompt (the quit-confirm's own shape). sub_41456C paints the
// WINZ 9-patch over its window (sub_41726B @ pseudo.c 17200) — pass the
// loaded WINZ sprite. The prompt INK is the CALLER's third argument:
// sub_412987 passes byte_49A390 (LUT offset 0x5000 -> idx 248 -> (164,0,0),
// a dark red) for the quit prompt; the editor confirms pass the general
// white — so the ink is a parameter here, outlined via
// draw_dialog_text (frontend-flow.md "sub_41696C — outlined dialog text").
// NOT pinned: which of the two packed lines (the low or the high half of the
// 64-bit argument) actually lands on TOP at the pixel level — the recovered
// register spill for that packing (pseudo.c 17167-17190) is ambiguous the
// same way sub_43C734's own X-placement is (see that TODO(RE)); this
// primitive always draws `line1` first (top) as the more legible ordering,
// a port convention rather than a confirmed fact.
//
// The OUTLINE colour (sub_41696C's a7) is a per-caller argument too. It
// defaults to black (byte_495390[0]), which is what the editor confirms
// pass; the main-menu quit prompt overrides it with a GOLD outline,
// (252,248,88) = byte_49D37A "percent readout yellow", giving the red-fill/
// gold-border emphasised look the original shows for that one dialog. The
// RE records sub_412987's outline arg as decompiler-lost ("black per every
// sibling call site", frontend-flow.md "Escape/Quit-row confirm dialog"),
// so this gold is PHOTO-DERIVED pending a binary re-read of that a2/a7 slot.
void draw_confirm_dialog(SDL_Renderer* ren, const FontTextures& font, const Sprite* winz,
                         const std::string& line1, const std::string& line2,
                         const std::string& yes_label, const std::string& no_label, Uint8 ink_r,
                         Uint8 ink_g, Uint8 ink_b, Uint8 outline_r = 0, Uint8 outline_g = 0,
                         Uint8 outline_b = 0);

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
