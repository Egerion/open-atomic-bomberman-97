#include "bomber/game/bmscreen.hpp"

#include <cstddef>
#include <cstdio>
#include <exception>

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
constexpr int kTextTop = 34;
constexpr int kTextLeft = 34;
constexpr int kVisibleHeight = 344;

// The panel the text sits on. The original composites over the menu page; we
// darken a full-width band so light text stays legible on any backdrop.
constexpr float kPanelX = 16.0f;
constexpr float kPanelY = 16.0f;
constexpr float kPanelW = 608.0f;  // ~ the 600 px scroll window
constexpr float kPanelH = 380.0f;

// Text ink. The original selects a palette index (byte_49D38F, the global draw
// colour); in truecolour we render a light near-white so the .BM prose reads on
// the dark panel. This is a cosmetic port choice (paletted VGA -> RGBA), noted
// in docs/re/frontend-flow.md; layout/advance are the faithful part.
constexpr Uint8 kInkR = 230, kInkG = 230, kInkB = 210;

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
        case SDLK_SPACE:
        case SDLK_ESCAPE:
            // Enter (13) and Escape (27) both finish the viewer in sub_41302D
            // (LABEL_100 sets the done flag on 13/27); Space accepts too.
            done_ = true;
            break;
        case SDLK_UP:
            if (top_ > 0) --top_;  // one line up (v54--)
            break;
        case SDLK_DOWN:
            if (top_ < max_scroll()) ++top_;  // one line down (v54++)
            break;
        case SDLK_PAGEUP: {
            top_ -= visible_rows() - 1;  // v54 -= v60 - 1
            if (top_ < 0) top_ = 0;
            break;
        }
        case SDLK_PAGEDOWN: {
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
    // Dark panel behind the text (see kPanel* rationale above).
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 200);
    SDL_FRect panel{kPanelX, kPanelY, kPanelW, kPanelH};
    SDL_RenderFillRect(ren, &panel);

    if (!font_ || !font_->loaded()) return;
    const int lh = font_->line_height();
    const int vis = visible_rows();
    const int total = static_cast<int>(doc_.lines.size());

    // One screen row per line, from the current top. Each line lays its
    // segments left to right: text runs are drawn with the font, an <IMG>
    // segment blits the named PCX inline and advances the pen by its width —
    // exactly sub_41302D's per-line split-at-tag draw.
    for (int j = 0; j < vis; ++j) {
        int li = top_ + j;
        if (li < 0 || li >= total) continue;
        float y = static_cast<float>(kTextTop + j * lh);
        float x = static_cast<float>(kTextLeft);
        for (const auto& seg : doc_.lines[static_cast<std::size_t>(li)]) {
            if (seg.is_text()) {
                x = font_->draw(ren, seg.value, x, y, kInkR, kInkG, kInkB);
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
}

}  // namespace bomber::game
