#pragma once

// The shared sub_43C734 window primitive + sub_432298 button widget — PINNED
// (docs/re/frontend-flow.md "The sub_43C734 dialog-chrome primitive",
// "sub_432298 — the button widget"). Every modal window in the original
// front-end opens through this ONE window constructor and draws its buttons
// through this ONE button widget: the boot LOADING dialog (sub_412E33), the
// main-menu quit confirm (sub_412987 -> sub_41456C), and — confirmed by a
// direct body read for this task, docs/re/results-and-options.md #5's
// "exact dialog chrome" item — the scheme editor's Ctrl+B reset / Ctrl+F
// fill / Esc save confirms (also sub_41456C), the powerup sub-editor's
// Forbidden/HasOverride prompts (the lighter sub_42EDE0 variant), and every
// text-entry prompt in the game (density/name/filename/born-with/override,
// sub_42E938). One shared implementation here keeps every caller
// pixel-identical instead of re-deriving the same grey fill/bevel per
// screen — exactly the "reuse pinned facts/helpers, do not re-derive"
// instruction this header exists to satisfy.

#include <SDL3/SDL.h>

#include <string>

#include "bomber/game/bmscreen.hpp"

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

// dword_45C46C = 10570 -> RGB(82,82,82): the ONE flat fill colour every
// sub_43C734 window in this family uses (colormode 256, the theme default —
// docs/re/frontend-flow.md "Fill colour resolution").
inline constexpr Uint8 kDialogFillR = 82, kDialogFillG = 82, kDialogFillB = 82;

// The one flat fill sub_43C734/sub_43D1C0 draw — NO border/shadow/bevel
// anywhere in the window body (frontend-flow.md, same section).
void draw_dialog_chrome(SDL_Renderer* ren, const DialogRect& r);

// sub_432298 — text-derived size, a real 2px inset bevel (light top/left,
// dark bottom/right — a raised "up" look, the only state drawn here), closed
// with a 1px black outline, label in the text-shadow ink (165,165,165).
// `x, y` are WINDOW-relative top-left pixels.
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
// single-line prompt (the quit-confirm's own shape, docs/re/frontend-flow.md
// "Escape/Quit-row confirm dialog"). The editor's Ctrl+B/Ctrl+F/Esc-save
// confirms all route through this SAME primitive with a real two-line prompt
// (a case-specific line + the shared "yes/no" line getstring(97) or (95)
// — docs/re/results-and-options.md #5, "The sub_41456C two-line prompt").
// NOT pinned: which of the two packed lines (`LODWORD`/`HIDWORD` of the
// `__int64` arg) actually lands on TOP at the pixel level — the decompiler's
// register-spill for that packing (pseudo.c 17167-17190) is ambiguous the
// same way sub_43C734's own X-placement is (see that TODO(RE) above); this
// primitive always draws `line1` first (top) as the more legible ordering,
// a port convention rather than a confirmed fact.
void draw_confirm_dialog(SDL_Renderer* ren, const FontTextures& font, const std::string& line1,
                         const std::string& line2, const std::string& yes_label,
                         const std::string& no_label);

// The sub_42EDE0 family (PARTIALLY pinned, pseudo.c ~32897-32920) — a
// lighter Yes/No variant used only by the powerup sub-editor's
// Forbidden/HasOverride prompts (docs/re/results-and-options.md #5b).
// CONFIRMED: same sub_43C734 colormode-256 grey fill, same sub_432298 button
// bevel, and — unlike sub_41456C's getstring(26)/(25) lookup — HARDCODED
// literal "Yes"/"No" labels (`aYes`/`aNo` in the decompile, not a message-
// table call) at the CONFIRMED tighter offsets `center-64`/`center+16`. The
// exact width/height baseline is decompiler-ambiguous (the intermediate
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
// CONFIRMED: same chrome family, height = `5*fontheight + 16`, HARDCODED
// literal "Done"/"Cancel" labels (`aDone_0`/`aCancel`, not message-table
// lookups). Width baseline has the same register-spilled ambiguity noted
// above for `draw_compact_confirm_dialog`; the port uses the same
// content-driven-width-with-minimum-160-clamp shape.
void draw_text_entry_dialog(SDL_Renderer* ren, const FontTextures& font, float y_px,
                            const std::string& label, const std::string& entry_text,
                            const std::string& done_label, const std::string& cancel_label);

}  // namespace bomber::game
