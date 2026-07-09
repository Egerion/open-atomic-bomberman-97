#include "bomber/game/asset_store.hpp"

#include <cstdio>
#include <exception>
#include <string>
#include <utility>

#include "bomber/assets/bmfont.hpp"
#include "bomber/assets/pcx.hpp"
#include "bomber/assets/rmp.hpp"

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
        duds_.load(ren, ani_dir / "DUDS.ANI");
        flame_.load(ren, ani_dir / "FLAME.ANI");
        stand_.load(ren, ani_dir / "STAND.ANI");
        walk_.load(ren, ani_dir / "WALK.ANI");
        kick_.load(ren, ani_dir / "KICK.ANI");
        punch_.load(ren, ani_dir / "PUNCH.ANI");
        shadow_.load(ren, ani_dir / "SHADOW.ANI");

        // Animated floor-powerup art (POWERS.ANI, seq "power <name>"). Shared and
        // NOT player-coloured, loaded once. Cosmetic: a missing/broken file must
        // NOT abort the load — draw_powerups falls back to the static POW*.PCX.
        try {
            auto p = ani_dir / "POWERS.ANI";
            if (fs::exists(p)) powers_.load(ren, p);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "POWERS.ANI load failed: %s\n", e.what());
        }

        // Stage-actor floor art (docs/re/stage-actors.md), shared/uncoloured.
        // CONVEYOR.ANI = "extra conveyor <dir>"; EXTRAS.ANI = "extra trampoline",
        // "extra arrow <dir>", "extra warp 1". Cosmetic and optional: a missing
        // file must NOT abort the load — the tile simply isn't drawn.
        try {
            auto p = ani_dir / "CONVEYOR.ANI";
            if (fs::exists(p)) conveyor_.load(ren, p);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "CONVEYOR.ANI load failed: %s\n", e.what());
        }
        try {
            auto p = ani_dir / "EXTRAS.ANI";
            if (fs::exists(p)) extras_.load(ren, p);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "EXTRAS.ANI load failed: %s\n", e.what());
        }

        // Trigger-bomb art (TRIGBOMB.ANI, seq "bomb trigger green"), recoloured
        // per owner like the regular bomb. Cosmetic: a missing/broken file must
        // NOT abort the load — the bomb draw falls back to the normal pulse.
        try {
            auto p = ani_dir / "TRIGBOMB.ANI";
            if (fs::exists(p)) trigbomb_.load(ren, p);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "TRIGBOMB.ANI load failed: %s\n", e.what());
        }

        // Front-end screen-transition wipe (HEADWIPE.ANI, single "HEAD"
        // sequence). Presentation-only and optional: a missing/broken file must
        // NOT abort the load — the Transition primitive falls back to a fade.
        // (docs/re/frontend-flow.md.)
        try {
            auto p = ani_dir / "HEADWIPE.ANI";
            if (fs::exists(p)) headwipe_.load(ren, p);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "HEADWIPE.ANI load failed: %s\n", e.what());
        }

        // MISC.ANI: teamring0/teamring1 (the scheme editor's start markers,
        // sub_4028D2 aTeamringU), plus cursor1/goldman/ring/safe/scan —
        // sequence table checked against the install's file (docs/re/
        // results-and-options.md §5). Cosmetic and optional: the editor
        // falls back to outline-box markers when this is missing.
        try {
            auto p = ani_dir / "MISC.ANI";
            if (fs::exists(p)) misc_.load(ren, p);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "MISC.ANI load failed: %s\n", e.what());
        }

        // Goldman wheel pointer ("ring" seq, docs/re/goldman-roulette.md §3/§7):
        // RESOLVED — MISC.ANI owns "ring" (see the misc_ load above), so it
        // heads the probe list; the older guesses stay as fallbacks for
        // partial installs. Cosmetic and optional: GoldmanScreen just draws
        // no pointer if every candidate is missing/lacks the sequence.
        {
            static constexpr const char* kRingCandidates[] = {"MISC.ANI", "ROULETTE.ANI",
                                                               "EXTRAS.ANI", "CURSOR.ANI"};
            for (const char* name : kRingCandidates) {
                auto p = ani_dir / name;
                try {
                    if (fs::exists(p)) {
                        AniTextures probe;
                        probe.load(ren, p);
                        if (!resolve_sequence(probe, "ring").steps.empty()) {
                            ring_ = std::move(probe);
                            break;
                        }
                    }
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "%s load failed (ring probe): %s\n", name, e.what());
                }
            }
        }

        // Front-end bitmap font for the .BM help/credits screens. The engine
        // draws every text string through the active font, which graphics-init
        // pins to FONT6 (sub_431E9C(6), BM95.EXE @ 0x417600). The FONT<n>.FON
        // files live in the install ROOT, not under DATA/. Presentation-only and
        // optional: a missing/broken font must NOT abort the load — the BM
        // viewer then simply renders no glyphs. (docs/formats/fon.md.)
        try {
            auto p = game_dir / "FONT6.FON";
            if (fs::exists(p)) frontend_font_ = assets::bmfont::load(p);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "FONT6.FON load failed: %s\n", e.what());
        }

        // The MESSAGES.TXT string table (getstring / sub_4124A4): the setup and
        // net-game screens format their labels from it. Install ROOT, like the
        // fonts. Optional — a missing file leaves getstring() returning fallbacks.
        try {
            auto p = game_dir / "MESSAGES.TXT";
            if (fs::exists(p)) messages_ = assets::res::load_messages(p);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "MESSAGES.TXT load failed: %s\n", e.what());
        }

        // The ten player-colour remap tables 0.RMP..9.RMP (install ROOT), the
        // authentic per-colour index remap the original blit applies to the
        // "green" player sprites (sub_415A1C via dword_460564[colour] —
        // docs/re/player-colour.md). Each is loaded in isolation and guarded:
        // a missing/short file just marks that colour rmp_ok_=false, and
        // build_player_sets then falls back to the truecolour recolour for that
        // slot. Never fatal — a slot without a .RMP still renders (approximately).
        for (int i = 0; i < kColors; ++i) {
            auto p = game_dir / (std::to_string(i) + ".RMP");
            try {
                if (fs::exists(p)) {
                    auto rt = assets::res::load_rmp(p);
                    rmp_[i] = rt.map;
                    rmp_rgb_[i] = rt.rgb;  // authoritative slot colour (setup screen)
                    rmp_ok_[i] = true;
                } else {
                    std::fprintf(stderr, "%d.RMP missing; using fallback recolour\n", i);
                }
            } catch (const std::exception& e) {
                std::fprintf(stderr, "%d.RMP load failed (%s); using fallback recolour\n", i,
                             e.what());
            }
        }

        // Idle "cornerhead" fidgets (CORNER0..7.ANI). Cosmetic and optional:
        // a missing/broken CORNER file must NOT abort the whole load, so each
        // is loaded in isolation — resolution just falls back to stand.
        for (int i = 0; i < kCornerFiles; ++i) {
            auto p = ani_dir / ("CORNER" + std::to_string(i) + ".ANI");
            try {
                if (fs::exists(p)) corner_[i].load(ren, p);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "cornerhead load failed (%s): %s\n",
                             p.string().c_str(), e.what());
            }
        }

        // "Carrying a bomb" poses (BWALK1..4.ANI, one direction per file). Like
        // the cornerhead fidgets these are cosmetic: a missing/broken BWALK file
        // must NOT abort the load, so each is isolated — resolution just falls
        // back to plain walk/stand.
        for (int i = 0; i < kBwalkFiles; ++i) {
            auto p = ani_dir / ("BWALK" + std::to_string(i + 1) + ".ANI");
            try {
                if (fs::exists(p)) bwalk_[i].load(ren, p);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "carry-bomb load failed (%s): %s\n",
                             p.string().c_str(), e.what());
            }
        }

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
        // Player slot p's intrinsic colour index is p itself (slot 0 = white /
        // 0.RMP, slot 1 = black / 1.RMP; docs/re/setup-screens.md — colour is
        // keyed by slot index, there is no picker). Prefer the authentic p.RMP
        // index remap; fall back to the truecolour recolour(color_rgb) only when
        // that colour's .RMP was missing/short (rmp_ok_[p] == false).
        const bool use_rmp = (p < kColors) && rmp_ok_[p];
        auto recolor = [&](const AniTextures& src) {
            // rmp_rgb_[p] = the .RMP tail — the fallback target for the 16bpp
            // type-4 frames the index remap cannot touch (sprites.cpp).
            return use_rmp ? src.recolored(ren_, rmp_[p], rmp_rgb_[p])
                           : src.recolored(ren_, colors[p]);
        };

        walk_c_[p] = recolor(walk_);
        stand_c_[p] = recolor(stand_);
        kick_c_[p] = recolor(kick_);
        punch_c_[p] = recolor(punch_);
        for (int f = 0; f < kCornerFiles; ++f)
            if (corner_[f].loaded()) corner_c_[f][p] = recolor(corner_[f]);
        for (int f = 0; f < kBwalkFiles; ++f)
            if (bwalk_[f].loaded()) bwalk_c_[f][p] = recolor(bwalk_[f]);
        bombs_c_[p] = recolor(bombs_);
        duds_c_[p] = recolor(duds_);
        if (trigbomb_.loaded()) trigbomb_c_[p] = recolor(trigbomb_);
        flame_c_[p] = recolor(flame_);
        deaths_c_[p].clear();
        xplode_c_[p].clear();
        for (const auto& src : xplode_) {
            AniTextures colored = recolor(src);
            collect_death_anims(colored, deaths_c_[p]);
            xplode_c_[p].push_back(std::move(colored));
        }
    }
}

void AssetStore::set_color_fallbacks(const std::int32_t colors[][3], int n) {
    // Fill in the slot colour for any colour index that had no .RMP tail, from
    // the VALUELST percent table. A loaded .RMP already set rmp_rgb_[i] from its
    // own (authoritative) tail, so only touch the ones that failed to load.
    for (int i = 0; i < kColors && i < n; ++i) {
        if (rmp_ok_[i]) continue;  // keep the .RMP tail
        rmp_rgb_[i][0] = static_cast<std::uint8_t>(colors[i][0]);
        rmp_rgb_[i][1] = static_cast<std::uint8_t>(colors[i][1]);
        rmp_rgb_[i][2] = static_cast<std::uint8_t>(colors[i][2]);
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

const AssetStore::StagePreview& AssetStore::stage_preview(int stage) const {
    if (auto it = stage_preview_.find(stage); it != stage_preview_.end()) return it->second;
    // Cache an entry (even a partial/empty one) for every stage requested, so
    // a missing/broken file logs once and thereafter just draws nothing —
    // same guarded-cache shape as frontend_pcx() above (docs/re/setup-
    // screens.md "sample-block preview").
    StagePreview sp{};
    try {
        AniTextures tiles;
        tiles.load(ren_, game_dir_ / "DATA" / "ANI" / ("TILES" + std::to_string(stage) + ".ANI"));
        const std::string n = std::to_string(stage);
        sp.solid = resolve_sequence(tiles, "tile " + n + " solid");
        sp.brick = resolve_sequence(tiles, "tile " + n + " brick");
        stage_preview_tiles_.emplace(stage, std::move(tiles));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "stage %d preview tiles load failed: %s\n", stage, e.what());
    }
    try {
        auto img = assets::pcx::load(game_dir_ / "DATA" / "RES" /
                                     ("FIELD" + std::to_string(stage) + ".PCX"));
        sdl::TexturePtr tex{make_texture(ren_, img)};
        sp.field = tex.get();
        stage_preview_field_.emplace(stage, std::move(tex));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "stage %d preview field load failed: %s\n", stage, e.what());
    }
    return stage_preview_.emplace(stage, std::move(sp)).first->second;
}

const std::vector<Anim>& AssetStore::deaths_for(int player) const {
    if (player >= 0 && player < kLocalPlayers && !deaths_c_[player].empty())
        return deaths_c_[player];
    return deaths_;
}

const Sprite& AssetStore::frontend_pcx(const std::string& name) const {
    if (auto it = front_pcx_.find(name); it != front_pcx_.end()) return it->second;
    // Cache an entry for every request (even failures) so a missing file logs
    // once and thereafter returns the same empty Sprite the Screen skips.
    Sprite sp{};
    try {
        auto img = assets::pcx::load(game_dir_ / "DATA" / "RES" / (name + ".PCX"));
        sdl::TexturePtr tex{make_texture(ren_, img)};
        sp = {tex.get(), img.width, img.height, 0, 0};
        front_textures_.push_back(std::move(tex));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "front-end PCX '%s' load failed: %s\n", name.c_str(), e.what());
    }
    return front_pcx_.emplace(name, sp).first->second;
}

}  // namespace bomber::game
