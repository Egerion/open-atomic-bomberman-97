#include "bomber/game/sprites.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace bomber::game {

SDL_Texture* make_texture(SDL_Renderer* ren, const assets::Image& img) {
    SDL_Surface* surf =
        SDL_CreateSurfaceFrom(img.width, img.height, SDL_PIXELFORMAT_RGBA32,
                              const_cast<std::uint8_t*>(img.rgba.data()), img.width * 4);
    if (!surf) return nullptr;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(ren, surf);
    SDL_DestroySurface(surf);
    if (tex) SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
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

void AniTextures::load(SDL_Renderer* ren, const std::filesystem::path& path) {
    reset();
    data_ = assets::ani::load(path);
    textures_.assign(data_.frames.size(), nullptr);
    for (std::size_t i = 0; i < data_.frames.size(); ++i)
        if (!data_.frames[i].image.empty()) textures_[i] = make_texture(ren, data_.frames[i].image);
}

AniTextures AniTextures::recolored(SDL_Renderer* ren, const std::int32_t rgb[3]) const {
    AniTextures out;
    out.data_ = data_;
    out.textures_.assign(out.data_.frames.size(), nullptr);
    for (std::size_t i = 0; i < out.data_.frames.size(); ++i) {
        auto& f = out.data_.frames[i];
        if (f.image.empty()) continue;
        f.image = recolor_image(std::move(f.image), rgb);
        out.textures_[i] = make_texture(ren, f.image);
    }
    return out;
}

AniTextures AniTextures::recolored(SDL_Renderer* ren, const std::array<std::uint8_t, 256>& rmp,
                                   const std::array<std::uint8_t, 3>& tail_rgb) const {
    // Per-frame dispatch: the index remap only exists for PALETTED (type 11)
    // frames; this install stores most player art as 16bpp type 4 (survey:
    // 2299 of 2327 frames), where recolor_image_rmp is a structural no-op —
    // which left every player green. For those frames apply the truecolour
    // green-excess recolour (the sub_414A65 BUILDER formula) with the .RMP
    // TAIL percents — the same authoritative per-colour value the builder
    // itself targets (docs/re/player-colour.md "the tail is the authoritative
    // per-colour value"), so both frame types resolve to the same colour.
    const std::int32_t tail[3] = {tail_rgb[0], tail_rgb[1], tail_rgb[2]};
    AniTextures out;
    out.data_ = data_;
    out.textures_.assign(out.data_.frames.size(), nullptr);
    for (std::size_t i = 0; i < out.data_.frames.size(); ++i) {
        auto& f = out.data_.frames[i];
        if (f.image.empty()) continue;
        f.image = f.image.paletted() ? recolor_image_rmp(std::move(f.image), rmp)
                                     : recolor_image(std::move(f.image), tail);
        out.textures_[i] = make_texture(ren, f.image);
    }
    return out;
}

void AniTextures::reset() {
    for (auto* t : textures_)
        if (t) SDL_DestroyTexture(t);
    textures_.clear();
    data_ = {};
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
            out.steps.push_back({ani.texture(static_cast<std::size_t>(st.frame)), f.image.width,
                                 f.image.height, f.hotspot_x, f.hotspot_y, st.dx, st.dy});
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
            a.steps.push_back({ani.texture(static_cast<std::size_t>(st.frame)), f.image.width,
                               f.image.height, f.hotspot_x, f.hotspot_y});
        }
        if (!a.steps.empty()) out.push_back(std::move(a));
    }
}

}  // namespace bomber::game
