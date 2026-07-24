#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <functional>  // boot-loading progress callback (load / build_player_sets)
#include <map>
#include <string>
#include <vector>

#include <array>

#include "bomber/assets/bmfont.hpp"
#include "bomber/assets/colorpal.hpp"
#include "bomber/assets/messages.hpp"
#include "bomber/assets/rmp.hpp"
#include "bomber/game/sdl.hpp"
#include "bomber/game/sprites.hpp"
#include "bomber/match/level_registry.hpp"
#include "bomber/sim/constants.hpp"

// Owns every texture the game renders: the shared ANIs, per-player recolored
// copies, the powerup icons, the death-animation pools, and the per-stage art
// (background + tiles + brick-burn). All loaded at runtime from the player's
// own original install.

namespace bomber::game {

class AssetStore {
public:
    // Loads the stage-independent assets. Returns false (and logs) on failure.
    // `progress` (optional) is invoked with a monotonic 0 -> 1 fraction as each
    // ANI/PCX group lands, so the boot LOADING dialog can animate + pump the
    // window during this multi-second decode; omit it (tools/tests) for a
    // silent load.
    bool load(SDL_Renderer* ren, const std::filesystem::path& game_dir,
              const std::function<void(float)>& progress = {});

    // Loads just FONT6.FON standalone, ahead of the full load() pass. RE fact
    // (docs/re/frontend-flow.md "sub_43C734 dialog chrome"): sub_41095A calls
    // sub_414DF4 (which ends by pinning FONT6 via sub_431E9C(6)) BEFORE it calls
    // sub_41D695/sub_42896E, the two boot LOADING dialogs — so the ORIGINAL has
    // FONT6 ready for both flashes, not just assets loaded later. GameApp::init
    // calls this before the first loading-dialog paint so the port matches that
    // order; load() below still calls it too (idempotent — a no-op once loaded)
    // so a caller that skips this early call still gets the font.
    void load_frontend_font(const std::filesystem::path& game_dir);

    // Loads just WINZ.PCX (the sub_43C734 window-chrome 9-patch skin, docs/
    // re/frontend-flow.md "The WINZ.PCX 9-patch window skin") standalone,
    // ahead of the full load() pass — mirroring sub_414DF4, which loads
    // "winz.plt" (the extension map resolves it to DATA/RES/WINZ.PCX) during
    // graphics init, BEFORE the boot LOADING dialogs run. Populates the same
    // frontend_pcx("WINZ") cache the rest of the front-end uses, so a later
    // frontend_pcx("WINZ") is a pure cache hit. Returns the cached sprite
    // (empty if the file is missing — callers fall back to the flat base
    // coat, matching a hypothetical missing-winz install).
    const Sprite& load_frontend_winz(SDL_Renderer* ren, const std::filesystem::path& game_dir);

    // Builds per-player recolored copies of the player-facing sprite sets
    // (walk/stand/bombs/flames/deaths). Prefers each slot's authentic .RMP index
    // remap; `colors` (VALUELST 200..247) is the truecolour fallback for a slot
    // whose .RMP was missing. `progress` (optional) is invoked with a 0 -> 1
    // fraction per player so the boot LOADING dialog can keep animating + pumping
    // through this heavy recolor pass; omit it for a silent build.
    void build_player_sets(const std::int32_t colors[][3],
                           const std::function<void(float)>& progress = {});

    // Seed the setup-screen slot colours (rmp_rgb_) for every colour whose .RMP
    // was absent, from the VALUELST 200..247 percent table (Tuning::color_rgb),
    // so slot_color() has an authoritative value for all 10 slots even without a
    // full set of .RMP files. A loaded .RMP's own tail is kept (it wins). `n` =
    // number of rows in `colors` (== 10). Presentation-only.
    void set_color_fallbacks(const std::int32_t colors[][3], int n);

    // Loads the per-stage art for `stage`, resolving the field/tiles/xbrick
    // base names from the level registry (built-ins -> FIELDn/TILESn/XBRICKn,
    // byte-identical to the old hardcoded concat; custom levels -> their own
    // LevelDef bases).
    bool load_stage(int stage);

    // The level catalogue (bomber::match, SDL-free). Defaults to the 11
    // built-ins; adding a custom map is a single `assets.levels().add({...})`
    // that load_stage/stage_preview and the LEVEL & ROUNDS screen all read.
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
    // Backed by TRIGANIM.ANI (NOT TRIGBOMB.ANI — see trigbomb_'s doc comment
    // below), still exposed as trigbomb() since that names the GAME CONCEPT
    // (the armed-trigger-bomb visual), not the backing file.
    const AniTextures& trigbomb(int player) const { return pick(trigbomb_, trigbomb_c_, player); }
    const AniTextures& flame(int player) const { return pick(flame_, flame_c_, player); }
    const AniTextures& stand(int player) const { return pick(stand_, stand_c_, player); }
    const AniTextures& walk(int player) const { return pick(walk_, walk_c_, player); }
    const AniTextures& kick(int player) const { return pick(kick_, kick_c_, player); }

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

    // One of the four PUNBOMB*.ANI masters (the punch action pose), recolored
    // when built. CORRECTED 2026-07-09 (docs/re/facts.md "ANI sequence-name
    // audit"): MASTER.ALI loads punbomb1..4.ani, not punch.ani (which is
    // never listed there and so never enters the original's sequence pool);
    // each file owns one direction ("punch <dir>", no "green" suffix — unlike
    // PUNCH.ANI's dead art), so callers probe every file like bwalk() above.
    const AniTextures& punch(int file, int player) const {
        if (file < 0 || file >= kPunchFiles) return punch_[0];
        return pick(punch_[file], punch_c_[file], player);
    }
    static constexpr int punch_files() { return kPunchFiles; }

    // One of the four PUP*.ANI masters (the "picking up a bomb" transitional
    // pose, sub_41F29B action-state 4 — pseudo.c aPickupS, "pickup <dir>", no
    // "green" suffix), recolored when built. MASTER.ALI loads pup1..4.ani
    // (BPICKUP.ANI is never listed there, so never in the original's pool).
    // Each file owns one direction; callers probe every file like bwalk().
    const AniTextures& pickup(int file, int player) const {
        if (file < 0 || file >= kPupFiles) return pickup_[0];
        return pick(pickup_[file], pickup_c_[file], player);
    }
    static constexpr int pickup_files() { return kPupFiles; }

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
    // Campaign rover/ghost hazard actors (docs/re/campaign.md "Per-tick
    // mover"): ALIENS1.ANI, shared/uncoloured — "ghost <dir>"/"rover <dir>"
    // (sub_4518D0(buf, aGhostS/aRoverS, dir)). Was previously believed cut
    // content (no file literally named GHOST.ANI/ROVER.ANI exists), but the
    // sequences ship under this file's name instead (CONFIRMED against the
    // install 2026-07-09, docs/re/facts.md "ANI sequence-name audit").
    const AniTextures& aliens1() const { return aliens1_; }
    // Runtime HD presentation toggle. It never changes simulation state or
    // original assets: DATA_HD/RES files are optional visual overrides, and
    // every missing override falls back to the matching DATA/RES resource.
    void set_hd_enabled(bool enabled) { hd_enabled_ = enabled; }
    bool hd_enabled() const { return hd_enabled_; }

    // Builds the per-player HD sprite sets on demand, ONCE. build_player_sets
    // deliberately skips the (16x-heavier) per-player HD recolor at boot because
    // HD artwork starts off — this fills those sets in the first time HD is
    // switched on (Tab), recolouring each base set's retained HD source frames
    // per player and freeing that source afterwards. Returns true if it built
    // this call (so the caller re-resolves the SequenceSet to pick up the new
    // tex_hd), false if the sets already existed (idempotent no-op). Presentation
    // only; never touches simulation state.
    bool ensure_player_hd_sets();

    SDL_Texture* field() const {
        return hd_enabled_ && field_hd_ ? field_hd_.get() : field_.get();
    }

    // One built-in level's SAMPLE-BLOCK preview art (docs/re/setup-screens.md
    // "The sample block preview", sub_406AA3): the level's own "tile <n>
    // solid"/"tile <n> brick" sequences (TILES<n>.ANI) plus its FIELD<n>.PCX
    // backdrop swatch, loaded independently of load_stage()'s live match slot
    // so the LEVEL & ROUNDS screen can cycle through every level's preview
    // without disturbing (or being disturbed by) an in-progress match's own
    // stage art. Cached by stage index; a missing/broken file leaves the
    // corresponding entry empty/null (drawn as nothing), never throws.
    struct StagePreview {
        Anim solid, brick;
        SDL_Texture* field = nullptr;
    };
    const StagePreview& stage_preview(int stage) const;

    // Front-end full-screen art (docs/re/frontend-flow.md), loaded lazily on
    // first request so the match path pays nothing for it and a missing file
    // just yields an empty Sprite the Screen skips. Keyed by the same base name
    // the original passes to its screen primitive (sub_42A088): "IPLOGO",
    // "HSLOGO", "MAINMENU", "DRAW", "BONUS", "CREDBAR", etc. Cached by name.
    const Sprite& frontend_pcx(const std::string& name) const;

    // The Goldman wheel's "ring" pointer sequence (docs/re/goldman-roulette.md
    // §3/§7, aRing) — RESOLVED: MISC.ANI owns it (its sequence table is
    // cursor1/goldman/ring/safe/scan/teamring0/teamring1, checked against the
    // install's file 2026-07-08), so the probe tries MISC.ANI first; the old
    // candidates stay as fallbacks for partial installs. Empty when nothing
    // owns "ring" — GoldmanScreen then draws no pointer sprite (the wheel
    // itself still works; see that class's fallback note).
    const AniTextures& ring() const { return ring_; }

    // MISC.ANI wholesale — the scheme editor's start markers draw its
    // "teamring%u" sequences (sub_4028D2's aTeamringU draw, docs/re/
    // results-and-options.md §5). Empty when the file is missing (the editor
    // falls back to outline-box markers).
    const AniTextures& misc() const { return misc_; }

    // EDIT.ANI — the scheme editor's schematic tile set, sequences "tile -1
    // blank/brick/solid" (CONFIRMED against the install 2026-07-09, docs/re/
    // facts.md "ANI sequence-name audit"). The editor's '0'-key tileset
    // toggle (sub_402206's dword_45B7B8 = -1 state) resolves those names
    // from the original's GLOBAL sequence pool (every MASTER.ALI file merged,
    // sub_41D957), landing on this file — it was never a dead state. Empty
    // when the file is missing (the editor falls back to flat swatches).
    const AniTextures& edit() const { return edit_; }

    // The install ROOT (parent of DATA) — where the `.BM` help/credits screens
    // and the `FONT<n>.FON` fonts live (not under DATA/RES). Used by the BM
    // screen viewer to resolve those install-root files.
    const std::filesystem::path& game_dir() const { return game_dir_; }

    // The front-end bitmap font (FONT6.FON, the font graphics-init pins via
    // sub_431E9C(6)). Empty when the file is missing/broken (the BM viewer then
    // draws no glyphs). Loaded once in load().
    const assets::bmfont::Font& frontend_font() const { return frontend_font_; }

    // The install's MESSAGES.TXT string table (the original's getstring /
    // sub_4124A4): the setup/net screens format these labels by id. Loaded once
    // in load() from the install root; the text stays in the user's own file.
    std::string getstring(int id, const std::string& fallback = std::string()) const {
        return messages_.get_or(id, fallback);
    }

    // The on-screen RGB888 colour of player-slot `i`, the faithful equivalent of
    // the original setup screen's per-slot label ink `sub_41672F(i)` (@0x41672F):
    // it quantises the slot's stored RGB (byte_460BD0/BDA/BE4[i], == the .RMP
    // tail) to 5 bits each (`min(v/3, 31)`), packs RGB555, and looks it up in the
    // palette LUT. We are truecolour, so instead of the LUT we expand the 5-bit
    // channels back to 8 bits (the same expand5 the ANI 16bpp path uses). Using
    // the .RMP tail (authoritative) through the game's own quantisation is what
    // makes each setup-screen slot read as its true in-game colour. `out[3]` gets
    // R,G,B. Falls back to a mid grey if the colour index is out of range.
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
    // Frees the recolour-source CPU pixels of the player-coloured BASE sets
    // (walk/stand/kick/bombs/duds/flame/trigbomb + corner/bwalk/punch/pickup/
    // xplode). Classic pixels are dropped once build_player_sets has consumed
    // them for every player; the HD source frames are dropped once the per-player
    // HD sets exist (ensure_player_hd_sets). Keeps each frame's w/h + GPU
    // textures, so base sprites still render and resolve. Presentation only.
    void drop_player_base_classic_cpu();
    void drop_player_base_hd_cpu();

    // bugprone-return-const-ref-from-parameter (NOLINT below) — private helper,
    // every call site (above) passes a member AniTextures_ with `this`'s
    // lifetime, never a temporary, so `base` never dangles in practice.
    const AniTextures& pick(const AniTextures& base, const AniTextures (&colored)[kLocalPlayers],
                            int player) const {
        if (player >= 0 && player < kLocalPlayers && colored[player].loaded())
            return colored[player];
        return base;  // NOLINT(bugprone-return-const-ref-from-parameter)
    }

    // CORNER0.ANI..CORNER7.ANI hold the 13 direction-independent "cornerhead"
    // idle fidget sequences (sub_41F29B), spread unevenly across the 8 files.
    static constexpr int kCornerFiles = 8;

    // BWALK1.ANI..BWALK4.ANI hold the "carrying a bomb" walk/stand poses, one
    // direction per file (1=south, 2=north, 3=west, 4=east).
    static constexpr int kBwalkFiles = 4;

    // PUNBOMB1..4.ANI hold the punch action pose, one direction per file (the
    // file-number -> direction mapping isn't a fixed convention, so callers
    // probe every file by resolved sequence name, like bwalk() above).
    static constexpr int kPunchFiles = 4;

    // PUP1..4.ANI hold the "picking up a bomb" transitional pose, one
    // direction per file, probed the same way.
    static constexpr int kPupFiles = 4;

    SDL_Renderer* ren_ = nullptr;
    std::filesystem::path game_dir_;

    // The level catalogue load_stage/stage_preview resolve asset paths from.
    // Seeded with the 11 built-ins whose LevelDef bases are exactly
    // FIELDn/TILESn/XBRICKn, so the resolved paths match the old hardcoded
    // concatenation byte-for-byte; a custom level added via levels() extends it.
    match::LevelRegistry levels_ = match::LevelRegistry::with_builtins();

    // The in-match shared-palette snap (colorpal.hpp), loaded once from the
    // install root. Inert (ok()==false) when COLOR.PAL is absent, so the game
    // degrades to the raw per-asset decode. Applied to the CLASSIC field PCX
    // and tile/brick ANIs in load_stage — reproduces the original's colour
    // quantization (docs/re/facts.md "In-match colour quantization").
    assets::colorpal::Palette colorpal_;

    AniTextures tiles_, xbrick_, bombs_, duds_, flame_, stand_, walk_, shadow_, kfont_, hurry_;
    AniTextures kick_;      // action-pose master (KICK.ANI)
    AniTextures powers_;    // animated floor-powerup art (POWERS.ANI), shared (uncoloured)
    AniTextures conveyor_;  // conveyor belt floor art (CONVEYOR.ANI), shared (uncoloured)
    AniTextures extras_;    // trampoline/arrow/warp floor art (EXTRAS.ANI), shared
    AniTextures aliens1_;   // campaign rover/ghost hazard art (ALIENS1.ANI), shared (uncoloured)
    // Trigger-bomb master. CORRECTED 2026-07-09 (docs/re/facts.md "ANI
    // sequence-name audit"): MASTER.ALI comments out `;-trigbomb.ani` and
    // loads `-triganim.ani` instead, so TRIGBOMB.ANI's "bomb trigger green"
    // (7 steps) never enters the original's sequence pool — TRIGANIM.ANI's
    // same-named sequence (19 steps, a rise-then-fall cycle) is the one
    // actually shown. Loaded from TRIGANIM.ANI despite the member name (the
    // name tracks the game concept — see trigbomb()'s doc comment above).
    AniTextures trigbomb_;
    AniTextures corner_[kCornerFiles];  // idle-fidget masters (CORNER0..7.ANI)
    AniTextures bwalk_[kBwalkFiles];    // carry-bomb masters (BWALK1..4.ANI)
    AniTextures punch_[kPunchFiles];    // punch-pose masters (PUNBOMB1..4.ANI)
    AniTextures pickup_[kPupFiles];     // pickup-pose masters (PUP1..4.ANI)
    AniTextures walk_c_[kLocalPlayers], stand_c_[kLocalPlayers];
    AniTextures bombs_c_[kLocalPlayers], duds_c_[kLocalPlayers], flame_c_[kLocalPlayers];
    AniTextures trigbomb_c_[kLocalPlayers];  // per-player recolor of trigbomb_ (TRIGANIM.ANI)
    AniTextures kick_c_[kLocalPlayers];
    AniTextures corner_c_[kCornerFiles][kLocalPlayers];
    AniTextures bwalk_c_[kBwalkFiles][kLocalPlayers];
    AniTextures punch_c_[kPunchFiles][kLocalPlayers];
    AniTextures pickup_c_[kPupFiles][kLocalPlayers];

    std::vector<AniTextures> xplode_;                   // XPLODE1..17 source files
    std::vector<Anim> deaths_;                          // green base pool
    std::vector<Anim> deaths_c_[kLocalPlayers];         // recolored pools
    std::vector<AniTextures> xplode_c_[kLocalPlayers];  // keep textures alive

    Sprite powerups_[sim::kPowerupKinds]{};
    std::vector<sdl::TexturePtr> powerup_textures_;  // owners for powerups_
    sdl::TexturePtr field_;
    sdl::TexturePtr field_hd_;  // optional DATA_HD/RES/FIELD<n>.PCX override

    // stage_preview() cache: keyed by stage index, populated lazily. Separate
    // AniTextures/texture owners from the live match slots above (tiles_/
    // xbrick_/field_) so map-select preview cycling never touches them.
    mutable std::map<int, StagePreview> stage_preview_;
    mutable std::map<int, AniTextures> stage_preview_tiles_;
    mutable std::map<int, sdl::TexturePtr> stage_preview_field_;

    AniTextures ring_;      // Goldman wheel pointer ("ring" seq), shared — see ring() doc comment
    AniTextures misc_;      // MISC.ANI (teamring0/1, cursor1, safe, scan) — editor markers
    AniTextures edit_;      // EDIT.ANI ("tile -1 blank/brick/solid") — editor schematic tiles
    assets::bmfont::Font frontend_font_;  // FONT6.FON, the .BM screen font
    assets::res::Messages messages_;      // MESSAGES.TXT string table (install root)

    // The ten player-colour remap tables (0.RMP..9.RMP, install root). The
    // original recolours player i's sprites by rewriting each pixel's palette
    // index through i.rmp at blit time (sub_415A1C, dword_460564[colour] —
    // docs/re/player-colour.md); build_player_sets does the same via
    // recolored(rmp). rmp_ok_[i] is false when the file is missing/short, in
    // which case that slot falls back to the truecolour recolour(color_rgb).
    static constexpr int kColors = 10;
    std::array<std::array<std::uint8_t, 256>, kColors> rmp_{};
    std::array<bool, kColors> rmp_ok_{};
    // The 3 tail bytes (R,G,B percent) of each colour's .RMP. When a .RMP is
    // absent the tail falls back to VALUELST color_rgb (set in load()). This is
    // the authoritative per-slot colour for the setup screen's label tint, the
    // same bytes the original stores in byte_460BD0/BDA/BE4 (sub_414A65). Empty
    // (zero) only if neither the .RMP nor a colour table was available.
    std::array<std::array<std::uint8_t, 3>, kColors> rmp_rgb_{};
    // Front-end full-screen PCX, loaded and cached on demand by base name.
    // mutable: frontend_pcx() is a const accessor but populates the cache
    // lazily. Owners live in front_textures_ to keep the Sprites' tex valid.
    mutable std::map<std::string, Sprite> front_pcx_;
    mutable std::vector<sdl::TexturePtr> front_textures_;
    // HD front-end textures retain the classic Sprite geometry. This keeps all
    // original UI coordinates intact while their backing texture is higher
    // resolution. They are only selected while hd_enabled_ is true.
    mutable std::map<std::string, Sprite> front_pcx_hd_;
    mutable std::vector<sdl::TexturePtr> front_textures_hd_;
    bool hd_enabled_ = false;
    // True once the per-player HD sprite sets have been built (either eagerly by
    // build_player_sets when HD was already on, or lazily by ensure_player_hd_sets
    // on the first Tab). Guards the one-time lazy build.
    bool player_hd_built_ = false;
};

}  // namespace bomber::game
