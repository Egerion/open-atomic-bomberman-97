#include "bomber/game/dialog_chrome.hpp"

#include <algorithm>

#include "bomber/game/renderer.hpp"  // kScreenW/kScreenH

namespace bomber::game {

namespace {

// Bevel/ink globals — siblings of the window fill colour, from the SAME
// sub_43C150 init block (literal LUT offsets), decoded through COLOR.PAL's
// REAL LUT bytes + the shared UI palette region (docs/re/frontend-flow.md
// "COLOR.PAL — byte_495390 decoded for real"; these CORRECT the earlier
// nearest-colour-search values (123,123,123)/(66,66,66)/(165,165,165)):
//   dword_45C470 = 15855 -> LUT idx  60 -> (108,116,128)  bevel light
//   dword_45C474 =  8456 -> LUT idx 142 -> ( 60, 68, 56)  bevel dark
//   dword_45C478 = 21140 -> LUT idx 178 -> (168,168,164)  button label ink
constexpr Uint8 kBevelLightR = 108, kBevelLightG = 116, kBevelLightB = 128;  // dword_45C470
constexpr Uint8 kBevelDarkR = 60, kBevelDarkG = 68, kBevelDarkB = 56;        // dword_45C474
constexpr Uint8 kButtonInkR = 168, kButtonInkG = 168, kButtonInkB = 164;     // dword_45C478

// Button face — the window base coat (idx 205) run through sub_442C28's
// whole-bitmap brightness wash (byte_475390 lighten-ramp entry 0x93 = step
// 19 of 128 toward white; the ramp scale factor is a decompiler-lost
// register, reconstructed as the canonical 128-step <<9 ramp): 5-bit
// (11,10,10) -> (13,13,13) -> LUT idx 123 -> (108,112,108). See
// frontend-flow.md "sub_432298 — the button widget (RE-PINNED)".
constexpr Uint8 kButtonFaceR = 108, kButtonFaceG = 112, kButtonFaceB = 108;

// sub_416B43 splits the source into a fixed 3x3 grid (v22 = srcW/3,
// v23 = srcH/3) — for the 72x72 WINZ.PCX that is 24-px cells.
constexpr float kPatchCells = 3.0f;

float line_h(const FontTextures& font) {
    return static_cast<float>(font.loaded() ? font.line_height() : 12);
}

float text_w(const FontTextures& font, const std::string& s) {
    return font.loaded() ? static_cast<float>(font.measure(s)) : 0.0f;
}

// One tiled band of a 9-patch: repeats the source cell across the dest rect,
// clipping the last partial tile — sub_416B43's `if (i + cell < extent) v =
// cell; else v = extent - i` loops, translated to src/dst rect pairs.
void tile_patch(SDL_Renderer* ren, SDL_Texture* tex, const SDL_FRect& src, float dx, float dy,
                float dw, float dh) {
    // Integer tile index (not a float loop counter): ox/oy are derived as
    // index*cell, mirroring sub_416B43's integer `i`-stepped extent walk and
    // avoiding accumulated float drift.
    for (int ix = 0; ix * src.w < dw; ++ix) {
        const float ox = ix * src.w;
        const float w = std::min(src.w, dw - ox);
        for (int iy = 0; iy * src.h < dh; ++iy) {
            const float oy = iy * src.h;
            const float h = std::min(src.h, dh - oy);
            SDL_FRect s{src.x, src.y, w, h};
            SDL_FRect d{dx + ox, dy + oy, w, h};
            SDL_RenderTexture(ren, tex, &s, &d);
        }
    }
}

}  // namespace

DialogRect dialog_rect(float y_px, float height_px, float width_px) {
    return DialogRect{(static_cast<float>(kScreenW) - width_px) / 2, y_px, width_px, height_px};
}

DialogRect dialog_rect_vcentered(float height_px, float width_px) {
    return dialog_rect((static_cast<float>(kScreenH) - height_px) / 2, height_px, width_px);
}

void draw_dialog_chrome(SDL_Renderer* ren, const DialogRect& r, const Sprite* winz) {
    if (winz == nullptr || winz->tex == nullptr) {
        // The constructor's flat base coat only — the look the editor's
        // sub_42E938/sub_42EDE0 prompts keep (no sub_41726B call there), and
        // the fallback when WINZ.PCX is missing from the install.
        SDL_FRect box{r.x, r.y, r.w, r.h};
        SDL_SetRenderDrawColor(ren, kDialogFillR, kDialogFillG, kDialogFillB, 255);
        SDL_RenderFillRect(ren, &box);
        return;
    }
    // sub_41726B -> sub_416B43: 9-patch of the WINZ image over the whole
    // window. Cell = src/3 (24 px). Original draw order (center, then
    // top/bottom edges, then left/right edges, then the four pinned corners)
    // reproduced as-is — later bands overpaint the tiled center exactly like
    // the original's separate loops do.
    const float cw = static_cast<float>(winz->w) / kPatchCells;
    const float ch = static_cast<float>(winz->h) / kPatchCells;
    SDL_Texture* tex = winz->tex;
    // Center cell tiled across the WHOLE window (the original's first loop
    // runs the full 0..w / 0..h range; the edge/corner bands then overwrite
    // their strips).
    tile_patch(ren, tex, SDL_FRect{cw, ch, cw, ch}, r.x, r.y, r.w, r.h);
    // Top / bottom edge cells, tiled horizontally.
    tile_patch(ren, tex, SDL_FRect{cw, 0, cw, ch}, r.x, r.y, r.w, std::min(ch, r.h));
    tile_patch(ren, tex, SDL_FRect{cw, 2 * ch, cw, ch}, r.x, r.y + r.h - ch, r.w, ch);
    // Left / right edge cells, tiled vertically.
    tile_patch(ren, tex, SDL_FRect{0, ch, cw, ch}, r.x, r.y, std::min(cw, r.w), r.h);
    tile_patch(ren, tex, SDL_FRect{2 * cw, ch, cw, ch}, r.x + r.w - cw, r.y, cw, r.h);
    // Four pinned corners.
    SDL_FRect tl_s{0, 0, cw, ch}, tl_d{r.x, r.y, cw, ch};
    SDL_RenderTexture(ren, tex, &tl_s, &tl_d);
    SDL_FRect tr_s{2 * cw, 0, cw, ch}, tr_d{r.x + r.w - cw, r.y, cw, ch};
    SDL_RenderTexture(ren, tex, &tr_s, &tr_d);
    SDL_FRect bl_s{0, 2 * ch, cw, ch}, bl_d{r.x, r.y + r.h - ch, cw, ch};
    SDL_RenderTexture(ren, tex, &bl_s, &bl_d);
    SDL_FRect br_s{2 * cw, 2 * ch, cw, ch}, br_d{r.x + r.w - cw, r.y + r.h - ch, cw, ch};
    SDL_RenderTexture(ren, tex, &br_s, &br_d);
}

void draw_dialog_text(SDL_Renderer* ren, const FontTextures& font, const std::string& text,
                      float x, float y, Uint8 r, Uint8 g, Uint8 b) {
    // sub_41696C: four outline passes in black (byte_495390[0] at every
    // visible call site), then the ink pass on top. The four pass offsets are
    // register-lost in the decompile; the four cardinal 1-px offsets are the
    // only reading that yields the classic 1-px text outline the count
    // implies.
    font.draw(ren, text, x - 1, y, 0, 0, 0);
    font.draw(ren, text, x + 1, y, 0, 0, 0);
    font.draw(ren, text, x, y - 1, 0, 0, 0);
    font.draw(ren, text, x, y + 1, 0, 0, 0);
    font.draw(ren, text, x, y, r, g, b);
}

void draw_dialog_button(SDL_Renderer* ren, const FontTextures& font, float x, float y,
                        const std::string& label, bool pressed) {
    const float label_w = text_w(font, label);
    const float h = line_h(font);
    const float w = label_w + 16.0f;
    const float bh = h + 6.0f;

    // sub_432298 "up" bitmap, in its own order: face fill (the washed base
    // coat), two light/dark rings at insets 2 and 1 (sub_44240C x2), then
    // the 1-px black outline at the very edge (sub_442384). Rings drawn as
    // per-edge 1-px strips: light top+left, dark bottom+right. The "down"
    // bitmap (pseudo.c 35238-35256) swaps the ring colour pair and skips the
    // sub_442C28 wash — its face is the raw window base coat.
    SDL_FRect face{x, y, w, bh};
    if (pressed)
        SDL_SetRenderDrawColor(ren, kDialogFillR, kDialogFillG, kDialogFillB, 255);
    else
        SDL_SetRenderDrawColor(ren, kButtonFaceR, kButtonFaceG, kButtonFaceB, 255);
    SDL_RenderFillRect(ren, &face);
    for (int inset = 2; inset >= 1; --inset) {
        const float x0 = x + static_cast<float>(inset), y0 = y + static_cast<float>(inset);
        const float rw = w - static_cast<float>(2 * inset), rh = bh - static_cast<float>(2 * inset);
        if (pressed)
            SDL_SetRenderDrawColor(ren, kBevelDarkR, kBevelDarkG, kBevelDarkB, 255);
        else
            SDL_SetRenderDrawColor(ren, kBevelLightR, kBevelLightG, kBevelLightB, 255);
        SDL_FRect top{x0, y0, rw, 1};
        SDL_RenderFillRect(ren, &top);
        SDL_FRect left{x0, y0, 1, rh};
        SDL_RenderFillRect(ren, &left);
        if (pressed)
            SDL_SetRenderDrawColor(ren, kBevelLightR, kBevelLightG, kBevelLightB, 255);
        else
            SDL_SetRenderDrawColor(ren, kBevelDarkR, kBevelDarkG, kBevelDarkB, 255);
        SDL_FRect bottom{x0, y0 + rh - 1, rw, 1};
        SDL_RenderFillRect(ren, &bottom);
        SDL_FRect right{x0 + rw - 1, y0, 1, rh};
        SDL_RenderFillRect(ren, &right);
    }
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);  // byte_495390[0]
    SDL_FRect outline{x, y, w, bh};
    SDL_RenderRect(ren, &outline);

    font.draw(ren, label, x + (w - label_w) / 2.0f, y + 3.0f, kButtonInkR, kButtonInkG,
              kButtonInkB);
}

void draw_bevel_rect(SDL_Renderer* ren, float x, float y, float w, float h, bool raised,
                     Uint8 face_r, Uint8 face_g, Uint8 face_b) {
    // sub_44240C: fill the interior, then a 1-px light strip on the top+left
    // and a 1-px dark strip on the bottom+right for the raised look (swapped
    // for sunken). The button widget uses the same pair.
    SDL_FRect face{x, y, w, h};
    SDL_SetRenderDrawColor(ren, face_r, face_g, face_b, 255);
    SDL_RenderFillRect(ren, &face);
    const Uint8 lr = raised ? kBevelLightR : kBevelDarkR;
    const Uint8 lg = raised ? kBevelLightG : kBevelDarkG;
    const Uint8 lb = raised ? kBevelLightB : kBevelDarkB;
    const Uint8 dr = raised ? kBevelDarkR : kBevelLightR;
    const Uint8 dg = raised ? kBevelDarkG : kBevelLightG;
    const Uint8 db = raised ? kBevelDarkB : kBevelLightB;
    SDL_SetRenderDrawColor(ren, lr, lg, lb, 255);
    SDL_FRect top{x, y, w, 1};
    SDL_RenderFillRect(ren, &top);
    SDL_FRect left{x, y, 1, h};
    SDL_RenderFillRect(ren, &left);
    SDL_SetRenderDrawColor(ren, dr, dg, db, 255);
    SDL_FRect bottom{x, y + h - 1, w, 1};
    SDL_RenderFillRect(ren, &bottom);
    SDL_FRect right{x + w - 1, y, 1, h};
    SDL_RenderFillRect(ren, &right);
}

ListDialogLayout draw_list_dialog(SDL_Renderer* ren, const FontTextures& font,
                                  const std::string& title, float y_px, float content_w,
                                  int visible_rows, int total_rows, int top_row) {
    const float h = line_h(font);
    // Window: title strip (1 line) + a small gap + `visible_rows` item lines
    // + a "Done" button row. Width = content + side padding (the audit's
    // "+20" over the widest of title/items; the caller already sized
    // content_w to the widest column). x is auto-centred.
    const float win_w = content_w + 20.0f;
    const float win_h = (h + 8.0f) + 8.0f + static_cast<float>(visible_rows) * h + (h + 12.0f);
    const DialogRect win = dialog_rect(y_px, win_h, win_w);

    // Raised panel with a 1-px black outer rect (sub_442384 + the bevel).
    draw_bevel_rect(ren, win.x, win.y, win.w, win.h, /*raised=*/true, kDialogFillR, kDialogFillG,
                    kDialogFillB);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_FRect outline{win.x, win.y, win.w, win.h};
    SDL_RenderRect(ren, &outline);

    // Sunken title strip at (5,5), the centred grey title inside it.
    const float strip_h = h + 8.0f;
    draw_bevel_rect(ren, win.x + 5.0f, win.y + 5.0f, win.w - 10.0f, strip_h, /*raised=*/false,
                    kDialogFillR, kDialogFillG, kDialogFillB);
    const float tw = text_w(font, title);
    draw_dialog_text(ren, font, title, win.x + (win.w - tw) / 2.0f, win.y + 5.0f + 4.0f,
                     kButtonInkR, kButtonInkG, kButtonInkB);

    const bool scrollbar = total_rows > visible_rows;
    ListDialogLayout lay{};
    lay.win = win;
    lay.item_x = win.x + 8.0f;
    lay.item_y0 = win.y + strip_h + 8.0f;
    lay.item_h = h;
    lay.item_w = win.w - 16.0f - (scrollbar ? 18.0f : 0.0f);
    lay.has_scrollbar = scrollbar;

    if (scrollbar) {
        // Right-hand scrollbar: up/down arrow buttons + a sunken track + a
        // proportional raised thumb (sub_42DBCC 32439-32469). FONT6 glyphs
        // \x18/\x19 are the arrows.
        const float sx = win.x + win.w - 17.0f;
        const float track_y0 = lay.item_y0;
        const float track_h = static_cast<float>(visible_rows) * h;
        draw_dialog_button(ren, font, sx, track_y0 - h - 4.0f, "\x18");
        draw_dialog_button(ren, font, sx, track_y0 + track_h + 2.0f, "\x19");
        draw_bevel_rect(ren, sx, track_y0, 15.0f, track_h, /*raised=*/false, kDialogFillR,
                        kDialogFillG, kDialogFillB);
        const float frac = static_cast<float>(visible_rows) / static_cast<float>(total_rows);
        const float thumb_h = std::max(track_h * frac, 8.0f);
        const float max_top = static_cast<float>(total_rows - visible_rows);
        const float pos = max_top > 0 ? static_cast<float>(top_row) / max_top : 0.0f;
        const float thumb_y = track_y0 + pos * (track_h - thumb_h);
        draw_bevel_rect(ren, sx, thumb_y, 15.0f, thumb_h, /*raised=*/true, kButtonFaceR,
                        kButtonFaceG, kButtonFaceB);
    }

    // "Done" button centred at the bottom (w/2 - 32).
    lay.done_x = win.x + win.w / 2.0f - 32.0f;
    lay.done_y = win.y + win.h - h - 8.0f;
    draw_dialog_button(ren, font, lay.done_x, lay.done_y, "Done");
    return lay;
}

void draw_list_selection(SDL_Renderer* ren, const ListDialogLayout& lay, int visible_index) {
    // The original inverts the video under the selected row (sub_442C28); in
    // truecolour we fill a bevel-light band the caller draws its dark-ink
    // item text over.
    SDL_FRect band{lay.item_x - 2.0f, lay.item_y0 + static_cast<float>(visible_index) * lay.item_h,
                   lay.item_w, lay.item_h};
    SDL_SetRenderDrawColor(ren, kBevelLightR, kBevelLightG, kBevelLightB, 255);
    SDL_RenderFillRect(ren, &band);
}

DialogRect acknowledge_dialog_rect(const FontTextures& font, const std::string& top,
                                   const std::string& bottom) {
    const float h = line_h(font);
    const float lines_h = (!top.empty() && !bottom.empty()) ? 2.0f * h : h;
    const float win_w = std::max(std::max(text_w(font, top), text_w(font, bottom)), 80.0f) + 64.0f;
    const float win_h = 4.0f * h + 64.0f + lines_h;
    return dialog_rect_vcentered(win_h, win_w);
}

DialogRect acknowledge_ok_rect(const FontTextures& font, const DialogRect& win,
                               const std::string& ok_label) {
    const float h = line_h(font);
    // sub_414340's pinned button placement: x = width/2 - 32,
    // y = height - 32 - fontheight - 6 (window-relative); the button sizes
    // itself from its label inside sub_432298 (measure+16 x fontheight+6).
    return DialogRect{win.x + win.w / 2.0f - 32.0f, win.y + win.h - 32.0f - h - 6.0f,
                      text_w(font, ok_label) + 16.0f, h + 6.0f};
}

void draw_acknowledge_dialog(SDL_Renderer* ren, const FontTextures& font, const Sprite* winz,
                             const std::string& top, const std::string& bottom,
                             const std::string& ok_label, Uint8 ink_r, Uint8 ink_g, Uint8 ink_b,
                             bool ok_pressed) {
    const float h = line_h(font);
    DialogRect win = acknowledge_dialog_rect(font, top, bottom);
    draw_dialog_chrome(ren, win, winz);

    // Lines at window-relative y = fontheight+32 (top) and +fontheight+2
    // more (bottom), centered via sub_4172BA's `cx - (w+2)/2`.
    const float line1_y = win.y + h + 32.0f;
    if (!top.empty())
        draw_dialog_text(ren, font, top, win.x + win.w / 2.0f - (text_w(font, top) + 2.0f) / 2.0f,
                         line1_y, ink_r, ink_g, ink_b);
    if (!bottom.empty())
        draw_dialog_text(ren, font, bottom,
                         win.x + win.w / 2.0f - (text_w(font, bottom) + 2.0f) / 2.0f,
                         line1_y + h + 2.0f, ink_r, ink_g, ink_b);

    DialogRect ok = acknowledge_ok_rect(font, win, ok_label);
    draw_dialog_button(ren, font, ok.x, ok.y, ok_label, ok_pressed);
}

void draw_confirm_dialog(SDL_Renderer* ren, const FontTextures& font, const Sprite* winz,
                         const std::string& line1, const std::string& line2,
                         const std::string& yes_label, const std::string& no_label, Uint8 ink_r,
                         Uint8 ink_g, Uint8 ink_b) {
    const float h = line_h(font);
    const float w1 = text_w(font, line1);
    const float w2 = line2.empty() ? 0.0f : text_w(font, line2);
    // v31 in sub_41456C is `2 * fontheight` for two lines (pseudo.c 17176),
    // NOT `2*fontheight + 2` — the "+2" gap (pseudo.c 17207's `v15 + 2 +
    // v35`) only offsets line2's DRAW position below line1, it is not added
    // to the window height itself.
    const float lines_h = line2.empty() ? h : (2.0f * h);
    const float win_w = std::max(std::max(w1, w2), 80.0f) + 64.0f;
    const float win_h = 4.0f * h + 64.0f + lines_h;
    DialogRect win = dialog_rect_vcentered(win_h, win_w);
    draw_dialog_chrome(ren, win, winz);

    // Prompt centring is sub_4172BA's exact `cx - (w+2)/2` (pseudo.c
    // 18703-18704) — one px left of a plain (win.w - w)/2 for even widths
    // (level&rounds audit 2026-07-12; the port's last unexplained constant).
    const float line1_y = win.y + h + 32.0f;
    draw_dialog_text(ren, font, line1, win.x + win.w / 2.0f - (w1 + 2.0f) / 2.0f, line1_y, ink_r,
                     ink_g, ink_b);
    if (!line2.empty())
        draw_dialog_text(ren, font, line2, win.x + win.w / 2.0f - (w2 + 2.0f) / 2.0f,
                         line1_y + h + 2.0f, ink_r, ink_g, ink_b);

    const float btn_y = win.y + win.h - 32.0f - h - 6.0f;
    draw_dialog_button(ren, font, win.x + win.w / 2.0f - 80.0f, btn_y, yes_label);
    draw_dialog_button(ren, font, win.x + win.w / 2.0f + 22.0f, btn_y, no_label);
}

void draw_compact_confirm_dialog(SDL_Renderer* ren, const FontTextures& font,
                                 const std::string& line, const std::string& yes_label,
                                 const std::string& no_label) {
    const float h = line_h(font);
    const float win_w = std::max(text_w(font, line), 128.0f) + 32.0f;  // see header's width note
    const float win_h = 3.0f * h + 16.0f;                              // CONFIRMED formula
    DialogRect win = dialog_rect_vcentered(win_h, win_w);
    draw_dialog_chrome(ren, win, nullptr);  // sub_42EDE0 has no sub_41726B call — flat

    draw_dialog_text(ren, font, line, win.x + (win.w - text_w(font, line)) / 2.0f,
                     win.y + h / 2.0f + 4.0f, kDialogInkR, kDialogInkG, kDialogInkB);

    const float btn_y = win.y + win.h - h - 12.0f;
    draw_dialog_button(ren, font, win.x + win.w / 2.0f - 64.0f, btn_y, yes_label);
    draw_dialog_button(ren, font, win.x + win.w / 2.0f + 16.0f, btn_y, no_label);
}

void draw_text_entry_dialog(SDL_Renderer* ren, const FontTextures& font, float y_px,
                            const std::string& label, const std::string& entry_text,
                            const std::string& done_label, const std::string& cancel_label) {
    const float h = line_h(font);
    const float label_w = text_w(font, label);
    const float entry_w = text_w(font, entry_text);
    const float win_w = std::max(std::max(label_w, entry_w) + 28.0f, 160.0f);  // see header note
    const float win_h = 5.0f * h + 16.0f;                                      // CONFIRMED formula
    DialogRect win = dialog_rect(y_px, win_h, win_w);
    draw_dialog_chrome(ren, win, nullptr);  // sub_42E938 has no sub_41726B call — flat

    draw_dialog_text(ren, font, label, win.x + (win.w - label_w) / 2.0f, win.y + h * 0.5f,
                     kDialogInkR, kDialogInkG, kDialogInkB);
    // The live editable buffer, with a plain text-cursor caret (our own
    // reproduction; sub_42FF1C's real caret draw wasn't pinned by this pass).
    std::string shown = entry_text + "_";
    font.draw(ren, shown, win.x + (win.w - text_w(font, shown)) / 2.0f, win.y + h * 2.0f, 255, 220,
              80);  // our own selection-tint ink, not an RE'd colour

    const float btn_y = win.y + win.h - h - 10.0f;
    draw_dialog_button(ren, font, win.x + win.w / 2.0f - 72.0f, btn_y, done_label);
    draw_dialog_button(ren, font, win.x + win.w / 2.0f + 8.0f, btn_y, cancel_label);
}

}  // namespace bomber::game
