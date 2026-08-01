#include "bomber/frontend/options_screens.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cctype>
#include <cstddef>
#include <optional>
#include <string>

#include "bomber/editor/editor_screen.hpp"      // SchemeFilePicker
#include "bomber/frontend/keyremap_screen.hpp"  // KeyRemapScreen, KeySet, kKeyboardSets
#include "bomber/frontend/options_screen.hpp"   // OptionsScreen
#include "bomber/game_util/frontend_util.hpp"   // pick_glue, reload_scheme
#include "bomber/render/asset_store.hpp"
#include "bomber/ui/dialog_chrome.hpp"  // draw_acknowledge_dialog / draw_text_entry_dialog
#include "bomber/ui/help_screens.hpp"   // HelpBrowserScreen

namespace bomber::game {

namespace {

// sub_4074DC — Options row 2's node-name line edit (docs/re/network-screens.md
// §3), in the SAME sub_42E938 text-entry family every other prompt uses.
class NodeNamePrompt {
public:
    NodeNamePrompt(ScreenContext& ctx, const std::string& current, const std::string& backdrop)
        : ctx_(ctx), backdrop_(backdrop), result_(current) {
        entry_ = current.size() > kFieldMax ? current.substr(0, kFieldMax) : current;
    }

    // Returns the current name unchanged on Escape/window-close, mirroring
    // SchemeFilenamePrompt's cancel.
    std::string run() {
        SDL_StartTextInput(ctx_.window);
        while (waiting_) {
            pump_events();
            ctx_.audio.update_music();
            draw();
            SDL_RenderPresent(ctx_.sdl);
            SDL_Delay(2);
        }
        SDL_StopTextInput(ctx_.window);
        return result_;
    }

private:
    // CONFIRMED field width (§3 row 2): 30 chars — shorter than the 39 the file
    // itself can hold (assets::kNodeNameMax), which is the original's split too.
    static constexpr std::size_t kFieldMax = 30;

    void pump_events() {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) {
                waiting_ = false;  // result stays as it entered
                return;
            }
            if (ev.type == SDL_EVENT_TEXT_INPUT) append_text(ev.text.text);
            if (ev.type == SDL_EVENT_KEY_DOWN) on_key(ev.key.key);
        }
    }

    // Only what the FON can draw and nodename.ini can hold — save_node_name would
    // strip anything else anyway, so reject it at the keystroke rather than
    // silently later.
    void append_text(const char* text) {
        for (const char* p = text; p != nullptr && *p != '\0'; ++p) {
            if (entry_.size() >= kFieldMax) return;
            const unsigned char u = static_cast<unsigned char>(*p);
            if (u >= 32 && u < 127) entry_ += *p;
        }
    }

    void on_key(SDL_Keycode k) {
        if (k == SDLK_BACKSPACE) {
            if (!entry_.empty()) entry_.pop_back();
            return;
        }
        if (k == SDLK_ESCAPE) {
            ctx_.audio.play(20);  // nav blip; cancel keeps the old name
            waiting_ = false;
            return;
        }
        if (k != SDLK_RETURN && k != SDLK_KP_ENTER) return;
        ctx_.audio.play(10);  // accept sting
        // An emptied field keeps the old name: the original's node name is never
        // blank (an absent file draws a random one), and a nameless lobby row is
        // unreadable.
        if (!entry_.empty()) result_ = entry_;
        waiting_ = false;
    }

    void draw() {
        SDL_SetRenderDrawColor(ctx_.sdl, 0, 0, 0, 255);
        SDL_RenderClear(ctx_.sdl);
        // The Options screen's own GLUE backdrop under the prompt, exactly like
        // the two sub-screens above (sub_415CA4's saved-backdrop restore).
        const Sprite& bg = ctx_.assets.frontend_pcx(backdrop_);
        if (bg.tex) {
            SDL_FRect d{0, 0, static_cast<float>(bg.w), static_cast<float>(bg.h)};
            SDL_RenderTexture(ctx_.sdl, bg.tex, nullptr, &d);
        }
        // y = 180 is the CONFIRMED sub_42E938 prompt anchor. sub_4074DC's own y is
        // register-lost in the decompile, so the port reuses the family's one
        // pinned anchor rather than guessing a new one.
        draw_text_entry_dialog(ctx_.sdl, ctx_.front_font, 180.0f,
                               ctx_.assets.getstring(290, "Enter new node name:"), entry_, "Done",
                               "Cancel");
    }

    ScreenContext& ctx_;
    std::string backdrop_;
    std::string entry_;
    std::string result_;
    bool waiting_ = true;
};

// Convert a window-space mouse position into the 640x480 logical space (the same
// pattern the editor canvas uses).
SDL_FPoint to_logical(SDL_Renderer* ren, float wx, float wy) {
    SDL_FPoint p{0, 0};
    SDL_RenderCoordinatesFromWindow(ren, wx, wy, &p.x, &p.y);
    return p;
}

}  // namespace

// The interactive Options screen over a random GLUE<n> backdrop. F1 opens the
// generic *.BM browser — CORRECTED 2026-07-08: sub_4080DC's F1 dispatch calls the
// SAME sub_41431C row 5 opens, not a fixed OPTIONS.BM cut. Music is left
// untouched, because this screen is reached straight from the menu rather than
// the Play handler and there is no confirmed "inherits 1020" citation.
AppInput OptionsScreenRunner::run() {
    OptionsScreen opt(ctx_.assets, ctx_.front_font);
    // The GLUE pick doubles as the backdrop the two modal sub-screens restore
    // each frame (sub_415CA4's saved-backdrop memcpy holds this same picture).
    const std::string glue = pick_glue(state_.setup_lcg, ctx_.values);
    opt.enter(state_.options, glue);
    while (!opt.done()) {
        if (const std::optional<AppInput> exit = pump_events(opt, glue)) return *exit;
        ctx_.audio.update_music();
        // cursor1 blink inputs: the wall clock in seconds plus VALUELST 690's
        // {base, spread} columns (cursor_indicator.hpp's sub_413BD6 model).
        opt.tick(SDL_GetTicks() / 1000ull, static_cast<int>(ctx_.values.column_or(690, 0, 2)),
                 static_cast<int>(ctx_.values.column_or(690, 1, 2)));
        SDL_SetRenderDrawColor(ctx_.sdl, 0, 0, 0, 255);
        SDL_RenderClear(ctx_.sdl);
        opt.draw(ctx_.sdl);
        SDL_RenderPresent(ctx_.sdl);
        SDL_Delay(2);
    }
    commit(opt);
    return result_;
}

std::optional<AppInput> OptionsScreenRunner::pump_events(OptionsScreen& opt,
                                                         const std::string& glue) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
        if (ev.type != SDL_EVENT_KEY_DOWN) continue;
        if (ev.key.key == SDLK_F1) {
            // CONFIRMED (pseudo.c 9298-9299, 9384-9388): SFX 20 fires for ANY real
            // key, F1 INCLUDED, before sub_4080DC dispatches. The earlier port
            // silently skipped it for F1 specifically.
            ctx_.audio.play(20);
            // The browser owns its own loop, so the in-progress edits survive.
            if (HelpBrowserScreen(ctx_).run() == AppInput::Quit) return AppInput::Quit;
            continue;
        }
        if (ev.key.key == SDLK_ESCAPE) result_ = AppInput::Back;
        opt.on_key(ev.key.key, ctx_.audio);
        open_sub_screens(opt, glue);
    }
    return std::nullopt;
}

// The three rows that push a modal sub-screen (§3 rows 15 / 8 / 2). Each owns its
// own loop and resumes the Options screen with its in-progress edits untouched.
void OptionsScreenRunner::open_sub_screens(OptionsScreen& opt, const std::string& glue) {
    if (opt.open_keyremap()) KeyRemapScreenRunner(ctx_, state_).run(glue);
    if (opt.open_scheme_picker()) SchemePickerRunner(ctx_, state_).run(opt, glue);
    // Row 2's name is the port's net identity too — GameApp feeds it to the
    // ADR-0011 lobby as the roster display name.
    if (opt.open_node_name_prompt())
        opt.set_node_name(NodeNamePrompt(ctx_, opt.snapshot().node_name, glue).run());
}

// Write-on-exit semantics (§2's CONFIRMED "held in memory ... only flushed ...
// when the application exits normally"): options.ini is untouched here. The
// gold-forfeit clear sits OUTSIDE the changed() gate and uses the touch flags
// rather than a snapshot diff, because ANY PRESS of those two rows forfeits a
// pending gold player — even a pair of presses back to the original value.
void OptionsScreenRunner::commit(const OptionsScreen& opt) {
    if (opt.gold_forfeiting_row_touched()) state_.gold_player = -1;
    if (!opt.changed()) return;
    state_.options = opt.snapshot();
    state_.team_play = state_.options.team_play;
    state_.conveyor_speed_index = state_.options.conveyor_speed_index;
    state_.options_dirty = true;
}

// The key-remap UI (sub_407B9D). It runs its OWN event pump so raw scancode
// captures and clicks never leak into the Options cursor underneath, and each
// frame re-blits the Options screen's GLUE backdrop (sub_415CA4's saved-backdrop
// restore — the picture, not the rows).
void KeyRemapScreenRunner::run(const std::string& backdrop) {
    KeyRemapScreen remap(ctx_.assets, ctx_.front_font);
    const std::array<KeySet, kKeyboardSets> current{ctx_.keyboard.key_set(0),
                                                    ctx_.keyboard.key_set(1)};
    remap.enter(current, backdrop);
    // sub_431178/sub_431360 bracket: the system cursor yields to the widget
    // library's own 8x8 arrow (drawn by remap.draw()) for this screen only.
    SDL_HideCursor();
    while (!remap.done()) {
        remap.tick(SDL_GetTicks());
        if (!pump_events(remap)) {
            SDL_ShowCursor();
            return;  // window close mid-screen: discard this visit's edits
        }
        // sub_407AD9's raw keyboard-state poll — binds a key already held when
        // the 500 ms arm delay elapses (the event path alone misses it).
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

// False means "leave now, discarding this visit" (the app-level Quit is re-raised
// by the Options pump above).
bool KeyRemapScreenRunner::pump_events(KeyRemapScreen& remap) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_EVENT_QUIT) return false;
        if (ev.type == SDL_EVENT_KEY_DOWN) {
            // F1 (0x13B) -> the generic *.BM help browser, same dispatch as the
            // Options screen's own F1 — but NOT while capturing (F1 must be
            // bindable) or under the NOTE modal (whose key loop just blips).
            if (ev.key.key == SDLK_F1 && !remap.capturing() && !remap.showing_note())
                return HelpBrowserScreen(ctx_).run() != AppInput::Quit;
            remap.on_key(ev.key.key, ev.key.scancode, ctx_.audio);
            continue;
        }
        handle_mouse(remap, ev);
    }
    return true;
}

void KeyRemapScreenRunner::handle_mouse(KeyRemapScreen& remap, const SDL_Event& ev) {
    if (ev.type == SDL_EVENT_MOUSE_MOTION) {
        const SDL_FPoint p = to_logical(ctx_.sdl, ev.motion.x, ev.motion.y);
        remap.on_mouse_move(p.x, p.y);
        return;
    }
    const bool down = ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
    const bool up = ev.type == SDL_EVENT_MOUSE_BUTTON_UP;
    if (!down && !up) return;
    if (ev.button.button != SDL_BUTTON_LEFT) return;
    const SDL_FPoint p = to_logical(ctx_.sdl, ev.button.x, ev.button.y);
    // sub_432998 fires a widget on RELEASE inside it, so only the up edge is
    // audible.
    if (down) remap.on_mouse_down(p.x, p.y);
    if (up) remap.on_mouse_up(p.x, p.y, ctx_.audio);
}

// sub_407582 (§3 row 8) — the SAME routine the editor's "edit an existing scheme"
// path calls, hence the SAME component. The empty-glob case is NOT special-cased
// here: sub_407582 raises its own error box @0x4076CA, so the component owns it
// and both entry points get it from one place.
void SchemePickerRunner::run(OptionsScreen& opt, const std::string& backdrop) {
    SchemeFilePicker picker(ctx_.assets, ctx_.front_font);
    picker.enter(state_.game_dir / "DATA" / "SCHEMES", backdrop);
    while (!picker.done()) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return;
            if (dispatch_list_mouse(ctx_.sdl, ev, picker)) continue;
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
    // sub_407582's write-back @0x40767A-0x4076B8, CORRECTED 2026-07-26: the cut
    // character is 0x3A, a COLON, not a '.', so cutting "BASIC.SCH: Basic
    // Bomberman" at its first ':' keeps the EXTENSION. It still loads — the strip
    // lives in the READER (sub_403EEE) — and the only visible difference is
    // Options row 8, which prints the buffer verbatim.
    std::string name = picker.selected().filename().string();
    for (auto& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    opt.set_scheme_filename(name);
    // The original re-parses byte_4648C4 at the next Play-flow entry; reloading
    // immediately keeps scheme_ and the displayed row in lockstep.
    reload_scheme(state_.scheme, state_.game_dir, name);
}

}  // namespace bomber::game
