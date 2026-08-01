#include "bomber/render/asset_store.hpp"

#include <algorithm>  // std::min (boot-loading progress clamp)
#include <cstdint>
#include <exception>
#include <span>
#include <string>
#include <utility>

#include "bomber/assets/bmfont.hpp"
#include "bomber/assets/pcx.hpp"
#include "bomber/assets/rmp.hpp"
#include "bomber/game_util/key_color.hpp"
#include "bomber/game_util/log.hpp"

namespace bomber::game {

namespace fs = std::filesystem;

// The shared arguments of one load() pass — a parameter object rather than the
// same five positional arguments at every group helper (§3). `snap` is the
// in-match master palette, applied to every match-drawn asset because the
// original renders the whole match on one shared 256-colour palette; front-end
// art and DATA_HD are deliberately NOT snapped (the original loads those
// through its non-snapping path).
struct AssetLoadPass {
    SDL_Renderer* ren = nullptr;
    fs::path game_dir;
    fs::path ani_dir;
    fs::path res_dir;
    const assets::colorpal::Palette* snap = nullptr;
    const std::function<void(float)>* progress = nullptr;
    int total_steps = 1;
    mutable int done_steps = 0;

    // Boot LOADING progress, one step per load point: the port's stand-in for
    // the original's sub_412E33(100*read/total) per-MASTER.ALI-entry readout,
    // reported 0 -> 1 so GameApp::draw_boot_loading can pump the window and
    // animate the bar between chunks. std::min clamps any drift; overshoot on a
    // missing-file path just fills a touch faster, never past 100%.
    void step() const {
        if (progress && *progress)
            (*progress)(
                std::min(1.0f, static_cast<float>(++done_steps) / static_cast<float>(total_steps)));
    }

    // An OPTIONAL ANI: a missing or broken file must NOT abort the whole load,
    // so each is isolated and logged once and the consumer degrades on its own.
    void optional_ani(AniTextures& dst, const std::string& file, bool snapped = true) const {
        const fs::path p = ani_dir / file;
        try {
            if (fs::exists(p)) dst.load(ren, p, snapped ? snap : nullptr);
        } catch (const std::exception& e) {
            log_warn("%s load failed: %s", file.c_str(), e.what());
        }
        step();
    }

    // An optional DATA_HD/ANI truecolour re-encode of a classic set: a 1:1
    // frame upscale, uploaded LINEAR and un-snapped inside the AniTextures it
    // overlays. A missing or mismatched file is ignored (classic look).
    void hd_overlay(AniTextures& dst, const std::string& file) const {
        step();  // counted per ATTEMPT (34 total; only reached when DATA_HD exists)
        const fs::path p = game_dir / "DATA_HD" / "ANI" / file;
        if (!fs::exists(p)) return;
        try {
            dst.load_hd_overlay(ren, p);
        } catch (const std::exception& e) {
            log_warn("HD ANI '%s' skipped: %s", file.c_str(), e.what());
        }
    }
};

void AssetStore::load_frontend_font(const fs::path& game_dir) {
    // FONT6.FON lives in the install ROOT (not under DATA/) and is otherwise
    // independent of every other load, so it is safe to load standalone ahead
    // of load(). Idempotent: a second call is a cheap re-parse.
    try {
        auto p = game_dir / "FONT6.FON";
        if (fs::exists(p)) frontend_font_ = assets::bmfont::load(p);
    } catch (const std::exception& e) {
        log_warn("FONT6.FON load failed: %s", e.what());
    }
}

const Sprite& AssetStore::load_frontend_winz(SDL_Renderer* ren, const fs::path& game_dir) {
    // frontend_pcx() needs the renderer + install root; load() re-assigns the
    // same values later, so seeding them here keeps this a plain pre-warm.
    ren_ = ren;
    game_dir_ = game_dir;
    return frontend_pcx("WINZ");
}

void AssetStore::load_core_anis(const AssetLoadPass& in) {
    // REQUIRED art: unlike every group below, a failure here propagates out of
    // load() and fails the whole store.
    kfont_.load(in.ren, in.ani_dir / "KFONT.ANI", in.snap);
    hurry_.load(in.ren, in.ani_dir / "HURRY.ANI", in.snap);
    bombs_.load(in.ren, in.ani_dir / "BOMBS.ANI", in.snap);
    duds_.load(in.ren, in.ani_dir / "DUDS.ANI", in.snap);
    // MFLAME.ANI, not FLAME.ANI — CORRECTED 2026-07-09 (docs/re/facts.md "ANI
    // sequence-name audit"): MASTER.ALI comments out `;-flame.ani` and loads
    // `-mflame.ani`, so FLAME.ANI's 7-step "flame <piece> green" cycles never
    // enter the original's pool; MFLAME.ANI's 5-step ones are what is shown.
    flame_.load(in.ren, in.ani_dir / "MFLAME.ANI", in.snap);
    stand_.load(in.ren, in.ani_dir / "STAND.ANI", in.snap);
    walk_.load(in.ren, in.ani_dir / "WALK.ANI", in.snap);
    kick_.load(in.ren, in.ani_dir / "KICK.ANI", in.snap);
    shadow_.load(in.ren, in.ani_dir / "SHADOW.ANI", in.snap);
    in.step();  // core match ANIs (kfont..shadow)
}

void AssetStore::load_optional_anis(const AssetLoadPass& in) {
    in.optional_ani(powers_, "POWERS.ANI");      // animated floor powerups
    in.optional_ani(conveyor_, "CONVEYOR.ANI");  // "extra conveyor <dir>"
    in.optional_ani(extras_, "EXTRAS.ANI");      // trampoline / arrow / warp
    in.optional_ani(aliens1_, "ALIENS1.ANI");    // campaign rover + ghost
    in.optional_ani(trigbomb_, "TRIGANIM.ANI");  // armed trigger bomb
    // HEADWIPE.ANI is deliberately NOT loaded: it is absent from MASTER.ALI, so
    // the original engine never loads it — dead art, like FLAME.ANI and
    // TRIGBOMB.ANI. The menu->setup screen change is a CUT in the original.
    //
    // MISC.ANI and EDIT.ANI are front-end art and take the NON-snapping path,
    // exactly as the original loads them.
    in.optional_ani(misc_, "MISC.ANI", /*snapped=*/false);
    in.optional_ani(edit_, "EDIT.ANI", /*snapped=*/false);
}

void AssetStore::load_ring(const AssetLoadPass& in) {
    // Goldman wheel pointer ("ring", docs/re/goldman-roulette.md §3/§7) —
    // RESOLVED: MISC.ANI owns it (its sequence table is checked against the
    // install), so it heads the probe list and the older guesses stay as
    // fallbacks for partial installs. GoldmanScreen draws no pointer if every
    // candidate is missing or lacks the sequence.
    static constexpr const char* kRingCandidates[] = {"MISC.ANI", "ROULETTE.ANI", "EXTRAS.ANI",
                                                      "CURSOR.ANI"};
    for (const char* name : kRingCandidates) {
        auto p = in.ani_dir / name;
        try {
            if (!fs::exists(p)) continue;
            AniTextures probe;
            probe.load(in.ren, p);
            if (resolve_sequence(probe, "ring").steps.empty()) continue;
            ring_ = std::move(probe);
            break;
        } catch (const std::exception& e) {
            log_warn("%s load failed (ring probe): %s", name, e.what());
        }
    }
    in.step();  // ring probe
}

void AssetStore::load_root_files(const AssetLoadPass& in) {
    // GameApp::init calls load_frontend_font() standalone before this, matching
    // sub_41095A's real order; this call stays so load() alone still gets it.
    load_frontend_font(in.game_dir);
    in.step();  // FONT6.FON

    // The MESSAGES.TXT string table (getstring / sub_4124A4), install ROOT like
    // the fonts. Optional — a missing file leaves getstring() on its fallbacks.
    try {
        auto p = in.game_dir / "MESSAGES.TXT";
        if (fs::exists(p)) messages_ = assets::res::load_messages(p);
    } catch (const std::exception& e) {
        log_warn("MESSAGES.TXT load failed: %s", e.what());
    }
    in.step();  // MESSAGES.TXT

    // The ten player-colour remap tables 0.RMP..9.RMP (install ROOT), the
    // authentic per-colour index remap the original blit applies to the "green"
    // player sprites (sub_415A1C via dword_460564[colour]). Each is guarded: a
    // missing or short file just marks that colour rmp_ok_=false and
    // build_player_sets falls back to the truecolour recolour for that slot.
    for (int i = 0; i < kColors; ++i) {
        load_rmp_table(in.game_dir, i);
        in.step();  // i.RMP
    }
}

void AssetStore::load_rmp_table(const fs::path& game_dir, int i) {
    const fs::path p = game_dir / (std::to_string(i) + ".RMP");
    try {
        if (!fs::exists(p)) {
            log_warn("%d.RMP missing; using fallback recolour", i);
            return;
        }
        auto rt = assets::res::load_rmp(p);
        rmp_[i] = rt.map;
        rmp_rgb_[i] = rt.rgb;  // authoritative slot colour (setup screen)
        rmp_ok_[i] = true;
    } catch (const std::exception& e) {
        log_warn("%d.RMP load failed (%s); using fallback recolour", i, e.what());
    }
}

void AssetStore::load_pose_families(const AssetLoadPass& in) {
    // The four multi-file pose families. All cosmetic and optional (optional_ani
    // isolates each file), so a partial install degrades to the next pose down
    // rather than failing the load. CORRECTED 2026-07-09: MASTER.ALI lists
    // punbomb1..4 and pup1..4, never punch.ani or bpickup.ani.
    for (int i = 0; i < kCornerFiles; ++i)
        in.optional_ani(corner_[i], "CORNER" + std::to_string(i) + ".ANI");
    for (int i = 0; i < kBwalkFiles; ++i)
        in.optional_ani(bwalk_[i], "BWALK" + std::to_string(i + 1) + ".ANI");
    for (int i = 0; i < kPunchFiles; ++i)
        in.optional_ani(punch_[i], "PUNBOMB" + std::to_string(i + 1) + ".ANI");
    for (int i = 0; i < kPupFiles; ++i)
        in.optional_ani(pickup_[i], "PUP" + std::to_string(i + 1) + ".ANI");
}

void AssetStore::load_powerup_icons(const AssetLoadPass& in) {
    static constexpr const char* kPowFiles[] = {
        "POWBOMB",  "POWFLAME", "POWDISEA", "POWKICK",  "POWSKATE", "POWPUNCH", "POWGRAB",
        "POWSPOOG", "POWGOLD",  "POWTRIG",  "POWJELLY", "POWEBOLA", "POWRAND"};
    for (int i = 0; i < sim::kPowerupKinds; ++i) {
        auto img = assets::pcx::load(in.res_dir / (std::string(kPowFiles[i]) + ".PCX"));
        sdl::TexturePtr tex{make_texture(in.ren, img, TextureArt::Classic, in.snap)};
        powerups_[i] = {tex.get(), img.width, img.height, 0, 0};
        powerup_textures_.push_back(std::move(tex));
        // Optional HD icon, truecolour + LINEAR, kept at the classic Sprite
        // geometry — only the static-fallback draw path uses these (the
        // animated POWERS.ANI HD is the primary route).
        auto hp = in.game_dir / "DATA_HD" / "RES" / (std::string(kPowFiles[i]) + ".PCX");
        try {
            if (fs::exists(hp)) {
                auto himg = assets::pcx::load(hp);
                sdl::TexturePtr htex{make_texture(in.ren, himg, TextureArt::HighRes, nullptr)};
                if (htex) {
                    powerups_[i].tex_hd = htex.get();
                    powerup_textures_.push_back(std::move(htex));
                }
            }
        } catch (const std::exception& e) {
            log_warn("HD powerup icon '%s' skipped: %s", kPowFiles[i], e.what());
        }
        in.step();  // POW<i> icon (+ optional HD)
    }
}

void AssetStore::load_death_anims(const AssetLoadPass& in) {
    // Every 'die green N' sequence across XPLODE*.ANI.
    for (int i = 1; i <= 32; ++i) {
        in.step();  // XPLODE<i>.ANI (+ optional HD) — counts all 32 slots
        auto p = in.ani_dir / ("XPLODE" + std::to_string(i) + ".ANI");
        if (!fs::exists(p)) continue;
        AniTextures ani;
        ani.load(in.ren, p, in.snap);
        // HD overlay BEFORE collecting so the death Sprites carry tex_hd (and
        // build_player_sets' recolor propagates it to the coloured pools).
        auto hp = in.game_dir / "DATA_HD" / "ANI" / ("XPLODE" + std::to_string(i) + ".ANI");
        try {
            if (fs::exists(hp)) ani.load_hd_overlay(in.ren, hp);
        } catch (const std::exception& e) {
            log_warn("HD XPLODE%d skipped: %s", i, e.what());
        }
        collect_death_anims(ani, deaths_);
        xplode_.push_back(std::move(ani));
    }
}

void AssetStore::apply_hd_overlays(const AssetLoadPass& in) {
    // Loaded here so the per-player recolour in build_player_sets (which runs
    // afterwards) propagates the HD frames into the coloured sets too.
    in.hd_overlay(bombs_, "BOMBS.ANI");
    in.hd_overlay(duds_, "DUDS.ANI");
    in.hd_overlay(flame_, "MFLAME.ANI");
    in.hd_overlay(stand_, "STAND.ANI");
    in.hd_overlay(walk_, "WALK.ANI");
    in.hd_overlay(kick_, "KICK.ANI");
    in.hd_overlay(shadow_, "SHADOW.ANI");
    in.hd_overlay(hurry_, "HURRY.ANI");
    in.hd_overlay(kfont_, "KFONT.ANI");
    in.hd_overlay(powers_, "POWERS.ANI");
    in.hd_overlay(conveyor_, "CONVEYOR.ANI");
    in.hd_overlay(extras_, "EXTRAS.ANI");
    in.hd_overlay(aliens1_, "ALIENS1.ANI");
    in.hd_overlay(trigbomb_, "TRIGANIM.ANI");
    for (int f = 0; f < kCornerFiles; ++f)
        in.hd_overlay(corner_[f], "CORNER" + std::to_string(f) + ".ANI");
    for (int f = 0; f < kBwalkFiles; ++f)
        in.hd_overlay(bwalk_[f], "BWALK" + std::to_string(f + 1) + ".ANI");
    for (int f = 0; f < kPunchFiles; ++f)
        in.hd_overlay(punch_[f], "PUNBOMB" + std::to_string(f + 1) + ".ANI");
    for (int f = 0; f < kPupFiles; ++f)
        in.hd_overlay(pickup_[f], "PUP" + std::to_string(f + 1) + ".ANI");
}

void AssetStore::load_all_groups(const AssetLoadPass& in) {
    load_core_anis(in);
    load_optional_anis(in);
    load_ring(in);
    load_root_files(in);
    load_pose_families(in);
    load_powerup_icons(in);
    load_death_anims(in);
    apply_hd_overlays(in);
    // The shared, never-recoloured sets now have all their GPU textures — free
    // their CPU pixel buffers. Unlike the player-coloured sets these are never a
    // recolour source, so both the classic AND the HD source pixels are dead
    // weight. (tiles_/xbrick_ are per-stage: dropped in load_stage each time.)
    for (AniTextures* t : {&kfont_, &hurry_, &shadow_, &powers_, &conveyor_, &extras_, &aliens1_,
                           &misc_, &edit_, &ring_}) {
        t->drop_classic_cpu();
        t->drop_hd_cpu();
    }
}

bool AssetStore::load(SDL_Renderer* ren, const fs::path& game_dir,
                      const std::function<void(float)>& progress) {
    ren_ = ren;
    // The step denominator is the count of step() points across the helpers
    // below; the DATA_HD overlay block's 34 are counted only when DATA_HD
    // exists, so a classic install fills the bar exactly as XPLODE finishes
    // rather than stalling short.
    const bool has_hd = fs::exists(game_dir / "DATA_HD");
    AssetLoadPass in;
    in.ren = ren;
    in.game_dir = game_dir;
    in.ani_dir = game_dir / "DATA" / "ANI";
    in.res_dir = game_dir / "DATA" / "RES";
    in.snap = &colorpal_;
    in.progress = &progress;
    in.total_steps = has_hd ? 121 : 87;

    // COLOR.PAL lives in the install ROOT. Optional — a missing or short file
    // leaves colorpal_ inert and the game renders the raw per-asset decode.
    try {
        colorpal_ = assets::colorpal::Palette::load(game_dir / "COLOR.PAL");
    } catch (const std::exception& e) {
        log_warn("COLOR.PAL unavailable (%s); classic colour snap disabled", e.what());
    }
    in.step();  // colorpal
    try {
        load_all_groups(in);
    } catch (const std::exception& e) {
        log_warn("asset load failed: %s", e.what());
        return false;
    }
    if (progress) progress(1.0f);  // snap to full whichever optional files were absent
    game_dir_ = game_dir;
    return true;
}

void AssetStore::build_player_set(int p, const std::int32_t colors[][3], bool with_hd) {
    // Player slot p's intrinsic colour index is p itself (slot 0 = white /
    // 0.RMP, slot 1 = black / 1.RMP; docs/re/setup-screens.md — colour is keyed
    // by slot index, there is no picker). Prefer the authentic p.RMP index
    // remap; fall back to the truecolour recolour only when that colour's .RMP
    // was missing or short.
    const bool use_rmp = (p < kColors) && rmp_ok_[p];
    const PlayerRemap colour{rmp_[p], rmp_rgb_[p]};
    auto recolor = [&](const AniTextures& src) {
        // The master-palette snap runs AFTER the recolor, so the player sprites
        // are constrained to the shared match palette like every other cel.
        return use_rmp ? src.recolored(ren_, colour, &colorpal_, with_hd)
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

void AssetStore::build_player_sets(const std::int32_t colors[][3],
                                   const std::function<void(float)>& progress) {
    // HD is off by default at boot, and skipping it avoids retaining ten
    // 16x-heavier sprite sets the classic renderer never samples;
    // ensure_player_hd_sets() fills them lazily on the first Tab.
    const bool with_hd = hd_enabled_;
    for (int p = 0; p < kLocalPlayers; ++p) {
        // Report BEFORE each slot so the boot LOADING dialog repaints + pumps
        // the window between players: this pass recolors ~15 ANI groups per
        // player, each a per-pixel remap + GPU upload.
        if (progress) progress(static_cast<float>(p) / static_cast<float>(kLocalPlayers));
        build_player_set(p, colors, with_hd);
    }
    player_hd_built_ = with_hd;
    drop_player_base_classic_cpu();
    if (with_hd) drop_player_base_hd_cpu();
    if (progress) progress(1.0f);
}

std::vector<AniTextures*> AssetStore::player_base_sets() {
    std::vector<AniTextures*> out{&walk_, &stand_, &kick_, &bombs_, &duds_, &flame_, &trigbomb_};
    for (auto& t : corner_) out.push_back(&t);
    for (auto& t : bwalk_) out.push_back(&t);
    for (auto& t : punch_) out.push_back(&t);
    for (auto& t : pickup_) out.push_back(&t);
    for (auto& t : xplode_) out.push_back(&t);
    return out;
}

void AssetStore::drop_player_base_classic_cpu() {
    for (AniTextures* t : player_base_sets()) t->drop_classic_cpu();
}

void AssetStore::drop_player_base_hd_cpu() {
    for (AniTextures* t : player_base_sets()) t->drop_hd_cpu();
}

void AssetStore::build_player_hd_set(int p) {
    // HD frames always take the green-excess tail recolour (truecolour type-4,
    // never paletted). rmp_rgb_[p] is that tail for BOTH the .RMP and the
    // VALUELST-fallback slots (set_color_fallbacks seeds it where a .RMP was
    // absent), so this reproduces exactly what recolored()'s HD block would
    // have used for either branch.
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

bool AssetStore::ensure_player_hd_sets() {
    if (player_hd_built_) return false;
    player_hd_built_ = true;
    for (int p = 0; p < kLocalPlayers; ++p) build_player_hd_set(p);
    // The base HD source frames existed only to feed this build — release them.
    drop_player_base_hd_cpu();
    return true;
}

void AssetStore::set_color_fallbacks(const std::int32_t colors[][3], int n) {
    // A loaded .RMP already set rmp_rgb_[i] from its own (authoritative) tail,
    // so only touch the colours that failed to load.
    for (int i = 0; i < kColors && i < n; ++i) {
        if (rmp_ok_[i]) continue;  // keep the .RMP tail
        rmp_rgb_[i][0] = static_cast<std::uint8_t>(colors[i][0]);
        rmp_rgb_[i][1] = static_cast<std::uint8_t>(colors[i][1]);
        rmp_rgb_[i][2] = static_cast<std::uint8_t>(colors[i][2]);
    }
}

void AssetStore::load_stage_field(const std::string& field_base) {
    // Classic field: decode the 8-bit PCX, then run the in-match master-palette
    // snap before upload. Identity for FIELD0/2..10 (authored in the palette);
    // the visible fix is FIELD1's blue/green dither, which the original snaps to
    // the muted (20,40,108)/(4,132,0) pair (pixel-exact vs a live capture).
    assets::Image field_img = assets::pcx::load(game_dir_ / "DATA" / "RES" / (field_base + ".PCX"));
    colorpal_.remap(field_img);
    field_.reset(make_texture(ren_, field_img));
    // DATA_HD is deliberately optional: the game keeps the exact original field
    // when a modern replacement has not been authored, so Tab switches
    // instantly without changing any gameplay data. Truecolour, NEVER snapped.
    field_hd_.reset();
    const fs::path hd_field = game_dir_ / "DATA_HD" / "RES" / (field_base + ".PCX");
    if (!fs::exists(hd_field)) return;
    try {
        field_hd_.reset(make_texture(ren_, assets::pcx::load(hd_field), TextureArt::HighRes));
    } catch (const std::exception& e) {
        log_warn("HD field '%s' load failed: %s", field_base.c_str(), e.what());
    }
}

void AssetStore::load_stage_tiles(const std::string& tiles_base, const std::string& xbrick_base) {
    // Type-4 RGB555 cels, snapped to the master palette like the field (a subtle
    // ~2-3% shift — the art is mostly authored in-palette, but the original
    // snaps it and so do we). The HD overlays load before resolve_stage
    // resolves the solid/brick/burn sequences, so those Sprites carry tex_hd.
    const fs::path ani = game_dir_ / "DATA" / "ANI";
    // Optional HD overlay, then release the CPU pixels: per-stage tiles/bricks
    // are shared (never recoloured), so once the GPU textures exist the source
    // is dead weight. resolve_stage reads only w/h, which drop_classic_cpu keeps.
    auto overlay_then_release = [&](AniTextures& set, const std::string& base) {
        const fs::path p = game_dir_ / "DATA_HD" / "ANI" / (base + ".ANI");
        try {
            if (fs::exists(p)) set.load_hd_overlay(ren_, p);
        } catch (const std::exception& e) {
            log_warn("HD '%s' skipped: %s", base.c_str(), e.what());
        }
        set.drop_classic_cpu();
        set.drop_hd_cpu();
    };
    tiles_.load(ren_, ani / (tiles_base + ".ANI"), &colorpal_);
    xbrick_.load(ren_, ani / (xbrick_base + ".ANI"), &colorpal_);
    overlay_then_release(tiles_, tiles_base);
    overlay_then_release(xbrick_, xbrick_base);
}

bool AssetStore::load_stage(int stage) {
    try {
        // Asset base names from the level registry: built-ins resolve to
        // FIELDn/TILESn/XBRICKn, byte-identical to the old hardcoded concat; a
        // custom level supplies its own. The null fallback keeps the legacy
        // default for any stray index the registry does not know.
        const match::LevelDef* def = levels_.find(stage);
        const std::string n = std::to_string(stage);
        load_stage_field(def ? def->field_asset : "FIELD" + n);
        load_stage_tiles(def ? def->tiles_asset : "TILES" + n,
                         def ? def->xbrick_asset : "XBRICK" + n);
    } catch (const std::exception& e) {
        log_warn("stage %d load failed: %s", stage, e.what());
        return false;
    }
    return field_ != nullptr;
}

const AssetStore::StagePreview& AssetStore::stage_preview(int stage) const {
    if (auto it = stage_preview_.find(stage); it != stage_preview_.end()) return it->second;
    // The LEVEL & ROUNDS swatch is snapped in the original: sub_406AA3 loads
    // FIELD<n>.PLT and the tile art through the SAME snapping loader sub_4150F0
    // the match uses (pseudo.c 7985), NOT the front-end own-palette path — so
    // the preview's FIELD1 reads the muted master-palette dither, exactly like
    // an in-match FIELD1. The sequence NAMES key off the stage NUMBER ("tile
    // <n> …") independently of the file's base name, which comes from the
    // registry. An entry is cached even when partial/empty, so a missing file
    // logs once and thereafter just draws nothing.
    const match::LevelDef* def = levels_.find(stage);
    const std::string n = std::to_string(stage);
    const std::string tiles_base = def ? def->tiles_asset : "TILES" + n;
    const std::string field_base = def ? def->field_asset : "FIELD" + n;
    StagePreview sp{};
    try {
        AniTextures tiles;
        tiles.load(ren_, game_dir_ / "DATA" / "ANI" / (tiles_base + ".ANI"), &colorpal_);
        sp.solid = resolve_sequence(tiles, "tile " + n + " solid");
        sp.brick = resolve_sequence(tiles, "tile " + n + " brick");
        tiles.drop_classic_cpu();  // sequences resolved; only w/h + textures needed
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

Sprite AssetStore::hd_override(const std::string& name, Sprite classic, bool key_black,
                               std::vector<sdl::TexturePtr>& owner) const {
    // The HD replacement keeps the CLASSIC Sprite's w/h: screen layout, inline
    // art and UI geometry all stay in the faithful 640x480 coordinate space
    // while SDL samples the modern texture at the output resolution. `key_black`
    // is decided by the CLASSIC image, because the 24-bit HD file has no palette
    // index to test (key_color.hpp).
    const fs::path hd_path = game_dir_ / "DATA_HD" / "RES" / (name + ".PCX");
    if (!fs::exists(hd_path)) return classic;
    try {
        auto img = assets::pcx::load(hd_path);
        if (key_black) apply_key_black(img);
        sdl::TexturePtr tex{make_texture(ren_, img, TextureArt::HighRes)};
        if (tex) {
            classic.tex = tex.get();
            owner.push_back(std::move(tex));
        }
    } catch (const std::exception& e) {
        log_warn("HD PCX '%s' skipped: %s", name.c_str(), e.what());
    }
    return classic;
}

Sprite AssetStore::load_front_sprite(const std::string& name) const {
    // MAINMENU and the GLUE screens are SNAPPED to the master palette in the
    // original: sub_42B9CE (30760) and sub_4148E5 (17342-17343) load them
    // through sub_4151CC -> sub_4150F0, the same snapping loader the match uses,
    // with COLOR.PAL left active. The logos/TITLE/DRAW/results backdrops bring
    // their OWN palette (sub_42A088 -> sub_41522D) and are NOT snapped, so a
    // name-keyed rule is exact (docs/re/facts.md "In-match colour quantization"
    // front-end addendum).
    const bool snap = name == "MAINMENU" || name.rfind("GLUE", 0) == 0;
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
    return sp;
}

Sprite AssetStore::load_bm_sprite(const std::string& name) const {
    Sprite sp{};
    try {
        auto img = assets::pcx::load(game_dir_ / "DATA" / "RES" / (name + ".PCX"));
        bm_pcx_keyed_.emplace(name, uses_key_index(img));
        apply_key_index(img);  // sub_44AED5's index-0 skip
        // Snapped to COLOR.PAL like every other sub_4150F0 load; remap() leaves
        // the keyed texels alone, matching sub_41BBBD's own `v5[0] = 0`.
        sdl::TexturePtr tex{make_texture(ren_, img, TextureArt::Classic, &colorpal_)};
        sp = {tex.get(), img.width, img.height, 0, 0};
        bm_textures_.push_back(std::move(tex));
    } catch (const std::exception& e) {
        log_warn("BM inline PCX '%s' load failed: %s", name.c_str(), e.what());
    }
    return sp;
}

const Sprite& AssetStore::frontend_pcx(const std::string& name) const {
    // An entry is cached even on failure, so a missing file logs once and
    // thereafter returns the same empty Sprite the Screen skips.
    auto classic = front_pcx_.find(name);
    if (classic == front_pcx_.end())
        classic = front_pcx_.emplace(name, load_front_sprite(name)).first;
    if (!hd_enabled_) return classic->second;
    if (auto it = front_pcx_hd_.find(name); it != front_pcx_hd_.end()) return it->second;
    return front_pcx_hd_
        .emplace(name, hd_override(name, classic->second, false, front_textures_hd_))
        .first->second;
}

const Sprite& AssetStore::bm_inline_pcx(const std::string& name) const {
    auto classic = bm_pcx_.find(name);
    if (classic == bm_pcx_.end()) classic = bm_pcx_.emplace(name, load_bm_sprite(name)).first;
    if (!hd_enabled_) return classic->second;
    if (auto it = bm_pcx_hd_.find(name); it != bm_pcx_hd_.end()) return it->second;
    const auto k = bm_pcx_keyed_.find(name);
    const bool keyed = k != bm_pcx_keyed_.end() && k->second;
    return bm_pcx_hd_.emplace(name, hd_override(name, classic->second, keyed, bm_textures_))
        .first->second;
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
        auto img =
            assets::pcx::parse(std::span<const std::uint8_t>(kAuthorPhotoPcx, kAuthorPhotoPcx_size),
                               "<embedded author photo>");
        author_photo_tex_.reset(make_texture(ren_, img, TextureArt::HighRes));
        author_photo_ = {author_photo_tex_.get(), img.width, img.height, 0, 0};
    } catch (const std::exception& e) {
        log_warn("embedded author photo failed: %s", e.what());
    }
    return author_photo_;
}

}  // namespace bomber::game
