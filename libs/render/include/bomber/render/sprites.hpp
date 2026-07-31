#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "bomber/assets/ani.hpp"
#include "bomber/assets/colorpal.hpp"
#include "bomber/assets/image.hpp"
#include "bomber/game_util/scale_filter.hpp"

// Render-side sprite primitives shared by the game and the asset viewer.

namespace bomber::game {

// One recolored sprite set per player COLOUR. The original supports up to 10
// players, each drawn through its colour's .RMP remap table (0.rmp..9.rmp,
// docs/re/player-colour.md); player i is drawn with colour i. This must cover
// all kMaxPlayers slots — at 2 the renderer collapsed every player past index 1
// to colour 0 (white), so a 4-player match showed white/black/white/white.
// (Presentation only: the 10 recolored sets are baked once at init.)
inline constexpr int kLocalPlayers = 10;

// One drawable frame: a texture plus its hotspot-based anchor.
struct Sprite {
    SDL_Texture* tex = nullptr;
    int w = 0, h = 0, hx = 0, hy = 0;
    // The per-STAT FRAM-leaf offset_x/offset_y (assets::ani::SeqStep::dx/dy).
    // NOT applied by the general draw path (docs/formats/ani.md "Rendering a
    // step" — render by the frame hotspot alone); carried here only for the
    // ONE confirmed exception (real flame arms, sub_426D06's sub_41DB41 call,
    // docs/re/facts.md "Flame draw offset"), which reads it explicitly at its
    // own draw site. Every other caller must keep ignoring these fields.
    int dx = 0, dy = 0;
    // Optional HD override texture (DATA_HD/ANI, 4x de-dithered truecolour). The
    // classic w/h/hx/hy above are KEPT unchanged (1x logical geometry), so
    // positioning is identical; the renderer just samples this higher-res
    // texture into the same logical dst rect when hd_enabled() — the exact
    // trick the front-end HD PCX path uses (asset_store.cpp frontend_pcx). Null
    // when no HD ANI override was authored for this frame -> classic tex used.
    SDL_Texture* tex_hd = nullptr;
};

// A resolved animation: one Sprite per sequence step.
struct Anim {
    std::vector<Sprite> steps;
};

// THE SCALING FILTER (scale_filter.hpp — the F10 "SOFT SCALING" toggle).
//
// SDL3 (3.4.10) implements SDL_SetRenderLogicalPresentation as a viewport +
// scale transform on the draw calls themselves, NOT as an intermediate
// render-target texture (SDL_render.c: the logical mode only feeds
// GetRenderViewportInPixels/logical_scale; there is no logical target to give a
// scale mode to). So there is no single "logical-presentation scaler" knob: the
// upscale is filtered PER TEXTURE, by each texture's own SDL_SetTextureScaleMode
// — and SDL_SetDefaultTextureScaleMode is no substitute, because it only seeds
// `texture->scaleMode` at CREATION (SDL_render.c @SDL_CreateTextureWithProperties)
// and does nothing to the thousands of textures the boot load already uploaded.
// Set it and only later-created textures would smooth: the classic
// silently-half-works bug.
//
// A live toggle therefore has to reach every EXISTING texture, so make_texture
// (the single texture-creation funnel in the whole codebase) keeps a registry of
// the classic ones and set_scale_filter re-stamps them. The registry, not a walk
// over AssetStore, because the owners are spread across AssetStore's ~40
// containers, FontTextures' glyph atlas, BmScreen's inline images and the
// viewer app — a per-container walk would have to be extended by hand for every
// future container, and forgetting one is invisible except as a patch of the
// screen that did not smooth. Registration is keyed on TextureArt, so DATA_HD
// art (always Soft) is never re-stamped.
//
// Single-threaded, like everything else that touches the renderer.
void set_scale_filter(ScaleFilter filter);
ScaleFilter scale_filter();

// SDL's spelling of a ScaleFilter. NEAREST, not SDL_SCALEMODE_PIXELART: the
// crisp path must stay a plain per-pixel replicate, which is what every existing
// tests/visual pin was captured under.
SDL_ScaleMode sdl_scale_mode(ScaleFilter filter);

// Destroys a texture and drops it from the scaling-filter registry. EVERY
// texture from make_texture must be released through this (sdl::TextureDeleter
// does it for the TexturePtr owners) — a raw SDL_DestroyTexture would leave a
// dangling pointer for the next set_scale_filter to stamp.
void destroy_texture(SDL_Texture* tex);

// Uploads an RGBA8 image (nullptr on error). `art` picks the sampling mode via
// art_filter(): Classic art follows the live SOFT SCALING filter and is
// registered for later re-stamping; HighRes (DATA_HD) art is always linear and
// is left out of the registry.
//
// When `snap` is non-null and ok(), the image is run through the in-match
// master-palette quantization (colorpal.hpp) before upload — the original
// snaps EVERY match-time asset (field, tiles, sprites, flames, powerups) to
// the one shared 256-colour hardware palette. Pass it for classic match art;
// leave nullptr for front-end screens and DATA_HD truecolour, which the
// original loads through its non-snapping path.
SDL_Texture* make_texture(SDL_Renderer* ren, const assets::Image& img,
                          TextureArt art = TextureArt::Classic,
                          const assets::colorpal::Palette* snap = nullptr);

// Maps a source rect expressed in a Sprite's CLASSIC 640x480 coordinate space
// onto its BACKING TEXTURE, which may be a DATA_HD replacement several times
// larger.
//
// Every Sprite keeps its classic w/h whatever texture is behind it (that is what
// keeps the front-end's 1997 layout coordinates intact — see Sprite::tex_hd and
// asset_store.cpp frontend_pcx), so any draw that passes a PARTIAL src rect has
// to rescale it or it samples the top-left corner of an HD texture and shows a
// sliver of the art. Draws that pass `nullptr` (the whole texture) are already
// correct and need nothing.
//
// Returns `classic` unchanged when the texture is null or already 1x, so the
// classic path is bit-identical.
SDL_FRect texture_src_rect(SDL_Texture* tex, int classic_w, int classic_h,
                           const SDL_FRect& classic);

// Retargets the green armour of the pre-rendered player sprites, a faithful
// port of the engine's remap-table builder sub_414A65 (0x414A65): a
// green-dominant pixel (G > R && G > B) has its green EXCESS over the (R+B)/2
// baseline scaled into the target percent-RGB (VALUELST 200-247) and the
// baseline added back, so casing/shading survive; other pixels stay put. This
// is the FALLBACK used only when a colour's `.RMP` file is missing (it is a
// truecolour approximation of the table sub_414A65 would have built).
assets::Image recolor_image(assets::Image img, const std::int32_t rgb[3]);

// The AUTHENTIC recolour: apply a colour's loaded `.RMP` index-remap table, the
// same thing the original blit does (sub_415A1C rewrites each pixel's palette
// index through dword_460564[colour] before the palette lookup — docs/re/
// player-colour.md). For every non-transparent pixel of a paletted image
// (8bpp CIMG type 11) the source index is remapped `dst = rmp[src]` and the
// pixel recoloured from the frame's own palette `palette[dst]`; the alpha (the
// key-colour transparency already baked into rgba) is preserved. A non-paletted
// image (16bpp CIMG type 4, no indices) is returned unchanged.
assets::Image recolor_image_rmp(assets::Image img, const std::array<std::uint8_t, 256>& rmp);

// An ANI file with all frames uploaded as textures. Move-only RAII.
class AniTextures {
public:
    AniTextures() = default;
    AniTextures(const AniTextures&) = delete;
    AniTextures& operator=(const AniTextures&) = delete;
    AniTextures(AniTextures&& o) noexcept { *this = std::move(o); }
    AniTextures& operator=(AniTextures&& o) noexcept {
        if (this != &o) {
            reset();
            data_ = std::move(o.data_);
            textures_ = std::move(o.textures_);
            hd_textures_ = std::move(o.hd_textures_);
            hd_images_ = std::move(o.hd_images_);
            o.textures_.clear();
            o.hd_textures_.clear();
        }
        return *this;
    }
    ~AniTextures() { reset(); }

    // Parses the ANI and uploads every frame. Throws on malformed files.
    // When `snap` is non-null and ok(), every frame is run through the
    // in-match master-palette quantization (colorpal.hpp) before upload —
    // used for the classic map art (tiles/bricks), NOT front-end or HD sets.
    void load(SDL_Renderer* ren, const std::filesystem::path& path,
              const assets::colorpal::Palette* snap = nullptr);

    // Loads an optional HD override ANI (DATA_HD/ANI/<same-name>.ANI): 4x
    // de-dithered truecolour frames re-encoded as CIMG type 4. Parsed with the
    // SAME ani::load, uploaded LINEAR and WITHOUT the master-palette snap (the
    // HD path is truecolour, exactly like the HD field/front-end PCX). The
    // override must have the SAME frame count/order as the classic file (it is
    // a 1:1 upscale of it) — a mismatch is ignored, leaving the classic look.
    // Call AFTER load(). The HD frames' own dims/hotspots are irrelevant: the
    // renderer keeps the classic 1x geometry and only swaps the texture. Throws
    // (like load) on a malformed file; callers guard so a bad override just
    // falls back to classic.
    void load_hd_overlay(SDL_Renderer* ren, const std::filesystem::path& hd_path);

    // A copy with the fallback truecolour player-armour recolor (sub_414A65
    // approximation) applied to every frame. Used only when a colour has no
    // `.RMP` file. `snap` (colorpal.hpp) quantizes AFTER the recolor — the
    // original snaps the final displayed colour, so the recoloured result is
    // constrained to the shared match palette exactly like every other cel.
    // `with_hd` gates the (16x-heavier) per-player HD texture build: pass false
    // when HD artwork is off (the default at boot) so the recolor pays nothing
    // for HD; the per-player HD sets are then built lazily on the first Tab via
    // build_recolored_hd(). The returned set's CPU pixel buffers are freed after
    // upload (only its GPU textures + per-frame w/h are retained).
    AniTextures recolored(SDL_Renderer* ren, const std::int32_t rgb[3],
                          const assets::colorpal::Palette* snap = nullptr,
                          bool with_hd = true) const;

    // A copy with the AUTHENTIC `.RMP` index-remap applied to every PALETTED
    // frame (the original blit's per-colour table, docs/re/player-colour.md).
    // tail_rgb = the `.RMP` tail percents (0..100): non-paletted 16bpp type-4
    // frames (most of this install — 2299 of 2327) instead get the truecolour
    // green-excess recolour targeting the same tail colour, so the whole set
    // resolves to one colour rather than silently staying green. `snap` snaps
    // AFTER recolor (see the sibling overload). `with_hd` gates the per-player
    // HD build exactly like the sibling overload.
    AniTextures recolored(SDL_Renderer* ren, const std::array<std::uint8_t, 256>& rmp,
                          const std::array<std::uint8_t, 3>& tail_rgb,
                          const assets::colorpal::Palette* snap = nullptr,
                          bool with_hd = true) const;

    // Builds THIS (already classic-recoloured) set's HD override textures from a
    // base set's retained HD source frames `src`, recoloured to `rgb` (the
    // green-excess tail — HD frames are truecolour type-4, never paletted, so
    // they always take recolor_image, matching recolored()'s HD block). Used to
    // build the per-player HD sets lazily when HD artwork is switched on at
    // runtime (Tab), after the boot-time recolor skipped HD to save memory. The
    // recolour SOURCE pixels (src.hd_images_) stay on the base set; this set does
    // not retain any HD CPU buffer (only the uploaded hd_textures_).
    void build_recolored_hd(SDL_Renderer* ren, const AniTextures& src,
                            const std::int32_t rgb[3]);

    // Presentation memory reclaim (post-upload): once every frame is a GPU
    // texture the CPU-side pixel buffers are dead weight — the renderer samples
    // the textures and resolve_sequence/collect_death_anims read only each
    // frame's w/h. drop_classic_cpu() frees the classic frames' rgba/indices/
    // palette (KEEPING w/h so the sequence resolvers still anchor); drop_hd_cpu()
    // frees the HD recolour-source frames. Call ONLY after the textures exist and
    // the set will not be recoloured again (recolored()/build_recolored_hd()
    // consume the source pixels, so drop AFTER them).
    void drop_classic_cpu();
    void drop_hd_cpu();

    void reset();

    bool loaded() const { return !data_.frames.empty(); }
    const assets::ani::AniFile& data() const { return data_; }
    SDL_Texture* texture(std::size_t frame) const {
        return frame < textures_.size() ? textures_[frame] : nullptr;
    }
    // The HD override texture for a frame, or nullptr when no override is loaded
    // for it (frame index is shared 1:1 with texture() above). Recoloured copies
    // carry their own HD textures, so this is valid on player sets too.
    SDL_Texture* texture_hd(std::size_t frame) const {
        return frame < hd_textures_.size() ? hd_textures_[frame] : nullptr;
    }

private:
    assets::ani::AniFile data_;
    std::vector<SDL_Texture*> textures_;
    // Optional HD override, parallel to textures_ (same frame indices). Empty
    // when no DATA_HD/ANI override exists. hd_images_ is retained so recolored()
    // can rebuild recoloured HD textures for the per-player sets.
    std::vector<SDL_Texture*> hd_textures_;
    std::vector<assets::Image> hd_images_;
};

// Builds the Anim for the named sequence ("walk north", "bomb regular green"...).
Anim resolve_sequence(const AniTextures& ani, const std::string& name);

// Appends every 'die ...' sequence of the file as a play-once Anim.
void collect_death_anims(const AniTextures& ani, std::vector<Anim>& out);

}  // namespace bomber::game
