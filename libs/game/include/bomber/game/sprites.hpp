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
};

// A resolved animation: one Sprite per sequence step.
struct Anim {
    std::vector<Sprite> steps;
};

// Uploads an RGBA8 image with the requested sampling mode (nullptr on error).
// Classic assets retain crisp nearest-neighbour sampling; high-resolution
// override art uses linear sampling for a modern presentation.
SDL_Texture* make_texture(SDL_Renderer* ren, const assets::Image& img,
                          SDL_ScaleMode scale_mode = SDL_SCALEMODE_NEAREST);

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
            o.textures_.clear();
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

    // A copy with the fallback truecolour player-armour recolor (sub_414A65
    // approximation) applied to every frame. Used only when a colour has no
    // `.RMP` file.
    AniTextures recolored(SDL_Renderer* ren, const std::int32_t rgb[3]) const;

    // A copy with the AUTHENTIC `.RMP` index-remap applied to every PALETTED
    // frame (the original blit's per-colour table, docs/re/player-colour.md).
    // tail_rgb = the `.RMP` tail percents (0..100): non-paletted 16bpp type-4
    // frames (most of this install — 2299 of 2327) instead get the truecolour
    // green-excess recolour targeting the same tail colour, so the whole set
    // resolves to one colour rather than silently staying green.
    AniTextures recolored(SDL_Renderer* ren, const std::array<std::uint8_t, 256>& rmp,
                          const std::array<std::uint8_t, 3>& tail_rgb) const;

    void reset();

    bool loaded() const { return !data_.frames.empty(); }
    const assets::ani::AniFile& data() const { return data_; }
    SDL_Texture* texture(std::size_t frame) const {
        return frame < textures_.size() ? textures_[frame] : nullptr;
    }

private:
    assets::ani::AniFile data_;
    std::vector<SDL_Texture*> textures_;
};

// Builds the Anim for the named sequence ("walk north", "bomb regular green"...).
Anim resolve_sequence(const AniTextures& ani, const std::string& name);

// Appends every 'die ...' sequence of the file as a play-once Anim.
void collect_death_anims(const AniTextures& ani, std::vector<Anim>& out);

}  // namespace bomber::game
