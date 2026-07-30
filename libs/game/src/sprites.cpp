#include "bomber/game/sprites.hpp"

#include <algorithm>
#include <cstddef>
#include <unordered_set>
#include <utility>

namespace bomber::game {

namespace {

// The live filter + the classic textures it applies to (sprites.hpp
// "THE SCALING FILTER" for why this is a registry rather than a container walk).
// File-scope because make_texture is a free function called from every texture
// owner in the process (AssetStore, FontTextures, BmScreen, the viewer) — there
// is no object all of them already share to hang it off.
ScaleFilter g_filter = ScaleFilter::Crisp;
// unordered_set, not vector: teardown destroys tens of thousands of textures
// (2327 ANI frames x 10 recoloured player sets alone), and a linear erase per
// destroy would make quitting quadratic.
std::unordered_set<SDL_Texture*>& classic_textures() {
    static std::unordered_set<SDL_Texture*> reg;
    return reg;
}

// Make an image SAFE TO SAMPLE LINEARLY, by bleeding each fully-transparent
// texel's RGB out of its opaque neighbours (the standard "alpha dilate").
//
// Linear filtering reads a texel's RGB even where its ALPHA is zero, and SDL's
// default blend mode is non-premultiplied — so the blend at a sprite edge mixes
// in whatever colour sits behind the transparency. The 1997 art stores the KEY
// COLOUR there: ani.cpp's decode zeroes only the alpha (`rgba[i*4+3] = (v ==
// key_color) ? 0 : 255`) and leaves the key colour's RGB in place. Without this
// pass, switching SOFT SCALING on drew a magenta halo around every cel — a
// defect a player would report as a bug, not as "the smoothing I asked for".
// Measured on the tick-10 demo frame before the fix: a visible key-colour fringe
// along every tile and sprite edge.
//
// Applied to EVERY uploaded image, not just the classic ones: the DATA_HD
// overrides are always uploaded LINEAR (they are de-dithered 4x art), so they
// have always had this fringe, independently of the soft-scaling toggle.
//
// NEAREST cannot see the difference — a zero-alpha texel contributes nothing to
// the blend whatever its RGB — so the crisp path, and every tests/visual pin
// captured under it, stays byte-identical (verified: visual_golden 5/5).
//
// In-place is safe: the pass only WRITES texels with alpha 0 and only READS
// texels with alpha != 0, so a bled texel can never become a source and the
// result does not cascade across the image.
void bleed_transparent_rgb(assets::Image& img) {
    const int w = img.width, h = img.height;
    if (w <= 0 || h <= 0) return;
    const std::size_t need = static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4;
    if (img.rgba.size() < need) return;  // 1997 files are untrusted input
    std::uint8_t* px = img.rgba.data();
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            std::uint8_t* p = px + (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                                   static_cast<std::size_t>(x)) *
                                      4;
            if (p[3] != 0) continue;
            int r = 0, g = 0, b = 0, n = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                const int ny = y + dy;
                if (ny < 0 || ny >= h) continue;
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = x + dx;
                    if ((dx == 0 && dy == 0) || nx < 0 || nx >= w) continue;
                    const std::uint8_t* q =
                        px + (static_cast<std::size_t>(ny) * static_cast<std::size_t>(w) +
                              static_cast<std::size_t>(nx)) *
                                 4;
                    if (q[3] == 0) continue;
                    r += q[0];
                    g += q[1];
                    b += q[2];
                    ++n;
                }
            }
            if (n == 0) continue;  // deep inside a transparent region: nothing to bleed
            p[0] = static_cast<std::uint8_t>(r / n);
            p[1] = static_cast<std::uint8_t>(g / n);
            p[2] = static_cast<std::uint8_t>(b / n);
        }
    }
}

}  // namespace

SDL_ScaleMode sdl_scale_mode(ScaleFilter filter) {
    return filter == ScaleFilter::Soft ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST;
}

ScaleFilter scale_filter() { return g_filter; }

void set_scale_filter(ScaleFilter filter) {
    g_filter = filter;
    const SDL_ScaleMode mode = sdl_scale_mode(filter);
    // Live: SDL_SetTextureScaleMode only writes a field the next draw reads, so
    // the very next presented frame is already filtered the new way — no
    // reload, no window recreate, nothing to restart.
    for (SDL_Texture* t : classic_textures()) SDL_SetTextureScaleMode(t, mode);
}

void destroy_texture(SDL_Texture* tex) {
    if (!tex) return;
    classic_textures().erase(tex);
    SDL_DestroyTexture(tex);
}

SDL_Texture* make_texture(SDL_Renderer* ren, const assets::Image& img, TextureArt art,
                          const assets::colorpal::Palette* snap) {
    // Work on a COPY so the caller's decoded image is left intact (the recolour
    // paths re-read it, and bleed_transparent_rgb below mutates pixels). The
    // in-match master-palette snap (colorpal.hpp) folds into the same copy; it is
    // a no-op when snap is null/inert.
    assets::Image work = img;
    if (snap && snap->ok()) snap->remap(work);
    // Every uploaded texture is left safe to sample linearly, whatever the
    // current filter is — the filter can be toggled at any time and an already
    // uploaded texture cannot be re-encoded then (the CPU pixels are freed after
    // upload, drop_classic_cpu).
    bleed_transparent_rgb(work);
    const assets::Image* src = &work;
    SDL_Surface* surf =
        SDL_CreateSurfaceFrom(src->width, src->height, SDL_PIXELFORMAT_RGBA32,
                              const_cast<std::uint8_t*>(src->rgba.data()), src->width * 4);
    if (!surf) return nullptr;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(ren, surf);
    SDL_DestroySurface(surf);
    if (tex) {
        SDL_SetTextureScaleMode(tex, sdl_scale_mode(art_filter(art, g_filter)));
        // Only classic art tracks the toggle; HD art is linear for good.
        if (art == TextureArt::Classic) classic_textures().insert(tex);
    }
    return tex;
}

assets::Image recolor_image(assets::Image img, const std::int32_t rgb[3]) {
    // Faithful port of the original's remap-table builder sub_414A65 (0x414A65),
    // the per-palette-entry green-armour recolor the engine bakes into each
    // player's `.rmp` table. For a green-dominant source colour it scales the
    // green EXCESS over the red/blue baseline into the target percent-RGB and
    // adds the baseline back, so the shading/casing survives; non-green pixels
    // are left untouched. rgb[] is the percent-RGB from VALUELST 200/201/202
    // (id 200+5k = R%, 201 = G%, 202 = B%; sub_414A65 args a2=R%, a4=G%, a3=B%).
    //
    // Our earlier approximation used lum=G (not the excess), dropped the
    // (R+B)/2 baseline, added a fabricated "glint" and a +24 test margin. On the
    // vivid regular bomb (R~12,B~2) the error was small, but on the DESATURATED
    // trigger bomb (mean 58,150,41) it discarded the ~49 baseline and blew the
    // bright pixels to pure white via the glint -> a featureless white blob for
    // the white player {100,100,100}. The original snaps to the nearest 8-bit
    // palette entry afterwards; we are truecolour so we keep the computed RGB.
    for (std::size_t i = 0; i + 3 < img.rgba.size(); i += 4) {
        if (img.rgba[i + 3] == 0) continue;
        const int r = img.rgba[i], g = img.rgba[i + 1], b = img.rgba[i + 2];
        if (g > r && g > b) {
            const int baseline = (r + b) / 2;  // sub_414A65's own baseline term
            const int excess = g - baseline;   // green above that baseline
            img.rgba[i + 0] =
                static_cast<std::uint8_t>(std::clamp(rgb[0] * excess / 100 + baseline, 0, 255));
            img.rgba[i + 1] =
                static_cast<std::uint8_t>(std::clamp(rgb[1] * excess / 100 + baseline, 0, 255));
            img.rgba[i + 2] =
                static_cast<std::uint8_t>(std::clamp(rgb[2] * excess / 100 + baseline, 0, 255));
        }
    }
    return img;
}

assets::Image recolor_image_rmp(assets::Image img, const std::array<std::uint8_t, 256>& rmp) {
    // The authentic recolour (docs/re/player-colour.md). The original stores each
    // player sprite as an 8-bit paletted image; the blit sub_415A1C rewrites each
    // pixel's palette index through the colour's remap table dword_460564[colour]
    // and then does the palette lookup. We reproduce that here directly on the
    // frame's retained indices + palette: dst = rmp[src], colour = palette[dst].
    //
    // The table is total after the load-time backfill (0 entries -> identity, so
    // shadow/casing/transparent indices map to themselves), which is why only the
    // colour band actually changes and the rest of the sprite is untouched. The
    // key-colour transparency is already baked into rgba's alpha by the ANI
    // loader, so we preserve alpha and only rewrite the RGB.
    if (!img.paletted()) return img;  // 16bpp CIMG (type 4): no indices to remap
    const std::size_t px =
        static_cast<std::size_t>(img.width) * static_cast<std::size_t>(img.height);
    // Defensive: the ANI loader sizes indices == px and palette == 1024 for every
    // type-11 frame, but 1997 files are untrusted — bail rather than run past a
    // short buffer (leaves the frame as its base colour).
    if (img.indices.size() < px || img.palette.size() < std::size_t{256} * 4 ||
        img.rgba.size() < px * 4)
        return img;
    for (std::size_t i = 0; i < px; ++i) {
        if (img.rgba[i * 4 + 3] == 0) continue;  // transparent: leave as-is
        const std::uint8_t dst = rmp[img.indices[i]];
        img.rgba[i * 4 + 0] = img.palette[dst * 4 + 0];
        img.rgba[i * 4 + 1] = img.palette[dst * 4 + 1];
        img.rgba[i * 4 + 2] = img.palette[dst * 4 + 2];
        // rgba[i*4+3] (alpha) preserved.
    }
    return img;
}

static assets::Image recolor_image_master(assets::Image img,
                                          const std::array<std::uint8_t, 256>& rmp,
                                          const assets::colorpal::Palette& snap) {
    // The FAITHFUL native player recolour for 16bpp (type-4) frames — which is
    // ALL player art in this install (a CIMG survey of STAND/WALK/KICK/BOMBS/
    // PUNBOMB/CORNER/BWALK/XPLODE returns 100% type-4, 0 type-11). The native
    // stores even type-4 cels in the 8-bit back buffer as MASTER-palette indices
    // (sub_41C837 decode: `*dst = byte_495390[rgb555]`); the player blit
    // sub_415A1C then rewrites that index through the colour's remap table
    // dword_460564[colour] (== the loaded `.RMP`, master-index -> master-index)
    // and re-looks-up the master palette. Reproduce it exactly, per pixel:
    //   master_idx = index_of(px) ; disp = master_rgb(rmp[master_idx]).
    // Non-green master colours are identity in the .RMP (load-time backfill), so
    // only the green armour band changes — the rest is snapped to its master
    // colour, same as the base texture. This REPLACES the green-excess TINT
    // approximation (recolor_image) with the artist-authored per-colour shades
    // the original actually shows (2026-07-22 colour-pipeline audit, decision a).
    for (std::size_t i = 0; i + 3 < img.rgba.size(); i += 4) {
        if (img.rgba[i + 3] == 0) continue;  // transparent: leave as-is
        const std::uint8_t idx = snap.index_of(img.rgba[i + 0], img.rgba[i + 1], img.rgba[i + 2]);
        snap.master_rgb(rmp[idx], img.rgba[i + 0], img.rgba[i + 1], img.rgba[i + 2]);
    }
    return img;
}

void AniTextures::load(SDL_Renderer* ren, const std::filesystem::path& path,
                       const assets::colorpal::Palette* snap) {
    reset();
    data_ = assets::ani::load(path);
    textures_.assign(data_.frames.size(), nullptr);
    // In-match master-palette snap for classic match art (colorpal.hpp): the
    // original quantizes every decoded cel to the shared 256-colour hardware
    // palette; our raw RGB555 expand5 otherwise renders a few % brighter/more-
    // saturated. Passed to make_texture so the retained data_ image stays raw
    // (recolored() re-snaps its own copies).
    for (std::size_t i = 0; i < data_.frames.size(); ++i)
        if (!data_.frames[i].image.empty())
            textures_[i] = make_texture(ren, data_.frames[i].image, TextureArt::Classic, snap);
}

AniTextures AniTextures::recolored(SDL_Renderer* ren, const std::int32_t rgb[3],
                                   const assets::colorpal::Palette* snap, bool with_hd) const {
    AniTextures out;
    out.data_ = data_;
    out.textures_.assign(out.data_.frames.size(), nullptr);
    for (std::size_t i = 0; i < out.data_.frames.size(); ++i) {
        auto& f = out.data_.frames[i];
        if (f.image.empty()) continue;
        f.image = recolor_image(std::move(f.image), rgb);
        out.textures_[i] = make_texture(ren, f.image, TextureArt::Classic, snap);
    }
    out.drop_classic_cpu();  // textures uploaded; free this set's classic CPU pixels
    // Recolour the base's retained HD frames too (never paletted -> green-excess),
    // so the per-player HD sprite sets exist. LINEAR + un-snapped, like
    // load_hd_overlay. Gated on with_hd: skipped when HD artwork is off (the boot
    // default) — build_recolored_hd() rebuilds these on the first Tab instead. The
    // recolour source (base hd_images_) is copied per frame and left intact; the
    // per-player set keeps only its uploaded HD textures, never a CPU copy.
    if (with_hd && !hd_images_.empty()) {
        out.hd_textures_.assign(hd_images_.size(), nullptr);
        for (std::size_t i = 0; i < hd_images_.size(); ++i) {
            if (hd_images_[i].empty()) continue;
            assets::Image img = recolor_image(hd_images_[i], rgb);
            out.hd_textures_[i] = make_texture(ren, img, TextureArt::HighRes, nullptr);
        }
    }
    return out;
}

AniTextures AniTextures::recolored(SDL_Renderer* ren, const std::array<std::uint8_t, 256>& rmp,
                                   const std::array<std::uint8_t, 3>& tail_rgb,
                                   const assets::colorpal::Palette* snap, bool with_hd) const {
    // Per-frame dispatch. All player art in this install is 16bpp type 4 (a CIMG
    // survey returns 100% type-4), which the native recolours the SAME way as
    // paletted art: snap each pixel to a master index, remap it through the
    // colour's `.RMP` (dword_460564), re-look-up the master palette
    // (recolor_image_master) — the artist-authored shades, not an arithmetic
    // tint. The paletted (type-11) branch is kept for completeness; the
    // green-excess `tail` path is only the LAST-resort fallback when no COLOR.PAL
    // snap is available (missing install data), matching the pre-audit look.
    const std::int32_t tail[3] = {tail_rgb[0], tail_rgb[1], tail_rgb[2]};
    const bool have_snap = snap && snap->ok();
    AniTextures out;
    out.data_ = data_;
    out.textures_.assign(out.data_.frames.size(), nullptr);
    for (std::size_t i = 0; i < out.data_.frames.size(); ++i) {
        auto& f = out.data_.frames[i];
        if (f.image.empty()) continue;
        if (f.image.paletted())
            f.image = recolor_image_rmp(std::move(f.image), rmp);
        else if (have_snap)
            f.image = recolor_image_master(std::move(f.image), rmp, *snap);
        else
            f.image = recolor_image(std::move(f.image), tail);
        // recolor_image_master already emits final master-palette RGB, so the
        // make_texture snap is an idempotent no-op there; it still snaps the
        // paletted/fallback outputs.
        out.textures_[i] = make_texture(ren, f.image, TextureArt::Classic, snap);
    }
    out.drop_classic_cpu();  // textures uploaded; free this set's classic CPU pixels
    // HD frames are re-encoded as type-4 truecolour (never paletted), so the
    // index remap can't touch them: use the same green-excess tail recolour the
    // non-paletted classic frames take, then upload LINEAR/un-snapped. Gated on
    // with_hd (see the sibling overload): the boot recolor skips this and Tab
    // builds it lazily via build_recolored_hd(). Source pixels (base hd_images_)
    // are copied per frame and left intact; no per-player HD CPU copy is kept.
    if (with_hd && !hd_images_.empty()) {
        out.hd_textures_.assign(hd_images_.size(), nullptr);
        for (std::size_t i = 0; i < hd_images_.size(); ++i) {
            if (hd_images_[i].empty()) continue;
            assets::Image img = recolor_image(hd_images_[i], tail);
            out.hd_textures_[i] = make_texture(ren, img, TextureArt::HighRes, nullptr);
        }
    }
    return out;
}

void AniTextures::build_recolored_hd(SDL_Renderer* ren, const AniTextures& src,
                                     const std::int32_t rgb[3]) {
    // Fresh HD set (a classic-built per-player set has none yet). Recolour the
    // base's retained HD source frames (truecolour type-4 -> green-excess tail),
    // upload LINEAR/un-snapped exactly like recolored()'s HD block. src.hd_images_
    // is the shared base source: copied per frame, never mutated or retained here.
    for (auto* t : hd_textures_)
        destroy_texture(t);
    hd_textures_.clear();
    hd_images_ = {};
    if (src.hd_images_.empty()) return;
    hd_textures_.assign(src.hd_images_.size(), nullptr);
    for (std::size_t i = 0; i < src.hd_images_.size(); ++i) {
        if (src.hd_images_[i].empty()) continue;
        assets::Image img = recolor_image(src.hd_images_[i], rgb);
        hd_textures_[i] = make_texture(ren, img, TextureArt::HighRes, nullptr);
    }
}

void AniTextures::drop_classic_cpu() {
    // Free the classic frames' pixel vectors (rgba/indices/palette) but KEEP each
    // frame's width/height — resolve_sequence()/collect_death_anims() read those
    // to anchor sprites, and Image::empty() keys off w/h (so `loaded()` and the
    // empty-frame guards stay correct). `= {}` releases the buffers' capacity.
    for (auto& f : data_.frames) {
        f.image.rgba = {};
        f.image.indices = {};
        f.image.palette = {};
    }
}

void AniTextures::drop_hd_cpu() { hd_images_ = {}; }

void AniTextures::reset() {
    for (auto* t : textures_)
        destroy_texture(t);
    textures_.clear();
    for (auto* t : hd_textures_)
        destroy_texture(t);
    hd_textures_.clear();
    hd_images_.clear();
    data_ = {};
}

void AniTextures::load_hd_overlay(SDL_Renderer* ren, const std::filesystem::path& hd_path) {
    // Fresh overlay each call.
    for (auto* t : hd_textures_)
        destroy_texture(t);
    hd_textures_.clear();
    hd_images_.clear();
    // The HD ANI is a 1:1 upscale of the classic file — decoded the same way.
    assets::ani::AniFile hd = assets::ani::load(hd_path);
    // Must line up frame-for-frame with the classic set, or the texture swap
    // would show the wrong cel. A mismatch means the override is stale/foreign;
    // ignore it (leaves the classic look) rather than corrupt the animation.
    if (hd.frames.size() != data_.frames.size()) return;
    hd_images_.reserve(hd.frames.size());
    hd_textures_.assign(hd.frames.size(), nullptr);
    for (std::size_t i = 0; i < hd.frames.size(); ++i) {
        hd_images_.push_back(std::move(hd.frames[i].image));
        if (!hd_images_[i].empty())
            // LINEAR + no palette snap: the HD path is truecolour, matching the
            // HD field/front-end PCX loads (asset_store.cpp).
            hd_textures_[i] = make_texture(ren, hd_images_[i], TextureArt::HighRes, nullptr);
    }
}

Anim resolve_sequence(const AniTextures& ani, const std::string& name) {
    Anim out;
    for (const auto& s : ani.data().sequences) {
        if (s.name != name) continue;
        for (const auto& st : s.steps) {
            if (st.frame < 0) continue;
            const auto& f = ani.data().frames[static_cast<std::size_t>(st.frame)];
            // Anchor by the FRAME hotspot only — dx/dy are carried through but
            // must NOT be applied by default. The original's standard blit
            // (sub_415920/sub_415A9F) does NOT apply the per-STAT offset
            // (FRAM leaf dx/dy) — those are large for tiles (brick dy=18) and
            // players (stand dy=19), so subtracting them here shoved every
            // sprite that far DOWN (bricks leaked below their cell, players
            // sank below their shadow). dy=0 sprites (bombs, shadow) were fine,
            // which is why only some things looked "too low". The one
            // confirmed exception (real flame arms) reads st.dx/st.dy itself
            // at its own draw site (renderer.cpp draw_world) rather than
            // having it folded in here.
            Sprite sp{ani.texture(static_cast<std::size_t>(st.frame)), f.image.width,
                      f.image.height, f.hotspot_x, f.hotspot_y, st.dx, st.dy};
            // Classic geometry is kept; only the HD texture (if any) is attached
            // — the renderer samples it into the same 1x logical dst rect.
            sp.tex_hd = ani.texture_hd(static_cast<std::size_t>(st.frame));
            out.steps.push_back(sp);
        }
        break;
    }
    return out;
}

void collect_death_anims(const AniTextures& ani, std::vector<Anim>& out) {
    for (const auto& sq : ani.data().sequences) {
        if (sq.name.rfind("die", 0) != 0) continue;
        Anim a;
        for (const auto& st : sq.steps) {
            if (st.frame < 0) continue;
            const auto& f = ani.data().frames[static_cast<std::size_t>(st.frame)];
            Sprite sp{ani.texture(static_cast<std::size_t>(st.frame)), f.image.width,
                      f.image.height, f.hotspot_x, f.hotspot_y};
            sp.tex_hd = ani.texture_hd(static_cast<std::size_t>(st.frame));
            a.steps.push_back(sp);
        }
        if (!a.steps.empty()) out.push_back(std::move(a));
    }
}

}  // namespace bomber::game
