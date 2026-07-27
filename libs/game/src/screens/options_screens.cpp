#include "bomber/game/screens/options_screens.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cctype>
#include <cstddef>
#include <string>

#include "bomber/game/asset_store.hpp"
#include "bomber/game/dialog_chrome.hpp"       // draw_acknowledge_dialog / draw_text_entry_dialog
#include "bomber/game/editor_screen.hpp"       // SchemeFilePicker
#include "bomber/game/frontend_util.hpp"       // pick_glue, reload_scheme
#include "bomber/game/keyremap_screen.hpp"     // KeyRemapScreen, KeySet, kKeyboardSets
#include "bomber/game/options_screen.hpp"      // OptionsScreen
#include "bomber/game/screens/help_screens.hpp"  // HelpBrowserScreen

namespace bomber::game {

namespace {

// sub_4074DC — Options row 2's node-name line edit (docs/re/results-and-
// options.md §3 row 2, docs/re/network-screens.md §3): getstring(290) "Enter
// new node name:" over a 30-char edit field, drawn with the SAME sub_42E938
// text-entry family every other prompt in the game uses (the editor's
// density/name/filename prompts, the lobby's JOIN BY CODE). Kept file-local
// rather than a fourth ...Runner class: it owns no state beyond the string it
// returns, and only this one call site can reach it. Returns the current name
// unchanged on Escape/window-close, mirroring SchemeFilenamePrompt's cancel.
std::string run_node_name_prompt(ScreenContext& ctx, const std::string& current,
                                 const std::string& backdrop) {
    const std::string label = ctx.assets.getstring(290, "Enter new node name:");
    // CONFIRMED field width (§3 row 2): 30 chars — shorter than the 39 the file
    // itself can hold (assets::kNodeNameMax), which is the original's split too.
    constexpr std::size_t kFieldMax = 30;
    std::string entry = current.size() > kFieldMax ? current.substr(0, kFieldMax) : current;
    std::string result = current;
    SDL_StartTextInput(ctx.window);
    bool waiting = true;
    while (waiting) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) {
                waiting = false;  // result stays `current`
                break;
            }
            if (ev.type == SDL_EVENT_TEXT_INPUT) {
                for (const char* p = ev.text.text; p != nullptr && *p != '\0'; ++p) {
                    if (entry.size() >= kFieldMax) break;
                    // Only what the FON can draw and nodename.ini can hold —
                    // assets::save_node_name would strip anything else anyway,
                    // so reject it at the keystroke instead of silently later.
                    const unsigned char u = static_cast<unsigned char>(*p);
                    if (u >= 32 && u < 127) entry += *p;
                }
            } else if (ev.type == SDL_EVENT_KEY_DOWN) {
                if (ev.key.key == SDLK_BACKSPACE) {
                    if (!entry.empty()) entry.pop_back();
                } else if (ev.key.key == SDLK_RETURN || ev.key.key == SDLK_KP_ENTER) {
                    ctx.audio.play(10);  // accept sting
                    // An emptied field keeps the old name: the original's node
                    // name is never blank (an absent file draws a random one),
                    // and a nameless lobby row is unreadable.
                    if (!entry.empty()) result = entry;
                    waiting = false;
                } else if (ev.key.key == SDLK_ESCAPE) {
                    ctx.audio.play(20);  // nav blip; cancel keeps the old name
                    waiting = false;
                }
            }
        }
        ctx.audio.update_music();
        SDL_SetRenderDrawColor(ctx.sdl, 0, 0, 0, 255);
        SDL_RenderClear(ctx.sdl);
        // The Options screen's own GLUE backdrop under the prompt, exactly like
        // the two sub-screens above (sub_415CA4's saved-backdrop restore).
        const Sprite& bg = ctx.assets.frontend_pcx(backdrop);
        if (bg.tex) {
            SDL_FRect d{0, 0, static_cast<float>(bg.w), static_cast<float>(bg.h)};
            SDL_RenderTexture(ctx.sdl, bg.tex, nullptr, &d);
        }
        // y = 180: the CONFIRMED sub_42E938 prompt anchor (sub_4028D2's
        // save-as); sub_4074DC's own y is register-lost in the decompile, so the
        // port reuses the family's one pinned anchor rather than guessing a new
        // one. The dialog sizes itself from the MEASURED label/entry, so a long
        // name widens the box instead of running out of it.
        draw_text_entry_dialog(ctx.sdl, ctx.front_font, 180.0f, label, entry, "Done", "Cancel");
        SDL_RenderPresent(ctx.sdl);
        SDL_Delay(2);
    }
    SDL_StopTextInput(ctx.window);
    return result;
}

}  // namespace

AppInput OptionsScreenRunner::run() {
    // The interactive Options screen (options_screen.hpp/.cpp): the full
    // §3 19-item list's LIVE subset, over a random GLUE<n> backdrop like
    // present_setup's documented convention (docs/re/setup-screens.md). F1
    // opens the generic *.BM help browser — CORRECTED 2026-07-08: reading
    // sub_4080DC's own F1 dispatch (pseudo.c calls sub_41431C when the raw key
    // code is <= 0x13B)
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
            // "Node Name" (row 2, §3): sub_4074DC's line edit, pushed the same
            // modal way. The name is the port's net identity too — GameApp
            // feeds it to the ADR-0011 lobby as the roster display name.
            if (opt.open_node_name_prompt())
                opt.set_node_name(run_node_name_prompt(ctx_, opt.snapshot().node_name, glue));
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
    // The empty-glob case is NOT special-cased here any more: sub_407582 raises
    // its own getstring(95)/getstring(720) sub_414340 box @0x4076CA, so
    // SchemeFilePicker owns that (draw + the Enter/Space/Esc key loop) and both
    // entry points get it from the one component, exactly as both get the list.
    // An empty glob therefore just runs the loop below and ends cancelled.
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
    // sub_407582's selection write-back @0x40767A-0x4076B8, CORRECTED
    // 2026-07-26 against the binary: the cut character is `mov edx, 0x3A` —
    // a COLON, not a '.' — fed to sub_45167A/strchr. Cutting the display line
    // "BASIC.SCH: Basic Bomberman" at its first ':' therefore recovers the
    // filename WITH its extension, and that is what is strcpy'd into
    // byte_4648C4 and then uppercased (sub_412A3B = strupr).
    //
    // It still loads: the extension strip lives in the READER, not here —
    // sub_403EEE @0x403FE8 does strrchr(name, '.'), truncates, then strcat's
    // ".sch", so "BASIC.SCH" and "BASIC" resolve to the same file. (The
    // shipped options.ini says "BASIC" because MAKECFG's default was never
    // round-tripped through the picker.) The visible difference is Options
    // row 8, which prints the buffer verbatim through getstring(258)
    // "Scheme File: %s".
    std::string name = picker.selected().filename().string();
    for (auto& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    opt.set_scheme_filename(name);
    // The original re-parses byte_4648C4 at the next Play-flow entry
    // (sub_410F81 -> sub_4046CC -> sub_403EEE); reloading immediately keeps
    // scheme_ and the displayed row in lockstep with no hidden latency.
    reload_scheme(state_.scheme, state_.game_dir, name);
}

}  // namespace bomber::game
