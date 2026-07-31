#include "bomber/game/asset_store.hpp"

#include <algorithm>  // std::min (boot-loading progress clamp)
#include <cstdint>
#include <exception>
#include <span>
#include <string>
#include <utility>

#include "bomber/assets/bmfont.hpp"
#include "bomber/assets/pcx.hpp"
#include "bomber/assets/rmp.hpp"
#include "bomber/game/key_color.hpp"
#include "bomber/game/log.hpp"

namespace bomber::game {

namespace fs = std::filesystem;

void AssetStore::load_frontend_font(const fs::path& game_dir) {
    // FONT6.FON lives in the install ROOT (not under DATA/) and is otherwise
    // independent of every other asset load() performs below — safe to load
    // standalone, ahead of load() (docs/re/frontend-flow.md's sub_41095A
    // trace: sub_414DF4 pins FONT6 before the boot LOADING dialogs run).
    // Idempotent: a second call (from load() itself) with the font already
    // populated is a cheap re-parse, not a correctness issue.
    try {
        auto p = game_dir / "FONT6.FON";
        if (fs::exists(p)) frontend_font_ = assets::bmfont::load(p);
    } catch (const std::exception& e) {
        log_warn("FONT6.FON load failed: %s", e.what());
    }
}

const Sprite& AssetStore::load_frontend_winz(SDL_Renderer* ren, const fs::path& game_dir) {
    // frontend_pcx() needs the renderer + install root; load() re-assigns the
    // same values later, so seeding them here is safe and keeps this a plain
    // pre-warm of the shared cache (idempotent, logs a miss once).
    ren_ = ren;
    game_dir_ = game_dir;
    return frontend_pcx("WINZ");
}

bool AssetStore::load(SDL_Renderer* ren, const fs::path& game_dir,
                      const std::function<void(float)>& progress) {
    ren_ = ren;
    auto ani_dir = game_dir / "DATA" / "ANI";
    auto res_dir = game_dir / "DATA" / "RES";
    // Boot LOADING progress: tick() bumps a step counter and reports 0 -> 1 so
    // GameApp::draw_boot_loading can pump the window + animate the "Loading
    // data..." bar between chunks (the port's stand-in for the original's
    // sub_412E33(100*read/total) per-MASTER.ALI-entry readout). The denominator
    // is the count of tick() points below; the trailing DATA_HD overlay block's
    // 34 ticks (one inside hd_ov per call) are counted only when DATA_HD exists,
    // so a classic install fills the bar exactly as XPLODE finishes rather than
    // stalling short. std::min clamps any drift; overshoot on a missing-file
    // path just fills a touch faster (never past 100%).
    const bool has_hd = fs::exists(game_dir / "DATA_HD");
    const int approx_steps = has_hd ? 121 : 87;
    int done_steps = 0;
    auto tick = [&] {
        if (progress)
            progress(std::min(1.0f, static_cast<float>(++done_steps) /
                                        static_cast<float>(approx_steps)));
    };
    // The in-match master-palette snap (colorpal.hpp): COLOR.PAL lives in the
    // install ROOT. Optional — a missing/short file leaves colorpal_ inert and
    // the game renders the raw per-asset decode (the pre-2026-07-13 look).
    try {
        colorpal_ = assets::colorpal::Palette::load(game_dir / "COLOR.PAL");
    } catch (const std::exception& e) {
        log_warn("COLOR.PAL unavailable (%s); classic colour snap disabled", e.what());
    }
    tick();  // colorpal
    try {
        // The in-match master-palette snap (colorpal.hpp) is applied to EVERY
        // match-drawn asset — the original renders the whole match on one
        // shared 256-colour palette. Front-end art (MISC cursor, EDIT tiles,
        // the goldman wheel, backdrops, fonts) and DATA_HD are deliberately
        // NOT snapped (the original loads those through its non-snapping path).
        const auto* snap = &colorpal_;
        kfont_.load(ren, ani_dir / "KFONT.ANI", snap);
        hurry_.load(ren, ani_dir / "HURRY.ANI", snap);
        bombs_.load(ren, ani_dir / "BOMBS.ANI", snap);
        duds_.load(ren, ani_dir / "DUDS.ANI", snap);
        // MFLAME.ANI, not FLAME.ANI — CORRECTED 2026-07-09 (docs/re/facts.md
        // "ANI sequence-name audit"): MASTER.ALI comments out `;-flame.ani`
        // and loads `-mflame.ani` instead, so FLAME.ANI's "flame <piece>
        // green" sequences (7-step cycles) never enter the original's pool;
        // MFLAME.ANI's same-named sequences (5-step cycles) are the ones
        // actually shown.
        flame_.load(ren, ani_dir / "MFLAME.ANI", snap);
        stand_.load(ren, ani_dir / "STAND.ANI", snap);
        walk_.load(ren, ani_dir / "WALK.ANI", snap);
        kick_.load(ren, ani_dir / "KICK.ANI", snap);
        shadow_.load(ren, ani_dir / "SHADOW.ANI", snap);
        tick();  // core match ANIs (kfont..shadow)

        // Animated floor-powerup art (POWERS.ANI, seq "power <name>"). Shared and
        // NOT player-coloured, loaded once. Cosmetic: a missing/broken file must
        // NOT abort the load — draw_powerups falls back to the static POW*.PCX.
        try {
            auto p = ani_dir / "POWERS.ANI";
            if (fs::exists(p)) powers_.load(ren, p, snap);
        } catch (const std::exception& e) {
            log_warn("POWERS.ANI load failed: %s", e.what());
        }
        tick();  // POWERS.ANI

        // Stage-actor floor art (docs/re/stage-actors.md), shared/uncoloured.
        // CONVEYOR.ANI = "extra conveyor <dir>"; EXTRAS.ANI = "extra trampoline",
        // "extra arrow <dir>", "extra warp 1". Cosmetic and optional: a missing
        // file must NOT abort the load — the tile simply isn't drawn.
        try {
            auto p = ani_dir / "CONVEYOR.ANI";
            if (fs::exists(p)) conveyor_.load(ren, p, snap);
        } catch (const std::exception& e) {
            log_warn("CONVEYOR.ANI load failed: %s", e.what());
        }
        tick();  // CONVEYOR.ANI
        try {
            auto p = ani_dir / "EXTRAS.ANI";
            if (fs::exists(p)) extras_.load(ren, p, snap);
        } catch (const std::exception& e) {
            log_warn("EXTRAS.ANI load failed: %s", e.what());
        }
        tick();  // EXTRAS.ANI

        // Campaign rover/ghost hazard art (ALIENS1.ANI, seq "ghost <dir>"/
        // "rover <dir>"). Shared/uncoloured — the sequence names carry no
        // "green" suffix, so the original never recolours them per player.
        // Cosmetic and optional: a missing file leaves the renderer's plain
        // marker fallback in place (docs/re/facts.md "ANI sequence-name audit").
        try {
            auto p = ani_dir / "ALIENS1.ANI";
            if (fs::exists(p)) aliens1_.load(ren, p, snap);
        } catch (const std::exception& e) {
            log_warn("ALIENS1.ANI load failed: %s", e.what());
        }
        tick();  // ALIENS1.ANI

        // Trigger-bomb art, recoloured per owner like the regular bomb.
        // TRIGANIM.ANI, not TRIGBOMB.ANI — see trigbomb_'s doc comment
        // (asset_store.hpp) for why. Cosmetic: a missing/broken file must NOT
        // abort the load — the bomb draw falls back to the normal pulse.
        try {
            auto p = ani_dir / "TRIGANIM.ANI";
            if (fs::exists(p)) trigbomb_.load(ren, p, snap);
        } catch (const std::exception& e) {
            log_warn("TRIGANIM.ANI load failed: %s", e.what());
        }
        tick();  // TRIGANIM.ANI

        // HEADWIPE.ANI is deliberately NOT loaded: it is absent from
        // MASTER.ALI, so the original engine never loads it — dead art, like
        // FLAME.ANI/TRIGBOMB.ANI (docs/re/frontend-flow.md "HEADWIPE.ANI is
        // dead art"). The menu→setup screen change is a CUT in the original.

        // MISC.ANI: teamring0/teamring1 (the scheme editor's start markers,
        // sub_4028D2 aTeamringU), plus cursor1/goldman/ring/safe/scan —
        // sequence table checked against the install's file (docs/re/
        // results-and-options.md §5). Cosmetic and optional: the editor
        // falls back to outline-box markers when this is missing.
        try {
            auto p = ani_dir / "MISC.ANI";
            if (fs::exists(p)) misc_.load(ren, p);
        } catch (const std::exception& e) {
            log_warn("MISC.ANI load failed: %s", e.what());
        }
        tick();  // MISC.ANI

        // EDIT.ANI: the scheme editor's schematic "tile -1 blank/brick/solid"
        // tiles (the '0'-key tileset toggle's -1 state — see edit()'s doc
        // comment). Cosmetic and optional: the editor falls back to flat
        // swatches when this is missing.
        try {
            auto p = ani_dir / "EDIT.ANI";
            if (fs::exists(p)) edit_.load(ren, p);
        } catch (const std::exception& e) {
            log_warn("EDIT.ANI load failed: %s", e.what());
        }
        tick();  // EDIT.ANI

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
                    log_warn("%s load failed (ring probe): %s", name, e.what());
                }
            }
        }

        tick();  // ring probe

        // Front-end bitmap font for the .BM help/credits screens AND the dialog
        // chrome (sub_43C734 family). GameApp::init now calls
        // load_frontend_font() standalone before this, matching sub_41095A's
        // real order (sub_414DF4 pins FONT6 before the boot LOADING dialogs);
        // this call stays so load() alone (e.g. tools that skip the early call)
        // still gets the font. (docs/formats/fon.md.)
        load_frontend_font(game_dir);
        tick();  // FONT6.FON

        // The MESSAGES.TXT string table (getstring / sub_4124A4): the setup and
        // net-game screens format their labels from it. Install ROOT, like the
        // fonts. Optional — a missing file leaves getstring() returning fallbacks.
        try {
            auto p = game_dir / "MESSAGES.TXT";
            if (fs::exists(p)) messages_ = assets::res::load_messages(p);
        } catch (const std::exception& e) {
            log_warn("MESSAGES.TXT load failed: %s", e.what());
        }
        tick();  // MESSAGES.TXT

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
                    log_warn("%d.RMP missing; using fallback recolour", i);
                }
            } catch (const std::exception& e) {
                log_warn("%d.RMP load failed (%s); using fallback recolour", i, e.what());
            }
            tick();  // i.RMP
        }

        // Idle "cornerhead" fidgets (CORNER0..7.ANI). Cosmetic and optional:
        // a missing/broken CORNER file must NOT abort the whole load, so each
        // is loaded in isolation — resolution just falls back to stand.
        for (int i = 0; i < kCornerFiles; ++i) {
            auto p = ani_dir / ("CORNER" + std::to_string(i) + ".ANI");
            try {
                if (fs::exists(p)) corner_[i].load(ren, p, snap);
            } catch (const std::exception& e) {
                log_warn("cornerhead load failed (%s): %s", p.string().c_str(), e.what());
            }
            tick();  // CORNER<i>.ANI
        }

        // "Carrying a bomb" poses (BWALK1..4.ANI, one direction per file). Like
        // the cornerhead fidgets these are cosmetic: a missing/broken BWALK file
        // must NOT abort the load, so each is isolated — resolution just falls
        // back to plain walk/stand.
        for (int i = 0; i < kBwalkFiles; ++i) {
            auto p = ani_dir / ("BWALK" + std::to_string(i + 1) + ".ANI");
            try {
                if (fs::exists(p)) bwalk_[i].load(ren, p, snap);
            } catch (const std::exception& e) {
                log_warn("carry-bomb load failed (%s): %s", p.string().c_str(), e.what());
            }
            tick();  // BWALK<i>.ANI
        }

        // Punch action pose (PUNBOMB1..4.ANI, seq "punch <dir>"). CORRECTED
        // 2026-07-09: MASTER.ALI never lists punch.ani, only punbomb1..4.ani,
        // so these — not PUNCH.ANI — are the files the original actually
        // loads. One direction per file, probed like BWALK above. Cosmetic:
        // a missing/broken file must NOT abort the load.
        for (int i = 0; i < kPunchFiles; ++i) {
            auto p = ani_dir / ("PUNBOMB" + std::to_string(i + 1) + ".ANI");
            try {
                if (fs::exists(p)) punch_[i].load(ren, p, snap);
            } catch (const std::exception& e) {
                log_warn("punch-pose load failed (%s): %s", p.string().c_str(), e.what());
            }
            tick();  // PUNBOMB<i>.ANI
        }

        // "Picking up a bomb" transitional pose (PUP1..4.ANI, seq "pickup
        // <dir>"). CORRECTED 2026-07-09: MASTER.ALI never lists bpickup.ani,
        // only pup1..4.ani. One direction per file, probed like BWALK above.
        // Cosmetic: a missing/broken file must NOT abort the load.
        for (int i = 0; i < kPupFiles; ++i) {
            auto p = ani_dir / ("PUP" + std::to_string(i + 1) + ".ANI");
            try {
                if (fs::exists(p)) pickup_[i].load(ren, p, snap);
            } catch (const std::exception& e) {
                log_warn("pickup-pose load failed (%s): %s", p.string().c_str(), e.what());
            }
            tick();  // PUP<i>.ANI
        }

        static constexpr const char* kPowFiles[] = {
            "POWBOMB",  "POWFLAME", "POWDISEA", "POWKICK",  "POWSKATE", "POWPUNCH", "POWGRAB",
            "POWSPOOG", "POWGOLD",  "POWTRIG",  "POWJELLY", "POWEBOLA", "POWRAND"};
        for (int i = 0; i < sim::kPowerupKinds; ++i) {
            auto img = assets::pcx::load(res_dir / (std::string(kPowFiles[i]) + ".PCX"));
            sdl::TexturePtr tex{make_texture(ren, img, TextureArt::Classic, snap)};
            powerups_[i] = {tex.get(), img.width, img.height, 0, 0};
            powerup_textures_.push_back(std::move(tex));
            // Optional HD icon (DATA_HD/RES/POW*.PCX), truecolour + LINEAR, kept
            // at the classic Sprite geometry — only the static-fallback draw
            // path uses these (the animated POWERS.ANI HD is the primary route).
            auto hp = game_dir / "DATA_HD" / "RES" / (std::string(kPowFiles[i]) + ".PCX");
            if (fs::exists(hp)) {
                try {
                    auto himg = assets::pcx::load(hp);
                    sdl::TexturePtr htex{make_texture(ren, himg, TextureArt::HighRes, nullptr)};
                    if (htex) {
                        powerups_[i].tex_hd = htex.get();
                        powerup_textures_.push_back(std::move(htex));
                    }
                } catch (const std::exception& e) {
                    log_warn("HD powerup icon '%s' skipped: %s", kPowFiles[i], e.what());
                }
            }
            tick();  // POW<i> icon (+ optional HD)
        }

        // Death animations: every 'die green N' sequence across XPLODE*.ANI.
        for (int i = 1; i <= 32; ++i) {
            tick();  // XPLODE<i>.ANI (+ optional HD) — counts all 32 slots
            auto p = ani_dir / ("XPLODE" + std::to_string(i) + ".ANI");
            if (!fs::exists(p)) continue;
            AniTextures ani;
            ani.load(ren, p, snap);
            // HD overlay BEFORE collecting so the death Sprites carry tex_hd
            // (and build_player_sets' recolor propagates it to the coloured pools).
            auto hp = game_dir / "DATA_HD" / "ANI" / ("XPLODE" + std::to_string(i) + ".ANI");
            if (fs::exists(hp)) {
                try {
                    ani.load_hd_overlay(ren, hp);
                } catch (const std::exception& e) {
                    log_warn("HD XPLODE%d skipped: %s", i, e.what());
                }
            }
            collect_death_anims(ani, deaths_);
            xplode_.push_back(std::move(ani));
        }

        // ---- HD ANI overlays (DATA_HD/ANI/<NAME>.ANI) --------------------------
        // Optional truecolour 4x re-encodes of the classic match ANIs. Each is a
        // 1:1 frame upscale, uploaded LINEAR + un-snapped inside the AniTextures
        // it overlays; a missing/mismatched file is silently ignored (classic
        // look). Loaded here so the per-player recolour in build_player_sets
        // (run afterwards) propagates the HD frames into the coloured sets too.
        const auto hd_ani = game_dir / "DATA_HD" / "ANI";
        auto hd_ov = [&](AniTextures& t, const std::string& file) {
            tick();  // one per HD-overlay attempt (34 total; counted only when has_hd)
            auto p = hd_ani / file;
            if (!fs::exists(p)) return;
            try {
                t.load_hd_overlay(ren, p);
            } catch (const std::exception& e) {
                log_warn("HD ANI '%s' skipped: %s", file.c_str(), e.what());
            }
        };
        hd_ov(bombs_, "BOMBS.ANI");
        hd_ov(duds_, "DUDS.ANI");
        hd_ov(flame_, "MFLAME.ANI");
        hd_ov(stand_, "STAND.ANI");
        hd_ov(walk_, "WALK.ANI");
        hd_ov(kick_, "KICK.ANI");
        hd_ov(shadow_, "SHADOW.ANI");
        hd_ov(hurry_, "HURRY.ANI");
        hd_ov(kfont_, "KFONT.ANI");
        hd_ov(powers_, "POWERS.ANI");
        hd_ov(conveyor_, "CONVEYOR.ANI");
        hd_ov(extras_, "EXTRAS.ANI");
        hd_ov(aliens1_, "ALIENS1.ANI");
        hd_ov(trigbomb_, "TRIGANIM.ANI");
        for (int f = 0; f < kCornerFiles; ++f)
            hd_ov(corner_[f], "CORNER" + std::to_string(f) + ".ANI");
        for (int f = 0; f < kBwalkFiles; ++f)
            hd_ov(bwalk_[f], "BWALK" + std::to_string(f + 1) + ".ANI");
        for (int f = 0; f < kPunchFiles; ++f)
            hd_ov(punch_[f], "PUNBOMB" + std::to_string(f + 1) + ".ANI");
        for (int f = 0; f < kPupFiles; ++f)
            hd_ov(pickup_[f], "PUP" + std::to_string(f + 1) + ".ANI");

        // The shared, never-recoloured sets now have all their GPU textures
        // (classic + any HD overlay) — free their CPU pixel buffers. Unlike the
        // player-coloured sets above, these are never a recolour source, so both
        // the classic AND HD source pixels are dead weight after upload.
        // (tiles_/xbrick_ are per-stage: dropped in load_stage after each load.)
        for (AniTextures* t : {&kfont_, &hurry_, &shadow_, &powers_, &conveyor_, &extras_,
                               &aliens1_, &misc_, &edit_, &ring_}) {
            t->drop_classic_cpu();
            t->drop_hd_cpu();
        }
    } catch (const std::exception& e) {
        log_warn("asset load failed: %s", e.what());
        return false;
    }
    if (progress) progress(1.0f);  // snap to full regardless of which optional files were absent
    game_dir_ = game_dir;
    return true;
}

void AssetStore::build_player_sets(const std::int32_t colors[][3],
                                   const std::function<void(float)>& progress) {
    // Build the per-player HD sets here only when HD artwork is already on (it is
    // off by default at boot — hd_enabled_ starts false). Skipping HD avoids
    // retaining ten 16x-heavier HD sprite sets that the classic renderer never
    // samples; ensure_player_hd_sets() fills them lazily on the first Tab. When
    // HD IS on, build them now and mark them built (below).
    const bool with_hd = hd_enabled_;
    for (int p = 0; p < kLocalPlayers; ++p) {
        // Report BEFORE each slot's recolor so the boot LOADING dialog repaints
        // + pumps the window between players (this pass recolors ~15 ANI groups
        // x kLocalPlayers, each a per-pixel remap + GPU upload — heavy enough to
        // hang the window if left un-pumped).
        if (progress) progress(static_cast<float>(p) / static_cast<float>(kLocalPlayers));
        // Player slot p's intrinsic colour index is p itself (slot 0 = white /
        // 0.RMP, slot 1 = black / 1.RMP; docs/re/setup-screens.md — colour is
        // keyed by slot index, there is no picker). Prefer the authentic p.RMP
        // index remap; fall back to the truecolour recolour(color_rgb) only when
        // that colour's .RMP was missing/short (rmp_ok_[p] == false).
        const bool use_rmp = (p < kColors) && rmp_ok_[p];
        auto recolor = [&](const AniTextures& src) {
            // rmp_rgb_[p] = the .RMP tail — the fallback target for the 16bpp
            // type-4 frames the index remap cannot touch (sprites.cpp). The
            // in-match master-palette snap runs AFTER the recolor (colorpal.hpp)
            // so the player sprites are constrained to the shared match palette
            // like every other cel.
            return use_rmp ? src.recolored(ren_, rmp_[p], rmp_rgb_[p], &colorpal_, with_hd)
                           : src.recolored(ren_, colors[p], &colorpal_, with_hd);
        };

        walk_c_[p] = recolor(walk_);
        stand_c_[p] = recolor(stand_);
        kick_c_[p] = recolor(kick_);
        for (int f = 0; f < kCornerFiles; ++f)
            if (corner_[f].loaded()) corner_c_[f][p] = recolor(corner_[f]);
        for (int f = 0; f < kBwalkFiles; ++f)
            if (bwalk_[f].loaded()) bwalk_c_[f][p] = recolor(bwalk_[f]);
        for (int f = 0; f < kPunchFiles; ++f)
            if (punch_[f].loaded()) punch_c_[f][p] = recolor(punch_[f]);
        for (int f = 0; f < kPupFiles; ++f)
            if (pickup_[f].loaded()) pickup_c_[f][p] = recolor(pickup_[f]);
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
    player_hd_built_ = with_hd;
    // The player-coloured base sets were the recolour source; their classic CPU
    // pixels are now dead (every per-player set is uploaded). Free them — the
    // base GPU textures + per-frame w/h remain so base sprites still render. The
    // base HD source frames stay retained for the lazy per-player HD build unless
    // HD was built eagerly just now, in which case they too are done.
    drop_player_base_classic_cpu();
    if (with_hd) drop_player_base_hd_cpu();
    if (progress) progress(1.0f);
}

void AssetStore::drop_player_base_classic_cpu() {
    auto d = [](AniTextures& t) { t.drop_classic_cpu(); };
    d(walk_);
    d(stand_);
    d(kick_);
    d(bombs_);
    d(duds_);
    d(flame_);
    d(trigbomb_);
    for (auto& t : corner_) d(t);
    for (auto& t : bwalk_) d(t);
    for (auto& t : punch_) d(t);
    for (auto& t : pickup_) d(t);
    for (auto& t : xplode_) d(t);
}

void AssetStore::drop_player_base_hd_cpu() {
    auto d = [](AniTextures& t) { t.drop_hd_cpu(); };
    d(walk_);
    d(stand_);
    d(kick_);
    d(bombs_);
    d(duds_);
    d(flame_);
    d(trigbomb_);
    for (auto& t : corner_) d(t);
    for (auto& t : bwalk_) d(t);
    for (auto& t : punch_) d(t);
    for (auto& t : pickup_) d(t);
    for (auto& t : xplode_) d(t);
}

bool AssetStore::ensure_player_hd_sets() {
    if (player_hd_built_) return false;
    player_hd_built_ = true;
    for (int p = 0; p < kLocalPlayers; ++p) {
        // HD frames always take the green-excess tail recolour (truecolour
        // type-4, never paletted). rmp_rgb_[p] is that tail for BOTH the .RMP and
        // the VALUELST-fallback slots (set_color_fallbacks seeds it from
        // color_rgb where a .RMP was absent), so it reproduces exactly what
        // recolored()'s HD block would have used for either branch.
        const std::int32_t tail[3] = {rmp_rgb_[p][0], rmp_rgb_[p][1], rmp_rgb_[p][2]};
        auto hd = [&](AniTextures& dst, const AniTextures& src) {
            if (src.loaded() && dst.loaded()) dst.build_recolored_hd(ren_, src, tail);
        };
        hd(walk_c_[p], walk_);
        hd(stand_c_[p], stand_);
        hd(kick_c_[p], kick_);
        for (int f = 0; f < kCornerFiles; ++f) hd(corner_c_[f][p], corner_[f]);
        for (int f = 0; f < kBwalkFiles; ++f) hd(bwalk_c_[f][p], bwalk_[f]);
        for (int f = 0; f < kPunchFiles; ++f) hd(punch_c_[f][p], punch_[f]);
        for (int f = 0; f < kPupFiles; ++f) hd(pickup_c_[f][p], pickup_[f]);
        hd(bombs_c_[p], bombs_);
        hd(duds_c_[p], duds_);
        hd(trigbomb_c_[p], trigbomb_);
        hd(flame_c_[p], flame_);
        // Death pools: give each recoloured XPLODE set its HD textures, then
        // re-collect deaths_c_[p] so its Anim Sprites carry the new tex_hd (the
        // classic textures are unchanged, so the classic look is identical).
        deaths_c_[p].clear();
        for (std::size_t s = 0; s < xplode_c_[p].size(); ++s) {
            if (s < xplode_.size()) xplode_c_[p][s].build_recolored_hd(ren_, xplode_[s], tail);
            collect_death_anims(xplode_c_[p][s], deaths_c_[p]);
        }
    }
    // The base HD source frames existed only to feed this build — release them.
    drop_player_base_hd_cpu();
    return true;
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
        // Asset base names from the level registry (LevelDef): built-ins resolve
        // to FIELDn/TILESn/XBRICKn, byte-identical to the old hardcoded concat;
        // a custom level supplies its own bases. The null fallback keeps the
        // legacy default for any stray index the registry does not know.
        const match::LevelDef* def = levels_.find(stage);
        const std::string field_base = def ? def->field_asset : "FIELD" + std::to_string(stage);
        const std::string tiles_base = def ? def->tiles_asset : "TILES" + std::to_string(stage);
        const std::string xbrick_base =
            def ? def->xbrick_asset : "XBRICK" + std::to_string(stage);
        // Classic field: decode the 8-bit PCX, then run the in-match
        // master-palette snap (colorpal.hpp) before upload. Identity for
        // FIELD0/2..10 (authored in the palette); the visible fix is FIELD1's
        // blue/green dither, which the original snaps to the muted
        // (20,40,108)/(4,132,0) pair (pixel-exact vs a live capture). The HD
        // override below is truecolour and is NEVER snapped.
        {
            assets::Image field_img =
                assets::pcx::load(game_dir_ / "DATA" / "RES" / (field_base + ".PCX"));
            colorpal_.remap(field_img);
            field_.reset(make_texture(ren_, field_img));
        }
        // DATA_HD is deliberately optional. The game keeps the exact original
        // field when a modern replacement has not been authored yet, allowing
        // Tab to switch instantly without changing any gameplay data.
        field_hd_.reset();
        const fs::path hd_field = game_dir_ / "DATA_HD" / "RES" / (field_base + ".PCX");
        if (fs::exists(hd_field)) {
            try {
                field_hd_.reset(
                    make_texture(ren_, assets::pcx::load(hd_field), TextureArt::HighRes));
            } catch (const std::exception& e) {
                log_warn("HD stage %d field load failed: %s", stage, e.what());
            }
        }
        // Tiles + crumbling bricks: type-4 RGB555 cels, snapped to the master
        // palette like the field (a subtle ~2-3% shift — the art is mostly
        // authored in-palette, but the original snaps it and so do we).
        tiles_.load(ren_, game_dir_ / "DATA" / "ANI" / (tiles_base + ".ANI"), &colorpal_);
        xbrick_.load(ren_, game_dir_ / "DATA" / "ANI" / (xbrick_base + ".ANI"), &colorpal_);
        // Optional HD overlays for the per-stage tiles + crumbling bricks
        // (DATA_HD/ANI/TILES<n>.ANI, XBRICK<n>.ANI). Loaded before resolve_stage
        // resolves the solid/brick/burn sequences, so those Sprites carry tex_hd.
        {
            auto tp = game_dir_ / "DATA_HD" / "ANI" / (tiles_base + ".ANI");
            if (fs::exists(tp)) {
                try {
                    tiles_.load_hd_overlay(ren_, tp);
                } catch (const std::exception& e) {
                    log_warn("HD TILES%d skipped: %s", stage, e.what());
                }
            }
            auto xp = game_dir_ / "DATA_HD" / "ANI" / (xbrick_base + ".ANI");
            if (fs::exists(xp)) {
                try {
                    xbrick_.load_hd_overlay(ren_, xp);
                } catch (const std::exception& e) {
                    log_warn("HD XBRICK%d skipped: %s", stage, e.what());
                }
            }
        }
        // Per-stage tiles/bricks are shared (never recoloured); their GPU
        // textures now exist, so free the CPU pixels (classic + HD source).
        // resolve_stage reads only w/h from these, which drop_classic_cpu keeps.
        tiles_.drop_classic_cpu();
        tiles_.drop_hd_cpu();
        xbrick_.drop_classic_cpu();
        xbrick_.drop_hd_cpu();
    } catch (const std::exception& e) {
        log_warn("stage %d load failed: %s", stage, e.what());
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
    // The LEVEL & ROUNDS preview swatch is snapped in the original: sub_406AA3
    // loads FIELD<n>.PLT and the tile art through the SAME snapping loader
    // sub_4150F0 the match uses (pseudo.c 7985), NOT the front-end own-palette
    // path. So the preview's FIELD1 reads the muted master-palette dither just
    // like an in-match FIELD1, not the raw vivid blue (docs/re/facts.md
    // "In-match colour quantization").
    // Asset base names from the registry (byte-identical FIELDn/TILESn for the
    // built-ins). The solid/brick SEQUENCE names below still key off the stage
    // NUMBER ("tile <n> …") — that is how the ANI names its sequences
    // internally, independent of the file's base name.
    const match::LevelDef* def = levels_.find(stage);
    const std::string field_base = def ? def->field_asset : "FIELD" + std::to_string(stage);
    const std::string tiles_base = def ? def->tiles_asset : "TILES" + std::to_string(stage);
    StagePreview sp{};
    try {
        AniTextures tiles;
        tiles.load(ren_, game_dir_ / "DATA" / "ANI" / (tiles_base + ".ANI"), &colorpal_);
        const std::string n = std::to_string(stage);
        sp.solid = resolve_sequence(tiles, "tile " + n + " solid");
        sp.brick = resolve_sequence(tiles, "tile " + n + " brick");
        tiles.drop_classic_cpu();  // sequences resolved; only w/h + textures needed now
        stage_preview_tiles_.emplace(stage, std::move(tiles));
    } catch (const std::exception& e) {
        log_warn("stage %d preview tiles load failed: %s", stage, e.what());
    }
    try {
        auto img = assets::pcx::load(game_dir_ / "DATA" / "RES" / (field_base + ".PCX"));
        sdl::TexturePtr tex{make_texture(ren_, img, TextureArt::Classic, &colorpal_)};
        sp.field = tex.get();
        stage_preview_field_.emplace(stage, std::move(tex));
    } catch (const std::exception& e) {
        log_warn("stage %d preview field load failed: %s", stage, e.what());
    }
    return stage_preview_.emplace(stage, std::move(sp)).first->second;
}

const std::vector<Anim>& AssetStore::deaths_for(int player) const {
    if (player >= 0 && player < kLocalPlayers && !deaths_c_[player].empty())
        return deaths_c_[player];
    return deaths_;
}

const Sprite& AssetStore::frontend_pcx(const std::string& name) const {
    auto classic = front_pcx_.find(name);
    if (classic == front_pcx_.end()) {
        // The main menu and the GLUE screens (options/setup/level-select) are
        // SNAPPED to the shared master palette in the original: sub_42B9CE
        // (30760) and sub_4148E5 (17342-17343) load MAINMENU/GLUE<n> through
        // sub_4151CC -> sub_4150F0, the SAME snapping loader the match uses,
        // with COLOR.PAL master left active (no per-screen palette upload).
        // The logos/TITLE/DRAW/results backdrops instead bring their OWN
        // palette (sub_42A088 -> sub_41522D) and are NOT snapped, so a
        // name-keyed rule is exact: MAINMENU + GLUE<n> snap, everything else
        // (TITLE, DRAW, WIN, WINZ, ...) stays raw. Verified in the binary
        // (docs/re/facts.md "In-match colour quantization" front-end addendum).
        const bool snap = name == "MAINMENU" || name.rfind("GLUE", 0) == 0;
        // Cache an entry for every request (even failures) so a missing file logs
        // once and thereafter returns the same empty Sprite the Screen skips.
        Sprite sp{};
        try {
            auto img = assets::pcx::load(game_dir_ / "DATA" / "RES" / (name + ".PCX"));
            sdl::TexturePtr tex{
                make_texture(ren_, img, TextureArt::Classic, snap ? &colorpal_ : nullptr)};
            sp = {tex.get(), img.width, img.height, 0, 0};
            front_textures_.push_back(std::move(tex));
        } catch (const std::exception& e) {
            log_warn("front-end PCX '%s' load failed: %s", name.c_str(), e.what());
        }
        classic = front_pcx_.emplace(name, sp).first;
    }
    if (!hd_enabled_) return classic->second;

    if (auto it = front_pcx_hd_.find(name); it != front_pcx_hd_.end()) return it->second;

    // The HD replacement deliberately keeps the original Sprite's w/h values:
    // screen layout, bitmap-text inline art, and hit-free UI geometry all stay
    // in their faithful 640x480 coordinate space while SDL samples the modern
    // texture at the output resolution.
    Sprite hd = classic->second;
    const fs::path hd_path = game_dir_ / "DATA_HD" / "RES" / (name + ".PCX");
    if (fs::exists(hd_path)) {
        try {
            auto img = assets::pcx::load(hd_path);
            sdl::TexturePtr tex{make_texture(ren_, img, TextureArt::HighRes)};
            if (tex) {
                hd.tex = tex.get();
                front_textures_hd_.push_back(std::move(tex));
            }
        } catch (const std::exception& e) {
            log_warn("HD front-end PCX '%s' load failed: %s", name.c_str(), e.what());
        }
    }
    return front_pcx_hd_.emplace(name, hd).first->second;
}

const Sprite& AssetStore::bm_inline_pcx(const std::string& name) const {
    auto classic = bm_pcx_.find(name);
    if (classic == bm_pcx_.end()) {
        // Cache an entry for every request (even failures) so a missing file
        // logs once and thereafter returns the same empty Sprite the viewer
        // skips — the same guarded-cache shape frontend_pcx uses.
        Sprite sp{};
        try {
            auto img = assets::pcx::load(game_dir_ / "DATA" / "RES" / (name + ".PCX"));
            bm_pcx_keyed_.emplace(name, uses_key_index(img));
            apply_key_index(img);  // sub_44AED5's index-0 skip
            // Snapped to COLOR.PAL like every other sub_4150F0 load; remap()
            // already leaves the keyed texels alone, matching sub_41BBBD's own
            // `v5[0] = 0`.
            sdl::TexturePtr tex{make_texture(ren_, img, TextureArt::Classic, &colorpal_)};
            sp = {tex.get(), img.width, img.height, 0, 0};
            bm_textures_.push_back(std::move(tex));
        } catch (const std::exception& e) {
            log_warn("BM inline PCX '%s' load failed: %s", name.c_str(), e.what());
        }
        classic = bm_pcx_.emplace(name, sp).first;
    }
    if (!hd_enabled_) return classic->second;

    if (auto it = bm_pcx_hd_.find(name); it != bm_pcx_hd_.end()) return it->second;

    // Same rule as frontend_pcx: the HD texture inherits the CLASSIC w/h so the
    // 640x480 layout is untouched. Whether to key comes from the CLASSIC image,
    // because the 24-bit HD file has no index to test (key_color.hpp).
    Sprite hd = classic->second;
    const fs::path hd_path = game_dir_ / "DATA_HD" / "RES" / (name + ".PCX");
    if (fs::exists(hd_path)) {
        try {
            auto img = assets::pcx::load(hd_path);
            if (auto k = bm_pcx_keyed_.find(name); k != bm_pcx_keyed_.end() && k->second)
                apply_key_black(img);
            sdl::TexturePtr tex{make_texture(ren_, img, TextureArt::HighRes)};
            if (tex) {
                hd.tex = tex.get();
                bm_textures_.push_back(std::move(tex));
            }
        } catch (const std::exception& e) {
            log_warn("HD BM inline PCX '%s' skipped: %s", name.c_str(), e.what());
        }
    }
    return bm_pcx_hd_.emplace(name, hd).first->second;
}

// Defined by the generated author_photo_data.cpp (cmake/EmbedBinary.cmake).
extern const unsigned char kAuthorPhotoPcx[];
extern const unsigned int kAuthorPhotoPcx_size;

const Sprite& AssetStore::author_photo() const {
    if (author_photo_ready_) return author_photo_;
    author_photo_ready_ = true;  // one attempt; a failure stays an empty Sprite
    try {
        // TextureArt::HighRes, not Classic: this is a photograph, not 1997
        // palettised art, so it has no business in the soft-scaling registry
        // that toggles the classic textures between nearest and linear.
        auto img = assets::pcx::parse(
            std::span<const std::uint8_t>(kAuthorPhotoPcx, kAuthorPhotoPcx_size),
            "<embedded author photo>");
        author_photo_tex_.reset(make_texture(ren_, img, TextureArt::HighRes));
        author_photo_ = {author_photo_tex_.get(), img.width, img.height, 0, 0};
    } catch (const std::exception& e) {
        log_warn("embedded author photo failed: %s", e.what());
    }
    return author_photo_;
}

}  // namespace bomber::game
