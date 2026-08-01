#include "bomber/render/sprites.hpp"

#include <algorithm>
#include <cstddef>
#include <unordered_set>
#include <utility>

#include "bomber/game_util/alpha_bleed.hpp"

namespace bomber::game {

namespace {

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

}  // namespace

SDL_ScaleMode sdl_scale_mode(ScaleFilter filter) {
    return filter == ScaleFilter::Soft ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST;
}

ScaleFilter scale_filter() {
    return g_filter;
}

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
    // paths re-read it, and bleed_transparent_rgb mutates pixels).
    assets::Image work = img;
    if (snap && snap->ok()) snap->remap(work);
    // Every uploaded texture must be left safe to sample LINEARLY whatever the
    // current filter is: the filter can be toggled at any time and an already
    // uploaded texture cannot be re-encoded then (drop_classic_cpu has freed
    // its pixels).
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

SDL_FRect texture_src_rect(SDL_Texture* tex, int classic_w, int classic_h,
                           const SDL_FRect& classic) {
    if (!tex || classic_w <= 0 || classic_h <= 0) return classic;
    float tw = 0, th = 0;
    if (!SDL_GetTextureSize(tex, &tw, &th)) return classic;
    const float sx = tw / static_cast<float>(classic_w);
    const float sy = th / static_cast<float>(classic_h);
    if (sx == 1.0f && sy == 1.0f) return classic;
    return SDL_FRect{classic.x * sx, classic.y * sy, classic.w * sx, classic.h * sy};
}

assets::Image recolor_image(assets::Image img, const std::int32_t rgb[3]) {
    // sub_414A65: the green EXCESS over the red/blue baseline is scaled into the
    // target percent-RGB (VALUELST 200/201/202) and the baseline added back, so
    // casing and shading survive. The original then snaps to the nearest 8-bit
    // palette entry; we are truecolour and keep the computed RGB.
    //
    // RETRACTED approximation: an earlier version used lum=G rather than the
    // excess, dropped the baseline and added a fabricated "glint". On the vivid
    // regular bomb the error was small, but on the DESATURATED trigger bomb
    // (mean 58,150,41) it blew the bright pixels to white — a featureless blob
    // for the white player {100,100,100}.
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
    // The authentic recolour (docs/re/player-colour.md): sub_415A1C rewrites
    // each pixel's palette index through dword_460564[colour] and then does the
    // palette lookup, i.e. dst = rmp[src], colour = palette[dst]. The table is
    // total after the load-time backfill (0 entries -> identity), which is why
    // only the colour band changes. Alpha already carries the key-colour
    // transparency from the ANI loader, so only RGB is rewritten.
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
    // The FAITHFUL native recolour for type-4 frames — which is ALL player art
    // in this install (a CIMG survey returns 100% type-4). The native stores
    // even type-4 cels in the 8-bit back buffer as MASTER-palette indices
    // (sub_41C837: `*dst = byte_495390[rgb555]`), and sub_415A1C then remaps
    // that index through the `.RMP` and re-looks-up the master palette:
    //   master_idx = index_of(px) ; disp = master_rgb(rmp[master_idx]).
    // REPLACES the green-excess tint approximation with the artist-authored
    // per-colour shades (2026-07-22 colour-pipeline audit).
    for (std::size_t i = 0; i + 3 < img.rgba.size(); i += 4) {
        if (img.rgba[i + 3] == 0) continue;  // transparent: leave as-is
        const std::uint8_t idx = snap.index_of(img.rgba[i + 0], img.rgba[i + 1], img.rgba[i + 2]);
        snap.master_rgb(rmp[idx], img.rgba[i + 0], img.rgba[i + 1], img.rgba[i + 2]);
    }
    return img;
}

// One frame of the AUTHENTIC per-colour recolour, in priority order. Four
// parameters (the §3 hard ceiling) because each is an independent input the
// choice reads.
static assets::Image recolor_frame(assets::Image img, const std::array<std::uint8_t, 256>& rmp,
                                   const std::int32_t tail[3],
                                   const assets::colorpal::Palette* snap) {
    if (img.paletted()) return recolor_image_rmp(std::move(img), rmp);
    if (snap && snap->ok()) return recolor_image_master(std::move(img), rmp, *snap);
    return recolor_image(std::move(img), tail);
}

void AniTextures::load(SDL_Renderer* ren, const std::filesystem::path& path,
                       const assets::colorpal::Palette* snap) {
    reset();
    data_ = assets::ani::load(path);
    textures_.assign(data_.frames.size(), nullptr);
    // The snap is passed to make_texture rather than applied here, so the
    // retained data_ image stays RAW and recolored() can re-snap its own copies.
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
    // Gated on with_hd: skipped when HD artwork is off (the boot default), and
    // build_recolored_hd() fills these in on the first Tab instead.
    if (with_hd) out.recolor_hd_from(ren, hd_images_, rgb);
    return out;
}

AniTextures AniTextures::recolored(SDL_Renderer* ren, const PlayerRemap& colour,
                                   const assets::colorpal::Palette* snap, bool with_hd) const {
    // Per-frame dispatch. The paletted (type-11) branch is kept for
    // completeness — no frame in this install takes it — and the green-excess
    // `tail` path is the LAST-resort fallback when no COLOR.PAL snap is
    // available (missing install data), matching the pre-audit look.
    const std::int32_t tail[3] = {colour.tail[0], colour.tail[1], colour.tail[2]};
    AniTextures out;
    out.data_ = data_;
    out.textures_.assign(out.data_.frames.size(), nullptr);
    for (std::size_t i = 0; i < out.data_.frames.size(); ++i) {
        auto& f = out.data_.frames[i];
        if (f.image.empty()) continue;
        f.image = recolor_frame(std::move(f.image), colour.rmp, tail, snap);
        // recolor_image_master already emits final master-palette RGB, so the
        // make_texture snap is an idempotent no-op there; it still snaps the
        // paletted/fallback outputs.
        out.textures_[i] = make_texture(ren, f.image, TextureArt::Classic, snap);
    }
    out.drop_classic_cpu();  // textures uploaded; free this set's classic CPU pixels
    // The tail, not the rmp: HD frames are re-encoded as truecolour type-4, so
    // the index remap cannot touch them.
    if (with_hd) out.recolor_hd_from(ren, hd_images_, tail);
    return out;
}

void AniTextures::recolor_hd_from(SDL_Renderer* ren, const std::vector<assets::Image>& src,
                                  const std::int32_t rgb[3]) {
    // HD cels are truecolour type-4 and never paletted, so they always take the
    // green-excess tail recolour, LINEAR and un-snapped. `src` is the BASE set's
    // shared source: copied per frame, never mutated, never retained here.
    if (src.empty()) return;
    hd_textures_.assign(src.size(), nullptr);
    for (std::size_t i = 0; i < src.size(); ++i) {
        if (src[i].empty()) continue;
        assets::Image img = recolor_image(src[i], rgb);
        hd_textures_[i] = make_texture(ren, img, TextureArt::HighRes, nullptr);
    }
}

void AniTextures::build_recolored_hd(SDL_Renderer* ren, const AniTextures& src,
                                     const std::int32_t rgb[3]) {
    // Fresh HD set — a classic-built per-player set has none yet.
    for (auto* t : hd_textures_) destroy_texture(t);
    hd_textures_.clear();
    hd_images_ = {};
    recolor_hd_from(ren, src.hd_images_, rgb);
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

void AniTextures::drop_hd_cpu() {
    hd_images_ = {};
}

void AniTextures::reset() {
    for (auto* t : textures_) destroy_texture(t);
    textures_.clear();
    for (auto* t : hd_textures_) destroy_texture(t);
    hd_textures_.clear();
    hd_images_.clear();
    data_ = {};
}

void AniTextures::load_hd_overlay(SDL_Renderer* ren, const std::filesystem::path& hd_path) {
    // Fresh overlay each call.
    for (auto* t : hd_textures_) destroy_texture(t);
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

namespace {

// One sequence's steps as an Anim, anchored by the FRAME hotspot ALONE. The
// original's standard blit (sub_415920/sub_415A9F) does NOT apply the per-STAT
// FRAM-leaf offset, and those are large for tiles (brick dy=18) and players
// (stand dy=19), so folding them in shoved every such sprite that far DOWN
// while dy=0 sprites looked fine — which is why only *some* things looked "too
// low". The one confirmed exception is the real flame arms, which read dx/dy at
// their own draw site; `carry_offsets` is what makes those values reach it, and
// nothing else may act on them.
Anim sequence_anim(const AniTextures& ani, const assets::ani::Sequence& sq, bool carry_offsets) {
    Anim out;
    for (const auto& st : sq.steps) {
        if (st.frame < 0) continue;
        const auto idx = static_cast<std::size_t>(st.frame);
        const auto& f = ani.data().frames[idx];
        Sprite sp{ani.texture(idx), f.image.width, f.image.height, f.hotspot_x, f.hotspot_y};
        if (carry_offsets) {
            sp.dx = st.dx;
            sp.dy = st.dy;
        }
        sp.tex_hd = ani.texture_hd(idx);
        out.steps.push_back(sp);
    }
    return out;
}

}  // namespace

Anim resolve_sequence(const AniTextures& ani, const std::string& name) {
    for (const auto& s : ani.data().sequences)
        if (s.name == name) return sequence_anim(ani, s, /*carry_offsets=*/true);
    return {};
}

void collect_death_anims(const AniTextures& ani, std::vector<Anim>& out) {
    for (const auto& sq : ani.data().sequences) {
        if (sq.name.rfind("die", 0) != 0) continue;
        Anim a = sequence_anim(ani, sq, /*carry_offsets=*/false);
        if (!a.steps.empty()) out.push_back(std::move(a));
    }
}

}  // namespace bomber::game
