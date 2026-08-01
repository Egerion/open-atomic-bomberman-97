#include "bomber/ui/bmscreen.hpp"

#include "bomber/ui/dialog_chrome.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <exception>
#include <system_error>

#include "bomber/assets/image.hpp"
#include "bomber/game_util/credits_addendum.hpp"
#include "bomber/game_util/log.hpp"
#include "bomber/render/sprites.hpp"

namespace bomber::game {

namespace {

// The viewer WINDOW — CONFIRMED (chrome audit 2026-07-12): sub_43C734(20, 440,
// 600, 256, 4) -> (20, 20, 600, 440), repainted every frame with the WINZ
// 9-patch (sub_41726B @ pseudo.c 16423). Text insets 34/34 from the window
// origin (pen x @16437, row-y base @16456) -> screen (54, 54); per-line clip
// budget 532 px (@16406); visible region 344 px, so the row count is
// 344 / line_height and PgUp/PgDn move one less than that.
constexpr float kWinX = 20.0f;
constexpr float kWinY = 20.0f;
constexpr float kWinW = 600.0f;
constexpr float kWinH = 440.0f;
constexpr int kTextTop = 54;   // kWinY + 34
constexpr int kTextLeft = 54;  // kWinX + 34
constexpr int kLineClipW = 532;
constexpr int kVisibleHeight = 344;

// Inline images are CENTERED on their text row and clipped to the window band
// [34, height-62] (window-relative) — sub_41302D @16456-16497. The render loop
// runs ±kImageBleed lines beyond the visible rows (the original's row index runs
// -16 .. count+16), so a tall centred image whose own line is just off-screen
// still blits the half that pokes into view.
constexpr float kImgClipBottom = kWinY + (kWinH - 62.0f);  // 398: window height-62
constexpr int kImageBleed = 16;

// Text ink — CONFIRMED byte_49D38F pure white, drawn through the low-level blit
// dword_45C378 with NO outline (pseudo.c 16458-16461), unlike every sub_41696C
// site. The old (230,230,210) tint was a port invention.
constexpr Uint8 kInkR = 255, kInkG = 255, kInkB = 255;

constexpr float kButtonRowY = 388.0f;  // window-relative

// HelpBrowser's list item ink — byte_49D38F again, the SAME ink
// SchemeFilePicker's list uses at the identical (100,100) call site (§4/§5).
constexpr Uint8 kListInkR = 255, kListInkG = 255, kListInkB = 255;

// The browser's two error dialogs (§4) draw in byte_49D0DA — PINNED in
// results-and-options.md §1's LUT table (offset 0x7D4A -> (252,80,80)), which
// also confirms byte_49D0DA IS sub_4141F8's team-1 ink: the same LUT element,
// not a coincidence of similar reds.
constexpr Uint8 kErrorInkR = 252, kErrorInkG = 80, kErrorInkB = 80;

// Expand a glyph's 0/255 coverage into white RGBA: the alpha carries the shape,
// the colour is applied at draw time via SDL_SetTextureColorMod.
assets::Image glyph_image(const assets::bmfont::Glyph& g, int height) {
    assets::Image img;
    img.width = g.width;
    img.height = height;
    img.rgba.assign(static_cast<std::size_t>(g.width) * height * 4, 0);
    for (std::size_t i = 0; i < g.pixels.size(); ++i) {
        const std::size_t o = i * 4;
        img.rgba[o + 0] = 255;
        img.rgba[o + 1] = 255;
        img.rgba[o + 2] = 255;
        img.rgba[o + 3] = g.pixels[i];
    }
    return img;
}

}  // namespace

void FontTextures::reset() {
    owners_.clear();
    glyphs_.clear();
    line_height_ = 0;
    spacing_ = 0;
}

void FontTextures::build(SDL_Renderer* ren, const assets::bmfont::Font& font) {
    reset();
    if (!ren || font.glyphs.empty() || font.glyph_height <= 0) return;
    line_height_ = font.glyph_height;
    spacing_ = font.spacing;
    glyphs_.resize(font.glyphs.size());
    for (std::size_t c = 0; c < font.glyphs.size(); ++c) {
        const assets::bmfont::Glyph& g = font.glyphs[c];
        glyphs_[c].w = g.width;
        if (g.width <= 0 || g.pixels.empty()) continue;  // blank/zero-width: advance only
        sdl::TexturePtr tex{make_texture(ren, glyph_image(g, font.glyph_height))};
        if (!tex) continue;
        SDL_SetTextureBlendMode(tex.get(), SDL_BLENDMODE_BLEND);
        glyphs_[c].tex = tex.get();
        owners_.push_back(std::move(tex));
    }
}

int FontTextures::advance(unsigned char c) const {
    if (static_cast<std::size_t>(c) >= glyphs_.size())
        return 0;  // uncovered code: no advance (sub_432120)
    return glyphs_[c].w + spacing_;
}

int FontTextures::measure(const std::string& s) const {
    int w = 0;
    for (char ch : s) w += advance(static_cast<unsigned char>(ch));
    return w;
}

float FontTextures::draw(SDL_Renderer* ren, const std::string& s, float x, float y, Uint8 r,
                         Uint8 g, Uint8 b, float scale) const {
    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (static_cast<std::size_t>(c) >= glyphs_.size()) continue;
        const GlyphTex& gt = glyphs_[c];
        if (gt.tex) {
            SDL_SetTextureColorMod(gt.tex, r, g, b);
            // `scale` shrinks the destination rect only (the glyph atlas is
            // untouched); scaling the dst is self-contained, unlike
            // SDL_SetRenderScale, which perturbs the whole render transform.
            SDL_FRect dst{x, y, static_cast<float>(gt.w) * scale,
                          static_cast<float>(line_height_) * scale};
            SDL_RenderTexture(ren, gt.tex, nullptr, &dst);
            SDL_SetTextureColorMod(gt.tex, 255, 255, 255);
        }
        x += static_cast<float>(gt.w + spacing_) * scale;
    }
    return x;
}

float FontTextures::draw_outlined(SDL_Renderer* ren, const std::string& s, float x, float y,
                                  Uint8 r, Uint8 g, Uint8 b, Uint8 outline_r, Uint8 outline_g,
                                  Uint8 outline_b, float max_w) const {
    // Clip the run to max_w pixels of advance (sub_41696C 18542-18551): stop at
    // the first glyph whose advance would cross the limit.
    std::string run = s;
    if (max_w > 0) {
        float w = 0;
        std::size_t n = 0;
        for (char ch : s) {
            const int a = advance(static_cast<unsigned char>(ch));
            if (w + static_cast<float>(a) > max_w) break;
            w += static_cast<float>(a);
            ++n;
        }
        run = s.substr(0, n);
    }
    // Outline at the four DIAGONAL neighbours of the ink — SETTLED 2026-08-01 at
    // the instruction level (facts.md "sub_41696C's four outline passes are
    // DIAGONAL"). sub_41696C assembles the string into a (w+2)x(h+4) scratch and
    // calls the font blit five times: the outline at buffer offsets (0,0),
    // (2,2), (0,2), (2,0) and the ink ONCE at (1,1), so relative to the ink the
    // outline sits at the four CORNERS, not the four edges. Nothing there is
    // register-lost, which is what the older reading assumed: all five
    // destinations are plain add chains off the scratch pointer at
    // 0x4169F9-0x416A87. draw_dialog_text now delegates here rather than keeping
    // a second, contradicting copy.
    //
    // KNOWN DIVERGENCE, recorded rather than fixed (facts.md, same entry): the
    // original blits that scratch with its TOP-LEFT at (x, y), so its ink lands
    // at (x+1, y+1) and its outline spans (x, y)..(x+2, y+2). The port centres
    // the composite on (x, y) instead, and every call site passes the original's
    // own x/y verbatim, so each outlined string sits one pixel up and left of
    // where the original puts it.
    static constexpr std::array<std::array<float, 2>, 4> kOff{{{-1, -1}, {1, 1}, {-1, 1}, {1, -1}}};
    for (const auto& o : kOff) draw(ren, run, x + o[0], y + o[1], outline_r, outline_g, outline_b);
    return draw(ren, run, x, y, r, g, b);
}

// --- BmScreen -------------------------------------------------------------

void BmScreen::enter(const std::string& bm_name) {
    top_ = 0;
    done_ = false;
    doc_.lines.clear();
    // The .BM files live in the install ROOT, not under DATA/. A missing or
    // broken file logs and leaves an empty doc, so the screen still dismisses.
    try {
        doc_ = assets::bmtext::load(assets_->game_dir() / (bm_name + ".BM"));
    } catch (const std::exception& e) {
        log_warn("BM screen '%s' load failed: %s", bm_name.c_str(), e.what());
    }
    // The port's own addendum, appended in memory only — the install's
    // CREDITS.BM is 1997 game data and is opened read-only (credits_addendum.hpp
    // explains the choice). Appended even when the load above failed, so a
    // missing CREDITS.BM still gets a page rather than a blank window.
    if (bm_name == "CREDITS") append_credits_addendum(doc_);
}

int BmScreen::visible_rows() const {
    int lh = font_ && font_->loaded() ? font_->line_height() : 16;
    if (lh <= 0) lh = 16;
    return kVisibleHeight / lh;
}

int BmScreen::max_scroll() const {
    // sub_41302D clamps the scroll top to (line count - visible rows).
    const int m = static_cast<int>(doc_.lines.size()) - visible_rows();
    return m < 0 ? 0 : m;
}

void BmScreen::on_key(SDL_Keycode key) {
    switch (key) {
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_ESCAPE:
            // Enter (13) and Escape (27) finish the viewer (sub_41302D
            // 16530-16594). Space does NOT — 32 falls through every branch
            // (CORRECTED 2026-07-12; the old Space-dismiss was invented).
            done_ = true;
            break;
        case SDLK_UP: top_ = std::max(0, top_ - 1); break;
        case SDLK_DOWN: top_ = std::min(max_scroll(), top_ + 1); break;
        case SDLK_PAGEUP:
        case SDLK_LEFT:
            // Left (331) pages up alongside PgUp (329) — sub_41302D treats both
            // identically (chrome audit 2026-07-12).
            top_ = std::max(0, top_ - (visible_rows() - 1));
            break;
        case SDLK_PAGEDOWN:
        case SDLK_RIGHT:
            // Right (333) pages down alongside PgDn (337).
            top_ = std::min(max_scroll(), top_ + visible_rows() - 1);
            break;
        default: break;
    }
}

namespace {

// The two asset sources one page row draws from, plus the pinned per-line clip
// budget and row pitch.
struct BmPage {
    const AssetStore& assets;
    const FontTextures& font;
    float clip_right;
    int line_height;
};

// The pen for one row: where the next segment lands, the row's top y, and
// whether the row is inside the visible window. Rows in the ±kImageBleed margin
// still advance the pen (so a trailing <IMG> lands at the right x) but draw no
// text.
struct RowPen {
    float x;
    float y;
    bool visible;
};

void draw_text_segment(SDL_Renderer* ren, const BmPage& page, const std::string& text,
                       RowPen& pen) {
    // Trim the run to the remaining clip budget — sub_41302D consumes that
    // budget per glyph advance.
    std::string run = text;
    float w = 0;
    std::size_t n = 0;
    for (char ch : run) {
        const int a = page.font.advance(static_cast<unsigned char>(ch));
        if (pen.x + w + static_cast<float>(a) > page.clip_right) break;
        w += static_cast<float>(a);
        ++n;
    }
    run.resize(n);
    if (pen.visible) page.font.draw(ren, run, pen.x, pen.y, kInkR, kInkG, kInkB);
    pen.x += w;
}

void draw_image_segment(SDL_Renderer* ren, const BmPage& page, const Sprite& sp, RowPen& pen) {
    if (!sp.tex) return;  // a missing image has no texture and no width: nothing to advance by
    // CENTER the image on the row: sub_41302D sets the blit Y to
    // rowY - (imageHeight - lineHeight)/2 (integer div), NOT the row top.
    // Top-aligning (the old port bug) shifted every inline image DOWN by ~half
    // its height, so the credits' "----->" arrows no longer met their photos.
    int img_top = static_cast<int>(pen.y) - (sp.h - page.line_height) / 2;
    int src_y = 0;
    int draw_h = sp.h;
    // Vertical clip to the image band: a source-row offset at the top, a height
    // clamp at the bottom.
    if (img_top < kTextTop) {
        src_y = kTextTop - img_top;
        draw_h = sp.h - src_y;
        img_top = kTextTop;
    }
    if (static_cast<float>(img_top + draw_h) > kImgClipBottom)
        draw_h = static_cast<int>(kImgClipBottom) - img_top;
    // Right clip to the 532-px line budget: the blit width is min(image width,
    // remaining budget).
    int draw_w = sp.w;
    if (pen.x + static_cast<float>(draw_w) > page.clip_right)
        draw_w = static_cast<int>(page.clip_right - pen.x);
    if (draw_h > 0 && draw_w > 0) {
        // The src rect is in the sprite's CLASSIC space; a 4x DATA_HD texture
        // behind it would otherwise sample a sliver of the corner (sprites.hpp
        // texture_src_rect). Identity when HD is off.
        const SDL_FRect src =
            texture_src_rect(sp.tex, sp.w, sp.h,
                             SDL_FRect{0.0f, static_cast<float>(src_y), static_cast<float>(draw_w),
                                       static_cast<float>(draw_h)});
        SDL_FRect dst{pen.x, static_cast<float>(img_top), static_cast<float>(draw_w),
                      static_cast<float>(draw_h)};
        SDL_RenderTexture(ren, sp.tex, &src, &dst);
    }
    pen.x += static_cast<float>(sp.w);
}

// One line's segments, laid left to right — sub_41302D's per-line split-at-tag
// draw.
void draw_bm_row(SDL_Renderer* ren, const BmPage& page, const assets::bmtext::BmLine& segments,
                 RowPen pen) {
    for (const auto& seg : segments) {
        if (seg.is_text()) {
            draw_text_segment(ren, page, seg.value, pen);
            continue;
        }
        // Inline image, through the VIEWER's own loader — keyed on palette index
        // 0 and snapped to the master palette, which is what sub_41302D's
        // sub_4150F0 load + sub_44AED5 blit do and frontend_pcx (the opaque
        // backdrop path) does not. Base name, case as written.
        const Sprite& sp = seg.value == kAuthorPhotoTag
                               ? page.assets.author_photo()  // compiled-in, not DATA/RES
                               : page.assets.bm_inline_pcx(seg.value);
        draw_image_segment(ren, page, sp, pen);
    }
}

// The bottom control row (pseudo.c 16408-16412): five sub_432298 buttons at
// window-relative y=388 posting key codes when clicked — the \x18/\x19 FONT6
// arrows (EXE bytes @0x459148), their "Page" variants and "Done", at the
// original's literal x offsets. Drawn for parity; this viewer is keyboard-driven
// with the same codes.
void draw_bm_buttons(SDL_Renderer* ren, const FontTextures& font) {
    struct ControlButton {
        float x;
        const char* label;
    };
    static constexpr std::array<ControlButton, 5> kButtons{{{30.0f, "\x18"},
                                                            {60.0f, "\x19"},
                                                            {120.0f, "Page \x18"},
                                                            {190.0f, "Page \x19"},
                                                            {516.0f, "Done"}}};
    for (const ControlButton& b : kButtons)
        draw_dialog_button(ren, font, kWinX + b.x, kWinY + kButtonRowY, b.label);
}

}  // namespace

void BmScreen::draw(SDL_Renderer* ren) const {
    // assets_ is set from a reference in every constructor, so it is only null on
    // a default-constructed-but-unused instance; guarding makes that
    // impossible-in-practice state a clean no-op.
    if (!ren || !assets_) return;
    // The WINZ 9-patch viewer window (see the kWin* block), repainted every
    // frame like the original's dirty repaint.
    draw_dialog_chrome(ren, DialogRect{kWinX, kWinY, kWinW, kWinH}, &assets_->frontend_pcx("WINZ"));
    if (!font_ || !font_->loaded()) return;

    const int lh = font_->line_height();
    const int vis = visible_rows();
    const int total = static_cast<int>(doc_.lines.size());
    const BmPage page{*assets_, *font_, static_cast<float>(kTextLeft + kLineClipW), lh};
    for (int j = -kImageBleed; j < vis + kImageBleed; ++j) {
        const int li = top_ + j;
        if (li < 0 || li >= total) continue;
        const RowPen pen{static_cast<float>(kTextLeft), static_cast<float>(kTextTop + j * lh),
                         j >= 0 && j < vis};
        draw_bm_row(ren, page, doc_.lines[static_cast<std::size_t>(li)], pen);
    }
    draw_bm_buttons(ren, *font_);
}

// --- HelpBrowser ------------------------------------------------------------

namespace {

bool has_extension(const std::filesystem::path& p, const char* upper_ext) {
    std::string ext = p.extension().string();
    for (auto& c : ext) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return ext == upper_ext;
}

}  // namespace

void HelpBrowser::enter(bool manual_enabled) {
    entries_.clear();
    nav_ = ListDialogNav{};
    item_w_ = 0.0f;
    pressed_ = ListDialogWidget::None;
    viewing_ = false;
    done_ = false;
    // sub_414235's own first act: gate on getvalue(15) BEFORE the *.BM glob even
    // runs (§4).
    disabled_ = !manual_enabled;
    if (disabled_ || !assets_) return;
    // sub_41404B: a DOS findfirst/findnext glob of "*.BM" over the install ROOT
    // (not DATA/), qsort_-sorted. directory_iterator plus a case-insensitive
    // extension check is the faithful equivalent — the original glob is
    // case-insensitive on the FAT install media.
    std::error_code ec;
    const std::filesystem::path& root = assets_->game_dir();
    if (!std::filesystem::is_directory(root, ec)) return;
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        if (entry.is_regular_file() && has_extension(entry.path(), ".BM"))
            entries_.push_back(entry.path());
    }
    std::sort(entries_.begin(), entries_.end());  // qsort_(sub_41400F, count)
    // sub_42FEF0 @0x42DC16 — the widest ITEM alone drives the width (the widget
    // folds the title in itself). Measured once here so the mouse handlers can
    // rebuild the same layout cheaply.
    if (!font_) return;
    for (const auto& e : entries_)
        item_w_ = std::max(item_w_, static_cast<float>(font_->measure(e.filename().string())));
}

std::string HelpBrowser::header() const {
    return assets_ ? assets_->getstring(600, "Available help files:")
                   : std::string("Available help files:");
}

ListDialogGeometry HelpBrowser::layout() const {
    return list_dialog_layout_for(*font_, header(), 100.0f, 100.0f, item_w_, kVisibleRows,
                                  static_cast<int>(entries_.size()), nav_.top_row);
}

bool HelpBrowser::list_active() const {
    return !viewing_ && !disabled_ && !entries_.empty() && font_ && font_->loaded();
}

void HelpBrowser::open_selected() {
    // sub_41302D is called on the highlighted glob entry; the list re-shows once
    // close_viewer() is called, matching sub_414235's do/while loop-back over
    // the same glob array.
    const int sel = nav_.top_row + nav_.highlight;                    // @0x42E39A
    if (sel < 0 || sel >= static_cast<int>(entries_.size())) return;  // @0x42E3A8
    bm_.enter(entries_[static_cast<std::size_t>(sel)].stem().string());
    viewing_ = true;
}

namespace {

// sub_42FEB0 @0x42FEB0 — a letter selects the first entry starting with it
// (case-insensitive). The match is pulled to the TOP of the window, not merely
// scrolled into view, and in a list that fits nothing happens at all.
// sub_42FEB0 is a leaf: it makes no calls, sound included.
int first_entry_starting_with(const std::vector<std::filesystem::path>& entries, char want) {
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const std::string f = entries[i].filename().string();
        if (!f.empty() && std::tolower(static_cast<unsigned char>(f[0])) == want)
            return static_cast<int>(i);
    }
    return -1;
}

}  // namespace

void HelpBrowser::on_key(SDL_Keycode key, AudioEngine& audio) {
    if (viewing_) {
        bm_.on_key(key);
        return;
    }
    if (disabled_ || entries_.empty()) {
        // §4's two gated error dialogs share sub_414340's two-line dismiss shape.
        // This branch is the ONE audible thing in the browser: sub_414340 opens
        // its key loop with an unconditional nav blip @0x414532 for every real
        // key and dismisses with NO accept sting.
        audio.play(20);
        if (key == SDLK_ESCAPE || key == SDLK_RETURN || key == SDLK_SPACE) done_ = true;
        return;
    }
    // THE LIST DIALOG IS SILENT. sub_41431C -> sub_414235 -> sub_41485A ->
    // sub_42DB80 -> sub_42DBCC: 344 functions of closure and NOT ONE calls a play
    // primitive (census in docs/re/sound-engine.md §8). The port invented every
    // cue that used to be in this handler.
    const int count = static_cast<int>(entries_.size());
    // Arrow-only: sub_42DBCC binds 0x0d/0x1b plus the 0x147..0x151 jump table
    // @0x42DBA0 and nothing else, so there is no W/S alias. Space is the port's
    // own accept alias — in the original it is a printable character that falls
    // into the type-ahead default and matches nothing, so it is inert there.
    const int code = (key == SDLK_SPACE) ? kListKeyEnter : list_dialog_key_code(key);
    if (code != 0) {
        switch (list_dialog_key(nav_, code, kVisibleRows, count)) {
            case ListDialogAction::Activate: open_selected(); break;
            case ListDialogAction::Cancel: done_ = true; break;
            case ListDialogAction::None: break;
        }
        return;
    }
    if (key < SDLK_A || key > SDLK_Z) return;
    const char want = static_cast<char>('a' + (key - SDLK_A));
    list_dialog_letter_jump(nav_, first_entry_starting_with(entries_, want), kVisibleRows, count);
}

void HelpBrowser::on_mouse_move(float x, float y, bool buttons_held) {
    if (!list_active()) return;
    if (buttons_held) return;  // @0x4330A0: the enter id needs an idle mouse
    list_dialog_mouse_move(nav_, list_dialog_hit_for(*font_, layout(), kVisibleRows, x, y),
                           static_cast<int>(entries_.size()));
}

void HelpBrowser::on_mouse_down(float x, float y) {
    if (!list_active()) return;
    const ListDialogGeometry g = layout();
    const ListDialogHit hit = list_dialog_hit_for(*font_, g, kVisibleRows, x, y);
    pressed_ = hit.widget;
    switch (list_dialog_mouse_down(nav_, g, hit, kVisibleRows, static_cast<int>(entries_.size()),
                                   static_cast<int>(y))) {
        case ListDialogAction::Activate: open_selected(); break;
        case ListDialogAction::Cancel: done_ = true; break;
        case ListDialogAction::None: break;
    }
}

void HelpBrowser::on_mouse_up(float x, float y) {
    if (!list_active()) return;
    const ListDialogHit hit = list_dialog_hit_for(*font_, layout(), kVisibleRows, x, y);
    const ListDialogWidget was = pressed_;
    pressed_ = ListDialogWidget::None;
    if (list_dialog_mouse_up(hit, was) == ListDialogAction::Cancel) done_ = true;
}

void HelpBrowser::draw_error_dialog(SDL_Renderer* ren) const {
    // §4's two gated dialogs, both drawn through sub_414340: "manual disabled" =
    // getstring(5), "no .BM files found" = getstring(4), header getstring(95)
    // "NOTE!", Ok label getstring(27) (batch_0x413AED.cpp:503 — the SAME id every
    // other call site uses, not getstring(90)). The old bare red text at
    // (100,100) with no chrome was a port stand-in.
    const char* body_default = disabled_ ? "Online manual disabled." : "No help files found!";
    const std::string body =
        assets_ ? assets_->getstring(disabled_ ? 5 : 4, body_default) : std::string(body_default);
    const std::string head = assets_ ? assets_->getstring(95, "NOTE!") : std::string("NOTE!");
    const std::string ok = assets_ ? assets_->getstring(27, " Ok ") : std::string(" Ok ");
    draw_acknowledge_dialog(ren, *font_, assets_ ? &assets_->frontend_pcx("WINZ") : nullptr, head,
                            body, ok, kErrorInkR, kErrorInkG, kErrorInkB);
}

void HelpBrowser::draw_topic_list(SDL_Renderer* ren) const {
    // The generic list dialog at the LITERAL (100, 100), header getstring(600) —
    // the SAME primitive and coordinates SchemeFilePicker uses (§4/§5c). The
    // width comes from the widest ENTRY alone; the widget folds the title in
    // itself, so pre-maxing here would inflate it by 16.
    const int count = static_cast<int>(entries_.size());
    const int last = std::min(count, nav_.top_row + kVisibleRows);
    const ListDialogLayout lay = draw_list_dialog(ren, *font_, header(), 100.0f, 100.0f, item_w_,
                                                  kVisibleRows, count, nav_.top_row);
    for (int i = nav_.top_row; i < last; ++i) {
        const int vi = i - nav_.top_row;
        const float ty = lay.item_y0 + static_cast<float>(vi) * lay.item_h;
        // sub_442C28 LIGHTENS the selected row rather than inverting it, so every
        // row keeps the same ink. The band tracks the highlight OFFSET, which is
        // what the original draws @0x42E656 — not an absolute selection.
        if (vi == nav_.highlight) draw_list_selection(ren, lay, vi);
        font_->draw(ren, entries_[static_cast<std::size_t>(i)].filename().string(), lay.item_x, ty,
                    kListInkR, kListInkG, kListInkB);
    }
}

void HelpBrowser::draw(SDL_Renderer* ren) const {
    if (!ren) return;
    if (viewing_) {
        // The .BM viewer paints its own window over whatever is on screen; the
        // caller is expected to have drawn the persistent backdrop first.
        bm_.draw(ren);
        return;
    }
    if (!font_ || !font_->loaded()) return;
    if (disabled_ || entries_.empty()) {
        draw_error_dialog(ren);
        return;
    }
    draw_topic_list(ren);
}

}  // namespace bomber::game
