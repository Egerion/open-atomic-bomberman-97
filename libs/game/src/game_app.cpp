#include "bomber/game/game_app.hpp"

#include <algorithm>  // std::max_element
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iterator>  // std::size
#include <string>

#include "bomber/assets/install.hpp"
#include "bomber/game/anim_pace.hpp"
#include "bomber/game/bmscreen.hpp"
#include "bomber/game/sprites.hpp"
#include "bomber/match/match_factory.hpp"

namespace bomber::game {

namespace fs = std::filesystem;

bool GameApp::init() {
    fs::path game = !opts_.game_dir.empty() ? opts_.game_dir : assets::default_game_dir();
    if (game.empty() || !fs::is_directory(game / "DATA")) {
        std::fprintf(stderr,
                     "usage: bomber_game [game_dir] [scheme.sch]\n"
                     "(or set BOMBER_GAME_DIR / gamedir.txt)\n");
        return false;
    }
    opts_.game_dir = game;
    fs::path scheme_path =
        !opts_.scheme.empty() ? opts_.scheme : game / "DATA" / "SCHEMES" / "BASIC.SCH";

    try {
        scheme_ = assets::sch::load(scheme_path);
        values_ = assets::res::load_values(game / "DATA" / "RES" / "VALUELST.RES");
        if (const char* env = std::getenv("BOMBER_GAME_SECONDS"); env && *env)
            values_.values[100] = std::atoi(env);  // testing hook
        // The install-root options.ini (sub_406238) — ALL 22 keys, docs/re/
        // results-and-options.md §3. Loaded ONCE here into options_; every
        // Options-screen edit thereafter mutates options_ in memory only
        // (write-on-exit, §2 — flush_options() is the sole writer).
        options_path_ = game / "options.ini";
        assets::Options loaded_opts = assets::load_options(options_path_);
        conveyor_speed_index_ = loaded_opts.conveyor_speed;
        // Team Play ("team_play="): absent key ⇒ OFF, matching the confirmed
        // team-mode default (docs/re/setup-screens.md: "Team mode is toggled on
        // the OPTIONS game-type screen, OFF by default").
        team_play_ = loaded_opts.team_play.value_or(false);
        options_.team_play = team_play_;
        options_.random_start = loaded_opts.random_start.value_or(false);
        options_.conveyor_speed_index = conveyor_speed_index_.value_or(1);
        options_.stomped_bombs_detonate = loaded_opts.stomped_bombs_detonate.value_or(false);
        options_.win_by_kills = loaded_opts.win_by_kills.value_or(false);
        options_.goldman = loaded_opts.goldman.value_or(false);
        options_.enclosement_depth = loaded_opts.enclosement_depth.value_or(1);
        options_.playtime_seconds = loaded_opts.playtime.value_or(150);
        options_.diseases_destroyable = loaded_opts.diseases_destroyable.value_or(false);
        options_.disable_game_music = loaded_opts.disable_game_music.value_or(false);
        // "keydef=" -> KeyboardMapper's two live key-sets (docs/re/results-and-
        // options.md §2). A KeyDef triple with scancode == -1 (never written)
        // keeps that action's compiled-in default (input.hpp's
        // default_key_set) rather than binding to scancode 0.
        if (loaded_opts.keydef) {
            for (int set = 0; set < assets::KeyDef::kSets; ++set) {
                KeySet ks = keyboard_.key_set(set);
                for (int action = 0; action < kKeyActionCount; ++action) {
                    int sc = loaded_opts.keydef->scancode[set][action];
                    if (sc >= 0) ks.scancode[action] = sc;
                }
                keyboard_.set_key_set(set, ks);
            }
        }
        // "num_to_win_match=" seeds win_target_'s default (task item 5, §5):
        // reset_match_scores() falls back to this when getvalue(310) is
        // absent/invalid, and the LEVEL & ROUNDS screen's WINS row still
        // overrides per-match on top of whichever default won.
        num_to_win_match_ = loaded_opts.num_to_win_match;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return false;
    }

    video_.emplace();
    if (!video_->ok()) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return false;
    }
    SDL_Window* win = nullptr;
    SDL_Renderer* ren = nullptr;
    if (!SDL_CreateWindowAndRenderer("Open Bomberman", kScreenW * 2, kScreenH * 2, 0, &win,
                                     &ren)) {
        std::fprintf(stderr, "SDL_CreateWindowAndRenderer: %s\n", SDL_GetError());
        return false;
    }
    window_.reset(win);
    sdl_renderer_.reset(ren);
    SDL_SetRenderLogicalPresentation(ren, kScreenW, kScreenH,
                                     SDL_LOGICAL_PRESENTATION_LETTERBOX);

    if (!assets_.load(ren, game)) return false;
    seqs_.resolve(assets_);

    // Initial joystick enumeration (docs/re/setup-screens.md joystick pane,
    // sub_429628). Hotplug events refresh this again in present_setup/run_match
    // so a stick plugged in after boot still shows up without a restart.
    gamepads_.refresh();

    if (!opts_.demo && !audio_.init(game))
        std::fprintf(stderr, "audio unavailable, continuing silent\n");

    base_tuning_ = match::build_match_config(scheme_, 2, 0, &values_).tuning;
    // Seed setup-screen slot colours from VALUELST for any colour without a .RMP
    // tail (a loaded .RMP keeps its own authoritative tail), then build the
    // per-player recolored sprite sets (authentic .RMP remap where available).
    assets_.set_color_fallbacks(base_tuning_.color_rgb, 10);
    assets_.build_player_sets(base_tuning_.color_rgb);
    seqs_.resolve(assets_);  // re-resolve: player sprite sets exist now

    renderer_.emplace(ren, assets_, seqs_);
    screen_.emplace(assets_, audio_);
    transition_.emplace(assets_);
    // Upload FONT6.FON glyph textures for the .BM help/credits screens. Empty
    // when the font is missing (the BM viewer then draws no glyphs).
    front_font_.build(ren, assets_.frontend_font());
    return true;
}

namespace {

// The front-end screen library (docs/re/frontend-flow.md). Data-only — each
// full-screen image maps to one ScreenDef; a screen carries NO music id (music
// is a separate concern, sub_42741E, started once by the boot/menu callers and
// played continuously — see run_boot_attract / present_menu). The waited-screen
// dwell is CONFIRMED getvalue(12) = 7 s (VALUELST line `12,7`; sub_42A088's wait
// loop times out at start + getvalue(12) using C time_() = seconds, synthesizing
// Enter). Logos and title all use that 7 s dwell and stay keypress-skippable;
// results wait a bounded beat then return. Missing art just clears to black and
// the screen still advances.

// The CONFIRMED front-end SOUNDLST ids (BM95.EXE, docs/re/frontend-flow.md):
//   boot/title music 1000 (0x3E8, sub_42741E in sub_42B060 @0x42B060)
//   menu music       1010 (0x3F2, sub_42741E in sub_42B9CE @0x42B9CE)
//   title intro sting 2800 (sub_427BFB(2800) in sub_42B060, one-shot)
//   accept sting       10  (menuexit, sub_427961(10) in sub_42A088)
//   nav blip           20  (letter1,  sub_427961(20) in sub_42A088)
// Music tracks loop (sub_4273A4 sets loop count 0xFFFF); stings/blips are
// one-shot (sub_427B36 sets loop count 0). AudioEngine mirrors this split:
// start_music() = the looping music channel, play() = a one-shot SFX voice.
constexpr int kBootMusicId = 1000;   // 0x3E8 — TITLE.RSS, the continuous boot track
constexpr int kMenuMusicId = 1010;   // 0x3F2 — MENU.RSS, started on menu entry
// The results/round handler music (sub_42A3F6). Entry starts the "win" track
// sub_42741E(0x3FC) = 1020, played under the VICTORY screen; the DRAW branch
// switches to sub_42741E(0x46A) = 1130 ("draw") before showing DRAW.PCX. Both
// are looping tracks (start_music), replacing the menu/stage music while the
// results screen is up. (0x3FC=1020 win, 0x46A=1130 draw — confirmed hex.)
constexpr int kWinMusicId = 1020;    // 0x3FC — WIN.RSS, VICTORY screen backdrop
constexpr int kDrawMusicId = 1130;   // 0x46A — DRAW.RSS, DRAW screen backdrop
// The title intro sting is a contiguous SOUNDLST GROUP (sub_427BFB(2800) picks a
// random member): 2800..2810 = "ATOMIC BOMBERMAN!" takes (GEN8A/…); the file's
// "2899 is the last intro" comment is the group's nominal end. We span 2800..2899
// and let play_random_in_range hit only the loaded ids.
constexpr int kTitleStingLo = 2800;
constexpr int kTitleStingHi = 2899;
// The menu-quit / exit sting group (sub_427BFB(2600) in the quit handler
// sub_412987): 2600..2699 = "go outside and play now!" takes (quitgame/EOFM7*/…).
constexpr int kQuitStingLo = 2600;
constexpr int kQuitStingHi = 2699;

// The waited-screen dwell — CONFIRMED getvalue(12) = 7 (VALUELST line `12,7`).
// sub_42A088's wait loop times out at start + getvalue(12) using C time_()
// (whole seconds), then synthesizes Enter (13) and advances. The logos and the
// title all share this one timeout; on the title's timeout the original falls
// straight through to the menu (it does NOT re-run the intro). We express it in
// ms (getvalue(12) * 1000) so it is resolution-independent.
constexpr std::uint32_t kBootDwellMs = 7000;  // getvalue(12) == 7 s

ScreenDef logo_screen(const char* bg) {
    return ScreenDef{bg, {}, /*dwell_ms*/ kBootDwellMs, /*skippable*/ true};
}
ScreenDef title_screen() {
    return ScreenDef{"TITLE", {}, /*dwell_ms*/ kBootDwellMs, /*skippable*/ true};
}
// Results: DRAW (no survivor / time up) or VICTORY<player> (one survivor). The
// original draws these with sub_42A088(name, 0) then a bespoke "any key, or 6 s
// in attract" loop (sub_42A3F6); we model it as a normal Screen with a bounded
// dwell so an unattended machine returns to the menu on its own.
constexpr std::uint32_t kResultsDwellMs = 6000;  // sub_42A3F6 attract auto-advance
// Format a MESSAGES.TXT label that carries a single %u/%d/%i with `v`, safely:
// the format string is the user's own file, so ignore any %s/%% (leave literal)
// rather than risk a wrong-type sprintf. A minimal, crash-proof getstring format.
std::string fmt_u(const std::string& f, int v) {
    auto p = f.find('%');
    if (p == std::string::npos) return f;
    std::size_t q = p + 1;
    while (q < f.size() && f[q] != 'u' && f[q] != 'd' && f[q] != 'i' && f[q] != 's' &&
           f[q] != '%')
        ++q;
    if (q < f.size() && (f[q] == 'u' || f[q] == 'd' || f[q] == 'i'))
        return f.substr(0, p) + std::to_string(v) + f.substr(q + 1);
    return f;
}

// Same crash-proof single-specifier substitution for a %s label (the level-line
// getstring(210)): splice `v` in for the first %s, leave any other specifier
// literal. The format string is the user's own MESSAGES.TXT entry.
std::string fmt_s(const std::string& f, const std::string& v) {
    auto p = f.find('%');
    if (p == std::string::npos) return f;
    std::size_t q = p + 1;
    while (q < f.size() && f[q] != 'u' && f[q] != 'd' && f[q] != 'i' && f[q] != 's' &&
           f[q] != '%')
        ++q;
    if (q < f.size() && f[q] == 's') return f.substr(0, p) + v + f.substr(q + 1);
    return f;
}

ScreenDef draw_screen() {
    // DRAW.PCX. The draw sting is a ONE-SHOT group play (sub_427BFB(1700) picks a
    // random member of the contiguous "tie game/draw game" SOUNDLST run at 1700),
    // fired once by run_app on entering Results via audio_.play_random_in_range —
    // NOT looped: a screen carries no music id, so nothing restarts the sting.
    return ScreenDef{"DRAW", {}, kResultsDwellMs, /*skippable*/ true};
}
// The SOUNDLST "tie game/draw game" voice group begins at 1700 (the file's own
// "; tie game/draw game" comment) and runs contiguously to its "1999 is the last
// tie game/draw game sound" bound; sub_427BFB(1700) plays a random member once.
// We span the full 1700..1999 group so play_random_in_range can pick any loaded
// take (GUMP1/GEN11*/ZAA*/…), matching the original's variety.
constexpr int kDrawStingLo = 1700;
constexpr int kDrawStingHi = 1999;
ScreenDef victory_screen(int player) {
    // VICTORY<player>.PCX — the original resolves "victory%u" against the winner
    // index (sub_42A3F6 aVictoryU); the install ships VICTORY0..VICTORY9. The
    // "we have a winner" voice group (2000) is played by run_app's Results
    // handler, under this screen, per §1 (fires as soon as v73 is computed).
    // (ScreenDef.background owns its own std::string copy, so this is safe.)
    return ScreenDef{"VICTORY" + std::to_string(player), {}, kResultsDwellMs,
                     /*skippable*/ true};
}
// --- Main-menu model (sub_42B9CE) -----------------------------------------
// The original menu highlights one of seven rows (its selection variable v10
// runs 0..6) over MAINMENU.PCX and dispatches on Enter: 0=Play (sub_42A3F6 runs
// a match + results), 1/2=setup screens (sub_42B0CE/sub_42B47D), 3=editor,
// 4=credits (.BM), 5=roulette, 6=quit (sub_412987, exit sting 2600). We keep
// the row set and order but map the not-yet-built leaves to their stub
// AppInputs; Start and Quit are wired live. (docs/re/frontend-flow.md.)
struct MenuItem {
    AppInput action;   // resolved when Enter selects this row
    bool live;         // false = a documented stub row (no handler yet, inert)
};

// Seven rows in the ORIGINAL's v10 order (sub_42B9CE), so the cursor anchor
// (getvalue 700-702) lands on the labels baked into MAINMENU.PCX:
//   0 Play           -> StartMatch   (live)
//   1 setup A        -> OpenOptions  (interactive Team Play / Conveyor Speed
//                                     screen; F1 on it reaches the OPTIONS.BM
//                                     help text)
//   2 setup B        -> OpenNetwork  (help overlay live; interactive UI = TODO)
//   3 Editor         -> stub         (map editor not built — inert, documented)
//   4 Credits        -> OpenCredits  (live: CREDITS.BM viewer)
//   5 Roulette       -> stub         (roulette/help not built — inert, documented)
//   6 Quit           -> Quit         (live)
// The Controllers help (INPUT.BM) is reachable from the interactive controller
// setup screen in the original; here it has no dedicated main-menu row, so the
// OpenControllers edge is exercised by the doctest/flow but not bound to a row
// (a documented gap — the row belongs to the deferred controller-setup UI).
constexpr MenuItem kMenuItems[] = {
    {AppInput::StartMatch, true},       // 0 Play
    {AppInput::OpenOptions, true},      // 1 setup A -> options help
    {AppInput::OpenNetwork, true},      // 2 setup B -> network help
    {AppInput::Advance, false},         // 3 Editor (stub, inert)
    {AppInput::OpenCredits, true},      // 4 Credits
    {AppInput::Advance, false},         // 5 Roulette (stub, inert)
    {AppInput::Quit, true},             // 6 Quit
};
constexpr int kMenuCount = static_cast<int>(std::size(kMenuItems));

// Cursor anchor over MAINMENU.PCX — CONFIRMED getvalue(700/701/702) (sub_42B9CE:
// v11=getvalue(700)=X, v1=getvalue(701)=Y, getvalue(702)=Y-step; the bomb-
// trigger sprite is blitted at x=X, y=Y + Ystep*row). Read live from VALUELST
// (columns of the multi-value row 700, whose own legend reads "X, Y - first item
// / YS - y-spacing"); these fallbacks are that install's values (332,140,38) so
// a stripped VALUELST still positions sanely. The idle/attract timeout uses
// getvalue(92) (sub_42B9CE), distinct from the waited-screen getvalue(12).
constexpr int kMenuCursorXFallback = 332;      // getvalue(700)
constexpr int kMenuCursorYFallback = 140;      // getvalue(701)
constexpr int kMenuCursorStepFallback = 38;    // getvalue(702)

}  // namespace

void GameApp::start_match(std::uint32_t seed) {
    // Random Start (options.ini "random_start=" / Options row 1, §3):
    // shuffles which of the scheme's own spawn slots each player index gets
    // (match_factory.hpp's own doc comment has the full clean-room rationale
    // — no exact placement algorithm is pinned in the RE brief, only the
    // toggle + key).
    sim::MatchConfig cfg = match::build_match_config(scheme_, sim::kMaxPlayers, seed, &values_,
                                                      options_.random_start);
    // Roster from the PLAYER INPUT screen (present_setup): OFF slots are inactive,
    // COMPUTER slots are AI-driven, KEYBOARD slots are local human(s). The per-slot
    // team feeds MatchConfig::team[] -> the hashed Player::team.
    //
    // Mapping: the original's +84 byte is 0 or 1 and IN TEAM MODE BOTH values
    // are real teams (sub_4141F8 inks team 1 vs "the rest" — two sides), while
    // the sim's convention reserves team 0 for "no team / solo side"
    // (simulation.cpp on_same_side). So shift the setup byte up by one when
    // team play is on: +84==0 -> sim team 1, +84==1 -> sim team 2. Without the
    // shift every un-toggled slot would wrongly fight solo.
    for (int i = 0; i < sim::kMaxPlayers; ++i) {
        cfg.active[i] = setup_type_[i] != 0;
        cfg.ai[i] = setup_type_[i] == 1;
        cfg.team[i] = static_cast<std::uint8_t>(setup_team_[i] + 1);
    }
    // Override the Conveyor Speed index from options.ini if present (this
    // install = 2 high); otherwise Tuning keeps the confirmed default (1
    // medium). conveyor_speed() clamps to [0, count-1], so a raw index is safe.
    if (conveyor_speed_index_) cfg.tuning.conveyor_speed_index = *conveyor_speed_index_;
    // Enclosement Depth (options.ini "enclosement_depth=" / Options row 7,
    // §3): a REAL Tuning consumer (enclosure.cpp/ai.cpp). base_tuning_ already
    // carries the VALUELST default; the Options screen's live edit overrides
    // it per match, same pattern as Conveyor Speed above.
    cfg.tuning.enclosement_depth = options_.enclosement_depth;
    // Play Time (options.ini "playtime=" / Options row 9, §3): a REAL Tuning
    // consumer (setup.cpp's ticks_left = game_seconds * kTicksPerSecond). The
    // "unlimited" sentinel (1001) has no sim meaning yet — a very long but
    // finite clock is the closest faithful stand-in without inventing a
    // separate "no clock" sim mode (out of scope: PRESENTATION/CONFIG ONLY).
    cfg.tuning.game_seconds =
        options_.playtime_seconds == 1001 ? 99999 : options_.playtime_seconds;
    // Team Play (options.ini "team_play=" / the interactive Options screen):
    // the game-type-level team-mode GATE (docs/re/setup-screens.md
    // `dword_464964`), separate from each slot's own +84 team byte. OFF means
    // team mode is off regardless of what a slot's 'T' toggle left behind, so
    // zero every slot's team here (sim team 0 = solo side) — MatchConfig::
    // team[] stays the single source of truth for the hashed Player::team.
    if (!team_play_) cfg.team.fill(0);
    // Goldman wheel award (docs/re/goldman-roulette.md §4): sub_4214BC grants
    // the last spin's prize to the gold player EVERY round of the following
    // match, not just the round right after the spin — build_match_config
    // runs at every start_match() call (including RoundContinue's re-init),
    // so re-applying gold_prize_/gold_player_ here reproduces that "persists
    // until the next spin" behaviour for free. A no-op (all-false overlay)
    // whenever gold_prize_ < 0 (no successful spin yet) or the mapped
    // sim::PowerupType is None (clogs, doc §8 — no sim kind exists yet).
    if (gold_player_ >= 0 && gold_prize_ >= 0) {
        sim::PowerupType pt = wheel_prize_to_powerup(gold_prize_);
        if (pt != sim::PowerupType::None) {
            auto kind = static_cast<int>(pt);
            if (team_play_) {
                // Team mode: the doc's "team id encoded as 0 or 2" compares
                // against the RAW +84 byte, i.e. our setup_team_[] before the
                // +1 shift above — every member of the gold TEAM gets the
                // bump (doc §4 "every member of the gold team").
                for (int i = 0; i < sim::kMaxPlayers; ++i)
                    if (cfg.active[i] && setup_team_[i] == gold_player_)
                        cfg.born_with_extra[i][kind] = true;
            } else if (gold_player_ < sim::kMaxPlayers && cfg.active[gold_player_]) {
                cfg.born_with_extra[gold_player_][kind] = true;
            }
        }
    }
    // Level from the LEVEL screen (present_map_select -> dword_464998): the match
    // init (sub_410B6E) resolves it to a stage index dword_46499C. RANDOM (-1) ->
    // keep pick_stage over the enabled rotation (VALUELST 1150-1160, the same
    // 200-try random loop the original runs); a specific level (0..10) -> use that
    // index directly. Clamp to the valid stage range defensively.
    int stage;
    if (selected_level_ < 0) {
        stage = match::pick_stage(base_tuning_, seed);
    } else {
        stage = selected_level_;
        if (stage > 10) stage = 10;
    }
    // Overlay this board's stage actors (conveyors/trampolines/etc) from
    // EXTRA<stage>.RES before constructing the sim — the actor layout is a
    // hashed setup input like the cell grid (docs/re/stage-actors.md). A board
    // with no EXTRA file simply has none. Random '-T,H' trampolines resolve off
    // a setup-only RNG inside apply_actors, never the sim's per-tick stream.
    auto actors = assets::extra::load_for_board(opts_.game_dir, stage, sim::kGridWidth,
                                                sim::kGridHeight);
    match::apply_actors(cfg, actors, seed);
    sim_ = sim::Simulation(cfg);
    if (assets_.load_stage(stage)) {
        seqs_.resolve_stage(assets_, stage);
        // Disable music during gameplay (options.ini "disable_game_music=" /
        // Options row 13, §3): a REAL consumer — simply don't start the
        // in-match track. Menu/results music is untouched (the option is
        // specifically "during gameplay").
        if (!options_.disable_game_music) audio_.start_music(1100 + stage);  // SOUNDLST 1100+n
    }
    renderer_->reset_match();
    sounds_.reset();
}

int GameApp::run_demo() {
    for (int t = 0; t < opts_.demo_ticks; ++t) {
        sim_.tick(demo_inputs(t));
        sounds_.on_tick(sim_.state());
        renderer_->on_events(sim_.state());
        renderer_->draw_frame(sim_.state());  // keeps walk-anim sampling in sync
    }
    SDL_Surface* shot = SDL_RenderReadPixels(sdl_renderer_.get(), nullptr);
    int rc = 1;
    if (shot) {
        rc = SDL_SaveBMP(shot, opts_.demo_out.string().c_str()) ? 0 : 1;
        SDL_DestroySurface(shot);
        std::printf("demo: %d ticks, alive %d, screenshot %s\n", opts_.demo_ticks,
                    sim::alive_count(sim_.state()), opts_.demo_out.string().c_str());
    }
    return rc;
}

AppInput GameApp::present_screen(const ScreenDef& def) {
    // Enter the screen (resets its clock/counter; music is NOT touched here —
    // the caller owns the continuous track, sub_42A088 only presents an image).
    screen_->enter(def, SDL_GetTicks());
    AppInput result = AppInput::Advance;
    bool waiting = true;
    while (waiting) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
            if (ev.type == SDL_EVENT_KEY_DOWN) {
                // Feed every key to the screen: sub_42A088 blips (SFX 20) on any
                // key and, for the accept keys (Enter/Space/Escape), plays the
                // accept sting (SFX 10) and finishes. Escape additionally routes
                // us "back"; Enter/Space "advance". The blip/sting come from the
                // Screen, so the music track is untouched — only the screen ends.
                screen_->on_key(ev.key.key);
                if (ev.key.key == SDLK_ESCAPE) {
                    result = AppInput::Back;
                    waiting = false;
                }
            }
        }
        std::uint64_t now = SDL_GetTicks();
        screen_->update(now);
        if (screen_->done()) waiting = false;

        audio_.update_music();
        SDL_SetRenderDrawColor(sdl_renderer_.get(), 0, 0, 0, 255);
        SDL_RenderClear(sdl_renderer_.get());
        screen_->draw(sdl_renderer_.get());
        SDL_RenderPresent(sdl_renderer_.get());
        SDL_Delay(2);
    }

    // No transition out: sub_42A088 CUTS between screens — it sets the palette
    // (sub_41522D, instant; the >>2 is the 8->6-bit VGA palette conversion, NOT
    // a fade loop), blits (sub_429FF1), and flips (sub_41043C). There is no wipe
    // on a waited screen (logos, title, results, .BM), so the next screen simply
    // replaces this one. (The menu->match select wipe in present_menu is a
    // separate, intentional use and is left alone.)
    return result;
}

AppInput GameApp::present_bm_screen(const std::string& bm_name) {
    // The `.BM` text-screen viewer (sub_41302D): MAINMENU.PCX as the persistent
    // backdrop (the original composites the scroll window over the menu page),
    // the parsed .BM text + inline images over it, keyboard line/page scroll,
    // and Enter/Escape to dismiss. No auto-scroll or dwell — it waits for input
    // exactly like the original.
    BmScreen bm(assets_, front_font_);
    bm.enter(bm_name);
    AppInput result = AppInput::Advance;
    while (!bm.done()) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
            if (ev.type == SDL_EVENT_KEY_DOWN) {
                if (ev.key.key == SDLK_ESCAPE) result = AppInput::Back;
                bm.on_key(ev.key.key);
            }
        }
        audio_.update_music();
        SDL_SetRenderDrawColor(sdl_renderer_.get(), 0, 0, 0, 255);
        SDL_RenderClear(sdl_renderer_.get());
        // Backdrop: keep the menu art behind the text panel.
        const Sprite& bg = assets_.frontend_pcx("MAINMENU");
        if (bg.tex) {
            SDL_FRect d{0, 0, static_cast<float>(bg.w), static_cast<float>(bg.h)};
            SDL_RenderTexture(sdl_renderer_.get(), bg.tex, nullptr, &d);
        }
        bm.draw(sdl_renderer_.get());
        SDL_RenderPresent(sdl_renderer_.get());
        SDL_Delay(2);
    }

    // No wipe out: the .BM viewer (sub_41302D) dismisses back to the menu by a
    // cut, like every sub_42A088-style screen — the menu is redrawn from scratch
    // on the next frame. No screen-to-screen transition here.
    return result;
}

AppInput GameApp::present_options_screen() {
    // The interactive Options screen (options_screen.hpp/.cpp): the full
    // §3 19-item list's LIVE subset, over a random GLUE<n> backdrop like
    // present_setup's documented convention (docs/re/setup-screens.md). F1
    // layers the original's OPTIONS.BM help text on top, same content the row
    // used to open exclusively. Music left untouched here — unlike
    // present_setup this screen is reached straight from the main menu (not
    // the Play handler sub_42A3F6), so there is no confirmed "inherits 1020"
    // citation; it plays on under whatever the menu already started (1010,
    // kMenuMusicId).
    OptionsScreen opt(assets_, front_font_);
    opt.enter(options_, pick_glue());
    AppInput result = AppInput::Advance;
    while (!opt.done()) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
            if (ev.type != SDL_EVENT_KEY_DOWN) continue;
            if (ev.key.key == SDLK_F1) {
                // Keep the .BM help reachable without leaving the interactive
                // screen: present it modally, then resume with the same
                // in-progress edits (present_bm_screen owns its own loop).
                AppInput help = present_bm_screen("OPTIONS");
                if (help == AppInput::Quit) return AppInput::Quit;
                continue;
            }
            if (ev.key.key == SDLK_ESCAPE) result = AppInput::Back;
            opt.on_key(ev.key.key, audio_);
            // "Define keyboard layouts" (row 15, §3): push the key-remap
            // sub-screen (§2) modally, exactly like the F1 help overlay
            // above, then resume the Options screen with its in-progress
            // edits untouched (present_keyremap_screen owns its own loop and
            // applies its own result to keyboard_/options_dirty_ directly).
            if (opt.open_keyremap()) present_keyremap_screen();
        }
        audio_.update_music();
        SDL_SetRenderDrawColor(sdl_renderer_.get(), 0, 0, 0, 255);
        SDL_RenderClear(sdl_renderer_.get());
        opt.draw(sdl_renderer_.get());
        SDL_RenderPresent(sdl_renderer_.get());
        SDL_Delay(2);
    }

    // Edit the in-memory snapshot ONLY on an actual change (task requirement
    // 3's write-on-exit semantics, §2's CONFIRMED "held in memory ... only
    // flushed ... when the application exits normally") — options.ini itself
    // is untouched here; flush_options() (run()'s tail) is the sole writer.
    if (opt.changed()) {
        // doc §2: "Cleared to -1 by: ... the Options-screen Gold Bomberman
        // toggle" — ANY edit of that row (on or off) forfeits a pending gold
        // player, checked before options_ is overwritten with the new
        // snapshot so this compares old vs new.
        if (opt.snapshot().goldman != options_.goldman) gold_player_ = -1;
        options_ = opt.snapshot();
        team_play_ = options_.team_play;
        conveyor_speed_index_ = options_.conveyor_speed_index;
        options_dirty_ = true;
    }
    return result;
}

void GameApp::present_keyremap_screen() {
    // The key-remap UI (docs/re/results-and-options.md §2, sub_407B9D): a
    // 2x6 scancode-capture grid drawn OVER whatever the caller already
    // painted this frame (present_options_screen's Options backdrop — §2
    // "no new backdrop call"). Draws its own frame here rather than sharing
    // the caller's SDL_RenderPresent, since it needs its own event pump to
    // capture raw scancodes without those keys also driving the Options
    // cursor underneath.
    KeyRemapScreen remap(assets_, front_font_);
    std::array<KeySet, kKeyboardSets> current{keyboard_.key_set(0), keyboard_.key_set(1)};
    remap.enter(current);
    while (!remap.done()) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) { remap.on_key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE, audio_); return; }
            if (ev.type != SDL_EVENT_KEY_DOWN) continue;
            remap.on_key(ev.key.key, ev.key.scancode, audio_);
        }
        audio_.update_music();
        SDL_SetRenderDrawColor(sdl_renderer_.get(), 0, 0, 0, 255);
        SDL_RenderClear(sdl_renderer_.get());
        // §2: "no new backdrop call — sub_407B9D draws directly over the
        // Options screen's own frame". This screen has its own event pump
        // (to capture raw scancodes without leaking into the Options cursor
        // underneath), so there is no single shared frame to draw "over" —
        // a plain dark panel is the simplest faithful stand-in, since
        // sub_407B9D's own drawing (the grid + header) is self-contained and
        // legible against any backdrop.
        SDL_SetRenderDrawColor(sdl_renderer_.get(), 20, 20, 30, 255);
        SDL_RenderClear(sdl_renderer_.get());
        remap.draw(sdl_renderer_.get());
        SDL_RenderPresent(sdl_renderer_.get());
        SDL_Delay(2);
    }
    // Apply live (KeyboardMapper reads collect_inputs() every match tick) and
    // mark dirty for the write-on-exit flush — never write options.ini here.
    const auto& edited = remap.edited();
    keyboard_.set_key_set(0, edited[0]);
    keyboard_.set_key_set(1, edited[1]);
    options_dirty_ = true;
}

int GameApp::round_winner() const {
    // A round win is exactly one SIDE of survivors with the clock still
    // running; a mutual wipe-out or a time-out is a draw. Mirrors sub_42A3F6,
    // which shows DRAW when the survivor query (sub_4219B0) returns none and
    // VICTORY<idx> for the lone survivor. Team-aware via sim::winning_side
    // (docs/re/ai.md TEAM follow-up, "our semantics"): teammates count as one
    // side, so a solo match (every team byte 0) is unchanged — the returned
    // slot is still the sole survivor, just resolved through the same-side
    // rule instead of a raw single-player check.
    const sim::State& s = sim_.state();
    if (s.ticks_left == 0) return -1;  // time up -> draw
    return sim::winning_side(s);
}

bool GameApp::is_team_mode() const {
    // Team mode (docs/re/setup-screens.md dword_464964): any two ACTIVE
    // players sharing a MatchConfig team means team rows/strings apply.
    // setup_team_[] is the frontend's per-slot +84 byte; team_play_ is the
    // game-type gate (start_match zeroes every slot's team when it is off,
    // so gating on team_play_ here keeps this in lockstep with the roster
    // actually built for the match in progress).
    if (!team_play_) return false;
    const sim::State& s = sim_.state();
    std::array<bool, sim::kMaxPlayers> team_seen{};
    for (int i = 0; i < sim::kMaxPlayers; ++i) {
        if (!s.players[i].present) continue;
        int t = setup_team_[i];
        if (t < 0 || t >= sim::kMaxPlayers) continue;
        if (team_seen[t]) return true;
        team_seen[t] = true;
    }
    return false;
}

int GameApp::match_clinch() const {
    // §1 v73: the default win-count clinch, or — in team mode with
    // win_by_kills set (§1's "in team mode with win_by_kills set, the clinch
    // instead compares the highest round-kill total against ... the target,
    // breaking ties by requiring a single unique leader") — the kill-count
    // clinch via results.hpp's win_by_kills_clinch(), so both call sites
    // (run_app's Results handler and present_scoreboard) agree on whether
    // the match is over.
    const sim::State& s = sim_.state();
    if (is_team_mode() && options_.win_by_kills) {
        std::array<bool, sim::kMaxPlayers> present{};
        for (int i = 0; i < sim::kMaxPlayers; ++i) present[i] = s.players[i].present;
        return win_by_kills_clinch(kill_count_, present, win_target_);
    }
    for (int i = 0; i < sim::kMaxPlayers; ++i) {
        if (!s.players[i].present) continue;
        if (win_count_[i] >= win_target_) return i;
    }
    return -1;
}

AppInput GameApp::run_boot_attract() {
    // The boot presentation (sub_42B060 @0x42B060) — STRAIGHT-LINE, no loop.
    // The original's exact order:
    //   sub_42741E(0x3E8)         ; start the boot music (1000) FIRST of all
    //   if (!sub_413D01()) {      ; skip-logos gate
    //       show IPLOGO           ; sub_42A088(aIplogo, 1), a waited screen
    //       show HSLOGO           ; sub_42A088(aHslogo, 1), a waited screen
    //   }
    //   sub_427BFB(2800)          ; the one-shot title intro sting, before TITLE
    //   show TITLE                ; sub_42A088(aTitle, 1), a waited screen
    //   return                    ; caller enters the menu (sub_42B9CE)
    // sub_42B060 does NOT loop: each screen advances on a key OR the getvalue(12)
    // = 7 s timeout (which synthesizes Enter, 13), and after the title it simply
    // returns so the caller drops into the menu. There is NO attract re-run of
    // the logos/title. So the port is linear: present each screen; a plain
    // Advance (key accept OR the 7 s dwell) walks to the next; the title's
    // Advance returns to run_app, which enters present_menu and switches to the
    // 1010 menu music. Only Back/Quit short-circuit out.
    //
    // The boot music is started ONCE here and plays CONTINUOUSLY across the
    // logos and the title — the logos are NOT silent. We must not (re)start the
    // track per screen: start_music replaces the current track (sub_427342 frees
    // it first), so a per-screen call would restart the boot music every time.
    audio_.start_music(kBootMusicId);

    AppInput ev = present_screen(logo_screen("IPLOGO"));
    if (ev == AppInput::Quit || ev == AppInput::Back) return ev;
    ev = present_screen(logo_screen("HSLOGO"));
    if (ev == AppInput::Quit || ev == AppInput::Back) return ev;

    // The one-shot title intro sting, fired right before the title image. In the
    // binary this is sub_427BFB(2800), which is NOT a fixed clip: it picks a
    // RANDOM member of the contiguous SOUNDLST run starting at 2800 (the "ATOMIC
    // BOMBERMAN!" intro group 2800..2810 — GEN8A/GEN8B/GEN8C/… ; the file's own
    // "2899 is the last intro" comment bounds it). So each boot can voice a
    // different take. play_random_in_range picks across exactly the loaded ids in
    // that span, matching the group pick; it is a one-shot SFX voice, so it plays
    // over the still-running boot track without disturbing it.
    audio_.play_random_in_range(kTitleStingLo, kTitleStingHi);

    // The title: a normal waited screen. present_screen returns Advance on a
    // real accept OR the 7 s timeout — both fall through to the menu here,
    // faithful to sub_42B060 synthesizing Enter on timeout and returning. Back
    // (Escape) exits the app; Quit closes the window.
    ev = present_screen(title_screen());
    if (ev == AppInput::Quit || ev == AppInput::Back) return ev;
    return AppInput::Advance;  // key OR 7 s timeout -> caller enters the menu
}

AppInput GameApp::present_menu() {
    // The navigable main menu (sub_42B9CE @0x42B9CE): MAINMENU.PCX as the
    // backdrop, an up/down highlight over the item rows (wrapping), Enter
    // selects, Escape quits. On entry the original plays sub_42741E(0x3F2) once
    // (v14-gated) — the CONFIRMED menu track 1010 (0x3F2 == MENU.RSS; the RE
    // brief's 0x3FC/1020 was the round/results path sub_42A3F6, not this). This
    // switches the looping music from the boot track to the menu track and keeps
    // it playing while in the menu. Returns the AppInput the highlighted row
    // resolves to, or Quit on window close.
    audio_.start_music(kMenuMusicId);
    std::uint64_t frame = 0;
    while (true) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
            if (ev.type != SDL_EVENT_KEY_DOWN) continue;
            switch (ev.key.key) {
                case SDLK_UP:
                case SDLK_W:
                    menu_index_ = (menu_index_ + kMenuCount - 1) % kMenuCount;
                    audio_.play(20);  // nav blip (SOUNDLST 20, sub_427961(20))
                    break;
                case SDLK_DOWN:
                case SDLK_S:
                    menu_index_ = (menu_index_ + 1) % kMenuCount;
                    audio_.play(20);
                    break;
                case SDLK_ESCAPE:
                    // Escape's full sound path in sub_42B9CE: the "any real key"
                    // line fires the nav blip (SFX 20) for EVERY key including 27,
                    // then Escape (27) reaches the accept branch `if (v8>=17 &&
                    // (v8<=17 || v8==27)) { sub_427961(10); v10=6; }` — the accept
                    // sting (SFX 10) — and selects row 6 = Quit. The Quit row then
                    // dispatches to sub_412987, which on confirm plays the exit
                    // sting group sub_427BFB(2600) (2600..2699, "go outside and
                    // play now!"). We have no confirm dialog, so Escape = blip +
                    // accept + the exit sting, then back out.
                    audio_.play(20);   // nav blip on the key (SFX 20)
                    audio_.play(10);   // accept sting selecting Quit (SFX 10)
                    audio_.play_random_in_range(kQuitStingLo, kQuitStingHi);  // exit sting 2600
                    return AppInput::Quit;  // Escape backs out of the top menu
                case SDLK_RETURN:
                case SDLK_KP_ENTER:
                case SDLK_SPACE: {
                    // sub_42B9CE plays the accept sting (SFX 10, sub_427961(10))
                    // for BOTH Enter (13) and Space (32) on EVERY row — there is
                    // no "inert row" concept in the original; each row 0..6 is a
                    // live dispatch. So the accept sound fires first, always.
                    audio_.play(10);  // accept sting (SOUNDLST 10, menuexit)
                    // A row we have not built yet (Editor/Roulette) still plays the
                    // accept sting to stay faithful, but has no leaf to jump to, so
                    // it simply stays put instead of dead-ending on an unbuilt
                    // screen. (Documented inert stub — the accept is real, the
                    // destination is a deferred effort.)
                    if (!kMenuItems[menu_index_].live) break;
                    AppInput sel = kMenuItems[menu_index_].action;
                    // Quit selected from the menu: sub_412987 plays the exit sting
                    // group sub_427BFB(2600) on confirm. Quitting does not wipe to a
                    // match, so play the exit sting and return Quit directly (no
                    // head-to-head transition, which is the menu->match effect).
                    if (sel == AppInput::Quit) {
                        audio_.play_random_in_range(kQuitStingLo, kQuitStingHi);
                        return AppInput::Quit;
                    }
                    // Otherwise wipe out, then hand the selection to the flow.
                    transition_->start(SDL_GetTicks());
                    while (transition_->active()) {
                        SDL_Event tev;
                        while (SDL_PollEvent(&tev))
                            if (tev.type == SDL_EVENT_QUIT) return AppInput::Quit;
                        std::uint64_t now = SDL_GetTicks();
                        transition_->update(now);
                        SDL_SetRenderDrawColor(sdl_renderer_.get(), 0, 0, 0, 255);
                        SDL_RenderClear(sdl_renderer_.get());
                        // keep the menu underneath the wipe
                        {
                            const Sprite& bg = assets_.frontend_pcx("MAINMENU");
                            if (bg.tex) {
                                SDL_FRect d{0, 0, static_cast<float>(bg.w),
                                            static_cast<float>(bg.h)};
                                SDL_RenderTexture(sdl_renderer_.get(), bg.tex, nullptr, &d);
                            }
                        }
                        transition_->draw(sdl_renderer_.get(), now);
                        SDL_RenderPresent(sdl_renderer_.get());
                        SDL_Delay(2);
                    }
                    return sel;
                }
                default:
                    break;
            }
        }

        audio_.update_music();
        SDL_SetRenderDrawColor(sdl_renderer_.get(), 0, 0, 0, 255);
        SDL_RenderClear(sdl_renderer_.get());
        // Backdrop.
        const Sprite& bg = assets_.frontend_pcx("MAINMENU");
        if (bg.tex) {
            SDL_FRect d{0, 0, static_cast<float>(bg.w), static_cast<float>(bg.h)};
            SDL_RenderTexture(sdl_renderer_.get(), bg.tex, nullptr, &d);
        }
        // Animated "bomb trigger green" cursor at the CONFIRMED anchor
        // (sub_42B9CE: x=getvalue(700), y=getvalue(701)+getvalue(702)*row; frame
        // = counter % statecnt). Anchor read live from VALUELST row 700's
        // columns, with this install's values as fallback. TRIGBOMB.ANI holds
        // the "bomb trigger green" sequence; if it is absent we draw a pulsing
        // highlight bar instead so the selection stays visible.
        {
            ++frame;
            int cx = static_cast<int>(values_.column_or(700, 0, kMenuCursorXFallback));
            int cy0 = static_cast<int>(values_.column_or(700, 1, kMenuCursorYFallback));
            int cstep = static_cast<int>(values_.column_or(700, 2, kMenuCursorStepFallback));
            int cy = cy0 + menu_index_ * cstep;
            Anim cur = resolve_sequence(assets_.trigbomb(-1), "bomb trigger green");
            if (!cur.steps.empty()) {
                const Sprite& sp = cur.steps[anim_step_index(frame, cur.steps.size())];
                if (sp.tex) {
                    SDL_FRect d{static_cast<float>(cx - sp.hx), static_cast<float>(cy - sp.hy),
                                static_cast<float>(sp.w), static_cast<float>(sp.h)};
                    SDL_RenderTexture(sdl_renderer_.get(), sp.tex, nullptr, &d);
                }
            } else {
                Uint8 pulse = static_cast<Uint8>(90 + 60 * ((frame / 8) % 2));
                SDL_FRect bar{static_cast<float>(cx), static_cast<float>(cy), 240.0f, 22.0f};
                SDL_SetRenderDrawBlendMode(sdl_renderer_.get(), SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(sdl_renderer_.get(), 255, 220, 60, pulse);
                SDL_RenderFillRect(sdl_renderer_.get(), &bar);
            }
        }
        SDL_RenderPresent(sdl_renderer_.get());
        SDL_Delay(2);
    }
}

void GameApp::reset_match_scores() {
    win_count_.fill(0);
    kill_count_.fill(0);
    // getvalue(310) "how many wins to win a match?" (first-column value, else
    // options.ini's num_to_win_match= if the VALUELST key is absent, else our
    // own fallback of 2 (task item 5 / §5: "num_to_win_match ... should seed
    // the frontend's win_target_ default"). The LEVEL & ROUNDS screen's WINS
    // row (present_map_select) still overrides on top of whichever default
    // wins here — this only affects the value shown before the player edits it.
    auto it = values_.values.find(310);
    if (it != values_.values.end())
        win_target_ = static_cast<int>(it->second);
    else
        win_target_ = num_to_win_match_.value_or(2);
    if (win_target_ < 1) win_target_ = 1;
}

// The between-round RESULTS cumulative-tally screen (sub_42A3F6 tail,
// docs/re/results-and-options.md §1): RESULTS.PCX backdrop, a header drawn
// once per round, one row per active player/team with a win-count + kill-
// count tally in per-player ink, and an outcome line reporting either "still
// need N" (match not yet clinched) or "wins the match" (clinched). Any key
// (or the 6 s idle dwell) dismisses it; the caller then starts the next round
// or, if the outcome line reports a clinch, the flow instead shows the
// VICTORY screen and never reaches this scoreboard (run_app's Results case).
AppInput GameApp::present_scoreboard() {
    const sim::State& s = sim_.state();

    // Header — getstring(30) "Game Winner was %s !", getvalue(780/781/783).
    const float hx = static_cast<float>(values_.column_or(780, 0, 150));
    const float hy = static_cast<float>(values_.column_or(780, 1, 140));
    // getvalue(783) is a colour index in the original (palette LUT); we have
    // no general colour-index -> RGB table outside the per-slot .RMP path, so
    // the header (not tied to any one player) draws in a fixed light ink —
    // documented simplification, the POSITION is exact.
    constexpr Uint8 kHeaderR = 255, kHeaderG = 255, kHeaderB = 255;

    // Per-player row — getstring(31) non-team "Player %u score: %u (kills: %d)"
    // / getstring(38) team "Team %u score: %u", getvalue(785/786/787/788).
    const float rx = static_cast<float>(values_.column_or(785, 0, 150));
    const float ry0 = static_cast<float>(values_.column_or(785, 1, 210));
    const float rystep = static_cast<float>(values_.column_or(785, 2, 20));

    // Outcome line — getvalue(800/801/803); string 120/121 "still need N" vs
    // 35/36 "wins the match" depending on team mode (§1's dword_46497C /
    // win_by_kills branch, wired below via options_.win_by_kills).
    const float ox = static_cast<float>(values_.column_or(800, 0, 150));
    const float oy = static_cast<float>(values_.column_or(800, 1, 94));

    // Team mode + the §1 v73 match-clinch check — factored into is_team_mode()
    // / match_clinch() (game_app.hpp) so run_app's Results handler (the
    // VICTORY-vs-scoreboard decision) and this render agree on the exact same
    // predicate, including the win_by_kills branch (docs/re/
    // results-and-options.md §3 row 5, now live).
    bool team_mode = is_team_mode();
    int clinched_player = match_clinch();

    // Header text (getstring(30), "Game Winner was %s !"), drawn once per
    // round on entry — the winner named is this ROUND's winner (round_winner()),
    // not necessarily the player who clinched the whole match.
    const int round_w = round_winner();
    const std::string header =
        fmt_s(assets_.getstring(30, "Game Winner was %s !"),
              round_w >= 0 ? "P" + std::to_string(round_w + 1) : std::string("-"));

    const std::uint64_t start = SDL_GetTicks();
    AppInput result = AppInput::Advance;
    bool waiting = true;
    while (waiting) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
            if (ev.type == SDL_EVENT_KEY_DOWN) {
                audio_.play(20);  // any-key blip then accept sting (sub_42A088)
                audio_.play(10);
                result = ev.key.key == SDLK_ESCAPE ? AppInput::Back : AppInput::Advance;
                waiting = false;
            }
        }
        if (SDL_GetTicks() - start >= kResultsDwellMs) waiting = false;  // attract auto-advance
        audio_.update_music();
        SDL_SetRenderDrawColor(sdl_renderer_.get(), 0, 0, 0, 255);
        SDL_RenderClear(sdl_renderer_.get());
        const Sprite& bg = assets_.frontend_pcx("RESULTS");
        if (bg.tex) {
            SDL_FRect dst{0.0f, 0.0f, static_cast<float>(bg.w), static_cast<float>(bg.h)};
            SDL_RenderTexture(sdl_renderer_.get(), bg.tex, nullptr, &dst);
        }

        // Header, drawn once per round (this screen IS one round's worth of
        // display, so we always draw it — sub_42A3F6's "!dword_464AEC" gate is
        // about not re-drawing across frames of the SAME round, which our
        // per-round call already satisfies).
        front_font_.draw(sdl_renderer_.get(), header, hx, hy, kHeaderR, kHeaderG, kHeaderB);

        // Per-player / per-team tally rows, each in that player's/team's ink
        // (AssetStore::slot_color, docs/re/player-colour.md). Team ink uses
        // the lowest-indexed active player on that team as a stand-in for the
        // original's fixed 2-colour sub_4141F8 helper (byte_49D0DA/byte_49D38F),
        // which we have not ported as an independent constant.
        if (team_mode) {
            std::array<bool, sim::kMaxPlayers> team_drawn{};
            int row = 0;
            for (int i = 0; i < sim::kMaxPlayers; ++i) {
                if (!s.players[i].present) continue;
                int t = setup_team_[i];
                if (t < 0 || t >= sim::kMaxPlayers || team_drawn[t]) continue;
                team_drawn[t] = true;
                std::string line = fmt_u(assets_.getstring(38, "Team %u score: %u"),
                                          static_cast<unsigned>(t + 1));
                // getstring(38) carries one %u (team number); splice the score
                // in after it manually since fmt_u only substitutes the first.
                line += " " + std::to_string(win_count_[i]);
                std::uint8_t c[3];
                assets_.slot_color(i, c);
                front_font_.draw(sdl_renderer_.get(), line, rx,
                                 ry0 + rystep * static_cast<float>(row), c[0], c[1], c[2]);
                ++row;
            }
        } else {
            int row = 0;
            for (int i = 0; i < sim::kMaxPlayers; ++i) {
                if (!s.players[i].present) continue;
                // getstring(31) "Player %u score: %u (kills: %d)" — two
                // independent counters (§1): win_count_ (match score) and
                // kill_count_ (round kills; TODO(§1) — see kill_count_'s
                // declaration in game_app.hpp for why this is 0 for now).
                std::string line = assets_.getstring(31, "Player %u score: %u (kills: %d)");
                line = fmt_u(line, i + 1);
                // fmt_u only substitutes the FIRST specifier; splice the
                // remaining two (score, kills) in by hand so the RE'd format
                // string still reads naturally with real fallback text.
                auto splice_next = [](std::string& f, int v) {
                    auto p = f.find('%');
                    if (p == std::string::npos) return;
                    std::size_t q = p + 1;
                    while (q < f.size() && f[q] != 'u' && f[q] != 'd' && f[q] != 'i') ++q;
                    if (q < f.size()) f = f.substr(0, p) + std::to_string(v) + f.substr(q + 1);
                };
                splice_next(line, win_count_[i]);
                splice_next(line, kill_count_[i]);
                std::uint8_t c[3];
                assets_.slot_color(i, c);
                front_font_.draw(sdl_renderer_.get(), line, rx,
                                 ry0 + rystep * static_cast<float>(row), c[0], c[1], c[2]);
                ++row;
            }
        }

        // Outcome line: "still need N" (not yet clinched, ink byte_49A624 —
        // approximated with a distinct amber "still playing" tone) vs "wins
        // the match" (clinched, ink byte_497F8F — approximated with a
        // distinct bright "match over" tone). Both approximations keep the
        // POSITION and STRING selection exact; only the literal RGB triples
        // are our own since the palette-index bytes are not yet ported.
        {
            std::string outcome;
            std::uint8_t oc[3];
            if (clinched_player < 0) {
                // "Still needs N" reports against whichever tally the active
                // clinch mode actually compares (§1): kill_count_ under
                // win_by_kills, win_count_ otherwise — keeps this line
                // consistent with what clinched_player was decided from.
                const auto& lead_tally =
                    (team_mode && options_.win_by_kills) ? kill_count_ : win_count_;
                int needed = win_target_ - *std::max_element(lead_tally.begin(), lead_tally.end());
                if (needed < 0) needed = 0;
                std::string fmt = team_mode ? assets_.getstring(121, "Team still needs %u to win")
                                            : assets_.getstring(120, "Still need %u to win");
                outcome = fmt_u(fmt, needed);
                oc[0] = 255; oc[1] = 200; oc[2] = 60;  // "still playing" amber
            } else {
                if (team_mode) {
                    std::string fmt = assets_.getstring(36, "TEAM %u WINS THE MATCH!");
                    outcome = fmt_u(fmt, static_cast<unsigned>(setup_team_[clinched_player] + 1));
                } else {
                    std::string fmt = assets_.getstring(35, "%s WINS THE MATCH!");
                    outcome = fmt_s(fmt, "P" + std::to_string(clinched_player + 1));
                }
                oc[0] = 255; oc[1] = 255; oc[2] = 255;  // "match over" bright white
            }
            front_font_.draw(sdl_renderer_.get(), outcome, ox, oy, oc[0], oc[1], oc[2]);
        }

        SDL_RenderPresent(sdl_renderer_.get());
        SDL_Delay(2);
    }
    return result;
}

// A random GLUE<n> backdrop (sub_4148E5 @0x4148E5): getvalue(16) = glue count,
// rand() % count, load GLUE<n>.PCX. Both pre-match screens share it. The pick is
// a presentation LCG (setup_lcg_), never State::rng.
std::string GameApp::pick_glue() {
    setup_lcg_ = setup_lcg_ * 1664525u + 1013904223u;
    int glue_n = static_cast<int>(values_.column_or(16, 0, 7));  // getvalue(16)
    if (glue_n < 1) glue_n = 1;
    return "GLUE" + std::to_string(static_cast<int>((setup_lcg_ >> 16) %
                                                    static_cast<unsigned>(glue_n)));
}

// The Goldman Roulette wheel (docs/re/goldman-roulette.md), sub_4034BC. Run
// from run_app's Menu/StartMatch handler, BEFORE present_setup — the exact
// gate order at the head of sub_410F81 (doc §2): !attract (this port has no
// attract-mode match yet, so that leg is always true) && goldman option on
// && local game (always true, no network play) && a gold player pending
// (gold_player_ >= 0 — doc's re-entry check re-derived from sub_4034BC's own
// internal guard, "with no pending gold player the function is a silent
// no-op"). The caller (run_app) is expected to have already checked
// options_.goldman && gold_player_ >= 0 before calling this, matching the
// doc's gate order; this function itself only runs the spin/award, plus the
// Esc-abort's gold_player_ clear (doc §2 "Cleared to -1 by: Esc on the
// wheel").
AppInput GameApp::present_goldman_wheel() {
    audio_.start_music(kWinMusicId);  // 1020 inherits from the Play handler (doc §7); no new music
    const int segment_steps = static_cast<int>(values_.column_or(1004, 0, kWheelSegmentSteps));
    const int cx = static_cast<int>(values_.column_or(1000, 0, 320));
    const int cy = static_cast<int>(values_.column_or(1000, 1, 240));
    const int rx = static_cast<int>(values_.column_or(1002, 0, 200));
    const int ry = static_cast<int>(values_.column_or(1002, 1, 150));
    const int freq_x = static_cast<int>(values_.column_or(1006, 0, 1));
    const int freq_y = static_cast<int>(values_.column_or(1006, 1, 1));

    GoldmanScreen wheel(assets_, seqs_, front_font_);
    // Advance a dedicated presentation LCG seed per spin (never State::rng) —
    // same shape as setup_lcg_/panic_lcg_ elsewhere in this file.
    goldman_lcg_ = goldman_lcg_ * 1664525u + 1013904223u;
    wheel.enter(goldman_lcg_, segment_steps);

    AppInput result = AppInput::Advance;
    while (!wheel.done()) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
            if (ev.type != SDL_EVENT_KEY_DOWN) continue;
            const SDL_Keycode k = ev.key.key;
            if (k == SDLK_F1) {
                // doc §5: F1 opens the help browser (local host); our port has
                // no separate ROULETTE.BM help text, so this reaches the same
                // OPTIONS.BM viewer the rest of the front end falls back to
                // rather than doing nothing on the key.
                AppInput help = present_bm_screen("OPTIONS");
                if (help == AppInput::Quit) return AppInput::Quit;
                continue;
            }
            wheel.on_key(k, audio_);
        }
        wheel.tick(audio_);
        audio_.update_music();
        SDL_SetRenderDrawColor(sdl_renderer_.get(), 0, 0, 0, 255);
        SDL_RenderClear(sdl_renderer_.get());
        wheel.draw(sdl_renderer_.get(), cx, cy, rx, ry, freq_x, freq_y);
        SDL_RenderPresent(sdl_renderer_.get());
        SDL_Delay(2);
    }

    if (wheel.aborted()) {
        // doc §2/§5: Esc aborts the WHOLE Play flow and forfeits the gold
        // player — the caller must skip present_setup/present_map_select and
        // return to the menu on AppInput::Back.
        gold_player_ = -1;
        return AppInput::Back;
    }
    // doc §4: the prize persists (gold_prize_) until the NEXT spin; start_match
    // re-applies it every round of the following match via born_with_extra.
    gold_prize_ = wheel.prize();
    return result;
}

// Cycle a slot's input type FORWARD one step (sub_421E80 @0x421E80): 0 off ->
// 1 computer -> 2 keyboard sub 0 -> 2 keyboard sub 1 -> 3 joystick per present
// stick -> back to 0. The pure wrap-order logic lives in cycle_slot_input_type
// (input.hpp, unit-tested); this just supplies the live connected-gamepad
// count so the cycle offers exactly the sticks in gamepads_ right now.
void GameApp::cycle_input_type(int slot) {
    cycle_slot_input_type(setup_type_[slot], setup_sub_[slot], gamepads_.count());
}

// The PLAYER INPUT TYPE SELECTION screen (sub_410F81 @0x410F81, VALUELST
// "PLAYER INPUT TYPE SELECTION" getvalue 705-713). Screen 1 of the pre-match
// flow reached from Play. A random GLUE<n> backdrop under the 1020 track
// (inherited from the Play handler sub_42A3F6 — this screen starts no music),
// header getstring(50), and the 10-slot list: each slot's input type via
// getstring(220..224), PREFIXED by getstring(51) (Player %u) and TINTED with the
// slot's intrinsic colour (VALUELST 200-247 = Tuning::color_rgb — there is no
// colour picker; colour is fixed per slot index, applied in-game via i.rmp), plus
// a team marker when the slot's team flag is set. Keys mirror the confirmed table
// (docs/re/setup-screens.md): Up/Down pick a slot, Right cycles its type
// (OFF->CPU->KBD0->KBD1->OFF), Left/'0' set it OFF, 'T' toggles its team, Enter
// goes on to the LEVEL screen, Escape cancels to the menu. Presentation only.
AppInput GameApp::present_setup() {
    audio_.start_music(kWinMusicId);  // 1020, the Play-handler track (sub_42A3F6)
    const std::string glue = pick_glue();
    // Layout (VALUELST X,Y,YS,colour -> consecutive getvalue ids): header 705,
    // list 710, joystick pane heading 715, joystick pane list 720.
    const float hx = static_cast<float>(values_.column_or(705, 0, 40));
    const float hy = static_cast<float>(values_.column_or(705, 1, 140));
    const float lx = static_cast<float>(values_.column_or(710, 0, 70));
    const float ly = static_cast<float>(values_.column_or(710, 1, 170));
    const float lys = static_cast<float>(values_.column_or(710, 2, 24));
    const float jhx = static_cast<float>(values_.column_or(715, 0, 300));
    const float jhy = static_cast<float>(values_.column_or(715, 1, 140));
    const float jlx = static_cast<float>(values_.column_or(720, 0, 320));
    const float jly = static_cast<float>(values_.column_or(720, 1, 170));
    const float jlys = static_cast<float>(values_.column_or(720, 2, 24));

    int cursor = 0;
    bool waiting = true;
    while (waiting) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
            // Hotplug (sub_429628 "joystick present" is polled live in the
            // original; SDL3 gives us an event instead): rescan so the pane
            // and the Right-cycle's joystick count reflect what's plugged in
            // right now, without needing a restart.
            if (ev.type == SDL_EVENT_GAMEPAD_ADDED || ev.type == SDL_EVENT_GAMEPAD_REMOVED) {
                gamepads_.refresh();
                continue;
            }
            if (ev.type != SDL_EVENT_KEY_DOWN) continue;
            const SDL_Keycode k = ev.key.key;
            if (k == SDLK_ESCAPE) {
                audio_.play(20);
                audio_.play(10);
                // doc §2: "Cleared to -1 by: ... Esc on the player-setup
                // screen" — cancelling the whole Play flow here also forfeits
                // any gold player pending from an earlier match.
                gold_player_ = -1;
                return AppInput::Back;
            }
            // Enter (< 0x20 branch in sub_410F81) leaves this screen and proceeds
            // to match init / the LEVEL screen.
            if (k == SDLK_RETURN || k == SDLK_KP_ENTER) { audio_.play(10); waiting = false; break; }
            audio_.play(20);  // any real key blips first (sub_427961(20))
            if (k == SDLK_UP) cursor = (cursor + 9) % 10;               // 328
            else if (k == SDLK_DOWN) cursor = (cursor + 1) % 10;        // 336
            else if (k == SDLK_RIGHT) cycle_input_type(cursor);         // 333 sub_421E80
            else if (k == SDLK_LEFT || k == SDLK_0) {                   // 331 / '0'
                setup_type_[cursor] = 0;                                // sub_421E33(i,0,0)
                setup_sub_[cursor] = 0;
            } else if (k == SDLK_T) {                                   // 'T' team toggle (+84)
                setup_team_[cursor] = setup_team_[cursor] ? 0 : 1;
            }
        }
        audio_.update_music();
        SDL_SetRenderDrawColor(sdl_renderer_.get(), 0, 0, 0, 255);
        SDL_RenderClear(sdl_renderer_.get());
        const Sprite& bg = assets_.frontend_pcx(glue);
        if (bg.tex) {
            SDL_FRect dst{0.0f, 0.0f, static_cast<float>(bg.w), static_cast<float>(bg.h)};
            SDL_RenderTexture(sdl_renderer_.get(), bg.tex, nullptr, &dst);
        }
        front_font_.draw(sdl_renderer_.get(), assets_.getstring(50, "PLAYER SETUP"), hx, hy, 255,
                         255, 255);
        for (int i = 0; i < 10; ++i) {
            const std::string label = fmt_u(assets_.getstring(51, "Player %u - "), i + 1);
            const int t = setup_type_[i];
            std::string type;
            switch (t) {
                case 1: type = assets_.getstring(221, "COMPUTER"); break;
                case 2: type = fmt_u(assets_.getstring(222, "KEYBOARD %u"), setup_sub_[i]); break;
                case 3: type = fmt_u(assets_.getstring(223, "JOYSTICK %u"), setup_sub_[i]); break;
                case 4: type = assets_.getstring(224, "OTHER"); break;
                default: type = assets_.getstring(220, "OFF"); break;
            }
            std::string line = label + type;
            if (setup_team_[i]) line += "  " + fmt_u(assets_.getstring(230, "[T%u]"), setup_team_[i]);
            // Tint the label with the slot's authentic on-screen colour: the
            // original inks each slot line via sub_41672F(i), which quantises the
            // slot's stored RGB (the .RMP tail) to 5 bits/channel and looks it up
            // in the palette. AssetStore::slot_color reproduces that (truecolour
            // expand5 of the quantised channels) from the loaded .RMP tail, so a
            // slot reads as its real in-game colour. The selected row is nudged
            // brighter so the cursor is legible over any colour (ours; the
            // original moves a separate cursor glyph, sub_413BD6).
            std::uint8_t sc[3];
            assets_.slot_color(i, sc);
            const bool sel = i == cursor;
            auto boost = [sel](std::uint8_t v) {
                int x = v + (sel ? 70 : 0);
                return static_cast<Uint8>(x > 255 ? 255 : x);
            };
            front_font_.draw(sdl_renderer_.get(), line, lx, ly + lys * static_cast<float>(i),
                             boost(sc[0]), boost(sc[1]), boost(sc[2]));
        }
        // Joystick pane (getvalue 715/720): heading msg 40, then one line per
        // detected stick (msg 41 + index, from GamepadMapper::name) or, if none
        // are connected, the single "none" line (msg 42) — sub_429628(i)'s
        // present/absent branch collapsed to "any present at all" since we
        // enumerate rather than poll per-index.
        front_font_.draw(sdl_renderer_.get(), assets_.getstring(40, "JOYSTICKS"), jhx, jhy, 255,
                         255, 255);
        const int joy_count = gamepads_.count();
        if (joy_count == 0) {
            front_font_.draw(sdl_renderer_.get(), assets_.getstring(42, "none"), jlx, jly, 150,
                             150, 150);
        } else {
            for (int j = 0; j < joy_count; ++j) {
                std::string jline =
                    fmt_u(assets_.getstring(41, "JOYSTICK %u"), j) + " " + gamepads_.name(j);
                front_font_.draw(sdl_renderer_.get(), jline, jlx,
                                 jly + jlys * static_cast<float>(j), 200, 200, 200);
            }
        }
        front_font_.draw(sdl_renderer_.get(),
                         "UP/DN PICK  RIGHT CYCLE  0 OFF  T TEAM  ENTER NEXT", lx,
                         ly + lys * 11.0f, 150, 150, 150);
        SDL_RenderPresent(sdl_renderer_.get());
        SDL_Delay(2);
    }
    return AppInput::Advance;
}

// The 11 built-in level names (VALUELST 450-460 / getvalue(150+n)); RANDOM is
// getstring(149). These GENERIC fallbacks are ours — the real names live in the
// user's MESSAGES.TXT and load at runtime via getstring, never committed.
const char* GameApp::level_fallback(int idx) {
    static const char* kNames[] = {
        "NEW TRADITIONALIST",  "CLASSIC GREEN ACRES", "HOCKEY RINK",   "ANCIENT EGYPT",
        "COAL MINE",           "BEACH",               "ALIENS",        "HAUNTED HOUSE",
        "UNDER THE OCEAN",     "DEEP FOREST GREEN",   "INNER CITY TRASH"};
    return (idx >= 0 && idx < static_cast<int>(std::size(kNames))) ? kNames[idx] : "LEVEL";
}

// The LEVEL & ROUNDS screen (sub_406DDE @0x406DDE, the VALUELST "OPTIONS SCREEN"
// getvalue 730/735). Screen 2 of the pre-match flow. A 2-row list on a random
// GLUE<n> backdrop (1020 track inherited): row 0 = LEVEL (-1 RANDOM else 0..10 of
// getvalue(35)=11 built-ins, named getstring(150+n) / getstring(149)); row 1 =
// NUMBER OF WINS (1..100). Left/Right cycle the highlighted row's value (level
// wraps [-1 .. 10]; wins +-1 or +-5 on PgUp/PgDn), Up/Down switch rows. Enter
// commits the level (selected_level_ -> dword_464998) and win target (win_target_
// -> dword_464A7C) and starts; Escape backs to the player screen. Presentation
// only — the committed level drives start_match's stage choice.
AppInput GameApp::present_map_select() {
    const std::string glue = pick_glue();
    const int level_count = static_cast<int>(values_.column_or(35, 0, 11));  // getvalue(35)
    const float lx = static_cast<float>(values_.column_or(735, 0, 55));
    const float ly = static_cast<float>(values_.column_or(735, 1, 170));
    const float lys = static_cast<float>(values_.column_or(735, 2, 24));

    int row = 0;  // 0 = level, 1 = wins (v34 = 2 rows in sub_406DDE)
    bool waiting = true;
    while (waiting) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
            if (ev.type != SDL_EVENT_KEY_DOWN) continue;
            const SDL_Keycode k = ev.key.key;
            if (k == SDLK_ESCAPE) { audio_.play(20); return AppInput::Back; }  // back to setup
            if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {  // commit + start (LABEL_101)
                audio_.play(10);
                waiting = false;
                break;
            }
            audio_.play(20);
            if (k == SDLK_UP) row = (row + 1) % 2;
            else if (k == SDLK_DOWN) row = (row + 1) % 2;
            else if (row == 0 && k == SDLK_LEFT) {         // --level, wrap below -1
                if (--selected_level_ < -1) selected_level_ = level_count - 1;
            } else if (row == 0 && k == SDLK_RIGHT) {      // ++level, wrap above count-1 to -1
                if (++selected_level_ >= level_count) selected_level_ = -1;
            } else if (row == 1 && (k == SDLK_LEFT)) {     // wins -1
                if (--win_target_ < 1) win_target_ = 1;
            } else if (row == 1 && (k == SDLK_RIGHT)) {    // wins +1
                if (++win_target_ > 100) win_target_ = 100;
            } else if (row == 1 && k == SDLK_PAGEUP) {     // wins +5 (sub_406DDE 0x174)
                if ((win_target_ += 5) > 100) win_target_ = 100;
            } else if (row == 1 && k == SDLK_PAGEDOWN) {   // wins -5 (371)
                if ((win_target_ -= 5) < 1) win_target_ = 1;
            }
        }
        audio_.update_music();
        SDL_SetRenderDrawColor(sdl_renderer_.get(), 0, 0, 0, 255);
        SDL_RenderClear(sdl_renderer_.get());
        const Sprite& bg = assets_.frontend_pcx(glue);
        if (bg.tex) {
            SDL_FRect dst{0.0f, 0.0f, static_cast<float>(bg.w), static_cast<float>(bg.h)};
            SDL_RenderTexture(sdl_renderer_.get(), bg.tex, nullptr, &dst);
        }
        // Row 0: LEVEL. getstring(210) is the level-line format (%s = name);
        // name = getstring(150+n) for a specific level, getstring(149) for RANDOM.
        const std::string level_name = selected_level_ < 0
            ? assets_.getstring(149, "RANDOM")
            : assets_.getstring(150 + selected_level_, level_fallback(selected_level_));
        const std::string level_line = fmt_s(assets_.getstring(210, "LEVEL: %s"), level_name);
        front_font_.draw(sdl_renderer_.get(), level_line, lx, ly, row == 0 ? 255 : 180,
                         row == 0 ? 220 : 180, row == 0 ? 60 : 180);
        // Row 1: NUMBER OF WINS. getstring(211) is the rounds-line format (%u).
        const std::string wins_line = fmt_u(assets_.getstring(211, "WINS TO WIN: %u"), win_target_);
        front_font_.draw(sdl_renderer_.get(), wins_line, lx, ly + lys, row == 1 ? 255 : 180,
                         row == 1 ? 220 : 180, row == 1 ? 60 : 180);
        front_font_.draw(sdl_renderer_.get(), "UP/DN ROW  LEFT/RIGHT CHANGE  ENTER START", lx,
                         ly + lys * 3.0f, 150, 150, 150);
        SDL_RenderPresent(sdl_renderer_.get());
        SDL_Delay(2);
    }
    return AppInput::Advance;
}

// Assembles one tick's TickInputs across every roster slot (docs/re/setup-
// screens.md: OFF/COMPUTER slots contribute neutral input — AISystem drives
// COMPUTER from Player::ai, OFF is simply absent — KEYBOARD slots read the
// shared KeyboardMapper's player 0/1 half by sub-index, JOYSTICK slots read
// GamepadMapper::read(sub). A disconnected pad (index now out of range, or
// still indexed but closed) falls through GamepadMapper::read's own
// out-of-range/null guard to neutral input, so a mid-match unplug degrades
// gracefully instead of crashing or freezing that slot's last input.
sim::TickInputs GameApp::collect_inputs() const {
    sim::TickInputs in;
    const sim::TickInputs kb = keyboard_.read();
    for (int i = 0; i < sim::kMaxPlayers; ++i) {
        switch (static_cast<SlotInputType>(setup_type_[i])) {
            case SlotInputType::Keyboard:
                in.players[i] = kb.players[setup_sub_[i] == 0 ? 0 : 1];
                break;
            case SlotInputType::Joystick:
                in.players[i] = gamepads_.read(setup_sub_[i]);
                break;
            default:
                break;  // Off/Computer/Other: neutral — AI or absence owns the slot
        }
    }
    return in;
}

AppInput GameApp::run_match() {
    start_match(next_seed_++);
    const std::uint64_t tick_ms = 1000 / sim::kTicksPerSecond;
    std::uint64_t last = SDL_GetTicks();
    std::uint64_t acc = 0;
    int over_ticks = -1;
    while (true) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
            if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE)
                return AppInput::MatchOver;  // Esc bails out of the match to the menu
            // A pad unplugged/plugged mid-match: rescan so a disconnect drops
            // that slot to neutral input (via collect_inputs' range check)
            // rather than leaving it wedged, and a reconnect resumes control at
            // its old index without needing a trip back to the setup screen.
            if (ev.type == SDL_EVENT_GAMEPAD_ADDED || ev.type == SDL_EVENT_GAMEPAD_REMOVED)
                gamepads_.refresh();
        }

        std::uint64_t now = SDL_GetTicks();
        acc += now - last;
        last = now;
        while (acc >= tick_ms) {
            acc -= tick_ms;
            sim_.tick(collect_inputs());
            sounds_.on_tick(sim_.state());
            renderer_->on_events(sim_.state());
            // §1's kill tally (sub_421B0F): a GameApp-side pass over this
            // tick's events, separate from the renderer's own on_events walk
            // (renderer_ never mutates GameApp state — CLAUDE.md's libs/game
            // boundary). Cumulative for the whole match (see kill_count_'s
            // doc comment); reset only in reset_match_scores().
            tally_kills(sim_.state().events, kill_count_);

            const sim::State& s = sim_.state();
            // Team-aware round-over: "one SIDE left", not "one player left"
            // (docs/re/ai.md TEAM follow-up). sides_remaining() degenerates to
            // alive_count() when every team byte is 0 (the default), so a solo
            // match's timing is unchanged.
            if (over_ticks < 0 && (sim::sides_remaining(s) <= 1 || s.ticks_left == 0)) {
                // Linger a few seconds on the final frame, then hand back to the
                // flow so the Results screen can come up.
                over_ticks = 3 * sim::kTicksPerSecond;
                if (s.ticks_left == 0) {
                    std::printf("time up — draw!\n");
                } else {
                    // The "we have a winner" voice group (2000) fires under the
                    // RESULTS scoreboard itself once v73 is computed (§1), NOT
                    // here during the match's own end-of-round linger — moved to
                    // run_app's Results handler (present_scoreboard/victory_screen
                    // call site) so it plays under the right screen.
                    for (int i = 0; i < sim::kMaxPlayers; ++i)
                        if (s.players[i].present && s.players[i].alive)
                            std::printf("player %d wins!\n", i);
                }
            }
            if (over_ticks > 0 && --over_ticks == 0) return AppInput::MatchOver;
        }

        audio_.update_music();
        renderer_->draw_frame(sim_.state());
        SDL_RenderPresent(sdl_renderer_.get());
        SDL_Delay(2);
    }
}

int GameApp::run_app() {
    // The front-end shell: the pure flow graph (app_flow.hpp) decides where each
    // event leads; these per-state handlers are the SDL side. A dev fast-path
    // can start in Match and return to the menu when it ends.
    //
    // Two states own their own multi-screen loop rather than a single
    // present_screen: Boot runs the linear IPLOGO->HSLOGO->TITLE chain
    // (run_boot_attract; no attract re-run — the title's timeout falls through
    // to the menu), and Menu runs the navigable menu (present_menu), which
    // resolves the highlighted row into a concrete AppInput. Everything else is
    // one asset-driven Screen.
    AppState state = opts_.boot_match ? AppState::Match : AppState::Boot;
    while (!is_terminal(state)) {
        AppInput ev = AppInput::Advance;
        switch (state) {
            case AppState::Boot:
                // The whole LINEAR boot presentation (IPLOGO -> HSLOGO -> TITLE,
                // sub_42B060). Returns Advance on the title's key/7 s timeout (no
                // attract re-run — the timeout falls through to the menu) or
                // Back/Quit; next() forwards Advance into Logo, and the Logo/Title
                // cases below are pass-throughs on to the menu (this one handler
                // already drew the whole chain).
                ev = run_boot_attract();
                break;
            case AppState::Logo:
            case AppState::Title:
                // Pass-throughs: run_boot_attract already presented the logos
                // and title, so nothing is drawn here — just walk the flow graph
                // on to the menu (keeps next() authoritative over the boot hops).
                ev = AppInput::Advance;
                break;
            case AppState::Menu: {
                ev = present_menu();  // navigable; resolves the selected row
                if (ev == AppInput::StartMatch) {
                    // The pre-match flow reached from Play (sub_42A3F6): the PLAYER
                    // INPUT screen (sub_410F81) then the LEVEL & ROUNDS screen
                    // (sub_406DDE), then the match. Escape backs up ONE step at
                    // each screen: cancel on the level screen -> back to the player
                    // screen; cancel on the player screen -> back to the menu.
                    // reset_match_scores() clears the tally; the level screen owns
                    // the win target so we reset FIRST, then let the level screen
                    // adjust win_target_.
                    reset_match_scores();
                    // The Goldman wheel (docs/re/goldman-roulette.md §2): at
                    // the head of every Play entry, before present_setup —
                    // gated on not-attract (always true here, no attract
                    // match yet), the goldman option, local-only (always
                    // true), and a gold player actually pending from a
                    // previous match's rounds. An Esc abort forfeits the
                    // whole Play flow (skip straight back to the menu,
                    // mirroring sub_410F81's post-call `if (dword_464A68)
                    // return`).
                    if (options_.goldman && gold_player_ >= 0) {
                        AppInput wheelResult = present_goldman_wheel();
                        if (wheelResult == AppInput::Quit) return 0;
                        if (wheelResult == AppInput::Back) { ev = AppInput::Advance; break; }
                    }
                    bool started = false;
                    while (!started) {
                        AppInput setup = present_setup();
                        if (setup == AppInput::Quit) return 0;
                        if (setup == AppInput::Back) { ev = AppInput::Advance; break; }
                        // Player screen accepted -> the LEVEL screen.
                        AppInput lvl = present_map_select();
                        if (lvl == AppInput::Quit) return 0;
                        if (lvl == AppInput::Back) continue;  // back to the player screen
                        started = true;  // both screens confirmed -> start the match
                    }
                    if (!started) ev = AppInput::Advance;  // cancelled all the way out
                }
                break;
            }
            case AppState::Match:
                ev = run_match();
                break;
            case AppState::Results: {
                // The three-tier sub_42A3F6 results tail (docs/re/frontend-flow.md
                // "results flow"): a round win bumps that player's tally; the
                // match is decided (VICTORY<n>) once the tally reaches
                // win_target_ (the LEVEL & ROUNDS screen's WINS row); otherwise
                // a survivor shows the RESULTS cumulative scoreboard, a draw
                // (round_winner() folds no-survivor and time-up) shows DRAW —
                // and both replay the next round. The winner voice group (2000)
                // fires here, as soon as v73 (the round winner / match-over
                // check) is computed (§1) — i.e. under BOTH the scoreboard and
                // the VICTORY screen, not only the latter. On a DRAW we fire the
                // tie-game sting instead (sub_427BFB(1700)) — a one-shot group
                // pick, not looped music.
                //
                // Results MUSIC (sub_42A3F6): the handler starts the looping "win"
                // track sub_42741E(0x3FC)=1020 at entry (under the VICTORY screen),
                // and the DRAW branch switches to sub_42741E(0x46A)=1130 ("draw")
                // before DRAW.PCX. start_music replaces the leftover stage/menu
                // track, so the results screen carries its own backdrop music —
                // previously our DRAW/VICTORY screens played under whatever music
                // was left running, a silent-vs-original gap now closed.
                int w = round_winner();
                if (w >= 0) ++win_count_[w];  // tally the round win
                // The match-over check (§1 v73): the default win-count target,
                // or (team mode + win_by_kills) the kill-count clinch —
                // match_clinch() (game_app.hpp) so this agrees with
                // present_scoreboard's own clinch/outcome-line render. The
                // clinching slot can differ from the round winner `w` under
                // win_by_kills (a team's kill leader need not be this round's
                // sole survivor), so VICTORY names whoever match_clinch()
                // returns, not `w`.
                int clinched = w >= 0 ? match_clinch() : -1;
                bool match_over = clinched >= 0;
                // Gold player assignment (docs/re/goldman-roulette.md §2,
                // pseudo.c 30004-30022, LABEL_102): sub_42A3F6 only reaches
                // the RESULTS tier (and its unconditional dword_46492C
                // write) when sub_4219B0(...) != -1, i.e. a ROUND SURVIVOR
                // exists (`w >= 0` below) — a DRAW falls through to the
                // separate DRAW.PCX branch instead and never touches
                // dword_46492C at all, so a pending gold player survives a
                // draw round unchanged. When RESULTS does run, v73 (== our
                // `clinched` above) is the MATCH-CLINCH winner, never the
                // per-round winner `w` — so the gold player only changes
                // when a match is actually decided, and reverts to "none
                // pending" on every other clinch-less RESULTS pass (v73's
                // own -1 reset at the top of every RESULTS pass). Team mode
                // stores the raw team id (setup_team_[]), matching the wheel
                // award consumer in build_match_config and
                // present_scoreboard's own clinched_player -> setup_team_[]
                // lookup.
                if (w >= 0) {
                    gold_player_ =
                        assign_gold_player(options_.goldman, is_team_mode(), clinched, setup_team_);
                }
                if (match_over) {
                    // MATCH win: the target was reached -> VICTORY, then
                    // back to the menu (next(Results, Advance) = Menu).
                    audio_.start_music(kWinMusicId);  // 1020 win track under VICTORY
                    audio_.play_random_in_range(2000, 2299);  // "we have a winner", under VICTORY
                    ev = present_screen(victory_screen(clinched));
                } else if (w >= 0) {
                    // Round win, match not over: show the running scores. The
                    // winner sting plays under THIS screen too (§1) — the
                    // original fires it as soon as the round decision is known,
                    // regardless of whether that decision also clinches the match.
                    audio_.start_music(kWinMusicId);
                    audio_.play_random_in_range(2000, 2299);  // "we have a winner", under RESULTS
                    ev = present_scoreboard();
                } else {
                    // DRAW (no survivor / time-up): nobody scores; replay a round.
                    audio_.start_music(kDrawMusicId);  // 1130 draw track under DRAW
                    audio_.play_random_in_range(kDrawStingLo, kDrawStingHi);
                    ev = present_screen(draw_screen());
                }
                // Fold the screen's dismissal into the flow-graph event: an
                // undecided round's Advance becomes RoundContinue, so
                // next(Results, RoundContinue) loops straight back into Match
                // (sub_42A3F6's round loop) — the next round reuses the SAME
                // roster/level/win-target members the pre-match screens set;
                // only run_match's start_match reruns (fresh sim, next seed).
                // Back (Escape) abandons the match to the menu; a decided
                // match's Advance ends it there too. next() stays the single
                // authority over the state walk — no side-channel override.
                if (!match_over && ev == AppInput::Advance) ev = AppInput::RoundContinue;
                break;
            }
            // The .BM-backed leaves render their real help/credits text
            // (sub_41302D via BmScreen). Network/Controllers still show the HELP
            // overlays for those menu items (the controller-remap UI remains a
            // documented TODO); Credits shows CREDITS.BM with its inline
            // CREDBAR/JERM/KURT images. Options is now the fully-interactive
            // Team Play / Conveyor Speed screen (present_options_screen); its
            // own F1 key still reaches the original OPTIONS.BM help text.
            case AppState::Options:
                ev = present_options_screen();
                break;
            case AppState::Controllers:
                ev = present_bm_screen("INPUT");
                break;
            case AppState::Network:
                ev = present_bm_screen("NETWORK");
                break;
            case AppState::Credits:
                ev = present_bm_screen("CREDITS");
                break;
            case AppState::Quit:
                break;
        }
        state = next(state, ev);
    }
    return 0;
}

void GameApp::flush_options() {
    // Write-on-exit (docs/re/results-and-options.md §2 "Persistence —
    // CONFIRMED via an exit-time write-back": sub_405DE3, the writer, is only
    // ever reached through sub_410EBF's atexit-style hook on a NORMAL app
    // exit — never per-edit). Guarded so a run that never touched an Options
    // row, or a run that never resolved a game_dir (init() already bailed),
    // does nothing.
    if (!options_dirty_ || options_path_.empty()) return;
    assets::Options to_write;
    to_write.team_play = options_.team_play;
    to_write.random_start = options_.random_start;
    to_write.conveyor_speed = options_.conveyor_speed_index;
    to_write.stomped_bombs_detonate = options_.stomped_bombs_detonate;
    to_write.win_by_kills = options_.win_by_kills;
    to_write.goldman = options_.goldman;
    to_write.enclosement_depth = options_.enclosement_depth;
    to_write.playtime = options_.playtime_seconds;
    to_write.assign_keyboards = std::nullopt;  // row omitted — never edited by this port
    to_write.diseases_destroyable = options_.diseases_destroyable;
    to_write.disable_game_music = options_.disable_game_music;
    // keydef=: always write the live KeyboardMapper bindings (both sets, all
    // 6 UI-exposed actions) so a rebind through the remap screen survives a
    // restart. Slots 6-9 per set (no in-game UI, §2) are left at -1/absent
    // here — save_options skips a -1 scancode, so any pre-existing keydef=
    // line for those slots (from a hand-edit or a future feature) is left
    // untouched by the read-modify-write rather than being clobbered blank.
    assets::KeyDef kd;
    for (int set = 0; set < assets::KeyDef::kSets; ++set) {
        const KeySet& ks = keyboard_.key_set(set);
        for (int action = 0; action < kKeyActionCount; ++action)
            kd.scancode[set][action] = ks.scancode[action];
    }
    to_write.keydef = kd;
    try {
        assets::save_options(options_path_, to_write);
        options_dirty_ = false;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "options.ini save failed: %s\n", e.what());
    }
}

int GameApp::run() {
    if (const char* env = std::getenv("BOMBER_BOOT_MATCH"); env && *env) opts_.boot_match = true;
    if (!init()) return opts_.game_dir.empty() ? 2 : 1;
    if (opts_.demo) {
        start_match(0xB0BB1E5);
        int rc = run_demo();
        flush_options();
        return rc;
    }
    int rc = run_app();
    flush_options();
    return rc;
}

}  // namespace bomber::game
