#include "bomber/game/game_app.hpp"

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
        // The install-root options.ini (sub_406238) carries the Conveyor Speed
        // game option ("conveyor_speed="). Absent key/file ⇒ empty optional ⇒
        // the sim keeps the binary default (1=medium). This install sets 2=high.
        conveyor_speed_index_ = assets::load_options(game / "options.ini").conveyor_speed;
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

    if (!opts_.demo && !audio_.init(game))
        std::fprintf(stderr, "audio unavailable, continuing silent\n");

    base_tuning_ = match::build_match_config(scheme_, 2, 0, &values_).tuning;
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
    // "we have a winner" voice group (2000) is played by run_match already.
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
//   1 setup A        -> OpenOptions  (help overlay live; interactive UI = TODO)
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
    sim::MatchConfig cfg = match::build_match_config(scheme_, 2, seed, &values_);
    // DEV hook (temporary): make player 1 a computer opponent so the AI
    // (ADR-0005) is visible in-game before the match-setup UI exists. Player 0
    // stays keyboard-driven. Remove when the setup screen can pick AI slots.
    cfg.ai[1] = true;
    // Override the Conveyor Speed index from options.ini if present (this
    // install = 2 high); otherwise Tuning keeps the confirmed default (1
    // medium). conveyor_speed() clamps to [0, count-1], so a raw index is safe.
    if (conveyor_speed_index_) cfg.tuning.conveyor_speed_index = *conveyor_speed_index_;
    int stage = match::pick_stage(base_tuning_, seed);
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
        audio_.start_music(1100 + stage);  // SOUNDLST: stage music = 1100 + n
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

int GameApp::round_winner() const {
    // A round win is exactly one survivor with the clock still running; a
    // mutual wipe-out or a time-out is a draw. Mirrors sub_42A3F6, which shows
    // DRAW when the survivor query (sub_4219B0) returns none and VICTORY<idx>
    // for the lone survivor.
    const sim::State& s = sim_.state();
    if (s.ticks_left == 0) return -1;  // time up -> draw
    int winner = -1;
    for (int i = 0; i < sim::kMaxPlayers; ++i) {
        if (s.players[i].present && s.players[i].alive) {
            if (winner != -1) return -1;  // more than one alive -> not decided as a win
            winner = i;
        }
    }
    return winner;  // -1 if nobody is alive (mutual wipe-out -> draw)
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
        }

        std::uint64_t now = SDL_GetTicks();
        acc += now - last;
        last = now;
        while (acc >= tick_ms) {
            acc -= tick_ms;
            sim_.tick(keyboard_.read());
            sounds_.on_tick(sim_.state());
            renderer_->on_events(sim_.state());

            const sim::State& s = sim_.state();
            if (over_ticks < 0 && (sim::alive_count(s) <= 1 || s.ticks_left == 0)) {
                // Linger a few seconds on the final frame, then hand back to the
                // flow so the Results screen can come up.
                over_ticks = 3 * sim::kTicksPerSecond;
                if (s.ticks_left == 0) {
                    std::printf("time up — draw!\n");
                } else {
                    audio_.play_random_in_range(2000, 2299);  // "we have a winner"
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
            case AppState::Menu:
                ev = present_menu();  // navigable; resolves the selected row
                break;
            case AppState::Match:
                ev = run_match();
                break;
            case AppState::Results: {
                // One survivor -> VICTORY<player> naming the winner; no survivor
                // or time-up -> DRAW (round_winner() folds both cases). The
                // winner voice group (2000) was already played by run_match on
                // match-over (sub_427BFB(2000)); on a DRAW we fire the tie-game
                // sting once here (sub_427BFB(1700)) — a one-shot group pick, not
                // looped music.
                //
                // Results MUSIC (sub_42A3F6): the handler starts the looping "win"
                // track sub_42741E(0x3FC)=1020 at entry (under the VICTORY screen),
                // and the DRAW branch switches to sub_42741E(0x46A)=1130 ("draw")
                // before DRAW.PCX. start_music replaces the leftover stage/menu
                // track, so the results screen carries its own backdrop music —
                // previously our DRAW/VICTORY screens played under whatever music
                // was left running, a silent-vs-original gap now closed.
                int w = round_winner();
                if (w >= 0) {
                    audio_.start_music(kWinMusicId);  // 1020 win track under VICTORY
                    ev = present_screen(victory_screen(w));
                } else {
                    audio_.start_music(kDrawMusicId);  // 1130 draw track under DRAW
                    audio_.play_random_in_range(kDrawStingLo, kDrawStingHi);
                    ev = present_screen(draw_screen());
                }
                break;
            }
            // The .BM-backed leaves render their real help/credits text now
            // (sub_41302D via BmScreen). Options/Network/Controllers show the
            // HELP overlays for those menu items (the fully-interactive settings
            // and controller-remap UIs remain a documented TODO — see below);
            // Credits shows CREDITS.BM with its inline CREDBAR/JERM/KURT images.
            case AppState::Options:
                ev = present_bm_screen("OPTIONS");
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

int GameApp::run() {
    if (const char* env = std::getenv("BOMBER_BOOT_MATCH"); env && *env) opts_.boot_match = true;
    if (!init()) return opts_.game_dir.empty() ? 2 : 1;
    if (opts_.demo) {
        start_match(0xB0BB1E5);
        return run_demo();
    }
    return run_app();
}

}  // namespace bomber::game
