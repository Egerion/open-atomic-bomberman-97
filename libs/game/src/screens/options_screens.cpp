#include "bomber/game/screens/options_screens.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cctype>
#include <string>

#include "bomber/game/asset_store.hpp"
#include "bomber/game/dialog_chrome.hpp"       // draw_acknowledge_dialog
#include "bomber/game/editor_screen.hpp"       // SchemeFilePicker
#include "bomber/game/frontend_util.hpp"       // pick_glue, reload_scheme
#include "bomber/game/keyremap_screen.hpp"     // KeyRemapScreen, KeySet, kKeyboardSets
#include "bomber/game/options_screen.hpp"      // OptionsScreen
#include "bomber/game/screens/help_screens.hpp"  // HelpBrowserScreen

namespace bomber::game {

AppInput OptionsScreenRunner::run() {
    // The interactive Options screen (options_screen.hpp/.cpp): the full
    // §3 19-item list's LIVE subset, over a random GLUE<n> backdrop like
    // present_setup's documented convention (docs/re/setup-screens.md). F1
    // opens the generic *.BM help browser — CORRECTED 2026-07-08: reading
    // sub_4080DC's own F1 dispatch (pseudo.c, `if (v165 <= 0x13B) sub_41431C();`)
    // shows it calls the SAME sub_41431C generic browser row 5 and the
    // in-round F1 key open (§4), not a fixed OPTIONS.BM cut. OPTIONS.BM is
    // just one entry in that browser's *.BM glob, same as EDITOR.BM (§3's
    // own correction: "reachable only as a directory-listing entry of the
    // help browser's *.BM glob"). Music left untouched here — unlike
    // present_setup this screen is reached straight from the main menu (not
    // the Play handler sub_42A3F6), so there is no confirmed "inherits 1020"
    // citation; it plays on under whatever the menu already started (1010,
    // kMenuMusicId).
    OptionsScreen opt(ctx_.assets, ctx_.front_font);
    // The GLUE pick doubles as the backdrop the two modal sub-screens
    // restore each frame (sub_415CA4's saved-backdrop memcpy holds this
    // same picture) — keep the name for them.
    const std::string glue = pick_glue(state_.setup_lcg, ctx_.values);
    opt.enter(state_.options, glue);
    AppInput result = AppInput::Advance;
    while (!opt.done()) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
            if (ev.type != SDL_EVENT_KEY_DOWN) continue;
            if (ev.key.key == SDLK_F1) {
                // CONFIRMED (pseudo.c 9298-9299, 9384-9388): the nav-blip SFX
                // 20 fires unconditionally for ANY real key, F1 included,
                // before sub_4080DC dispatches to sub_41431C — the earlier
                // port silently skipped this for F1 specifically.
                ctx_.audio.play(20);
                // Keep the .BM help reachable without leaving the interactive
                // screen: present the generic browser modally (same routine
                // row 5 and in-round F1 open, §4), then resume with the same
                // in-progress edits (present_help_browser owns its own loop).
                AppInput help = HelpBrowserScreen(ctx_).run();
                if (help == AppInput::Quit) return AppInput::Quit;
                continue;
            }
            if (ev.key.key == SDLK_ESCAPE) result = AppInput::Back;
            opt.on_key(ev.key.key, ctx_.audio);
            // "Define keyboard layouts" (row 15, §3): push the key-remap
            // sub-screen (§2) modally, exactly like the F1 help overlay
            // above, then resume the Options screen with its in-progress
            // edits untouched (present_keyremap_screen owns its own loop and
            // applies its own result to keyboard_/options_dirty_ directly).
            if (opt.open_keyremap()) KeyRemapScreenRunner(ctx_, state_).run(glue);
            // "Scheme File" (row 8, §3 CORRECTED 2026-07-13): push
            // sub_407582's *.SCH picker the same modal way; a selection
            // updates the snapshot row AND the live scheme_.
            if (opt.open_scheme_picker()) SchemePickerRunner(ctx_, state_).run(opt, glue);
        }
        ctx_.audio.update_music();
        // cursor1 blink inputs: wall clock (seconds, like the original's
        // time_()) + VALUELST 690's {base, spread} columns — see
        // cursor_indicator.hpp for the sub_413BD6 pacing model.
        opt.tick(SDL_GetTicks() / 1000ull, static_cast<int>(ctx_.values.column_or(690, 0, 2)),
                 static_cast<int>(ctx_.values.column_or(690, 1, 2)));
        SDL_SetRenderDrawColor(ctx_.sdl, 0, 0, 0, 255);
        SDL_RenderClear(ctx_.sdl);
        opt.draw(ctx_.sdl);
        SDL_RenderPresent(ctx_.sdl);
        SDL_Delay(2);
    }

    // Edit the in-memory snapshot ONLY on an actual change (task requirement
    // 3's write-on-exit semantics, §2's CONFIRMED "held in memory ... only
    // flushed ... when the application exits normally") — options.ini itself
    // is untouched here; flush_options() (run()'s tail) is the sole writer.
    // doc §2: "Cleared to -1 by: ... the Options-screen Gold Bomberman
    // toggle" AND, per the 2026-07-09 gold sweep, the Team Play toggle
    // (pseudo.c 9310-9311/9334-9335/9410-9412/9436-9437) — ANY PRESS of
    // either row forfeits a pending gold player in the original, inline at
    // toggle time, not gated on the net before/after value (an even number
    // of presses back to the original value still clears it), which is why
    // this sits OUTSIDE the changed() gate below and uses the touch flags
    // instead of a snapshot diff.
    if (opt.gold_forfeiting_row_touched()) state_.gold_player = -1;
    if (opt.changed()) {
        state_.options = opt.snapshot();
        state_.team_play = state_.options.team_play;
        state_.conveyor_speed_index = state_.options.conveyor_speed_index;
        state_.options_dirty = true;
    }
    return result;
}

void KeyRemapScreenRunner::run(const std::string& backdrop) {
    // The key-remap UI (docs/re/results-and-options.md §2, sub_407B9D),
    // 1:1 rebuild 2026-07-13: MOUSE-DRIVEN widget grid (keyremap_screen.hpp's
    // file doc has the full pin list). Runs its own event pump so raw
    // scancode captures and clicks never leak into the Options cursor
    // underneath; each frame re-blits the Options screen's GLUE backdrop
    // (sub_415CA4's saved-backdrop restore — the picture, not the rows).
    KeyRemapScreen remap(ctx_.assets, ctx_.front_font);
    std::array<KeySet, kKeyboardSets> current{ctx_.keyboard.key_set(0), ctx_.keyboard.key_set(1)};
    remap.enter(current, backdrop);
    // sub_431178/sub_431360 bracket: the system cursor yields to the widget
    // library's own 8x8 arrow (drawn by remap.draw()) for this screen only.
    SDL_HideCursor();
    while (!remap.done()) {
        remap.tick(SDL_GetTicks());
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) {
                // Window close mid-screen: discard this visit's edits (the
                // app-level Quit is re-raised by the Options pump).
                SDL_ShowCursor();
                return;
            }
            if (ev.type == SDL_EVENT_KEY_DOWN) {
                // F1 (0x13B) -> the generic *.BM help browser (sub_41431C),
                // same dispatch as the Options screen's own F1 — but NOT
                // while capturing (F1 must be bindable) or under the NOTE
                // modal (whose own key loop just blips on it).
                if (ev.key.key == SDLK_F1 && !remap.capturing() && !remap.showing_note()) {
                    if (HelpBrowserScreen(ctx_).run() == AppInput::Quit) {
                        SDL_ShowCursor();
                        return;
                    }
                    continue;
                }
                remap.on_key(ev.key.key, ev.key.scancode, ctx_.audio);
                continue;
            }
            // Mouse, converted into the 640x480 logical space (same
            // SDL_RenderCoordinatesFromWindow pattern as the editor canvas).
            if (ev.type == SDL_EVENT_MOUSE_MOTION) {
                float lx = 0, ly = 0;
                SDL_RenderCoordinatesFromWindow(ctx_.sdl, ev.motion.x, ev.motion.y, &lx, &ly);
                remap.on_mouse_move(lx, ly);
            } else if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                       ev.button.button == SDL_BUTTON_LEFT) {
                float lx = 0, ly = 0;
                SDL_RenderCoordinatesFromWindow(ctx_.sdl, ev.button.x, ev.button.y, &lx, &ly);
                remap.on_mouse_down(lx, ly);
            } else if (ev.type == SDL_EVENT_MOUSE_BUTTON_UP &&
                       ev.button.button == SDL_BUTTON_LEFT) {
                float lx = 0, ly = 0;
                SDL_RenderCoordinatesFromWindow(ctx_.sdl, ev.button.x, ev.button.y, &lx, &ly);
                remap.on_mouse_up(lx, ly, ctx_.audio);
            }
        }
        // sub_407AD9's raw keyboard-state poll — binds a key already held
        // when the 500 ms arm delay elapses (the event path alone misses it).
        if (remap.capturing()) remap.poll_capture();
        ctx_.audio.update_music();
        SDL_SetRenderDrawColor(ctx_.sdl, 0, 0, 0, 255);
        SDL_RenderClear(ctx_.sdl);
        remap.draw(ctx_.sdl);
        SDL_RenderPresent(ctx_.sdl);
        SDL_Delay(2);
    }
    SDL_ShowCursor();
    // Apply live (KeyboardMapper reads collect_inputs() every match tick) and
    // mark dirty for the write-on-exit flush — never write options.ini here.
    const auto& edited = remap.edited();
    ctx_.keyboard.set_key_set(0, edited[0]);
    ctx_.keyboard.set_key_set(1, edited[1]);
    state_.options_dirty = true;
}

void SchemePickerRunner::run(OptionsScreen& opt, const std::string& backdrop) {
    // sub_407582 (§3 row 8) — the SAME routine the editor's "edit an
    // existing scheme" path calls (pseudo.c 5501), so this reuses the SAME
    // SchemeFilePicker component: "*.SCH" glob over DATA/SCHEMES, rows
    // "%s: %s" (filename + the file's -N name, aSS), header getstring(721).
    SchemeFilePicker picker(ctx_.assets, ctx_.front_font);
    picker.enter(state_.game_dir / "DATA" / "SCHEMES", backdrop);
    if (picker.empty()) {
        // Empty glob (pseudo.c 8467-8473): sub_414340 with getstring(95)
        // "NOTE!" on top, getstring(720) "No Scheme files found!" below, in
        // byte_49A390's ink — LUT offset 0x5000 -> idx 248 -> (164,0,0),
        // the SAME dark red as the quit-confirm prompt (docs/re/
        // frontend-flow.md "COLOR.PAL" table). sub_414340's own key loop:
        // nav blip on any key, close on Enter/Space/Esc.
        const std::string top = ctx_.assets.getstring(95, "NOTE!");
        const std::string bottom = ctx_.assets.getstring(720, "No Scheme files found!");
        const std::string ok = ctx_.assets.getstring(27, " Ok ");
        while (true) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_EVENT_QUIT) return;
                if (ev.type != SDL_EVENT_KEY_DOWN) continue;
                ctx_.audio.play(20);
                if (ev.key.key == SDLK_RETURN || ev.key.key == SDLK_KP_ENTER ||
                    ev.key.key == SDLK_SPACE || ev.key.key == SDLK_ESCAPE)
                    return;
            }
            ctx_.audio.update_music();
            SDL_SetRenderDrawColor(ctx_.sdl, 0, 0, 0, 255);
            SDL_RenderClear(ctx_.sdl);
            const Sprite& bg = ctx_.assets.frontend_pcx(backdrop);
            if (bg.tex) {
                SDL_FRect d{0, 0, static_cast<float>(bg.w), static_cast<float>(bg.h)};
                SDL_RenderTexture(ctx_.sdl, bg.tex, nullptr, &d);
            }
            // Ink = byte_49A390 = DARK RED (164,0,0): sub_407582's empty-glob box
            // is sub_414340(getstring(95)|getstring(720), byte_49D37A,
            // byte_49A390) (batch_0x4074DC.cpp:180-184); a3 (byte_49A390) is the
            // foreground/ink = (164,0,0) warning red (docs/re/frontend-flow.md),
            // NOT white. (Restores the correct red.)
            draw_acknowledge_dialog(ctx_.sdl, ctx_.front_font,
                                    &ctx_.assets.frontend_pcx("WINZ"), top, bottom, ok, 164, 0, 0);
            SDL_RenderPresent(ctx_.sdl);
            SDL_Delay(2);
        }
    }
    while (!picker.done()) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return;
            if (ev.type != SDL_EVENT_KEY_DOWN) continue;
            picker.on_key(ev.key.key, ctx_.audio);
        }
        ctx_.audio.update_music();
        SDL_SetRenderDrawColor(ctx_.sdl, 0, 0, 0, 255);
        SDL_RenderClear(ctx_.sdl);
        picker.draw(ctx_.sdl);
        SDL_RenderPresent(ctx_.sdl);
        SDL_Delay(2);
    }
    if (picker.cancelled()) return;
    // sub_407582's selection write-back (pseudo.c 8457-8463): the display
    // line is cut at its FIRST '.' (strchr, which also drops the ": <name>"
    // suffix in one stroke), copied into byte_4648C4, then uppercased
    // (sub_412A3B = strupr).
    std::string name = picker.selected().filename().string();
    if (auto dot = name.find('.'); dot != std::string::npos) name.erase(dot);
    for (auto& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    opt.set_scheme_filename(name);
    // The original re-parses byte_4648C4 at the next Play-flow entry
    // (sub_410F81 -> sub_4046CC -> sub_403EEE); reloading immediately keeps
    // scheme_ and the displayed row in lockstep with no hidden latency.
    reload_scheme(state_.scheme, state_.game_dir, name);
}

}  // namespace bomber::game
