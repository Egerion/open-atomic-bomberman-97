#include "bomber/ui/dialog_chrome.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "bomber/game_util/list_dialog_geometry.hpp"  // sub_42DBCC's pinned pixel geometry
#include "bomber/render/renderer.hpp"                 // kScreenW/kScreenH

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
// dword_45C478 — the one exported in the header (screens dim un-actionable text
// with it), aliased here so the chrome keeps reading in its own vocabulary.
constexpr Uint8 kButtonInkR = kDialogDimR, kButtonInkG = kDialogDimG, kButtonInkB = kDialogDimB;

// Button face — the window base coat (idx 205) run through sub_442C28's
// whole-bitmap brightness wash (byte_475390 lighten-ramp entry 0x93 = step
// 19 of 128 toward white; the ramp scale factor is a decompiler-lost
// register, reconstructed as the canonical 128-step <<9 ramp): 5-bit
// (11,10,10) -> (13,13,13) -> LUT idx 123 -> (108,112,108). See
// frontend-flow.md "sub_432298 — the button widget (RE-PINNED)".
constexpr Uint8 kButtonFaceR = 108, kButtonFaceG = 112, kButtonFaceB = 108;

// sub_416B43 splits the source into a fixed 3x3 grid (cell width = source
// width / 3, cell height = source height / 3) — for the 72x72 WINZ.PCX that is
// 24-px cells.
constexpr float kPatchCells = 3.0f;

float line_h(const FontTextures& font) {
    return static_cast<float>(font.loaded() ? font.line_height() : 12);
}

float text_w(const FontTextures& font, const std::string& s) {
    return font.loaded() ? static_cast<float>(font.measure(s)) : 0.0f;
}

// One tiled band of a 9-patch: repeats the source cell across the dest rect,
// clipping the last partial tile — sub_416B43's loops take a full cell while
// the walking offset plus one cell still fits inside the extent, and the
// remainder (extent minus offset) otherwise, translated here to src/dst rect
// pairs.
// `src` and the cell geometry are in the sprite's CLASSIC space; `cw`/`ch` are
// its classic pixel size so a DATA_HD texture behind it can be sampled at the
// right scale (sprites.hpp texture_src_rect — identity while HD is off).
void tile_patch(SDL_Renderer* ren, SDL_Texture* tex, int cw, int ch, const SDL_FRect& src,
                float dx, float dy, float dw, float dh) {
    // Integer tile index (not a float loop counter): ox/oy are derived as
    // index*cell, mirroring sub_416B43's integer `i`-stepped extent walk and
    // avoiding accumulated float drift.
    for (int ix = 0; ix * src.w < dw; ++ix) {
        const float ox = ix * src.w;
        const float w = std::min(src.w, dw - ox);
        for (int iy = 0; iy * src.h < dh; ++iy) {
            const float oy = iy * src.h;
            const float h = std::min(src.h, dh - oy);
            SDL_FRect s = texture_src_rect(tex, cw, ch, SDL_FRect{src.x, src.y, w, h});
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
    // Every src rect below is in WINZ's classic 72x72 space; src() rescales it
    // onto whatever texture is actually bound (a 4x DATA_HD WINZ would otherwise
    // hand every band the top-left ninth of its border art).
    const int cls_w = winz->w, cls_h = winz->h;
    const auto src = [tex, cls_w, cls_h](const SDL_FRect& rect) {
        return texture_src_rect(tex, cls_w, cls_h, rect);
    };
    // Center cell tiled across the WHOLE window (the original's first loop
    // runs the full 0..w / 0..h range; the edge/corner bands then overwrite
    // their strips).
    tile_patch(ren, tex, cls_w, cls_h, SDL_FRect{cw, ch, cw, ch}, r.x, r.y, r.w, r.h);
    // Top / bottom edge cells, tiled horizontally.
    tile_patch(ren, tex, cls_w, cls_h, SDL_FRect{cw, 0, cw, ch}, r.x, r.y, r.w,
               std::min(ch, r.h));
    tile_patch(ren, tex, cls_w, cls_h, SDL_FRect{cw, 2 * ch, cw, ch}, r.x, r.y + r.h - ch, r.w,
               ch);
    // Left / right edge cells, tiled vertically.
    tile_patch(ren, tex, cls_w, cls_h, SDL_FRect{0, ch, cw, ch}, r.x, r.y, std::min(cw, r.w),
               r.h);
    tile_patch(ren, tex, cls_w, cls_h, SDL_FRect{2 * cw, ch, cw, ch}, r.x + r.w - cw, r.y, cw,
               r.h);
    // Four pinned corners.
    SDL_FRect tl_s = src(SDL_FRect{0, 0, cw, ch}), tl_d{r.x, r.y, cw, ch};
    SDL_RenderTexture(ren, tex, &tl_s, &tl_d);
    SDL_FRect tr_s = src(SDL_FRect{2 * cw, 0, cw, ch}), tr_d{r.x + r.w - cw, r.y, cw, ch};
    SDL_RenderTexture(ren, tex, &tr_s, &tr_d);
    SDL_FRect bl_s = src(SDL_FRect{0, 2 * ch, cw, ch}), bl_d{r.x, r.y + r.h - ch, cw, ch};
    SDL_RenderTexture(ren, tex, &bl_s, &bl_d);
    SDL_FRect br_s = src(SDL_FRect{2 * cw, 2 * ch, cw, ch}),
              br_d{r.x + r.w - cw, r.y + r.h - ch, cw, ch};
    SDL_RenderTexture(ren, tex, &br_s, &br_d);
}

void draw_dialog_text(SDL_Renderer* ren, const FontTextures& font, const std::string& text,
                      float x, float y, Uint8 r, Uint8 g, Uint8 b, Uint8 outline_r,
                      Uint8 outline_g, Uint8 outline_b) {
    // sub_41696C: four 1-px outline passes in the outline ink (sub_41696C's a7
    // argument — byte_495390[0] = black for every dialog except the quit
    // prompt, which passes a gold), then the ink pass on top. The four pass
    // offsets are register-lost in the decompile; the four cardinal 1-px
    // offsets are the only reading that yields the classic 1-px text outline
    // the count implies.
    font.draw(ren, text, x - 1, y, outline_r, outline_g, outline_b);
    font.draw(ren, text, x + 1, y, outline_r, outline_g, outline_b);
    font.draw(ren, text, x, y - 1, outline_r, outline_g, outline_b);
    font.draw(ren, text, x, y + 1, outline_r, outline_g, outline_b);
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

HintBlock pack_hint_lines(const FontTextures& font, const std::vector<std::string>& parts,
                          const std::vector<std::string>& budget, float max_w) {
    // Greedy pack: keep appending parts to the current line while its BUDGETED
    // width still fits, then break. The budgeted text (the caller's longest
    // variant of a state-dependent hint) drives both the break decision and the
    // reported width, so the two states of a toggle share one stable layout.
    HintBlock block;
    const bool has_budget = budget.size() == parts.size();
    std::string line;         // what gets drawn
    std::string line_budget;  // what it is measured as
    auto flush = [&] {
        if (line_budget.empty()) return;
        block.lines.push_back(line);
        block.width = std::max(block.width, text_w(font, line_budget));
        line.clear();
        line_budget.clear();
    };
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (parts[i].empty()) continue;
        const std::string& b = has_budget ? budget[i] : parts[i];
        std::string candidate = line_budget;
        if (!candidate.empty()) candidate += kHintGap;
        candidate += b;
        if (!line_budget.empty() && text_w(font, candidate) > max_w) {
            flush();
            line = parts[i];
            line_budget = b;
            continue;
        }
        if (!line.empty()) line += kHintGap;
        line += parts[i];
        line_budget = std::move(candidate);
    }
    flush();
    return block;
}

namespace {

// sub_44240C takes INCLUSIVE corner coordinates; draw_bevel_rect takes an
// origin plus a size. One converter, so each bevel below reads exactly like
// the original's (x0, y0, x1, y1) argument quad. `ox`/`oy` are the window
// origin (sub_42DBCC draws window-relative, into the window's own buffer).
void bevel_corners(SDL_Renderer* ren, float ox, float oy, int x0, int y0, int x1, int y1,
                   bool raised, Uint8 fr, Uint8 fg, Uint8 fb) {
    draw_bevel_rect(ren, ox + static_cast<float>(x0), oy + static_cast<float>(y0),
                    static_cast<float>(x1 - x0 + 1), static_cast<float>(y1 - y0 + 1), raised, fr,
                    fg, fb);
}

// Base-coat fill of a window-relative rect (sub_442A5C).
void fill_base(SDL_Renderer* ren, float ox, float oy, int x, int y, int w, int hgt) {
    SDL_FRect r{ox + static_cast<float>(x), oy + static_cast<float>(y), static_cast<float>(w),
                static_cast<float>(hgt)};
    SDL_SetRenderDrawColor(ren, kDialogFillR, kDialogFillG, kDialogFillB, 255);
    SDL_RenderFillRect(ren, &r);
}

}  // namespace

ListDialogLayout draw_list_dialog(SDL_Renderer* ren, const FontTextures& font,
                                  const std::string& title, float x_px, float y_px,
                                  float item_text_w, int visible_rows, int total_rows, int top_row,
                                  int footer_lines) {
    const float h = line_h(font);
    const float tw = text_w(font, title);
    // Every number below comes from list_dialog_geometry(), which carries the
    // per-offset sub_42DBCC citations — including the thumb's Y, the one field
    // `total_rows`/`top_row` feed.
    const ListDialogGeometry g = list_dialog_geometry(
        static_cast<int>(x_px), static_cast<int>(y_px), static_cast<int>(item_text_w),
        static_cast<int>(tw), static_cast<int>(h), visible_rows, footer_lines, total_rows, top_row);

    const float ox = static_cast<float>(g.win_x);
    const float oy = static_cast<float>(g.win_y);
    const DialogRect win{ox, oy, static_cast<float>(g.win_w), static_cast<float>(g.win_h)};

    // sub_43C734's flat colormode-256 base coat (this family paints no WINZ
    // 9-patch), then the 1-px black outer rect @0x42DCF9 and the RAISED bevel
    // at inset 1 @0x42DD39.
    fill_base(ren, ox, oy, 0, 0, g.win_w, g.win_h);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);  // byte_495390[0]
    SDL_FRect outline{win.x, win.y, win.w, win.h};
    SDL_RenderRect(ren, &outline);
    bevel_corners(ren, ox, oy, 1, 1, g.win_w - 2, g.win_h - 2, /*raised=*/true, kDialogFillR,
                  kDialogFillG, kDialogFillB);

    // Title strip: base-coat fill @0x42DD9E, SUNKEN bevel @0x42DE0E, then the
    // centred title in dword_45C478 grey @0x42DDCB.
    fill_base(ren, ox, oy, 5, 5, g.strip_fill_w, g.strip_fill_h);
    bevel_corners(ren, ox, oy, 5, 5, g.strip_x1, g.strip_y1, /*raised=*/false, kDialogFillR,
                  kDialogFillG, kDialogFillB);
    draw_dialog_text(ren, font, title, ox + static_cast<float>(g.title_x),
                     oy + static_cast<float>(g.title_y), kButtonInkR, kButtonInkG, kButtonInkB);

    // Item area: base-coat fill @0x42DEA2 and its SUNKEN frame @0x42DFD2.
    fill_base(ren, ox, oy, g.item_fill_x, g.item_fill_y, g.item_fill_w, g.item_fill_h);
    bevel_corners(ren, ox, oy, 5, g.item_fill_y - 1, g.item_frame_x1, g.item_frame_y1,
                  /*raised=*/false, kDialogFillR, kDialogFillG, kDialogFillB);

    // Scrollbar — drawn UNCONDITIONALLY (no branch guards it in sub_42DBCC),
    // so a list that fits still shows a full-height bar. FONT6 glyphs
    // \x18/\x19 are the arrows; both labels are the hardcoded literals at
    // 0x45AAAC/0x45AAB0, not message-table lookups.
    draw_dialog_button(ren, font, ox + static_cast<float>(g.sb_button_x),
                       oy + static_cast<float>(g.sb_up_y), "\x18");
    draw_dialog_button(ren, font, ox + static_cast<float>(g.sb_button_x),
                       oy + static_cast<float>(g.sb_down_y), "\x19");
    fill_base(ren, ox, oy, g.sb_track_x, g.sb_track_y, g.sb_track_w, g.sb_track_h);
    bevel_corners(ren, ox, oy, g.sb_frame_x0, g.sb_frame_y0, g.sb_frame_x1, g.sb_frame_y1,
                  /*raised=*/false, kDialogFillR, kDialogFillG, kDialogFillB);
    // The thumb: a fixed 15x15 RAISED bevel whose face gets the sub_442C28
    // wash @0x42E1E6 — the same wash sub_432298 gives a button face, hence the
    // same colour. Its Y slides with `top_row` (@0x42E6B5-0x42E6F7); the track
    // fill above stays put, exactly as the original's partial repaint does.
    bevel_corners(ren, ox, oy, g.sb_thumb_x0, g.sb_thumb_y0, g.sb_thumb_x1, g.sb_thumb_y1,
                  /*raised=*/true, kButtonFaceR, kButtonFaceG, kButtonFaceB);

    ListDialogLayout lay{};
    lay.win = win;
    lay.item_x = ox + static_cast<float>(g.item_x);
    lay.item_y0 = oy + static_cast<float>(g.item_y0);
    lay.item_h = h;
    lay.item_w = static_cast<float>(g.item_w);
    lay.footer_y0 = oy + static_cast<float>(g.footer_y0);
    lay.done_x = ox + static_cast<float>(g.done_x);
    lay.done_y = oy + static_cast<float>(g.done_y);
    // "Done" @0x42E072 — the literal at 0x45AAB4, widget id 27 (Esc).
    draw_dialog_button(ren, font, lay.done_x, lay.done_y, "Done");
    return lay;
}

void draw_list_selection(SDL_Renderer* ren, const ListDialogLayout& lay, int visible_index) {
    // sub_442C28 @0x42DF80 over exactly (item_x, item_y0 + i*fontheight),
    // sized item_w x fontheight. See the header: washing the base coat yields
    // the button-face colour by construction, so this invents no new constant
    // and the caller keeps drawing the row in its NORMAL ink.
    SDL_FRect band{lay.item_x, lay.item_y0 + static_cast<float>(visible_index) * lay.item_h,
                   lay.item_w, lay.item_h};
    SDL_SetRenderDrawColor(ren, kButtonFaceR, kButtonFaceG, kButtonFaceB, 255);
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
                         Uint8 ink_g, Uint8 ink_b, Uint8 outline_r, Uint8 outline_g,
                         Uint8 outline_b) {
    const float h = line_h(font);
    const float w1 = text_w(font, line1);
    const float w2 = line2.empty() ? 0.0f : text_w(font, line2);
    // sub_41456C's two-line text height is exactly 2 * fontheight (pseudo.c
    // 17176), NOT 2*fontheight + 2 — the "+2" gap seen at pseudo.c 17207 is
    // added to line1's y when placing line2, so it only offsets line2's DRAW
    // position below line1; it is not added to the window height itself.
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
                     ink_g, ink_b, outline_r, outline_g, outline_b);
    if (!line2.empty())
        draw_dialog_text(ren, font, line2, win.x + win.w / 2.0f - (w2 + 2.0f) / 2.0f,
                         line1_y + h + 2.0f, ink_r, ink_g, ink_b, outline_r, outline_g, outline_b);

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

// --- the list dialog's INPUT side ----------------------------------------

ListDialogGeometry list_dialog_layout_for(const FontTextures& font, const std::string& title,
                                          float x_px, float y_px, float item_text_w,
                                          int visible_rows, int total_rows, int top_row,
                                          int footer_lines) {
    // Deliberately the same call draw_list_dialog makes, argument for
    // argument: a hit test that re-derived the layout could drift from what
    // is on screen.
    return list_dialog_geometry(static_cast<int>(x_px), static_cast<int>(y_px),
                                static_cast<int>(item_text_w),
                                static_cast<int>(text_w(font, title)),
                                static_cast<int>(line_h(font)), visible_rows, footer_lines,
                                total_rows, top_row);
}

ListDialogHit list_dialog_hit_for(const FontTextures& font, const ListDialogGeometry& g,
                                  int visible_rows, float mx, float my) {
    const int fh = static_cast<int>(line_h(font));
    // The arrow buttons carry the FONT6 glyphs draw_list_dialog paints; both
    // boxes are sized from their own label, so take the wider of the two and
    // let the y bands separate them (they never overlap in x anyway).
    const int arrow_w = list_dialog_button_w(
        static_cast<int>(std::max(text_w(font, "\x18"), text_w(font, "\x19"))));
    const int done_w = list_dialog_button_w(static_cast<int>(text_w(font, "Done")));
    const int bh = list_dialog_button_h(fh);
    return list_dialog_hit_test(g, visible_rows, arrow_w, bh, done_w, bh, static_cast<int>(mx),
                                static_cast<int>(my));
}

bool list_mouse_point(SDL_Renderer* ren, const SDL_Event& ev, float& x, float& y) {
    if (ev.type == SDL_EVENT_MOUSE_MOTION) {
        SDL_RenderCoordinatesFromWindow(ren, ev.motion.x, ev.motion.y, &x, &y);
        return true;
    }
    if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN || ev.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        SDL_RenderCoordinatesFromWindow(ren, ev.button.x, ev.button.y, &x, &y);
        return true;
    }
    return false;
}

int list_dialog_key_code(SDL_Keycode key) {
    switch (key) {
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            return kListKeyEnter;
        case SDLK_ESCAPE:
            return kListKeyEscape;
        case SDLK_HOME:
            return kListKeyHome;
        case SDLK_UP:
            return kListKeyUp;
        case SDLK_PAGEUP:
            return kListKeyPageUp;
        case SDLK_END:
            return kListKeyEnd;
        case SDLK_DOWN:
            return kListKeyDown;
        case SDLK_PAGEDOWN:
            return kListKeyPageDown;
        default:
            return 0;
    }
}

}  // namespace bomber::game
