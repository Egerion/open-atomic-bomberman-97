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

// One recolored sprite set per player COLOUR (0.rmp..9.rmp,
// docs/re/player-colour.md); player i is drawn with colour i. Must cover all
// kMaxPlayers slots — at 2, every player past index 1 collapsed to colour 0 and
// a 4-player match showed white/black/white/white.
inline constexpr int kLocalPlayers = 10;

// One drawable frame: a texture plus its hotspot-based anchor.
struct Sprite {
    SDL_Texture* tex = nullptr;
    int w = 0, h = 0, hx = 0, hy = 0;
    // The per-STAT FRAM-leaf offset. NOT applied by the general draw path
    // (docs/formats/ani.md "Rendering a step"); carried only for the ONE
    // confirmed exception, the real flame arms (sub_426D06's sub_41DB41 call),
    // which reads it at its own draw site. Every other caller must ignore it.
    int dx = 0, dy = 0;
    // Optional HD override (DATA_HD/ANI). The classic w/h/hx/hy above are KEPT,
    // so the renderer samples it into the same logical dst rect.
    SDL_Texture* tex_hd = nullptr;
};

// A resolved animation: one Sprite per sequence step.
struct Anim {
    std::vector<Sprite> steps;
};

// The F10 "SOFT SCALING" toggle. SDL3 filters the upscale PER TEXTURE, so a
// live toggle has to reach every EXISTING texture: make_texture keeps a registry
// of the classic ones (keyed on TextureArt, so DATA_HD art is never re-stamped)
// and this walks it. docs/render-notes.md §8.
// Single-threaded, like everything else that touches the renderer.
void set_scale_filter(ScaleFilter filter);
ScaleFilter scale_filter();

// NEAREST, not SDL_SCALEMODE_PIXELART: the crisp path must stay a plain
// per-pixel replicate, which is what every tests/visual pin was captured under.
SDL_ScaleMode sdl_scale_mode(ScaleFilter filter);

// Destroys a texture and drops it from the scaling-filter registry. EVERY
// make_texture texture must be released through this (sdl::TextureDeleter does
// it for the TexturePtr owners) — a raw SDL_DestroyTexture would leave a
// dangling pointer for the next set_scale_filter to stamp.
void destroy_texture(SDL_Texture* tex);

// Uploads an RGBA8 image (nullptr on error). A non-null, ok() `snap` quantizes
// it to the shared 256-colour match palette first — the original snaps EVERY
// match-time asset, and nothing front-end or DATA_HD.
SDL_Texture* make_texture(SDL_Renderer* ren, const assets::Image& img,
                          TextureArt art = TextureArt::Classic,
                          const assets::colorpal::Palette* snap = nullptr);

// Maps a src rect in a Sprite's CLASSIC 640x480 space onto its BACKING texture,
// which may be a larger DATA_HD replacement. Every Sprite keeps its classic w/h
// whatever is behind it (that is what preserves the 1997 layout coordinates), so
// a PARTIAL src rect must be rescaled or it samples the top-left corner of an HD
// texture. `nullptr` draws are already correct.
SDL_FRect texture_src_rect(SDL_Texture* tex, int classic_w, int classic_h,
                           const SDL_FRect& classic);

// Retargets the green armour, a port of the remap-table builder sub_414A65: a
// green-dominant pixel has its green EXCESS over the (R+B)/2 baseline scaled
// into the target percent-RGB (VALUELST 200-247) and the baseline added back.
// The FALLBACK for a colour whose `.RMP` is missing.
assets::Image recolor_image(assets::Image img, const std::int32_t rgb[3]);

// The AUTHENTIC recolour for PALETTED (type 11) frames: the `.RMP` index-remap
// sub_415A1C applies before the palette lookup (docs/re/player-colour.md).
assets::Image recolor_image_rmp(assets::Image img, const std::array<std::uint8_t, 256>& rmp);

// One player colour's recolour inputs, travelling together because they are two
// halves of one answer: the `.RMP` index table, and its tail percents — the
// target for the type-4 frames the index remap cannot touch.
struct PlayerRemap {
    std::array<std::uint8_t, 256> rmp{};
    std::array<std::uint8_t, 3> tail{};
};

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
    void load(SDL_Renderer* ren, const std::filesystem::path& path,
              const assets::colorpal::Palette* snap = nullptr);

    // An optional HD override: a 1:1 upscale, LINEAR and un-snapped. It must
    // have the SAME frame count and order as the classic file — a mismatch is
    // ignored, leaving the classic look. Call AFTER load().
    void load_hd_overlay(SDL_Renderer* ren, const std::filesystem::path& hd_path);

    // A copy with the FALLBACK armour recolor, used only when a colour has no
    // `.RMP`. `snap` quantizes AFTER the recolor, because the original snaps the
    // final displayed colour. `with_hd` gates the 16x-heavier HD build.
    AniTextures recolored(SDL_Renderer* ren, const std::int32_t rgb[3],
                          const assets::colorpal::Palette* snap = nullptr,
                          bool with_hd = true) const;

    // A copy with the AUTHENTIC per-colour recolour. Paletted frames take the
    // `.RMP` index remap directly; the type-4 frames that are ALL of this
    // install's player art (2299 of 2327) reach the same table through the
    // master palette. The tail is the last-resort target with no COLOR.PAL.
    AniTextures recolored(SDL_Renderer* ren, const PlayerRemap& colour,
                          const assets::colorpal::Palette* snap = nullptr,
                          bool with_hd = true) const;

    // The lazy half of the recolor above: THIS set's HD textures, built from a
    // base set's retained HD source frames when Tab first turns HD on.
    void build_recolored_hd(SDL_Renderer* ren, const AniTextures& src, const std::int32_t rgb[3]);

    // Post-upload memory reclaim. drop_classic_cpu() KEEPS each frame's w/h,
    // which the sequence resolvers read to anchor sprites and which
    // Image::empty()/loaded() key off. Call ONLY once the textures exist and the
    // set will not be recoloured again.
    void drop_classic_cpu();
    void drop_hd_cpu();

    void reset();

    bool loaded() const { return !data_.frames.empty(); }
    const assets::ani::AniFile& data() const { return data_; }
    SDL_Texture* texture(std::size_t frame) const {
        return frame < textures_.size() ? textures_[frame] : nullptr;
    }
    // Frame index shared 1:1 with texture(). Recoloured copies carry their own
    // HD textures, so this is valid on player sets too.
    SDL_Texture* texture_hd(std::size_t frame) const {
        return frame < hd_textures_.size() ? hd_textures_[frame] : nullptr;
    }

private:
    // Shared by recolored() and build_recolored_hd(), which differ only in when
    // they run.
    void recolor_hd_from(SDL_Renderer* ren, const std::vector<assets::Image>& src,
                         const std::int32_t rgb[3]);

    assets::ani::AniFile data_;
    std::vector<SDL_Texture*> textures_;
    // Parallel to textures_ (same frame indices), empty without a DATA_HD
    // override. hd_images_ is retained so per-player sets recolour from it.
    std::vector<SDL_Texture*> hd_textures_;
    std::vector<assets::Image> hd_images_;
};

// Builds the Anim for the named sequence ("walk north", "bomb regular green"...).
Anim resolve_sequence(const AniTextures& ani, const std::string& name);

// Appends every 'die ...' sequence of the file as a play-once Anim.
void collect_death_anims(const AniTextures& ani, std::vector<Anim>& out);

}  // namespace bomber::game
