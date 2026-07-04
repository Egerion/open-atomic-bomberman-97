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
    const AniTextures& duds(int player) const { return pick(duds_, duds_c_, player); }
    const AniTextures& trigbomb(int player) const { return pick(trigbomb_, trigbomb_c_, player); }
    const AniTextures& flame(int player) const { return pick(flame_, flame_c_, player); }
    const AniTextures& stand(int player) const { return pick(stand_, stand_c_, player); }
    const AniTextures& walk(int player) const { return pick(walk_, walk_c_, player); }
    const AniTextures& kick(int player) const { return pick(kick_, kick_c_, player); }
    const AniTextures& punch(int player) const { return pick(punch_, punch_c_, player); }

    // One of the eight CORNER*.ANI masters (idle "cornerhead" fidget frames),
    // recolored when built. Callers resolve "cornerhead N" against each file.
    const AniTextures& corner(int file, int player) const {
        if (file < 0 || file >= kCornerFiles) return corner_[0];
        return pick(corner_[file], corner_c_[file], player);
    }
    static constexpr int corner_files() { return kCornerFiles; }

    // One of the four BWALK*.ANI masters ("carrying a bomb" walk/stand poses),
    // recolored when built. Each file owns one direction, so callers resolve
    // "walkbomb <dir>"/"standbomb <dir>" against each file until one has it.
    const AniTextures& bwalk(int file, int player) const {
        if (file < 0 || file >= kBwalkFiles) return bwalk_[0];
        return pick(bwalk_[file], bwalk_c_[file], player);
    }
    static constexpr int bwalk_files() { return kBwalkFiles; }

    // Death-animation pool (recolored when available).
    const std::vector<Anim>& deaths_for(int player) const;

    const Sprite& powerup(int kind) const { return powerups_[kind]; }
    // Shared animated floor-powerup art (POWERS.ANI). NOT player-coloured, so a
    // single copy is loaded once; callers resolve "power <name>" against it.
    const AniTextures& powers() const { return powers_; }
    // Stage-actor floor art (docs/re/stage-actors.md), shared/uncoloured:
    // CONVEYOR.ANI ("extra conveyor <dir>") and EXTRAS.ANI ("extra trampoline",
    // "extra arrow <dir>", "extra warp 1"). Callers resolve the names against
    // these; a missing file leaves the sequence empty (nothing drawn).
    const AniTextures& conveyor() const { return conveyor_; }
    const AniTextures& extras() const { return extras_; }
    SDL_Texture* field() const { return field_.get(); }

private:
    const AniTextures& pick(const AniTextures& base,
                            const AniTextures (&colored)[kLocalPlayers], int player) const {
        if (player >= 0 && player < kLocalPlayers && colored[player].loaded())
            return colored[player];
        return base;
    }

    // CORNER0.ANI..CORNER7.ANI hold the 13 direction-independent "cornerhead"
    // idle fidget sequences (sub_41F29B), spread unevenly across the 8 files.
    static constexpr int kCornerFiles = 8;

    // BWALK1.ANI..BWALK4.ANI hold the "carrying a bomb" walk/stand poses, one
    // direction per file (1=south, 2=north, 3=west, 4=east).
    static constexpr int kBwalkFiles = 4;

    SDL_Renderer* ren_ = nullptr;
    std::filesystem::path game_dir_;

    AniTextures tiles_, xbrick_, bombs_, duds_, flame_, stand_, walk_, shadow_, kfont_, hurry_;
    AniTextures kick_, punch_;  // action-pose masters (KICK.ANI / PUNCH.ANI)
    AniTextures powers_;  // animated floor-powerup art (POWERS.ANI), shared (uncoloured)
    AniTextures conveyor_; // conveyor belt floor art (CONVEYOR.ANI), shared (uncoloured)
    AniTextures extras_;   // trampoline/arrow/warp floor art (EXTRAS.ANI), shared
    AniTextures trigbomb_;  // trigger-bomb master (TRIGBOMB.ANI), green -> per-player recolor
    AniTextures corner_[kCornerFiles];  // idle-fidget masters (CORNER0..7.ANI)
    AniTextures bwalk_[kBwalkFiles];    // carry-bomb masters (BWALK1..4.ANI)
    AniTextures walk_c_[kLocalPlayers], stand_c_[kLocalPlayers];
    AniTextures bombs_c_[kLocalPlayers], duds_c_[kLocalPlayers], flame_c_[kLocalPlayers];
    AniTextures trigbomb_c_[kLocalPlayers];  // per-player recolor of TRIGBOMB.ANI
    AniTextures kick_c_[kLocalPlayers], punch_c_[kLocalPlayers];
    AniTextures corner_c_[kCornerFiles][kLocalPlayers];
    AniTextures bwalk_c_[kBwalkFiles][kLocalPlayers];

    std::vector<AniTextures> xplode_;                  // XPLODE1..17 source files
    std::vector<Anim> deaths_;                         // green base pool
    std::vector<Anim> deaths_c_[kLocalPlayers];        // recolored pools
    std::vector<AniTextures> xplode_c_[kLocalPlayers]; // keep textures alive

    Sprite powerups_[sim::kPowerupKinds]{};
    std::vector<sdl::TexturePtr> powerup_textures_;    // owners for powerups_
    sdl::TexturePtr field_;
};

}  // namespace bomber::game
