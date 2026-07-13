#include "bomber/game/bmscreen.hpp"

#include "bomber/game/dialog_chrome.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <system_error>

#include "bomber/assets/image.hpp"
#include "bomber/game/sprites.hpp"

namespace bomber::game {

namespace {

// Layout constants, all confirmed literals in sub_41302D (BM95.EXE @ 0x41302D):
//   - text lines start 34 px from the top of the scroll region, one line per
//     row at the font's cell height (v35 = j*height + 34; clip is `>= 34`);
//   - the left inset is 34 px (v37 starts at 34);
//   - the visible region is 344 px tall, so the on-screen row count is
//     344 / line_height (v60 = 344 / dword_45C37C());
//   - PgUp/PgDn move by (visible_rows - 1) lines (v60 - 1).
// The original draws into a 600x256-ish scroll window (sub_43C734(20,440,600,
// 256,4)); we paint the same inset text over a dark panel spanning the logical
// surface, since our front-end is RGBA rather than the paletted VGA page.
// The viewer WINDOW — CONFIRMED (chrome audit 2026-07-12): sub_43C734(20,
// 440, 600, 256, 4) = y=20, h=440, w=600, x auto-centred -> (20, 20, 600,
// 440), repainted every frame with the WINZ.PCX 9-patch (sub_41726B @
// pseudo.c 16423) — the blue tiled border the user compared against. Text
// insets 34/34 from the window origin (v37 = 34 @ 16437, v35 base 34 @
// 16456) -> screen (54, 54); per-line clip budget 532 px (v59 @ 16406).
constexpr float kWinX = 20.0f;
constexpr float kWinY = 20.0f;
constexpr float kWinW = 600.0f;
constexpr float kWinH = 440.0f;
constexpr int kTextTop = 54;   // kWinY + 34
constexpr int kTextLeft = 54;  // kWinX + 34
constexpr int kLineClipW = 532;
constexpr int kVisibleHeight = 344;

// Text ink — CONFIRMED byte_49D38F pure white, drawn through the low-level
// string blit dword_45C378 with NO outline (pseudo.c 16458-16461), unlike
// every sub_41696C site. The old (230,230,210) tint was a port invention.
constexpr Uint8 kInkR = 255, kInkG = 255, kInkB = 255;

// The bottom control row (pseudo.c 16408-16412): five sub_432298 bevel
// buttons at window-relative y=388, posting key codes when clicked — up/down
// arrows (FONT6 glyphs \x18/\x19, EXE bytes @ 0x459148), "Page" variants,
// and "Done". Drawn for parity; this port's viewer is keyboard-driven (the
// same codes the buttons would post).
constexpr float kButtonRowY = 388.0f;  // window-relative

// HelpBrowser's list dialog item ink — the general white draw colour
// byte_49D38F (§4/§5, the SAME ink SchemeFilePicker's own list dialog uses
// at the identical (100,100) sub_41485A call site); the SELECTED row inverts
// to the dark base coat over a light band (draw_list_selection).
constexpr Uint8 kListInkR = 255, kListInkG = 255, kListInkB = 255;

// The browser's two error dialogs (§4: "manual disabled" getstring(5)/(95),
// "no .BM files found" getstring(4)/(95)) draw in byte_49D0DA — PINNED in
// docs/re/results-and-options.md §1's LUT decode table: LUT offset 0x7D4A =
// r5,g5,b5(31,10,10), nearest-palette RGB (252, 80, 80). The doc separately
// confirms byte_49D0DA IS sub_4141F8's team-1 ink (its ELSE branch returns
// byte_49D38F/white; the non-ELSE branch returns this SAME global) — i.e.
// the error ink and the team-1 player ink are the identical LUT element, not
// a coincidence of similar reds. Faithful hardcode, same rationale as §1's
// scoreboard inks (no "active palette" concept in the truecolour renderer).
constexpr Uint8 kErrorInkR = 252, kErrorInkG = 80, kErrorInkB = 80;

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
        // Expand the 0/255 coverage into white RGBA (the alpha carries the shape,
        // colour is applied at draw time via SDL_SetTextureColorMod).
        assets::Image img;
        img.width = g.width;
        img.height = font.glyph_height;
        img.rgba.assign(static_cast<std::size_t>(g.width) * font.glyph_height * 4, 0);
        for (std::size_t i = 0; i < g.pixels.size(); ++i) {
            std::uint8_t a = g.pixels[i];
            std::size_t o = i * 4;
            img.rgba[o + 0] = 255;
            img.rgba[o + 1] = 255;
            img.rgba[o + 2] = 255;
            img.rgba[o + 3] = a;
        }
        sdl::TexturePtr tex{make_texture(ren, img)};
        if (tex) {
            SDL_SetTextureBlendMode(tex.get(), SDL_BLENDMODE_BLEND);
            glyphs_[c].tex = tex.get();
            owners_.push_back(std::move(tex));
        }
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
                         Uint8 g, Uint8 b) const {
    for (char ch : s) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (static_cast<std::size_t>(c) >= glyphs_.size()) continue;
        const GlyphTex& gt = glyphs_[c];
        if (gt.tex) {
            SDL_SetTextureColorMod(gt.tex, r, g, b);
            SDL_FRect dst{x, y, static_cast<float>(gt.w), static_cast<float>(line_height_)};
            SDL_RenderTexture(ren, gt.tex, nullptr, &dst);
            SDL_SetTextureColorMod(gt.tex, 255, 255, 255);
        }
        x += static_cast<float>(gt.w + spacing_);
    }
    return x;
}

float FontTextures::draw_outlined(SDL_Renderer* ren, const std::string& s, float x, float y,
                                  Uint8 r, Uint8 g, Uint8 b, Uint8 outline_r, Uint8 outline_g,
                                  Uint8 outline_b, float max_w) const {
    // Clip the run to max_w pixels of advance (sub_41696C 18542-18551): stop
    // at the first glyph whose advance would cross the limit.
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
    // Four outline passes (the (w+2) scratch buffer pins ±1 horizontally; see
    // the header comment), then the ink pass on top.
    static constexpr float kOff[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    for (const auto& o : kOff)
        draw(ren, run, x + o[0], y + o[1], outline_r, outline_g, outline_b);
    return draw(ren, run, x, y, r, g, b);
}

// --- BmScreen -------------------------------------------------------------

void BmScreen::enter(const std::string& bm_name) {
    top_ = 0;
    done_ = false;
    doc_.lines.clear();
    // The .BM files live in the install ROOT (CREDITS.BM, OPTIONS.BM, ...), not
    // under DATA/. A missing/broken file logs and leaves an empty doc so the
    // screen still dismisses on a key rather than aborting.
    try {
        doc_ = assets::bmtext::load(assets_->game_dir() / (bm_name + ".BM"));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "BM screen '%s' load failed: %s\n", bm_name.c_str(), e.what());
    }
}

int BmScreen::visible_rows() const {
    int lh = font_ && font_->loaded() ? font_->line_height() : 16;
    if (lh <= 0) lh = 16;
    return kVisibleHeight / lh;  // v60 = 344 / line_height
}

int BmScreen::max_scroll() const {
    int total = static_cast<int>(doc_.lines.size());
    int vis = visible_rows();
    int m = total - vis;  // sub_41302D clamps top to (count - v60)
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
        case SDLK_UP:
            if (top_ > 0) --top_;  // one line up (v54--)
            break;
        case SDLK_DOWN:
            if (top_ < max_scroll()) ++top_;  // one line down (v54++)
            break;
        case SDLK_PAGEUP:
        case SDLK_LEFT: {
            // Left (331) pages up alongside PgUp (329) — sub_41302D treats
            // both identically (chrome audit 2026-07-12).
            top_ -= visible_rows() - 1;  // v54 -= v60 - 1
            if (top_ < 0) top_ = 0;
            break;
        }
        case SDLK_PAGEDOWN:
        case SDLK_RIGHT: {
            // Right (333) pages down alongside PgDn (337).
            top_ += visible_rows() - 1;  // v54 += v60 - 1
            int m = max_scroll();
            if (top_ > m) top_ = m;
            break;
        }
        default:
            break;
    }
}

void BmScreen::draw(SDL_Renderer* ren) const {
    if (!ren) return;
    // The WINZ-9-patch viewer window (see the kWin* block above) — the
    // original repaints it every dirty frame (sub_41726B @ 16423); the old
    // translucent black band was a port stand-in from before the 9-patch
    // primitive existed.
    draw_dialog_chrome(ren, DialogRect{kWinX, kWinY, kWinW, kWinH},
                       assets_ ? &assets_->frontend_pcx("WINZ") : nullptr);

    if (!font_ || !font_->loaded()) return;
    const int lh = font_->line_height();
    const int vis = visible_rows();
    const int total = static_cast<int>(doc_.lines.size());

    // One screen row per line, from the current top. Each line lays its
    // segments left to right: text runs are drawn with the font, an <IMG>
    // segment blits the named PCX inline and advances the pen by its width —
    // exactly sub_41302D's per-line split-at-tag draw, clipped to the 532-px
    // line budget (v59).
    const float clip_right = static_cast<float>(kTextLeft + kLineClipW);
    for (int j = 0; j < vis; ++j) {
        int li = top_ + j;
        if (li < 0 || li >= total) continue;
        float y = static_cast<float>(kTextTop + j * lh);
        float x = static_cast<float>(kTextLeft);
        for (const auto& seg : doc_.lines[static_cast<std::size_t>(li)]) {
            if (seg.is_text()) {
                // Trim the run to the remaining clip budget (sub_41302D
                // consumes v59 per glyph advance).
                std::string run = seg.value;
                float w = 0;
                std::size_t n = 0;
                for (char ch : run) {
                    const int a = font_->advance(static_cast<unsigned char>(ch));
                    if (x + w + static_cast<float>(a) > clip_right) break;
                    w += static_cast<float>(a);
                    ++n;
                }
                run.resize(n);
                x = font_->draw(ren, run, x, y, kInkR, kInkG, kInkB);
            } else {
                // Inline image: look it up as a front-end PCX by its base name
                // (case as written; the install FS was case-insensitive). Blit
                // its top-left at the pen and advance past it. A missing image
                // just draws nothing and does not advance (matches the original
                // skipping an image whose palette/asset failed to load).
                const Sprite& sp = assets_->frontend_pcx(seg.value);
                if (sp.tex) {
                    SDL_FRect dst{x, y, static_cast<float>(sp.w), static_cast<float>(sp.h)};
                    SDL_RenderTexture(ren, sp.tex, nullptr, &dst);
                    x += static_cast<float>(sp.w);
                }
            }
        }
    }

    // Bottom control row (pseudo.c 16408-16412), window-relative x per the
    // original's literals: \x18 @30, \x19 @60, "Page \x18" @120,
    // "Page \x19" @190, "Done" @516.
    draw_dialog_button(ren, *font_, kWinX + 30.0f, kWinY + kButtonRowY, "\x18");
    draw_dialog_button(ren, *font_, kWinX + 60.0f, kWinY + kButtonRowY, "\x19");
    draw_dialog_button(ren, *font_, kWinX + 120.0f, kWinY + kButtonRowY, "Page \x18");
    draw_dialog_button(ren, *font_, kWinX + 190.0f, kWinY + kButtonRowY, "Page \x19");
    draw_dialog_button(ren, *font_, kWinX + 516.0f, kWinY + kButtonRowY, "Done");
}

// --- HelpBrowser ------------------------------------------------------------

void HelpBrowser::enter(bool manual_enabled) {
    entries_.clear();
    row_ = 0;
    top_ = 0;
    viewing_ = false;
    done_ = false;
    // sub_414235's own first act: gate on getvalue(15) ("is the online manual
    // enabled?", default 1) BEFORE the *.BM glob even runs (docs/re/
    // results-and-options.md §4). When disabled, the browser never lists
    // anything — it shows the pinned getstring(5)/getstring(95) error pair
    // instead (draw() below), same as the "no files found" case but with the
    // "disabled" first line.
    disabled_ = !manual_enabled;
    if (disabled_) return;
    if (!assets_) return;
    // sub_41404B: DOS findfirst/findnext glob of "*.BM" over the install
    // ROOT (not DATA/), qsort_-sorted. std::filesystem::directory_iterator +
    // a case-insensitive extension check is the faithful modern equivalent
    // (§4) — the original glob is case-insensitive on the FAT install media.
    std::error_code ec;
    const std::filesystem::path& root = assets_->game_dir();
    if (!std::filesystem::is_directory(root, ec)) return;
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        if (!entry.is_regular_file()) continue;
        std::string ext = entry.path().extension().string();
        for (auto& c : ext) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (ext == ".BM") entries_.push_back(entry.path());
    }
    std::sort(entries_.begin(), entries_.end());  // qsort_(sub_41400F, count)
}

void HelpBrowser::on_key(SDL_Keycode key, AudioEngine& audio) {
    if (viewing_) {
        bm_.on_key(key);
        return;
    }
    if (disabled_ || entries_.empty()) {
        // §4's two gated error dialogs — "manual disabled" (getstring(5)/(95))
        // and "no .BM files found" (getstring(4)/(95)) — share the same
        // sub_414340 two-line dismiss shape: any dismiss key closes the whole
        // browser, same as SchemeFilePicker's empty-glob path.
        if (key == SDLK_ESCAPE || key == SDLK_RETURN || key == SDLK_SPACE) done_ = true;
        return;
    }
    int count = static_cast<int>(entries_.size());
    switch (key) {
        case SDLK_UP:
        case SDLK_W:
            row_ = (row_ + count - 1) % count;
            audio.play(20);
            break;
        case SDLK_DOWN:
        case SDLK_S:
            row_ = (row_ + 1) % count;
            audio.play(20);
            break;
        case SDLK_PAGEUP:
            row_ = std::max(0, row_ - kVisibleRows);  // 329
            audio.play(20);
            break;
        case SDLK_PAGEDOWN:
            row_ = std::min(count - 1, row_ + kVisibleRows);  // 337
            audio.play(20);
            break;
        case SDLK_HOME:
            row_ = 0;  // 327
            audio.play(20);
            break;
        case SDLK_END:
            row_ = count - 1;  // 335
            audio.play(20);
            break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE:
            audio.play(10);
            // sub_41302D(v12[v14]): open the selected topic through the same
            // .BM viewer; the list re-shows once close_viewer() is called
            // (the caller drives that on bm_.done(), matching sub_414235's
            // do/while loop-back over the same glob array).
            bm_.enter(entries_[static_cast<std::size_t>(row_)].stem().string());
            viewing_ = true;
            break;
        case SDLK_ESCAPE:
            audio.play(10);
            done_ = true;
            break;
        default:
            // Letter-jump (sub_42FEB0 @ 32603): a printable key selects the
            // first entry whose filename starts with it (case-insensitive).
            if (key >= SDLK_A && key <= SDLK_Z) {
                const char want = static_cast<char>('a' + (key - SDLK_A));
                for (int i = 0; i < count; ++i) {
                    std::string f = entries_[static_cast<std::size_t>(i)].filename().string();
                    if (!f.empty() &&
                        std::tolower(static_cast<unsigned char>(f[0])) == want) {
                        row_ = i;
                        audio.play(20);
                        break;
                    }
                }
            }
            break;
    }
    if (row_ < top_) top_ = row_;
    if (row_ >= top_ + kVisibleRows) top_ = row_ - kVisibleRows + 1;
}

void HelpBrowser::draw(SDL_Renderer* ren) const {
    if (!ren) return;
    if (viewing_) {
        // The .BM viewer paints its own dark panel over whatever is already
        // on screen (BmScreen::draw); the caller is expected to have drawn
        // the persistent backdrop first, same as present_bm_screen.
        bm_.draw(ren);
        return;
    }
    // No backdrop paint here (class doc): sub_41485A's list is a floating
    // panel composited over whatever the caller drew (the menu art, or the
    // frozen match field).
    if (!font_ || !font_->loaded()) return;
    if (disabled_ || entries_.empty()) {
        // §4's two gated error dialogs, both drawn through sub_414340 — the
        // WINZ-9-patch acknowledge box (draw_acknowledge_dialog) with an " Ok "
        // button, ink byte_49D0DA (RGB (252,80,80) team-1 red). "manual
        // disabled" = getstring(5), "no .BM files found" = getstring(4); both
        // share getstring(95) "NOTE!" as the header. (The old bare red text at
        // (100,100) with no chrome was a port stand-in.)
        const std::string body =
            assets_ ? assets_->getstring(disabled_ ? 5 : 4,
                                          disabled_ ? "Online manual disabled."
                                                    : "No help files found!")
                    : std::string(disabled_ ? "Online manual disabled." : "No help files found!");
        const std::string head =
            assets_ ? assets_->getstring(95, "NOTE!") : std::string("NOTE!");
        const std::string ok = assets_ ? assets_->getstring(90, " Ok ") : std::string(" Ok ");
        draw_acknowledge_dialog(ren, *font_, assets_ ? &assets_->frontend_pcx("WINZ") : nullptr,
                                head, body, ok, kErrorInkR, kErrorInkG, kErrorInkB);
        return;
    }
    // The generic bevel list dialog (sub_42DBCC) at y=100, header
    // getstring(600) — the SAME primitive/coords SchemeFilePicker's *.SCH
    // picker uses (§4/§5). Width fits the widest of header/entries.
    const std::string header = assets_ ? assets_->getstring(600, "Available help files:")
                                        : std::string("Available help files:");
    int count = static_cast<int>(entries_.size());
    float content_w = static_cast<float>(font_->measure(header));
    for (const auto& e : entries_)
        content_w = std::max(content_w, static_cast<float>(font_->measure(e.filename().string())));
    content_w = std::max(content_w, 200.0f);

    const int last = std::min(count, top_ + kVisibleRows);
    const int visible = last - top_;
    const ListDialogLayout lay =
        draw_list_dialog(ren, *font_, header, 100.0f, content_w, kVisibleRows, count, top_);
    for (int i = top_; i < last; ++i) {
        const int vi = i - top_;
        const bool sel = (i == row_);
        const float ty = lay.item_y0 + static_cast<float>(vi) * lay.item_h;
        std::string name = entries_[static_cast<std::size_t>(i)].filename().string();
        if (sel) {
            // Inverted-band selection: dark base-coat ink over the light band.
            draw_list_selection(ren, lay, vi);
            font_->draw(ren, name, lay.item_x, ty, kDialogFillR, kDialogFillG, kDialogFillB);
        } else {
            font_->draw(ren, name, lay.item_x, ty, kListInkR, kListInkG, kListInkB);
        }
    }
    (void)visible;
}

}  // namespace bomber::game
