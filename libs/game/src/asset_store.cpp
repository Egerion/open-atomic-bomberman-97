#include "bomber/game/asset_store.hpp"

#include <cstdio>
#include <exception>
#include <string>
#include <utility>

#include "bomber/assets/pcx.hpp"

namespace bomber::game {

namespace fs = std::filesystem;

bool AssetStore::load(SDL_Renderer* ren, const fs::path& game_dir) {
    ren_ = ren;
    auto ani_dir = game_dir / "DATA" / "ANI";
    auto res_dir = game_dir / "DATA" / "RES";
    try {
        kfont_.load(ren, ani_dir / "KFONT.ANI");
        hurry_.load(ren, ani_dir / "HURRY.ANI");
        bombs_.load(ren, ani_dir / "BOMBS.ANI");
        flame_.load(ren, ani_dir / "FLAME.ANI");
        stand_.load(ren, ani_dir / "STAND.ANI");
        walk_.load(ren, ani_dir / "WALK.ANI");
        shadow_.load(ren, ani_dir / "SHADOW.ANI");

        static constexpr const char* kPowFiles[] = {
            "POWBOMB", "POWFLAME", "POWDISEA", "POWKICK", "POWSKATE", "POWPUNCH", "POWGRAB",
            "POWSPOOG", "POWGOLD", "POWTRIG", "POWJELLY", "POWEBOLA", "POWRAND"};
        for (int i = 0; i < sim::kPowerupKinds; ++i) {
            auto img = assets::pcx::load(res_dir / (std::string(kPowFiles[i]) + ".PCX"));
            sdl::TexturePtr tex{make_texture(ren, img)};
            powerups_[i] = {tex.get(), img.width, img.height, 0, 0};
            powerup_textures_.push_back(std::move(tex));
        }

        // Death animations: every 'die green N' sequence across XPLODE*.ANI.
        for (int i = 1; i <= 32; ++i) {
            auto p = ani_dir / ("XPLODE" + std::to_string(i) + ".ANI");
            if (!fs::exists(p)) continue;
            AniTextures ani;
            ani.load(ren, p);
            collect_death_anims(ani, deaths_);
            xplode_.push_back(std::move(ani));
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "asset load failed: %s\n", e.what());
        return false;
    }
    game_dir_ = game_dir;
    return true;
}

void AssetStore::build_player_sets(const std::int32_t colors[][3]) {
    for (int p = 0; p < kLocalPlayers; ++p) {
        walk_c_[p] = walk_.recolored(ren_, colors[p]);
        stand_c_[p] = stand_.recolored(ren_, colors[p]);
        bombs_c_[p] = bombs_.recolored(ren_, colors[p]);
        flame_c_[p] = flame_.recolored(ren_, colors[p]);
        deaths_c_[p].clear();
        xplode_c_[p].clear();
        for (const auto& src : xplode_) {
            AniTextures colored = src.recolored(ren_, colors[p]);
            collect_death_anims(colored, deaths_c_[p]);
            xplode_c_[p].push_back(std::move(colored));
        }
    }
}

bool AssetStore::load_stage(int stage) {
    try {
        field_.reset(make_texture(
            ren_, assets::pcx::load(game_dir_ / "DATA" / "RES" /
                                    ("FIELD" + std::to_string(stage) + ".PCX"))));
        tiles_.load(ren_, game_dir_ / "DATA" / "ANI" / ("TILES" + std::to_string(stage) + ".ANI"));
        xbrick_.load(ren_,
                     game_dir_ / "DATA" / "ANI" / ("XBRICK" + std::to_string(stage) + ".ANI"));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "stage %d load failed: %s\n", stage, e.what());
        return false;
    }
    return field_ != nullptr;
}

const std::vector<Anim>& AssetStore::deaths_for(int player) const {
    if (player >= 0 && player < kLocalPlayers && !deaths_c_[player].empty())
        return deaths_c_[player];
    return deaths_;
}

}  // namespace bomber::game
