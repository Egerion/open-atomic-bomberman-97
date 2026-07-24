#include "bomber/game/screens/menu_screen.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <iterator>  // std::size (kMenuCount)
#include <string>

#include "bomber/game/anim_pace.hpp"      // anim_step_index
#include "bomber/game/dialog_chrome.hpp"  // draw_confirm_dialog
#include "bomber/game/input.hpp"          // attract_computer_count/fill_attract_roster/attract_stage_pick
#include "bomber/game/screens/debug_info_screen.hpp"    // DebugInfoScreen
#include "bomber/game/screens/editor_screen_runner.hpp"  // EditorRunner + EditorEditState
#include "bomber/game/screens/help_screens.hpp"          // HelpBrowserScreen
#include "bomber/game/screens/video_settings_screen.hpp"  // VideoSettingsScreen + VideoToggleRefs
#include "bomber/game/sprites.hpp"        // Sprite, Anim, resolve_sequence
#include "bomber/platform/frame_clock.hpp"

namespace bomber::game {

namespace {

// The CONFIRMED front-end SOUNDLST menu-music id (BM95.EXE, docs/re/
// frontend-flow.md): menu music 1010 (0x3F2, sub_42741E in sub_42B9CE
// @0x42B9CE), started on menu entry. Music tracks loop (sub_4273A4 sets loop
// count 0xFFFF); AudioEngine::start_music() drives the looping music channel.
constexpr int kMenuMusicId = 1010;  // 0x3F2 — MENU.RSS, started on menu entry

// The menu-quit / exit sting group (sub_427BFB(2600) in the quit handler
// sub_412987): 2600..2699 = "go outside and play now!" takes (quitgame/EOFM7*/…).
constexpr int kQuitStingLo = 2600;
constexpr int kQuitStingHi = 2699;

// The main-menu ATTRACT idle timeout — CONFIRMED getvalue(92) = 30 (VALUELST
// `92,30`), gated `> 5` (the file's own legend: values < 5 disable attract
// entirely), distinct from the waited-screen getvalue(12)
// (docs/re/frontend-flow.md "Attract mode" / "Tunables"). Expressed as a
// fallback in seconds; run() reads the live VALUELST value via
// ctx_.values.at_or(92, ...) so a modified install's timeout is honoured.
constexpr std::int64_t kAttractIdleFallbackS = 30;
constexpr std::int64_t kAttractIdleMinS = 5;  // getvalue(92) <= 5 disables attract

// --- Main-menu model (sub_42B9CE) -----------------------------------------
// The original menu highlights one of seven rows (its selection variable v10
// runs 0..6) over MAINMENU.PCX and dispatches on Enter: 0=Play (sub_42A3F6 runs
// a match + results), 1/2=setup screens (sub_42B0CE/sub_42B47D), 3=editor,
// 4=credits (.BM), 5=the generic *.BM help browser (sub_41431C — CORRECTED,
// docs/re/results-and-options.md §4: the row's old "Roulette" label was wrong;
// it lists every *.BM in the install root, ROULETTE.BM among them), 6=quit
// (sub_412987, exit sting 2600). We keep the row set and order but map the
// not-yet-built leaves to their stub AppInputs; Start/Quit/Credits/Help are
// wired live. (docs/re/frontend-flow.md.)
struct MenuItem {
    AppInput action;  // resolved when Enter selects this row
    bool live;        // false = a documented stub row (no handler yet, inert)
};

// Seven rows in the ORIGINAL's v10 order (sub_42B9CE), so the cursor anchor
// (getvalue 700-702) lands on the labels baked into MAINMENU.PCX. Row targets
// per the CORRECTED dispatch (docs/re/results-and-options.md: v10==3 goes to
// sub_4080DC = the OPTIONS screen, NOT an editor; v10==1/2 are the START/JOIN
// NET GAME screens sub_42B0CE/sub_42B47D — now LIVE, our own UDP-lockstep
// netplay (ADR-0010 §3.3 step 5) rather than the original's IPX/serial link):
//   0 Play           -> StartMatch   (live)
//   1 START NET GAME -> OpenNetHost  (host a 2-player UDP lockstep match:
//                                     NetplayConnectScreen + seed handshake +
//                                     run_netplay_match — netplay increment 5c)
//   2 JOIN NET GAME  -> OpenNetJoin  (connect to a host: address prompt +
//                                     handshake, then run_netplay_match)
//   3 Options        -> OpenOptions  (the interactive sub_4080DC screen;
//                                     F1 on it reaches the help browser)
//   4 Credits        -> OpenCredits  (live: CREDITS.BM viewer)
//   5 Help browser   -> live, handled INLINE (see the SDLK_RETURN case below):
//                       sub_41431C dispatches with no wipe in the original, so
//                       row 5 is special-cased ahead of this table rather than
//                       routed through AppInput/next() like rows 0-4/6 are —
//                       its kMenuItems entry below is unused/dead for row 5.
//   6 Quit           -> Quit         (live)
// INPUT.BM has no dedicated main-menu row or controller-setup screen in the
// original — CONFIRMED negative (docs/re/frontend-flow.md, 2026-07-09): no
// "controller" string and no "INPUT.BM" literal exist anywhere in pseudo.c.
// It is one of ~10 topics the generic *.BM help browser (row 5/F1,
// HelpBrowser) already globs and lists, which this port already reproduces.
// The OpenControllers edge below is exercised by the doctest/flow but
// deliberately left unbound to any row — there is nothing in the original's
// seven menu rows to bind it to.
constexpr MenuItem kMenuItems[] = {
    {AppInput::StartMatch, true},   // 0 Play
    {AppInput::OpenNetHost, true},  // 1 START NET GAME -> host + handshake + match
    {AppInput::OpenNetJoin, true},  // 2 JOIN NET GAME -> connect + handshake + match
    {AppInput::OpenOptions, true},  // 3 Options (sub_4080DC) — was misbound to row 1
    {AppInput::OpenCredits, true},  // 4 Credits
    {AppInput::Advance, false},     // 5 Help browser (handled inline, entry unused)
    {AppInput::Quit, true},         // 6 Quit
};
constexpr int kMenuCount = static_cast<int>(std::size(kMenuItems));

// Cursor anchor over MAINMENU.PCX — CONFIRMED getvalue(700/701/702) (sub_42B9CE:
// v11=getvalue(700)=X, v1=getvalue(701)=Y, getvalue(702)=Y-step; the bomb-
// trigger sprite is blitted at x=X, y=Y + Ystep*row). Read live from VALUELST
// (columns of the multi-value row 700, whose own legend reads "X, Y - first item
// / YS - y-spacing"); these fallbacks are that install's values (332,140,38) so
// a stripped VALUELST still positions sanely. The idle/attract timeout uses
// getvalue(92) (sub_42B9CE), distinct from the waited-screen getvalue(12).
constexpr int kMenuCursorXFallback = 332;    // getvalue(700)
constexpr int kMenuCursorYFallback = 140;    // getvalue(701)
constexpr int kMenuCursorStepFallback = 38;  // getvalue(702)

}  // namespace

AppInput MenuScreen::run() {
    // The navigable main menu (sub_42B9CE @0x42B9CE): MAINMENU.PCX as the
    // backdrop, an up/down highlight over the item rows (wrapping), Enter
    // selects, Escape quits. On entry the original plays sub_42741E(0x3F2)
    // (the CONFIRMED menu track 1010, 0x3F2 == MENU.RSS; the RE brief's
    // 0x3FC/1020 was the round/results path sub_42A3F6, not this). This
    // switches the looping music to the menu track and keeps it playing while
    // in the menu. Returns the AppInput the highlighted row resolves to, or
    // Quit on window close.
    //
    // v14 (docs/re/frontend-flow.md "sub_42B9CE") is NOT a run-once-per-process
    // flag: sub_42B9CE has an OUTER while(1) (one iteration per menu visit,
    // pseudo.c ~30744) wrapping an INNER while(1) (the per-frame input-poll
    // loop, ~30768). `v14 = 1` is set once per OUTER iteration, right before
    // the inner loop starts; the inner loop's `if (v14) { sub_42741E(0x3F2);
    // v14 = 0; }` just stops it from re-firing on every polled FRAME within
    // that one visit. Every switch case at the bottom of the outer loop
    // (Play/setup cancel, Credits, Options, Quit-confirm-cancel, Results,
    // idle-timeout->attract, ...) falls through back to the top of the outer
    // loop, which re-arms v14=1, so sub_42741E(0x3F2) — a full free +
    // reload-from-disk + restart-from-sample-0 (sub_4273A4, no same-id
    // no-op) — fires again on EVERY return to the menu. present_menu() is
    // called once per Menu (re-)entry from run_app's dispatcher, i.e. once
    // per outer-loop iteration, so an unconditional start_music() call here
    // is the faithful port: it also switches back from the round/results
    // tracks (1020/1130) to 1010, which is exactly the audible "menu music
    // reclaims the loop when you back out" behaviour of the original. A
    // once-per-process gate (menu_music_started_, removed) broke that: after
    // the first call it never started 1010 again, so returning from a match
    // or the results screen left 1020/1130 stuck looping forever.
    ctx_.audio.start_music(kMenuMusicId);
    std::uint64_t frame = 0;
    // ATTRACT idle timer (docs/re/frontend-flow.md "Attract mode"): seeded to
    // "now" on every fresh visit to the menu (including a re-entry after an
    // attract match itself, so an unattended machine cycles demo matches
    // forever, one getvalue(92)-second gap apart — matching sub_42B9CE's
    // idle counter, which is never suppressed after firing once).
    state_.menu_idle_since_ms = SDL_GetTicks();
    const std::int64_t idle_s = ctx_.values.at_or(92, kAttractIdleFallbackS);
    const bool attract_enabled = idle_s > kAttractIdleMinS;  // legend: <=5 disables attract
    // The Quit confirm overlay (sub_412987 @0x412987, PINNED 2026-07-09): Escape
    // does NOT quit directly. It selects row 6 (Quit, with the usual blip-20 +
    // accept-10 sound pair), and the row-6 dispatch calls sub_412987, which pops
    // a modal yes/no dialog — sub_41456C(getstring(10), ...) — BEFORE anything
    // exits. getstring(10) = "Are you sure you want to exit?", buttons
    // getstring(26)=" Yes "/getstring(25)=" No ". sub_41456C's own key loop
    // (pseudo.c ~17220-17270) accepts Y/y/Enter/Space as Yes (returns 1) and
    // N/n/Escape as No (returns 0) — confirmed by the raw key-code ranges
    // (0x1B/78/110 -> No; 13/32/89/121 -> Yes). Only on Yes does sub_412987 play
    // the exit sting (sub_427BFB(2600), skip-logos-gated) and Sleep(0xFA0 = 4 s)
    // before the real process exit (sub_4128C9(0)); No just closes the dialog
    // and returns to the menu with nothing else touched. The port mirrors this
    // exactly: quit_confirm gates a small modal drawn over the menu backdrop
    // (same box-plus-FontTextures convention as EditorScreen's SaveConfirm).
    bool quit_confirm = false;
    // Refresh-boundary pacing (see refresh_period_ns / run_match): the blind
    // SDL_Delay(2) this replaced let the loop free-run at 300-500 Hz on
    // Windows (present does not block), so the `frame`-driven trigger cursor
    // animated far too fast. Pace to one animation step per real refresh.
    platform::FrameClock frame_clock(ctx_.window);
    while (true) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
            // ANY real input resets the idle clock (doc: "any input resets
            // the timer") — key, mouse button, or gamepad button, mirroring
            // the original's "any real key" blip path plus this port's own
            // mouse/pad input surfaces (sub_42B9CE only had a keyboard).
            if (ev.type == SDL_EVENT_KEY_DOWN || ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)
                state_.menu_idle_since_ms = SDL_GetTicks();
            // Pad navigation (sub_4102B7 @ 14286-14333): whenever the key
            // queue is empty the original's getkey polls every joystick and
            // SYNTHESIZES key codes from it — axis-threshold crossings become
            // up (328)/down (336) and any button rising edge becomes Enter
            // (13) — so the whole menu (and the quit confirm on top of it,
            // which reads the same getkey) is pad-navigable. SDL gives us
            // dpad/button edges directly; re-inject them as the synthetic
            // keys so every key path above/below (blips, dialog, rows) is
            // shared rather than duplicated.
            if (ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                SDL_Event synth{};
                synth.type = SDL_EVENT_KEY_DOWN;
                switch (ev.gbutton.button) {
                    case SDL_GAMEPAD_BUTTON_DPAD_UP: synth.key.key = SDLK_UP; break;
                    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: synth.key.key = SDLK_DOWN; break;
                    case SDL_GAMEPAD_BUTTON_SOUTH:
                    case SDL_GAMEPAD_BUTTON_EAST:
                    case SDL_GAMEPAD_BUTTON_WEST:
                    case SDL_GAMEPAD_BUTTON_NORTH:
                    case SDL_GAMEPAD_BUTTON_START: synth.key.key = SDLK_RETURN; break;
                    default: synth.key.key = SDLK_UNKNOWN; break;
                }
                if (synth.key.key != SDLK_UNKNOWN) SDL_PushEvent(&synth);
            }
            if (ev.type != SDL_EVENT_KEY_DOWN) continue;

            // F10 opens the PORT-ONLY Video Settings panel (see
            // present_video_settings). Not an RE'd key — a port entry point that
            // keeps the modern video/cadence toggles off the faithful Options
            // screen. Ignored while the quit-confirm modal is up.
            if (!quit_confirm && ev.key.key == SDLK_F10) {
                VideoSettingsScreen(ctx_, {.uncap_fps = &state_.uncap_fps,
                                           .native_cadence = &state_.native_cadence,
                                           .show_fps = &state_.show_fps,
                                           .options_dirty = &state_.options_dirty})
                    .run();
                continue;
            }

            if (quit_confirm) {
                // sub_41456C's key loop: any real key blips (20); Yes accepts
                // (Y/y/Enter/Space), No cancels (N/n/Q/q/Escape — the Q pair
                // is 17253-17267, missing from frontend-flow.md's old list) —
                // every other key is ignored and the dialog stays up. The
                // dialog itself resolves SILENTLY (no accept sting on either
                // answer — the old play(10)s here were invented).
                ctx_.audio.play(20);
                switch (ev.key.key) {
                    case SDLK_Y:
                    case SDLK_RETURN:
                    case SDLK_KP_ENTER:
                    case SDLK_SPACE:
                        // sub_412987 on Yes: FREE the music (sub_427342) first,
                        // THEN the 2600 exit sting — MENU.RSS must not keep
                        // looping under it — then Sleep(0xFA0) so the sting is
                        // audible rather than cut off by window teardown.
                        ctx_.audio.stop_music();
                        ctx_.audio.play_random_in_range(kQuitStingLo, kQuitStingHi);  // 2600 group
                        SDL_Delay(4000);
                        return AppInput::Quit;
                    case SDLK_N:
                    case SDLK_Q:  // 0x51 'Q' / 0x71 'q' cancel too (17253-17267)
                    case SDLK_ESCAPE:
                        quit_confirm = false;
                        // The cancel path exits through sub_42B9CE's OUTER
                        // loop: cursor home to row 0 (case 6 -> v10 = 0,
                        // 30920-30922) and MENU.RSS reloaded from sample 0
                        // (the outer loop re-arms v14 -> sub_42741E(0x3F2)).
                        state_.menu_index = 0;
                        ctx_.audio.start_music(kMenuMusicId);
                        break;
                    default: break;
                }
                continue;  // modal: no other input reaches the row switch below
            }

            // Hidden scheme-editor trigger (docs/re/results-and-options.md
            // §5, CONFIRMED): sub_42B9CE's input loop tracks a same-key
            // repeat counter on raw key code 5 (ASCII Ctrl+E); any OTHER key
            // resets it; `++counter > 5` fires on the 6th CONSECUTIVE press
            // (plays accept SFX 10, then calls sub_40330E -> sub_403184).
            // SDL reports Ctrl+E as SDLK_E with KMOD_CTRL set — there is no
            // menu row for this, so it is checked directly in the raw event
            // loop, ahead of (and independent of) the row-navigation switch
            // below, and does not fall through to it on a match.
            bool is_ctrl_e = (ev.key.key == SDLK_E) && (ev.key.mod & SDL_KMOD_CTRL) != 0;
            if (is_ctrl_e) {
                ctx_.audio.play(20);  // the any-real-key blip fires for each press (30787-30790)
                if (++state_.editor_trigger_count > 5) {
                    state_.editor_trigger_count = 0;
                    ctx_.audio.play(10);  // accept sting (SFX 10), §5
                    EditorRunner(ctx_, EditorEditState{.setup_lcg = state_.setup_lcg,
                                                       .scheme = state_.scheme,
                                                       .game_dir = state_.game_dir,
                                                       .scheme_path = state_.scheme_path})
                        .run();
                    // Return through the outer loop re-arms v14 -> MENU.RSS
                    // reloads from sample 0 (goto LABEL_2 at 30882).
                    ctx_.audio.start_music(kMenuMusicId);
                    // Time spent in the editor must NOT count toward the 30 s
                    // attract idle trigger — reseed the idle clock on return.
                    state_.menu_idle_since_ms = SDL_GetTicks();
                }
                continue;  // Ctrl+E itself never falls into the row switch
            }
            state_.editor_trigger_count = 0;  // any other key resets the counter

            // Menu hotkeys (sub_42B9CE's raw-code dispatch; every one rides
            // the any-key blip 20 first): Ctrl+Q (raw 17) behaves exactly
            // like Escape (30861-30867); Alt+O (280) jumps to and selects
            // Options (30806-30812); F1 (315) selects row 5 = the help
            // browser (30826-30832); Alt+A (286) starts an attract demo
            // match directly (30813 -> the 30888-30894 attract path —
            // frontend-flow.md's old "run the current selection" label for
            // 286 was wrong); Alt+D (288) pops the hidden debug-info window
            // (sub_413D45, 30819-30822).
            const bool menu_alt = (ev.key.mod & SDL_KMOD_ALT) != 0;
            if (ev.key.key == SDLK_Q && (ev.key.mod & SDL_KMOD_CTRL) != 0) {
                ctx_.audio.play(20);
                ctx_.audio.play(10);
                state_.menu_index = 6;
                quit_confirm = true;
                continue;
            }
            if (menu_alt && ev.key.key == SDLK_O) {
                ctx_.audio.play(20);
                ctx_.audio.play(10);
                // case 3's fall-through resets the cursor to row 0 for the
                // NEXT menu visit (30910-30912).
                AppInput opt_sel = kMenuItems[3].action;
                state_.menu_index = 0;
                return opt_sel;
            }
            if (ev.key.key == SDLK_F1) {
                ctx_.audio.play(20);
                ctx_.audio.play(10);
                state_.menu_index = 5;
                if (HelpBrowserScreen(ctx_).run() == AppInput::Quit) return AppInput::Quit;
                ctx_.audio.start_music(kMenuMusicId);  // outer-loop v14 re-arm
                state_.menu_idle_since_ms = SDL_GetTicks();  // help time is not idle
                continue;
            }
            if (menu_alt && ev.key.key == SDLK_A) {
                ctx_.audio.play(20);
                state_.menu_index = 0;  // the attract path's own v10 = 0 (30894)
                roll_attract_match();
                return AppInput::StartMatch;
            }
            if (menu_alt && ev.key.key == SDLK_D) {
                ctx_.audio.play(20);
                if (DebugInfoScreen(ctx_).run() == AppInput::Quit) return AppInput::Quit;
                state_.menu_idle_since_ms = SDL_GetTicks();  // debug-window time is not idle
                continue;
            }

            switch (ev.key.key) {
                case SDLK_UP:
                case SDLK_W:
                    state_.menu_index = (state_.menu_index + kMenuCount - 1) % kMenuCount;
                    ctx_.audio.play(20);  // nav blip (SOUNDLST 20, sub_427961(20))
                    break;
                case SDLK_DOWN:
                case SDLK_S:
                    state_.menu_index = (state_.menu_index + 1) % kMenuCount;
                    ctx_.audio.play(20);
                    break;
                case SDLK_ESCAPE:
                    // Escape's full sound path in sub_42B9CE: the "any real key"
                    // line fires the nav blip (SFX 20) for EVERY key including 27,
                    // then Escape (27) reaches the accept branch `if (v8>=17 &&
                    // (v8<=17 || v8==27)) { sub_427961(10); v10=6; }` — the accept
                    // sting (SFX 10) — and selects row 6 = Quit. The Quit row then
                    // dispatches to sub_412987, which pops the "Are you sure you
                    // want to exit?" confirm dialog (PINNED, see quit_confirm's
                    // comment above) — it does NOT exit directly. Only a Yes in
                    // that dialog plays the 2600 exit sting and quits.
                    ctx_.audio.play(20);  // nav blip on the key (SFX 20)
                    ctx_.audio.play(10);  // accept sting selecting Quit (SFX 10)
                    state_.menu_index = 6;  // v10 = 6, matches the cursor landing on Quit
                    quit_confirm = true;
                    break;
                case SDLK_RETURN:
                case SDLK_KP_ENTER:
                case SDLK_SPACE: {
                    // sub_42B9CE plays the any-key blip 20 FIRST (30787-30790,
                    // for every real key including Enter/Space), then the
                    // accept sting (SFX 10, sub_427961(10)) for BOTH Enter
                    // (13) and Space (32) on EVERY row — there is no "inert
                    // row" concept in the original; each row 0..6 is a live
                    // dispatch.
                    ctx_.audio.play(20);  // any-real-key blip
                    ctx_.audio.play(10);  // accept sting (SOUNDLST 10, menuexit)
                    // Row 5 = the generic help-file browser (sub_41431C, §4 —
                    // CORRECTED from the old "Roulette" label, see kMenuItems'
                    // comment above). sub_42B9CE's row switch calls it DIRECTLY
                    // (case 5: sub_41431C(); break;), staying inside the menu
                    // loop, unlike rows 0-4/6 which return through the AppState
                    // flow — so this row is handled here inline and
                    // never touches AppInput/next() (task brief: prefer not to
                    // add new AppInputs for this leaf).
                    if (state_.menu_index == 5) {
                        if (HelpBrowserScreen(ctx_).run() == AppInput::Quit) return AppInput::Quit;
                        // The inline return path falls through sub_42B9CE's
                        // outer loop -> v14 re-arm -> MENU.RSS reloads from
                        // sample 0.
                        ctx_.audio.start_music(kMenuMusicId);
                        state_.menu_idle_since_ms = SDL_GetTicks();  // help time is not idle
                        break;
                    }
                    // A row we have not built yet (Editor) still plays the
                    // accept sting to stay faithful, but has no leaf to jump to, so
                    // it simply stays put instead of dead-ending on an unbuilt
                    // screen. (Documented inert stub — the accept is real, the
                    // destination is a deferred effort.)
                    if (!kMenuItems[state_.menu_index].live) break;
                    AppInput sel = kMenuItems[state_.menu_index].action;
                    // Row 3 (Options) is one of the rows whose fall-through
                    // resets the cursor to row 0 for the next menu visit
                    // (case 3 -> v10 = 0, 30910-30912); Play/Credits/Help/
                    // editor keep the row (verified faithful list).
                    if (state_.menu_index == 3) state_.menu_index = 0;
                    // Quit selected from the menu (Enter/Space on row 6): the SAME
                    // sub_412987 dispatch Escape reaches, so it pops the SAME confirm
                    // dialog rather than quitting outright.
                    if (sel == AppInput::Quit) {
                        quit_confirm = true;
                        break;
                    }
                    // Hand the selection to the flow by a CUT — no wipe. The
                    // original's menu dispatch (sub_42B9CE) calls the selected
                    // handler directly; the next screen's own first frame
                    // replaces the menu. A HEADWIPE.ANI wipe used to play here
                    // (~3.5 s: 211 frames, one per vsynced frame — the "long
                    // pause into the player-setup screen" report), but that
                    // file is dead art the original never even loads: it is
                    // absent from MASTER.ALI, and the decompile contains no
                    // headwipe string or transition call site anywhere. See
                    // docs/re/frontend-flow.md "HEADWIPE.ANI is dead art".
                    return sel;
                }
                default:
                    // The any-real-key blip fires for EVERY key sub_42B9CE
                    // reads, mapped or not (30787-30790) — an unbound letter
                    // still clicks. Gate on the press edge so SDL's key
                    // repeats don't buzz.
                    if (!ev.key.repeat) ctx_.audio.play(20);
                    break;
            }
        }

        // ATTRACT trigger (docs/re/frontend-flow.md "Attract mode"): once the
        // idle clock exceeds getvalue(92) seconds (gated > 5), fire the SAME
        // Play dispatch a real Enter-on-row-0 would — sub_42B9CE forces
        // v10=0 regardless of the highlighted row, so this returns StartMatch
        // directly rather than nudging state_.menu_index. roll_attract_match() does
        // the sub_4224E2 save + the roster/stage rolls; run_app's StartMatch
        // handler (game_app.cpp) checks state_.attract and skips the goldman wheel/
        // setup/level screens, matching sub_410F81's short-circuit.
        // The quit-confirm dialog is modal (sub_41456C blocks sub_42B9CE's own
        // loop until answered) — hold off the attract idle trigger while it is
        // up so a demo match cannot yank the confirm away mid-decision.
        if (attract_enabled && !quit_confirm &&
            SDL_GetTicks() - state_.menu_idle_since_ms >= static_cast<std::uint64_t>(idle_s) * 1000) {
            state_.menu_index = 0;  // the attract path homes the cursor (v10 = 0, 30894)
            roll_attract_match();
            return AppInput::StartMatch;
        }

        ctx_.audio.update_music();
        SDL_SetRenderDrawColor(ctx_.sdl, 0, 0, 0, 255);
        SDL_RenderClear(ctx_.sdl);
        // Backdrop.
        const Sprite& bg = ctx_.assets.frontend_pcx("MAINMENU");
        if (bg.tex) {
            SDL_FRect d{0, 0, static_cast<float>(bg.w), static_cast<float>(bg.h)};
            SDL_RenderTexture(ctx_.sdl, bg.tex, nullptr, &d);
        }
        // "V1.0" version string, every menu frame (pseudo.c 30779:
        // sub_41696C(root, aV10, x=0, W=50, y=0, byte_49A624, black)) — the
        // ink is the general grey (168,168,164), the literal is hardcoded in
        // the binary (aV10, pseudo.c 1622), not a MESSAGES.TXT entry.
        ctx_.front_font.draw_outlined(ctx_.sdl, "V1.0", 0, 0, 168, 168, 164, 0, 0, 0,
                                  50.0f);
        // Animated "bomb trigger green" cursor at the CONFIRMED anchor
        // (sub_42B9CE: x=getvalue(700), y=getvalue(701)+getvalue(702)*row; frame
        // = counter % statecnt). Anchor read live from VALUELST row 700's
        // columns, with this install's values as fallback. The sequence comes
        // from TRIGANIM.ANI (the file MASTER.ALI actually loads — the
        // original's menu resolves the name from the same global pool the
        // in-match trigger bomb uses; TRIGBOMB.ANI's 7-step twin is dead art,
        // docs/re/facts.md "ANI sequence-name audit"); if it is absent we
        // draw a pulsing highlight bar instead so the selection stays visible.
        {
            // The confirm dialog is modal in the original (sub_41456C blocks
            // sub_42B9CE's own loop), so the menu frame under it is FROZEN —
            // hold the cursor's animation phase while it is up.
            if (!quit_confirm) ++frame;
            int cx = static_cast<int>(ctx_.values.column_or(700, 0, kMenuCursorXFallback));
            int cy0 = static_cast<int>(ctx_.values.column_or(700, 1, kMenuCursorYFallback));
            int cstep = static_cast<int>(ctx_.values.column_or(700, 2, kMenuCursorStepFallback));
            int cy = cy0 + state_.menu_index * cstep;
            Anim cur = resolve_sequence(ctx_.assets.trigbomb(-1), "bomb trigger green");
            if (!cur.steps.empty()) {
                const Sprite& sp = cur.steps[anim_step_index(frame, cur.steps.size())];
                if (sp.tex) {
                    SDL_FRect d{static_cast<float>(cx - sp.hx), static_cast<float>(cy - sp.hy),
                                static_cast<float>(sp.w), static_cast<float>(sp.h)};
                    SDL_RenderTexture(ctx_.sdl, sp.tex, nullptr, &d);
                }
            } else {
                Uint8 pulse = static_cast<Uint8>(90 + 60 * ((frame / 8) % 2));
                SDL_FRect bar{static_cast<float>(cx), static_cast<float>(cy), 240.0f, 22.0f};
                SDL_SetRenderDrawBlendMode(ctx_.sdl, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(ctx_.sdl, 255, 220, 60, pulse);
                SDL_RenderFillRect(ctx_.sdl, &bar);
            }
        }
        // The Quit confirm modal (sub_412987 -> sub_41456C, RE-PINNED
        // 2026-07-10 — docs/re/frontend-flow.md "Escape/Quit-row confirm
        // dialog"): the SAME sub_43C734 chrome as the loading dialog — the
        // WINZ.PCX 9-patch (sub_41726B @ pseudo.c 17200), NOT a flat grey —
        // sized from the prompt extent (v29=max(prompt-width,80),
        // v30=v29+64=width, v32=4*fontheight+64+fontheight=height for this
        // one-line prompt), centered on screen (both axes — see the chrome
        // comment's X-placement TODO(RE)). Prompt at y=fontheight+32
        // (window-relative, centered) in sub_412987's OWN ink byte_49A390 —
        // LUT offset 0x5000 -> idx 248 -> (164,0,0), a dark red (pseudo.c
        // 16027: `v0 = byte_49A390` is the a3/foreground argument;
        // correcting this pass's earlier white) — outlined black via
        // sub_41696C; two sub_432298 buttons at the pinned
        // y=height-32-fontheight-6, x=width/2-80 (Yes) / width/2+22 (No).
        // Behaviour (Y/Enter/Space confirm, N/Escape cancel, sound path, 4s
        // exit delay) is UNCHANGED — chrome-only pass.
        if (quit_confirm) {
            std::string prompt = ctx_.assets.getstring(10, "Are you sure you want to exit?");
            std::string yes_label = ctx_.assets.getstring(26, " Yes ");
            std::string no_label = ctx_.assets.getstring(25, " No ");
            // Prompt ink = byte_49A390 = DARK RED (164,0,0): sub_412987's quit
            // confirm is sub_41456C(getstring(10), 0, byte_49A390)
            // (batch_0x411CF8.cpp), and byte_49A390 resolves to LUT offset
            // 0x5000 -> (164,0,0) — a distinct WARNING RED, NOT the white
            // byte_49D38F/kDialogInk (docs/re/frontend-flow.md "byte_49A390").
            // sub_41456C's a3 slot is the foreground/ink.
            //
            // The prompt is drawn as this red FILL over a GOLD 1-px outline —
            // (252,248,88) = byte_49D37A "percent readout yellow" — the
            // emphasised red/gold look the original's exit pop-up shows in the
            // reference photo. The RE records sub_41696C's outline colour
            // (sub_412987's a2/a7) as decompiler-lost ("black per every sibling
            // call site", docs/re/frontend-flow.md "Escape/Quit-row confirm
            // dialog"), so this gold is PHOTO-DERIVED pending a binary re-read;
            // only the quit prompt overrides the default-black outline, since
            // the photo evidence is for this dialog alone.
            draw_confirm_dialog(ctx_.sdl, ctx_.front_font, &ctx_.assets.frontend_pcx("WINZ"),
                                prompt, "", yes_label, no_label, 164, 0, 0, 252, 248, 88);
        }
        SDL_RenderPresent(ctx_.sdl);
        frame_clock.pace();
    }
}

// Attract-mode entry — sub_4224E2's save + sub_410F81's attract branch
// (docs/re/frontend-flow.md "Attract mode" point 1, pseudo.c 15125-15143).
void MenuScreen::roll_attract_match() {
    // sub_4224E2: snapshot the CURRENT selections before overwriting them, so
    // restore_from_attract() (sub_422552) can put them back untouched.
    state_.attract_saved.type = state_.setup_type;
    state_.attract_saved.sub = state_.setup_sub;
    state_.attract_saved.team = state_.setup_team;
    state_.attract_saved.level = state_.selected_level;
    state_.attract_saved.team_play = state_.team_play;

    state_.attract = true;  // dword_464938 = 1

    // "forces team play off" (doc point 2) — a demo match is never team mode,
    // regardless of what the player had configured.
    state_.team_play = false;

    // Two presentation-LCG rolls (never State::rng), advanced in order —
    // roster count then stage, matching the doc's read order (roster is
    // rolled first in sub_410F81, the level right after).
    state_.attract_lcg = state_.attract_lcg * 1664525u + 1013904223u;
    int computer_count = attract_computer_count(state_.attract_lcg >> 16);
    fill_attract_roster(computer_count, state_.setup_type, state_.setup_sub, state_.setup_team);

    state_.attract_lcg = state_.attract_lcg * 1664525u + 1013904223u;
    int level_count = static_cast<int>(ctx_.values.column_or(35, 0, 11));  // getvalue(35)
    state_.selected_level = attract_stage_pick(state_.attract_lcg >> 16, level_count);

    // Campaign trigger inert during attract (task point 2): an attract match
    // never has campaign state armed (it can only be armed by present_setup's
    // 'C'x5 trigger, which the attract short-circuit never visits), so there
    // is nothing to suppress here beyond simply not touching
    // campaign_active_ — documented for the reader, not a defensive reset.
}

}  // namespace bomber::game
