#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "bomber/assets/ani.hpp"
#include "bomber/assets/image.hpp"

// Render-side sprite primitives shared by the game and the asset viewer.

namespace bomber::game {

// How many players get their own recolored sprite sets (local multiplayer).
inline constexpr int kLocalPlayers = 2;

// One drawable frame: a texture plus its hotspot-based anchor.
struct Sprite {
    SDL_Texture* tex = nullptr;
    int w = 0, h = 0, hx = 0, hy = 0;
};

// A resolved animation: one Sprite per sequence step.
struct Anim {
    std::vector<Sprite> steps;
};

// Uploads an RGBA8 image as a nearest-neighbour SDL texture (nullptr on error).
SDL_Texture* make_texture(SDL_Renderer* ren, const assets::Image& img);

// Retargets the green armour of the pre-rendered player sprites: pixels whose
// green channel clearly dominates get their intensity scaled into the target
// color (percent RGB from VALUELST 200-247). Visor/outline pixels stay put;
// specular glints survive so dark targets (black player) stay readable.
assets::Image recolor_image(assets::Image img, const std::int32_t rgb[3]);

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
    void load(SDL_Renderer* ren, const std::filesystem::path& path);

    // A copy with the player-armour recolor applied to every frame.
    AniTextures recolored(SDL_Renderer* ren, const std::int32_t rgb[3]) const;

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
