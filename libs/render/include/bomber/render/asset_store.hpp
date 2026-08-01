#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>  // boot-loading progress callback (load / build_player_sets)
#include <map>
#include <string>
#include <vector>

#include "bomber/assets/bmfont.hpp"
#include "bomber/assets/colorpal.hpp"
#include "bomber/assets/messages.hpp"
#include "bomber/assets/rmp.hpp"
#include "bomber/match/level_registry.hpp"
#include "bomber/render/sdl.hpp"
#include "bomber/render/sprites.hpp"
#include "bomber/sim/constants.hpp"

// Owns every texture the game renders: the shared ANIs, per-player recolored
// copies, the powerup icons, the death-animation pools, and the per-stage art.
// All loaded at runtime from the player's own original install.
// docs/render-notes.md §6 (the load pipeline) and §7 (the two PCX loaders).

namespace bomber::game {

// The shared arguments of one load() pass — defined in asset_store.cpp.
struct AssetLoadPass;

// SIZE EXCEPTION (coding-standards §3, "class length 200 lines"): 323 lines of
// body, 110 of them comment and ~177 code — under the target on code, over it
// on raw span. It also fails §5's single-responsibility: the match sprite banks
// and the front-end/`.BM` PCX caches are two jobs. Splitting them changes a
// public API that libs/ui, libs/frontend and libs/editor all call, which is a
// cross-package change and not this pass's to make.
class AssetStore {
public:
    // Loads the stage-independent assets. Returns false (and logs) on failure.
    // `progress` reports a monotonic 0 -> 1 as each group lands, so the boot
    // LOADING dialog can animate + pump the window.
    bool load(SDL_Renderer* ren, const std::filesystem::path& game_dir,
              const std::function<void(float)>& progress = {});

    // Ahead of the full load(): sub_41095A pins FONT6 via sub_414DF4 BEFORE the
    // two boot LOADING dialogs run, so the ORIGINAL has the font ready for both
    // flashes. Idempotent — load() calls it too.
    void load_frontend_font(const std::filesystem::path& game_dir);

    // Likewise WINZ.PCX (the sub_43C734 window-chrome 9-patch skin), which
    // sub_414DF4 loads as "winz.plt" during graphics init. Populates the shared
    // frontend_pcx("WINZ") cache.
    const Sprite& load_frontend_winz(SDL_Renderer* ren, const std::filesystem::path& game_dir);

    // Prefers each slot's authentic .RMP index remap; `colors` (VALUELST
    // 200..247) is the truecolour fallback for a slot whose .RMP was missing.
    void build_player_sets(const std::int32_t colors[][3],
                           const std::function<void(float)>& progress = {});

    // Seeds rmp_rgb_ for every colour whose .RMP was absent, so slot_color() has
    // an authoritative value for all 10 slots. A loaded .RMP's own tail wins.
    void set_color_fallbacks(const std::int32_t colors[][3], int n);

    // Base names come from the level registry: built-ins resolve to
    // FIELDn/TILESn/XBRICKn, byte-identical to the old hardcoded concat.
    bool load_stage(int stage);

    // The level catalogue (bomber::match, SDL-free). Adding a custom map is one
    // `assets.levels().add({...})`.
    match::LevelRegistry& levels() { return levels_; }
    const match::LevelRegistry& levels() const { return levels_; }

    // Stage-independent sets.
    const AniTextures& tiles() const { return tiles_; }
    const AniTextures& xbrick() const { return xbrick_; }
    const AniTextures& shadow() const { return shadow_; }
    const AniTextures& kfont() const { return kfont_; }
    const AniTextures& hurry() const { return hurry_; }

    // Player-facing sets: the recolored copy when built, else the green base.
    const AniTextures& bombs(int player) const { return pick(bombs_, bombs_c_, player); }
    const AniTextures& duds(int player) const { return pick(duds_, duds_c_, player); }
    // Backed by TRIGANIM.ANI (see trigbomb_), still named for the GAME CONCEPT.
    const AniTextures& trigbomb(int player) const { return pick(trigbomb_, trigbomb_c_, player); }
    const AniTextures& flame(int player) const { return pick(flame_, flame_c_, player); }
    const AniTextures& stand(int player) const { return pick(stand_, stand_c_, player); }
    const AniTextures& walk(int player) const { return pick(walk_, walk_c_, player); }
    const AniTextures& kick(int player) const { return pick(kick_, kick_c_, player); }

    // One master of a multi-file family. These four spread their sequences
    // across files with no fixed file->name convention, so callers resolve a
    // name against every file until one owns it (sequences.cpp).
    const AniTextures& corner(int file, int player) const {
        if (file < 0 || file >= kCornerFiles) return corner_[0];
        return pick(corner_[file], corner_c_[file], player);
    }
    static constexpr int corner_files() { return kCornerFiles; }

    const AniTextures& bwalk(int file, int player) const {
        if (file < 0 || file >= kBwalkFiles) return bwalk_[0];
        return pick(bwalk_[file], bwalk_c_[file], player);
    }
    static constexpr int bwalk_files() { return kBwalkFiles; }

    // CORRECTED 2026-07-09 (facts.md "ANI sequence-name audit"): MASTER.ALI
    // loads punbomb1..4.ani, never punch.ani, so PUNCH.ANI is dead art.
    const AniTextures& punch(int file, int player) const {
        if (file < 0 || file >= kPunchFiles) return punch_[0];
        return pick(punch_[file], punch_c_[file], player);
    }
    static constexpr int punch_files() { return kPunchFiles; }

    // Same correction: MASTER.ALI loads pup1..4.ani, never bpickup.ani.
    const AniTextures& pickup(int file, int player) const {
        if (file < 0 || file >= kPupFiles) return pickup_[0];
        return pick(pickup_[file], pickup_c_[file], player);
    }
    static constexpr int pickup_files() { return kPupFiles; }

    // Death-animation pool (recolored when available).
    const std::vector<Anim>& deaths_for(int player) const;

    const Sprite& powerup(int kind) const { return powerups_[kind]; }
    // Shared, NOT player-coloured, so one copy each. A missing file leaves the
    // sequence empty: draw_powerups falls back to POW*.PCX, the stage actors
    // draw nothing, the rovers get the renderer's plain marker.
    const AniTextures& powers() const { return powers_; }      // POWERS.ANI
    const AniTextures& conveyor() const { return conveyor_; }  // CONVEYOR.ANI
    const AniTextures& extras() const { return extras_; }      // EXTRAS.ANI
    // ALIENS1.ANI "ghost <dir>"/"rover <dir>" — believed cut content until
    // 2026-07-09, since no GHOST.ANI/ROVER.ANI exists; the sequences ship here.
    const AniTextures& aliens1() const { return aliens1_; }

    // Runtime HD toggle. Never changes simulation state or original assets:
    // DATA_HD files are optional overrides and every missing one falls back.
    void set_hd_enabled(bool enabled) { hd_enabled_ = enabled; }
    bool hd_enabled() const { return hd_enabled_; }

    // Builds the per-player HD sets on demand, ONCE (build_player_sets skips the
    // 16x-heavier HD recolor at boot). Returns true if it built this call, so
    // the caller re-resolves its SequenceSet to pick up the new tex_hd.
    bool ensure_player_hd_sets();

    SDL_Texture* field() const { return hd_enabled_ && field_hd_ ? field_hd_.get() : field_.get(); }

    // One level's SAMPLE-BLOCK preview art (sub_406AA3), loaded independently of
    // load_stage()'s live match slot so the LEVEL & ROUNDS screen can cycle
    // previews without disturbing an in-progress match.
    struct StagePreview {
        Anim solid, brick;
        SDL_Texture* field = nullptr;
    };
    const StagePreview& stage_preview(int stage) const;

    // Front-end full-screen art (docs/re/frontend-flow.md), keyed by the base
    // name the original passes to sub_42A088. Loaded fully OPAQUE.
    const Sprite& frontend_pcx(const std::string& name) const;

    // The SAME files, loaded the way the `.BM` viewer loads them: the inline
    // blit KEYS on palette index 0 and sub_41302D SNAPS to the master palette,
    // neither of which the backdrop path does. Without both, CREDITS.BM's inline
    // art draws as black rectangles. docs/render-notes.md §7.
    const Sprite& bm_inline_pcx(const std::string& name) const;

    // Decoded from a byte array COMPILED INTO the executable: the point of the
    // single-file build is that the user drops one exe into their install, so an
    // asset travelling beside it would undo that.
    const Sprite& author_photo() const;

    // The Goldman wheel's "ring" pointer — RESOLVED: MISC.ANI owns it, so the
    // probe tries that first and keeps the older guesses as fallbacks.
    const AniTextures& ring() const { return ring_; }

    // MISC.ANI wholesale — the editor's start markers draw its "teamring%u"
    // sequences (sub_4028D2).
    const AniTextures& misc() const { return misc_; }

    // EDIT.ANI — the editor's schematic "tile -1 blank/brick/solid" tiles. The
    // '0'-key toggle (sub_402206's dword_45B7B8 == -1) resolves those names from
    // the original's GLOBAL sequence pool, landing here, so it was never a dead
    // state.
    const AniTextures& edit() const { return edit_; }

    // The install ROOT (parent of DATA) — where the `.BM` screens and the
    // FONT<n>.FON fonts live.
    const std::filesystem::path& game_dir() const { return game_dir_; }

    // FONT6.FON, the font graphics-init pins via sub_431E9C(6).
    const assets::bmfont::Font& frontend_font() const { return frontend_font_; }

    // MESSAGES.TXT (the original's getstring / sub_4124A4).
    std::string getstring(int id, const std::string& fallback = std::string()) const {
        return messages_.get_or(id, fallback);
    }

    // Player-slot `i`'s on-screen RGB888 — the setup screen's label ink
    // sub_41672F, which quantises the slot's stored RGB (the .RMP tail) to 5
    // bits each and looks it up in the palette LUT. We are truecolour, so we
    // expand the 5-bit channels back to 8 instead. Running the authoritative
    // tail through the game's own quantisation is what makes each slot read as
    // its true in-game colour.
    void slot_color(int i, std::uint8_t out[3]) const {
        auto q = [](std::uint8_t v) -> std::uint8_t {
            int f = v / 3;
            if (f > 31) f = 31;                                     // sub_41672F clamp to 5 bits
            return static_cast<std::uint8_t>((f << 3) | (f >> 2));  // expand5
        };
        if (i < 0 || i >= kColors) {
            out[0] = out[1] = out[2] = 128;
            return;
        }
        out[0] = q(rmp_rgb_[i][0]);
        out[1] = q(rmp_rgb_[i][1]);
        out[2] = q(rmp_rgb_[i][2]);
    }

private:
    // One load() pass, group by group, in MASTER.ALI's order. Only
    // load_core_anis may throw past its own body: everything it loads is
    // required, while every group below it is optional and isolates its own
    // failures so a partial install still boots.
    void load_all_groups(const AssetLoadPass& in);
    void load_core_anis(const AssetLoadPass& in);
    void load_optional_anis(const AssetLoadPass& in);
    void load_ring(const AssetLoadPass& in);
    void load_root_files(const AssetLoadPass& in);
    void load_rmp_table(const std::filesystem::path& game_dir, int i);
    void load_pose_families(const AssetLoadPass& in);
    void load_powerup_icons(const AssetLoadPass& in);
    void load_death_anims(const AssetLoadPass& in);
    void apply_hd_overlays(const AssetLoadPass& in);

    void load_stage_field(const std::string& field_base);
    void load_stage_tiles(const std::string& tiles_base, const std::string& xbrick_base);

    // The classic decode behind each PCX cache, and the shared DATA_HD upgrade
    // both apply on top of it. `owner` is the cache's own texture-owner vector
    // (four parameters, the §3 hard ceiling: the two caches differ in exactly
    // that vector and in whether the HD upscale is keyed).
    Sprite load_front_sprite(const std::string& name) const;
    Sprite load_bm_sprite(const std::string& name) const;
    Sprite hd_override(const std::string& name, Sprite classic, bool key_black,
                       std::vector<sdl::TexturePtr>& owner) const;

    void build_player_set(int p, const std::int32_t colors[][3], bool with_hd);
    void build_player_hd_set(int p);

    // Every set build_player_sets recolours — the recolour SOURCES, whose CPU
    // pixels are dead once the per-player copies exist (classic once every
    // player is built, HD once the per-player HD sets are). Each frame's w/h
    // and GPU textures survive.
    std::vector<AniTextures*> player_base_sets();
    void drop_player_base_classic_cpu();
    void drop_player_base_hd_cpu();

    // bugprone-return-const-ref-from-parameter (NOLINT below) — private helper,
    // every call site passes a member AniTextures_ with `this`'s lifetime.
    const AniTextures& pick(const AniTextures& base, const AniTextures (&colored)[kLocalPlayers],
                            int player) const {
        if (player >= 0 && player < kLocalPlayers && colored[player].loaded())
            return colored[player];
        return base;  // NOLINT(bugprone-return-const-ref-from-parameter)
    }

    // CORNER0..7 hold the 13 direction-independent fidgets; BWALK1..4 the
    // carry poses (1=south, 2=north, 3=west, 4=east); PUNBOMB1..4 the punch;
    // PUP1..4 the pickup. One direction per file for the latter three.
    static constexpr int kCornerFiles = 8;
    static constexpr int kBwalkFiles = 4;
    static constexpr int kPunchFiles = 4;
    static constexpr int kPupFiles = 4;

    SDL_Renderer* ren_ = nullptr;
    std::filesystem::path game_dir_;

    match::LevelRegistry levels_ = match::LevelRegistry::with_builtins();

    // The in-match shared-palette snap, loaded once from the install root. Inert
    // when COLOR.PAL is absent, so the game degrades to the raw per-asset
    // decode. docs/re/facts.md "In-match colour quantization".
    assets::colorpal::Palette colorpal_;

    AniTextures tiles_, xbrick_, bombs_, duds_, flame_, stand_, walk_, shadow_, kfont_, hurry_;
    AniTextures kick_;      // action-pose master (KICK.ANI)
    AniTextures powers_;    // animated floor-powerup art (POWERS.ANI), shared
    AniTextures conveyor_;  // conveyor belt floor art (CONVEYOR.ANI), shared
    AniTextures extras_;    // trampoline/arrow/warp floor art (EXTRAS.ANI), shared
    AniTextures aliens1_;   // campaign rover/ghost hazard art (ALIENS1.ANI), shared
    // Loaded from TRIGANIM.ANI despite the member name. CORRECTED 2026-07-09:
    // MASTER.ALI comments out `;-trigbomb.ani` and loads `-triganim.ani`, so
    // TRIGBOMB.ANI's 7-step "bomb trigger green" never enters the original's
    // pool — TRIGANIM.ANI's 19-step rise-then-fall cycle is what is shown.
    AniTextures trigbomb_;
    AniTextures corner_[kCornerFiles];  // idle-fidget masters (CORNER0..7.ANI)
    AniTextures bwalk_[kBwalkFiles];    // carry-bomb masters (BWALK1..4.ANI)
    AniTextures punch_[kPunchFiles];    // punch-pose masters (PUNBOMB1..4.ANI)
    AniTextures pickup_[kPupFiles];     // pickup-pose masters (PUP1..4.ANI)
    AniTextures walk_c_[kLocalPlayers], stand_c_[kLocalPlayers];
    AniTextures bombs_c_[kLocalPlayers], duds_c_[kLocalPlayers], flame_c_[kLocalPlayers];
    AniTextures trigbomb_c_[kLocalPlayers];
    AniTextures kick_c_[kLocalPlayers];
    AniTextures corner_c_[kCornerFiles][kLocalPlayers];
    AniTextures bwalk_c_[kBwalkFiles][kLocalPlayers];
    AniTextures punch_c_[kPunchFiles][kLocalPlayers];
    AniTextures pickup_c_[kPupFiles][kLocalPlayers];

    std::vector<AniTextures> xplode_;                   // XPLODE1..32 source files
    std::vector<Anim> deaths_;                          // green base pool
    std::vector<Anim> deaths_c_[kLocalPlayers];         // recolored pools
    std::vector<AniTextures> xplode_c_[kLocalPlayers];  // keep textures alive

    Sprite powerups_[sim::kPowerupKinds]{};
    std::vector<sdl::TexturePtr> powerup_textures_;  // owners for powerups_
    sdl::TexturePtr field_;
    sdl::TexturePtr field_hd_;  // optional DATA_HD/RES/FIELD<n>.PCX override

    // Separate texture owners from the live match slots above, so preview
    // cycling never touches them.
    mutable std::map<int, StagePreview> stage_preview_;
    mutable std::map<int, AniTextures> stage_preview_tiles_;
    mutable std::map<int, sdl::TexturePtr> stage_preview_field_;

    AniTextures ring_;                    // Goldman wheel pointer ("ring" seq)
    AniTextures misc_;                    // MISC.ANI — editor markers, goldman, cursor
    AniTextures edit_;                    // EDIT.ANI — editor schematic tiles
    assets::bmfont::Font frontend_font_;  // FONT6.FON, the .BM screen font
    assets::res::Messages messages_;      // MESSAGES.TXT string table (install root)

    // 0.RMP..9.RMP: the original recolours player i by rewriting each pixel's
    // palette index through i.rmp at blit time (sub_415A1C, dword_460564).
    // rmp_ok_[i] false -> that slot takes the truecolour recolour instead.
    static constexpr int kColors = 10;
    std::array<std::array<std::uint8_t, 256>, kColors> rmp_{};
    std::array<bool, kColors> rmp_ok_{};
    // Each colour's .RMP tail (R,G,B percent), falling back to VALUELST
    // color_rgb — the same bytes the original stores in byte_460BD0/BDA/BE4.
    std::array<std::array<std::uint8_t, 3>, kColors> rmp_rgb_{};

    // Lazily populated, hence mutable on const accessors; the texture owners
    // keep the Sprites valid. The `.BM` viewer's keyed + snapped rendition is
    // cached SEPARATELY, and bm_pcx_keyed_ remembers whether the CLASSIC image
    // used index 0 — the only way to know whether the indexless 24-bit HD
    // upscale should be keyed.
    mutable std::map<std::string, Sprite> front_pcx_;
    mutable std::vector<sdl::TexturePtr> front_textures_;
    mutable std::map<std::string, Sprite> bm_pcx_;
    mutable std::map<std::string, Sprite> bm_pcx_hd_;
    mutable std::map<std::string, bool> bm_pcx_keyed_;
    mutable std::vector<sdl::TexturePtr> bm_textures_;
    mutable Sprite author_photo_{};
    mutable bool author_photo_ready_ = false;
    mutable sdl::TexturePtr author_photo_tex_;
    // HD front-end textures keep the CLASSIC Sprite geometry, so every original
    // UI coordinate stays in its faithful 640x480 space.
    mutable std::map<std::string, Sprite> front_pcx_hd_;
    mutable std::vector<sdl::TexturePtr> front_textures_hd_;
    bool hd_enabled_ = false;
    bool player_hd_built_ = false;  // guards ensure_player_hd_sets' one-time build
};

}  // namespace bomber::game
