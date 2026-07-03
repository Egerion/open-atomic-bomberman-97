#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <vector>

#include "bomber/game/sdl.hpp"
#include "bomber/game/sprites.hpp"
#include "bomber/sim/constants.hpp"

// Owns every texture the game renders: the shared ANIs, per-player recolored
// copies, the powerup icons, the death-animation pools, and the per-stage art
// (background + tiles + brick-burn). All loaded at runtime from the player's
// own original install.

namespace bomber::game {

class AssetStore {
public:
    // Loads the stage-independent assets. Returns false (and logs) on failure.
    bool load(SDL_Renderer* ren, const std::filesystem::path& game_dir);

    // Builds per-player recolored copies of the player-facing sprite sets
    // (walk/stand/bombs/flames/deaths) from the VALUELST 200..247 colors.
    void build_player_sets(const std::int32_t colors[][3]);

    // Loads the per-stage art (FIELDn.PCX + TILESn.ANI + XBRICKn.ANI).
    bool load_stage(int stage);

    // Stage-independent sets.
    const AniTextures& tiles() const { return tiles_; }
    const AniTextures& xbrick() const { return xbrick_; }
    const AniTextures& shadow() const { return shadow_; }
    const AniTextures& kfont() const { return kfont_; }
    const AniTextures& hurry() const { return hurry_; }

    // Player-facing sets: the recolored copy when built, else the green base.
    const AniTextures& bombs(int player) const { return pick(bombs_, bombs_c_, player); }
    const AniTextures& flame(int player) const { return pick(flame_, flame_c_, player); }
    const AniTextures& stand(int player) const { return pick(stand_, stand_c_, player); }
    const AniTextures& walk(int player) const { return pick(walk_, walk_c_, player); }

    // Death-animation pool (recolored when available).
    const std::vector<Anim>& deaths_for(int player) const;

    const Sprite& powerup(int kind) const { return powerups_[kind]; }
    SDL_Texture* field() const { return field_.get(); }

private:
    const AniTextures& pick(const AniTextures& base,
                            const AniTextures (&colored)[kLocalPlayers], int player) const {
        if (player >= 0 && player < kLocalPlayers && colored[player].loaded())
            return colored[player];
        return base;
    }

    SDL_Renderer* ren_ = nullptr;
    std::filesystem::path game_dir_;

    AniTextures tiles_, xbrick_, bombs_, flame_, stand_, walk_, shadow_, kfont_, hurry_;
    AniTextures walk_c_[kLocalPlayers], stand_c_[kLocalPlayers];
    AniTextures bombs_c_[kLocalPlayers], flame_c_[kLocalPlayers];

    std::vector<AniTextures> xplode_;                  // XPLODE1..17 source files
    std::vector<Anim> deaths_;                         // green base pool
    std::vector<Anim> deaths_c_[kLocalPlayers];        // recolored pools
    std::vector<AniTextures> xplode_c_[kLocalPlayers]; // keep textures alive

    Sprite powerups_[sim::kPowerupKinds]{};
    std::vector<sdl::TexturePtr> powerup_textures_;    // owners for powerups_
    sdl::TexturePtr field_;
};

}  // namespace bomber::game
