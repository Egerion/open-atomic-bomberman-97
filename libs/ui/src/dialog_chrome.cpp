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
// sub_43C150 init block (literal LUT offsets), decoded through COLOR.PAL's real
// LUT bytes + the shared UI palette region (docs/re/frontend-flow.md "COLOR.PAL
// — byte_495390 decoded for real"; these CORRECT the earlier nearest-colour
// values (123,123,123)/(66,66,66)/(165,165,165)).
constexpr Rgb kBevelLight{108, 116, 128};  // dword_45C470 = 15855 -> idx  60
constexpr Rgb kBevelDark{60, 68, 56};      // dword_45C474 =  8456 -> idx 142
constexpr Rgb kButtonInk = kDialogDim;     // dword_45C478
constexpr Rgb kBaseCoat = kDialogFill;

// The window base coat (idx 205) run through sub_442C28's whole-bitmap
// brightness wash (byte_475390 lighten-ramp entry 0x93 = step 19 of 128 toward
// white; the ramp scale factor is a decompiler-lost register, reconstructed as
// the canonical 128-step <<9 ramp): 5-bit (11,10,10) -> (13,13,13) -> idx 123.
// See frontend-flow.md "sub_432298 — the button widget (RE-PINNED)".
constexpr Rgb kButtonFace{108, 112, 108};

void set_color(SDL_Renderer* ren, Rgb c) {
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, 255);
}

void fill_rect(SDL_Renderer* ren, const SDL_FRect& r, Rgb c) {
    set_color(ren, c);
    SDL_RenderFillRect(ren, &r);
}

float line_h(const FontTextures& font) {
    return static_cast<float>(font.loaded() ? font.line_height() : 12);
}

float text_w(const FontTextures& font, const std::string& s) {
    return font.loaded() ? static_cast<float>(font.measure(s)) : 0.0f;
}

// --- the WINZ 9-patch ------------------------------------------------------

// sub_416B43 splits the source into a fixed 3x3 grid, so the 72x72 WINZ.PCX has
// 24-px cells.
constexpr float kPatchCells = 3.0f;

// The texture behind a 9-patch, plus the CLASSIC pixel size every src rect below
// is expressed in — a 4x DATA_HD WINZ would otherwise hand every band the
// top-left ninth of its border art (sprites.hpp texture_src_rect; identity while
// HD is off).
struct Patch9 {
    SDL_Texture* tex = nullptr;
    int cls_w = 0, cls_h = 0;
};

// One band of the patch: which source cell repeats, and over what dest rect.
struct PatchBand {
    SDL_FRect src;
    SDL_FRect dst;
};

// One clipped tile of a band. sub_416B43's loops take a full cell while the
// walking offset plus one cell still fits inside the extent, and the remainder
// otherwise. The offsets are derived as index*cell from an INTEGER index, not
// accumulated in a float, mirroring the original's i-stepped extent walk.
void blit_patch_cell(SDL_Renderer* ren, const Patch9& p, const PatchBand& b, SDL_Point cell) {
    const float ox = static_cast<float>(cell.x) * b.src.w;
    const float oy = static_cast<float>(cell.y) * b.src.h;
    const float w = std::min(b.src.w, b.dst.w - ox);
    const float h = std::min(b.src.h, b.dst.h - oy);
    const SDL_FRect s =
        texture_src_rect(p.tex, p.cls_w, p.cls_h, SDL_FRect{b.src.x, b.src.y, w, h});
    const SDL_FRect d{b.dst.x + ox, b.dst.y + oy, w, h};
    SDL_RenderTexture(ren, p.tex, &s, &d);
}

void tile_patch(SDL_Renderer* ren, const Patch9& p, const PatchBand& b) {
    for (int ix = 0; static_cast<float>(ix) * b.src.w < b.dst.w; ++ix)
        for (int iy = 0; static_cast<float>(iy) * b.src.h < b.dst.h; ++iy)
            blit_patch_cell(ren, p, b, SDL_Point{ix, iy});
}

// --- window-relative painting (sub_42DBCC's own coordinate space) ----------

// sub_44240C takes INCLUSIVE corner coordinates where draw_bevel_rect takes an
// origin plus a size.
struct Corners {
    int x0, y0, x1, y1;
};

struct Box {
    int x, y, w, h;
};

// sub_42DBCC draws window-relative, into the window's own buffer, so every
// literal in draw_list_dialog is an offset from the window origin. This carries
// that origin and the inclusive/size conversion, so each call below reads
// exactly like the original's own argument quad.
class WindowPainter {
public:
    WindowPainter(SDL_Renderer* ren, float ox, float oy) : ren_(ren), ox_(ox), oy_(oy) {}

    SDL_Renderer* ren() const { return ren_; }
    float x(int v) const { return ox_ + static_cast<float>(v); }
    float y(int v) const { return oy_ + static_cast<float>(v); }

    // sub_442A5C — a base-coat fill.
    void fill_base(Box b) const {
        fill_rect(ren_, SDL_FRect{x(b.x), y(b.y), static_cast<float>(b.w), static_cast<float>(b.h)},
                  kBaseCoat);
    }

    void bevel(Corners c, bool raised, Rgb face) const {
        draw_bevel_rect(ren_,
                        SDL_FRect{x(c.x0), y(c.y0), static_cast<float>(c.x1 - c.x0 + 1),
                                  static_cast<float>(c.y1 - c.y0 + 1)},
                        raised, face);
    }

private:
    SDL_Renderer* ren_;
    float ox_, oy_;
};

}  // namespace

DialogRect dialog_rect(float y_px, float height_px, float width_px) {
    return DialogRect{(static_cast<float>(kScreenW) - width_px) / 2, y_px, width_px, height_px};
}

DialogRect dialog_rect_vcentered(float height_px, float width_px) {
    return dialog_rect((static_cast<float>(kScreenH) - height_px) / 2, height_px, width_px);
}

void draw_dialog_chrome(SDL_Renderer* ren, const DialogRect& r, const Sprite* winz) {
    if (winz == nullptr || winz->tex == nullptr) {
        // The constructor's flat base coat alone — the look the editor's
        // sub_42E938/sub_42EDE0 prompts keep, and the fallback when WINZ.PCX is
        // missing from the install.
        fill_rect(ren, SDL_FRect{r.x, r.y, r.w, r.h}, kBaseCoat);
        return;
    }
    // sub_41726B -> sub_416B43. The original's draw order (centre, then
    // top/bottom edges, then left/right edges, then the four pinned corners) is
    // reproduced as-is: later bands overpaint the tiled centre exactly like the
    // original's separate loops do.
    const Patch9 p{winz->tex, winz->w, winz->h};
    const float cw = static_cast<float>(winz->w) / kPatchCells;
    const float ch = static_cast<float>(winz->h) / kPatchCells;

    // Centre cell over the WHOLE window (the original's first loop runs the full
    // 0..w / 0..h range), then the edge strips.
    tile_patch(ren, p, PatchBand{{cw, ch, cw, ch}, {r.x, r.y, r.w, r.h}});
    tile_patch(ren, p, PatchBand{{cw, 0, cw, ch}, {r.x, r.y, r.w, std::min(ch, r.h)}});
    tile_patch(ren, p, PatchBand{{cw, 2 * ch, cw, ch}, {r.x, r.y + r.h - ch, r.w, ch}});
    tile_patch(ren, p, PatchBand{{0, ch, cw, ch}, {r.x, r.y, std::min(cw, r.w), r.h}});
    tile_patch(ren, p, PatchBand{{2 * cw, ch, cw, ch}, {r.x + r.w - cw, r.y, cw, r.h}});

    // The four pinned corners.
    const float rx = r.x + r.w - cw, by = r.y + r.h - ch;
    blit_patch_cell(ren, p, PatchBand{{0, 0, cw, ch}, {r.x, r.y, cw, ch}}, SDL_Point{0, 0});
    blit_patch_cell(ren, p, PatchBand{{2 * cw, 0, cw, ch}, {rx, r.y, cw, ch}}, SDL_Point{0, 0});
    blit_patch_cell(ren, p, PatchBand{{0, 2 * ch, cw, ch}, {r.x, by, cw, ch}}, SDL_Point{0, 0});
    blit_patch_cell(ren, p, PatchBand{{2 * cw, 2 * ch, cw, ch}, {rx, by, cw, ch}}, SDL_Point{0, 0});
}

void draw_dialog_text(const DialogPen& pen, const std::string& text, SDL_FPoint at,
                      const DialogInk& ink) {
    // ONE port of sub_41696C, not two. This carried its own four-pass loop at the
    // CARDINAL neighbours while FontTextures::draw_outlined used the DIAGONAL
    // ones, both citing this address; the cardinals are wrong (facts.md
    // "sub_41696C's four outline passes are DIAGONAL"). Delegating rather than
    // copying the corrected offsets over is what stops the two from drifting
    // apart a second time — the dialog call sites only ever wanted the case
    // where nothing is clipped, which is the whole difference between them.
    pen.font.draw_outlined(pen.ren, text, at, OutlinedTextStyle{ink.ink, ink.outline});
}

void draw_dialog_button(const DialogPen& pen, SDL_FPoint at, const std::string& label,
                        bool pressed) {
    const float label_w = text_w(pen.font, label);
    const float h = line_h(pen.font);
    const float w = label_w + 16.0f;
    const float bh = h + 6.0f;

    // sub_432298's "up" bitmap in its own order: face fill (the washed base
    // coat), two light/dark rings at insets 2 then 1 (sub_44240C twice, drawn as
    // per-edge 1-px strips: light top+left, dark bottom+right), then the 1-px
    // black outline at the very edge (sub_442384). The "down" bitmap (pseudo.c
    // 35238-35256) swaps the ring pair and skips the wash, so its face is the
    // raw base coat.
    const Rgb face_ink = pressed ? kBaseCoat : kButtonFace;
    const Rgb hi = pressed ? kBevelDark : kBevelLight;
    const Rgb lo = pressed ? kBevelLight : kBevelDark;
    fill_rect(pen.ren, SDL_FRect{at.x, at.y, w, bh}, face_ink);
    for (int inset = 2; inset >= 1; --inset) {
        const float x0 = at.x + static_cast<float>(inset), y0 = at.y + static_cast<float>(inset);
        const float rw = w - static_cast<float>(2 * inset), rh = bh - static_cast<float>(2 * inset);
        fill_rect(pen.ren, SDL_FRect{x0, y0, rw, 1}, hi);
        fill_rect(pen.ren, SDL_FRect{x0, y0, 1, rh}, hi);
        fill_rect(pen.ren, SDL_FRect{x0, y0 + rh - 1, rw, 1}, lo);
        fill_rect(pen.ren, SDL_FRect{x0 + rw - 1, y0, 1, rh}, lo);
    }
    SDL_SetRenderDrawColor(pen.ren, 0, 0, 0, 255);  // byte_495390[0]
    SDL_FRect outline{at.x, at.y, w, bh};
    SDL_RenderRect(pen.ren, &outline);

    pen.font.draw(pen.ren, label, SDL_FPoint{at.x + (w - label_w) / 2.0f, at.y + 3.0f},
                  TextStyle{kButtonInk});
}

void draw_bevel_rect(SDL_Renderer* ren, const SDL_FRect& r, bool raised, Rgb face) {
    // sub_44240C: fill the interior, then a 1-px light strip on top+left and a
    // 1-px dark strip on bottom+right for the raised look (swapped for sunken).
    const Rgb hi = raised ? kBevelLight : kBevelDark;
    const Rgb lo = raised ? kBevelDark : kBevelLight;
    fill_rect(ren, r, face);
    fill_rect(ren, SDL_FRect{r.x, r.y, r.w, 1}, hi);
    fill_rect(ren, SDL_FRect{r.x, r.y, 1, r.h}, hi);
    fill_rect(ren, SDL_FRect{r.x, r.y + r.h - 1, r.w, 1}, lo);
    fill_rect(ren, SDL_FRect{r.x + r.w - 1, r.y, 1, r.h}, lo);
}

HintBlock pack_hint_lines(const FontTextures& font, const std::vector<std::string>& parts,
                          const std::vector<std::string>& budget, float max_w) {
    // Greedy pack: keep appending while the BUDGETED width still fits, then
    // break. The budgeted text drives both the break decision and the reported
    // width, so the two states of a toggle share one stable layout.
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

// The outer panel: base coat, the 1-px black rect @0x42DCF9 and the RAISED bevel
// at inset 1 @0x42DD39.
void draw_list_panel(const WindowPainter& p, const ListDialogGeometry& g) {
    p.fill_base(Box{0, 0, g.win_w, g.win_h});
    SDL_SetRenderDrawColor(p.ren(), 0, 0, 0, 255);  // byte_495390[0]
    SDL_FRect outline{p.x(0), p.y(0), static_cast<float>(g.win_w), static_cast<float>(g.win_h)};
    SDL_RenderRect(p.ren(), &outline);
    p.bevel(Corners{1, 1, g.win_w - 2, g.win_h - 2}, /*raised=*/true, kBaseCoat);
}

// Title strip: base-coat fill @0x42DD9E, SUNKEN bevel @0x42DE0E, then the centred
// title in dword_45C478 grey @0x42DDCB.
void draw_list_title(const WindowPainter& p, const FontTextures& font, const std::string& title,
                     const ListDialogGeometry& g) {
    p.fill_base(Box{5, 5, g.strip_fill_w, g.strip_fill_h});
    p.bevel(Corners{5, 5, g.strip_x1, g.strip_y1}, /*raised=*/false, kBaseCoat);
    draw_dialog_text(DialogPen{p.ren(), font}, title, SDL_FPoint{p.x(g.title_x), p.y(g.title_y)},
                     DialogInk{kButtonInk});
}

// Item area: base-coat fill @0x42DEA2 and its SUNKEN frame @0x42DFD2.
void draw_list_item_area(const WindowPainter& p, const ListDialogGeometry& g) {
    p.fill_base(Box{g.item_fill_x, g.item_fill_y, g.item_fill_w, g.item_fill_h});
    p.bevel(Corners{5, g.item_fill_y - 1, g.item_frame_x1, g.item_frame_y1}, /*raised=*/false,
            kBaseCoat);
}

// Drawn UNCONDITIONALLY (no branch guards it in sub_42DBCC), so a list that fits
// still shows a full-height bar. The FONT6 \x18/\x19 arrow labels are the
// hardcoded literals at 0x45AAAC/0x45AAB0. The thumb is a fixed 15x15 RAISED
// bevel whose face gets the sub_442C28 wash @0x42E1E6 — the same wash a button
// face gets, hence the same colour — and whose Y alone slides with `top_row`
// (@0x42E6B5-0x42E6F7), the track fill above staying put exactly as the
// original's partial repaint does.
void draw_list_scrollbar(const WindowPainter& p, const FontTextures& font,
                         const ListDialogGeometry& g) {
    const DialogPen pen{p.ren(), font};
    draw_dialog_button(pen, SDL_FPoint{p.x(g.sb_button_x), p.y(g.sb_up_y)}, "\x18");
    draw_dialog_button(pen, SDL_FPoint{p.x(g.sb_button_x), p.y(g.sb_down_y)}, "\x19");
    p.fill_base(Box{g.sb_track_x, g.sb_track_y, g.sb_track_w, g.sb_track_h});
    p.bevel(Corners{g.sb_frame_x0, g.sb_frame_y0, g.sb_frame_x1, g.sb_frame_y1},
            /*raised=*/false, kBaseCoat);
    p.bevel(Corners{g.sb_thumb_x0, g.sb_thumb_y0, g.sb_thumb_x1, g.sb_thumb_y1},
            /*raised=*/true, kButtonFace);
}

}  // namespace

ListDialogLayout draw_list_dialog(const DialogPen& pen, const ListDialogSpec& spec) {
    const float h = line_h(pen.font);
    // Every number below comes from list_dialog_geometry(), which carries the
    // per-offset sub_42DBCC citations — including the thumb's Y, the one field
    // total_rows/top_row feed.
    const ListDialogGeometry g = list_dialog_layout_for(pen.font, spec);
    const WindowPainter p{pen.ren, static_cast<float>(g.win_x), static_cast<float>(g.win_y)};

    draw_list_panel(p, g);
    draw_list_title(p, pen.font, spec.title, g);
    draw_list_item_area(p, g);
    draw_list_scrollbar(p, pen.font, g);

    ListDialogLayout lay{};
    lay.win = DialogRect{p.x(0), p.y(0), static_cast<float>(g.win_w), static_cast<float>(g.win_h)};
    lay.item_x = p.x(g.item_x);
    lay.item_y0 = p.y(g.item_y0);
    lay.item_h = h;
    lay.item_w = static_cast<float>(g.item_w);
    lay.footer_y0 = p.y(g.footer_y0);
    lay.done_x = p.x(g.done_x);
    lay.done_y = p.y(g.done_y);
    // "Done" @0x42E072 — the literal at 0x45AAB4, widget id 27 (Esc).
    draw_dialog_button(pen, SDL_FPoint{lay.done_x, lay.done_y}, "Done");
    return lay;
}

void draw_list_selection(SDL_Renderer* ren, const ListDialogLayout& lay, int visible_index) {
    // sub_442C28 @0x42DF80 over exactly (item_x, item_y0 + i*fontheight), sized
    // item_w x fontheight. Washing the base coat yields the button-face colour by
    // construction (see the header), so this invents no new constant and the
    // caller keeps drawing the row in its NORMAL ink.
    fill_rect(ren,
              SDL_FRect{lay.item_x, lay.item_y0 + static_cast<float>(visible_index) * lay.item_h,
                        lay.item_w, lay.item_h},
              kButtonFace);
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
    // sub_414340's pinned placement: x = width/2 - 32, y = height - 32 -
    // fontheight - 6 (window-relative); the button sizes itself from its label
    // inside sub_432298 (measure+16 x fontheight+6).
    return DialogRect{win.x + win.w / 2.0f - 32.0f, win.y + win.h - 32.0f - h - 6.0f,
                      text_w(font, ok_label) + 16.0f, h + 6.0f};
}

namespace {

// sub_4172BA's exact `cx - (w+2)/2` (pseudo.c 18703-18704) — one px left of a
// plain (win.w - w)/2 for even widths (level&rounds audit 2026-07-12; the port's
// last unexplained constant).
float centered_x(const DialogRect& win, float w) {
    return win.x + win.w / 2.0f - (w + 2.0f) / 2.0f;
}

}  // namespace

void draw_acknowledge_dialog(const DialogPen& pen, const Sprite* winz,
                             const AcknowledgeLabels& labels, const AcknowledgeStyle& style) {
    const float h = line_h(pen.font);
    const DialogRect win = acknowledge_dialog_rect(pen.font, labels.top, labels.bottom);
    draw_dialog_chrome(pen.ren, win, winz);

    // Lines at window-relative y = fontheight+32 and +fontheight+2 more.
    const float line1_y = win.y + h + 32.0f;
    if (!labels.top.empty())
        draw_dialog_text(pen, labels.top,
                         SDL_FPoint{centered_x(win, text_w(pen.font, labels.top)), line1_y},
                         DialogInk{style.ink});
    if (!labels.bottom.empty())
        draw_dialog_text(
            pen, labels.bottom,
            SDL_FPoint{centered_x(win, text_w(pen.font, labels.bottom)), line1_y + h + 2.0f},
            DialogInk{style.ink});

    const DialogRect ok = acknowledge_ok_rect(pen.font, win, labels.ok);
    draw_dialog_button(pen, SDL_FPoint{ok.x, ok.y}, labels.ok, style.ok_pressed);
}

void draw_confirm_dialog(const DialogPen& pen, const Sprite* winz, const ConfirmLabels& labels,
                         const DialogInk& ink) {
    const float h = line_h(pen.font);
    const float w1 = text_w(pen.font, labels.line1);
    const float w2 = labels.line2.empty() ? 0.0f : text_w(pen.font, labels.line2);
    const float lines_h = labels.line2.empty() ? h : (2.0f * h);
    const float win_w = std::max(std::max(w1, w2), 80.0f) + 64.0f;
    const float win_h = 4.0f * h + 64.0f + lines_h;
    const DialogRect win = dialog_rect_vcentered(win_h, win_w);
    draw_dialog_chrome(pen.ren, win, winz);

    const float line1_y = win.y + h + 32.0f;
    draw_dialog_text(pen, labels.line1, SDL_FPoint{centered_x(win, w1), line1_y}, ink);
    if (!labels.line2.empty())
        draw_dialog_text(pen, labels.line2, SDL_FPoint{centered_x(win, w2), line1_y + h + 2.0f},
                         ink);

    const float btn_y = win.y + win.h - 32.0f - h - 6.0f;
    draw_dialog_button(pen, SDL_FPoint{win.x + win.w / 2.0f - 80.0f, btn_y}, labels.yes);
    draw_dialog_button(pen, SDL_FPoint{win.x + win.w / 2.0f + 22.0f, btn_y}, labels.no);
}

void draw_compact_confirm_dialog(const DialogPen& pen, const std::string& line,
                                 const std::string& yes_label, const std::string& no_label) {
    const float h = line_h(pen.font);
    const float win_w = std::max(text_w(pen.font, line), 128.0f) + 32.0f;  // see header's width note
    const float win_h = 3.0f * h + 16.0f;                                  // CONFIRMED formula
    const DialogRect win = dialog_rect_vcentered(win_h, win_w);
    draw_dialog_chrome(pen.ren, win, nullptr);  // sub_42EDE0 has no sub_41726B call — flat

    draw_dialog_text(pen, line,
                     SDL_FPoint{win.x + (win.w - text_w(pen.font, line)) / 2.0f,
                                win.y + h / 2.0f + 4.0f},
                     DialogInk{kDialogInk});

    const float btn_y = win.y + win.h - h - 12.0f;
    draw_dialog_button(pen, SDL_FPoint{win.x + win.w / 2.0f - 64.0f, btn_y}, yes_label);
    draw_dialog_button(pen, SDL_FPoint{win.x + win.w / 2.0f + 16.0f, btn_y}, no_label);
}

void draw_text_entry_dialog(const DialogPen& pen, float y_px, const TextEntryLabels& labels) {
    const float h = line_h(pen.font);
    const float label_w = text_w(pen.font, labels.label);
    const float entry_w = text_w(pen.font, labels.entry);
    const float win_w = std::max(std::max(label_w, entry_w) + 28.0f, 160.0f);  // see header note
    const float win_h = 5.0f * h + 16.0f;                                      // CONFIRMED formula
    const DialogRect win = dialog_rect(y_px, win_h, win_w);
    draw_dialog_chrome(pen.ren, win, nullptr);  // sub_42E938 has no sub_41726B call — flat

    draw_dialog_text(pen, labels.label,
                     SDL_FPoint{win.x + (win.w - label_w) / 2.0f, win.y + h * 0.5f},
                     DialogInk{kDialogInk});
    // The live editable buffer with a plain caret — our own reproduction, in our
    // own selection tint; sub_42FF1C's real caret draw is not pinned.
    const std::string shown = labels.entry + "_";
    pen.font.draw(pen.ren, shown,
                  SDL_FPoint{win.x + (win.w - text_w(pen.font, shown)) / 2.0f, win.y + h * 2.0f},
                  TextStyle{{255, 220, 80}});

    const float btn_y = win.y + win.h - h - 10.0f;
    draw_dialog_button(pen, SDL_FPoint{win.x + win.w / 2.0f - 72.0f, btn_y}, labels.done);
    draw_dialog_button(pen, SDL_FPoint{win.x + win.w / 2.0f + 8.0f, btn_y}, labels.cancel);
}

// --- the list dialog's INPUT side ----------------------------------------

ListDialogGeometry list_dialog_layout_for(const FontTextures& font, const ListDialogSpec& spec) {
    // Deliberately the same call draw_list_dialog makes, spec for spec: a hit
    // test that re-derived the layout could drift from what is on screen.
    return list_dialog_geometry(static_cast<int>(spec.x_px), static_cast<int>(spec.y_px),
                                static_cast<int>(spec.item_text_w),
                                static_cast<int>(text_w(font, spec.title)),
                                static_cast<int>(line_h(font)), spec.visible_rows,
                                spec.footer_lines, spec.total_rows, spec.top_row);
}

ListDialogHit list_dialog_hit_for(const FontTextures& font, const ListDialogGeometry& g,
                                  int visible_rows, SDL_FPoint at) {
    const int fh = static_cast<int>(line_h(font));
    // Both arrow boxes are sized from their own FONT6 label, so take the wider
    // and let the y bands separate them (they never overlap in x anyway).
    const int arrow_w = list_dialog_button_w(
        static_cast<int>(std::max(text_w(font, "\x18"), text_w(font, "\x19"))));
    const int done_w = list_dialog_button_w(static_cast<int>(text_w(font, "Done")));
    const int bh = list_dialog_button_h(fh);
    return list_dialog_hit_test(g, visible_rows, arrow_w, bh, done_w, bh, static_cast<int>(at.x),
                                static_cast<int>(at.y));
}

bool list_mouse_point(SDL_Renderer* ren, const SDL_Event& ev, float& x, float& y) {
    if (ev.type == SDL_EVENT_MOUSE_MOTION) {
        SDL_RenderCoordinatesFromWindow(ren, ev.motion.x, ev.motion.y, &x, &y);
        return true;
    }
    if (ev.type != SDL_EVENT_MOUSE_BUTTON_DOWN && ev.type != SDL_EVENT_MOUSE_BUTTON_UP)
        return false;
    SDL_RenderCoordinatesFromWindow(ren, ev.button.x, ev.button.y, &x, &y);
    return true;
}

int list_dialog_key_code(SDL_Keycode key) {
    switch (key) {
        case SDLK_RETURN:
        case SDLK_KP_ENTER: return kListKeyEnter;
        case SDLK_ESCAPE: return kListKeyEscape;
        case SDLK_HOME: return kListKeyHome;
        case SDLK_UP: return kListKeyUp;
        case SDLK_PAGEUP: return kListKeyPageUp;
        case SDLK_END: return kListKeyEnd;
        case SDLK_DOWN: return kListKeyDown;
        case SDLK_PAGEDOWN: return kListKeyPageDown;
        default: return 0;
    }
}

}  // namespace bomber::game
