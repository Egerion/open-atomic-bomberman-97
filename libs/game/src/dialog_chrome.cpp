#include "bomber/game/dialog_chrome.hpp"

#include <algorithm>

#include "bomber/game/renderer.hpp"  // kScreenW/kScreenH

namespace bomber::game {

namespace {

// Bevel/ink globals — siblings of the window fill colour, from the SAME
// sub_414DF4 init block (docs/re/frontend-flow.md "Window default fill and
// the button bevel colours are siblings from the SAME init block").
constexpr Uint8 kBevelLightR = 123, kBevelLightG = 123, kBevelLightB = 123;  // dword_45C470
constexpr Uint8 kBevelDarkR = 66, kBevelDarkG = 66, kBevelDarkB = 66;        // dword_45C474
constexpr Uint8 kButtonInkR = 165, kButtonInkG = 165, kButtonInkB = 165;     // dword_45C478

float line_h(const FontTextures& font) {
    return static_cast<float>(font.loaded() ? font.line_height() : 12);
}

float text_w(const FontTextures& font, const std::string& s) {
    return font.loaded() ? static_cast<float>(font.measure(s)) : 0.0f;
}

}  // namespace

DialogRect dialog_rect(float y_px, float height_px, float width_px) {
    return DialogRect{(static_cast<float>(kScreenW) - width_px) / 2, y_px, width_px, height_px};
}

DialogRect dialog_rect_vcentered(float height_px, float width_px) {
    return dialog_rect((static_cast<float>(kScreenH) - height_px) / 2, height_px, width_px);
}

void draw_dialog_chrome(SDL_Renderer* ren, const DialogRect& r) {
    SDL_FRect box{r.x, r.y, r.w, r.h};
    SDL_SetRenderDrawColor(ren, kDialogFillR, kDialogFillG, kDialogFillB, 255);
    SDL_RenderFillRect(ren, &box);
}

void draw_dialog_button(SDL_Renderer* ren, const FontTextures& font, float x, float y,
                        const std::string& label) {
    const float label_w = text_w(font, label);
    const float h = line_h(font);
    const float w = label_w + 16.0f;
    const float bh = h + 6.0f;

    SDL_FRect outline{x, y, w, bh};
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);  // byte_495390[0]
    SDL_RenderFillRect(ren, &outline);
    SDL_FRect ring1{x + 1, y + 1, w - 2, bh - 2};
    SDL_SetRenderDrawColor(ren, kBevelDarkR, kBevelDarkG, kBevelDarkB, 255);  // bottom/right
    SDL_RenderFillRect(ren, &ring1);
    SDL_FRect ring1_lit{x + 1, y + 1, w - 3, bh - 3};  // top/left overpainted light
    SDL_SetRenderDrawColor(ren, kBevelLightR, kBevelLightG, kBevelLightB, 255);
    SDL_RenderFillRect(ren, &ring1_lit);
    SDL_FRect face{x + 2, y + 2, w - 4, bh - 4};
    SDL_SetRenderDrawColor(ren, kDialogFillR, kDialogFillG, kDialogFillB, 255);
    SDL_RenderFillRect(ren, &face);

    font.draw(ren, label, x + (w - label_w) / 2.0f, y + 3.0f, kButtonInkR, kButtonInkG,
              kButtonInkB);
}

void draw_confirm_dialog(SDL_Renderer* ren, const FontTextures& font, const std::string& line1,
                         const std::string& line2, const std::string& yes_label,
                         const std::string& no_label) {
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
    draw_dialog_chrome(ren, win);

    const float line1_y = win.y + h + 32.0f;
    font.draw(ren, line1, win.x + (win.w - w1) / 2.0f, line1_y, 255, 255, 255);  // byte_49D38F
    if (!line2.empty())
        font.draw(ren, line2, win.x + (win.w - w2) / 2.0f, line1_y + h + 2.0f, 255, 255, 255);

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
    draw_dialog_chrome(ren, win);

    font.draw(ren, line, win.x + (win.w - text_w(font, line)) / 2.0f, win.y + h / 2.0f + 4.0f, 255,
              255, 255);  // byte_49D38F (sub_4023A2's own ink arg)

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
    draw_dialog_chrome(ren, win);

    font.draw(ren, label, win.x + (win.w - label_w) / 2.0f, win.y + h * 0.5f, 255, 255, 255);
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
