#include "bomber/game/sprites.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace bomber::game {

SDL_Texture* make_texture(SDL_Renderer* ren, const assets::Image& img, SDL_ScaleMode scale_mode,
                          const assets::colorpal::Palette* snap) {
    // In-match master-palette snap (colorpal.hpp): quantize a COPY so the
    // caller's decoded image is left intact (recolor paths reuse it). A no-op
    // when snap is null/inert.
    assets::Image snapped;
    const assets::Image* src = &img;
    if (snap && snap->ok()) {
        snapped = img;
        snap->remap(snapped);
        src = &snapped;
    }
    SDL_Surface* surf =
        SDL_CreateSurfaceFrom(src->width, src->height, SDL_PIXELFORMAT_RGBA32,
                              const_cast<std::uint8_t*>(src->rgba.data()), src->width * 4);
    if (!surf) return nullptr;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(ren, surf);
    SDL_DestroySurface(surf);
    if (tex) SDL_SetTextureScaleMode(tex, scale_mode);
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
            const int baseline = (r + b) / 2;  // sub_414A65 v33
            const int excess = g - baseline;   // (v32 - v33)
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
            textures_[i] = make_texture(ren, data_.frames[i].image, SDL_SCALEMODE_NEAREST, snap);
}

AniTextures AniTextures::recolored(SDL_Renderer* ren, const std::int32_t rgb[3],
                                   const assets::colorpal::Palette* snap) const {
    AniTextures out;
    out.data_ = data_;
    out.textures_.assign(out.data_.frames.size(), nullptr);
    for (std::size_t i = 0; i < out.data_.frames.size(); ++i) {
        auto& f = out.data_.frames[i];
        if (f.image.empty()) continue;
        f.image = recolor_image(std::move(f.image), rgb);
        out.textures_[i] = make_texture(ren, f.image, SDL_SCALEMODE_NEAREST, snap);
    }
    // Recolour the retained HD frames too (never paletted -> green-excess), so
    // the per-player HD sprite sets exist. LINEAR + un-snapped, like load_hd_overlay.
    if (!hd_images_.empty()) {
        out.hd_images_ = hd_images_;
        out.hd_textures_.assign(out.hd_images_.size(), nullptr);
        for (std::size_t i = 0; i < out.hd_images_.size(); ++i) {
            if (out.hd_images_[i].empty()) continue;
            out.hd_images_[i] = recolor_image(std::move(out.hd_images_[i]), rgb);
            out.hd_textures_[i] =
                make_texture(ren, out.hd_images_[i], SDL_SCALEMODE_LINEAR, nullptr);
        }
    }
    return out;
}

AniTextures AniTextures::recolored(SDL_Renderer* ren, const std::array<std::uint8_t, 256>& rmp,
                                   const std::array<std::uint8_t, 3>& tail_rgb,
                                   const assets::colorpal::Palette* snap) const {
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
        out.textures_[i] = make_texture(ren, f.image, SDL_SCALEMODE_NEAREST, snap);
    }
    // HD frames are re-encoded as type-4 truecolour (never paletted), so the
    // index remap can't touch them: use the same green-excess tail recolour the
    // non-paletted classic frames take, then upload LINEAR/un-snapped.
    if (!hd_images_.empty()) {
        out.hd_images_ = hd_images_;
        out.hd_textures_.assign(out.hd_images_.size(), nullptr);
        for (std::size_t i = 0; i < out.hd_images_.size(); ++i) {
            if (out.hd_images_[i].empty()) continue;
            out.hd_images_[i] = recolor_image(std::move(out.hd_images_[i]), tail);
            out.hd_textures_[i] =
                make_texture(ren, out.hd_images_[i], SDL_SCALEMODE_LINEAR, nullptr);
        }
    }
    return out;
}

void AniTextures::reset() {
    for (auto* t : textures_)
        if (t) SDL_DestroyTexture(t);
    textures_.clear();
    for (auto* t : hd_textures_)
        if (t) SDL_DestroyTexture(t);
    hd_textures_.clear();
    hd_images_.clear();
    data_ = {};
}

void AniTextures::load_hd_overlay(SDL_Renderer* ren, const std::filesystem::path& hd_path) {
    // Fresh overlay each call.
    for (auto* t : hd_textures_)
        if (t) SDL_DestroyTexture(t);
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
            hd_textures_[i] = make_texture(ren, hd_images_[i], SDL_SCALEMODE_LINEAR, nullptr);
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
