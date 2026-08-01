#include "bomber/frontend/menu_screen.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <iterator>  // std::size (kMenuCount)
#include <optional>
#include <string>

#include "bomber/editor/editor_screen_runner.hpp"     // EditorRunner + EditorEditState
#include "bomber/frontend/debug_info_screen.hpp"      // DebugInfoScreen
#include "bomber/frontend/video_settings_screen.hpp"  // VideoSettingsScreen + VideoToggleRefs
#include "bomber/game_util/anim_pace.hpp"             // anim_step_index
#include "bomber/input/input.hpp"  // attract_computer_count/fill_attract_roster/attract_stage_pick
#include "bomber/platform/frame_clock.hpp"
#include "bomber/render/sprites.hpp"    // Sprite, Anim, resolve_sequence
#include "bomber/ui/dialog_chrome.hpp"  // draw_confirm_dialog
#include "bomber/ui/help_screens.hpp"   // HelpBrowserScreen

// The navigable main menu (sub_42B9CE @0x42B9CE). Row dispatch, the per-visit
// music re-arm, the quit-confirm chrome, the hidden triggers, attract and the
// cursor anchor are all in docs/frontend-menu.md.

namespace bomber::game {

namespace {

// CONFIRMED front-end SOUNDLST ids: menu music 1010 (sub_42741E in sub_42B9CE),
// started on every menu entry, and the quit sting group base 2600 (sub_427BFB in
// sub_412987). The sleep is sub_412987's own Sleep(0xFA0), so the sting is
// audible rather than cut off by window teardown.
constexpr int kMenuMusicId = 1010;
constexpr int kQuitStingLo = 2600;
constexpr std::uint32_t kQuitStingSleepMs = 4000;

// The ATTRACT idle timeout — CONFIRMED getvalue(92) = 30, gated `> 5` (the
// VALUELST legend: <= 5 disables attract entirely). The live column is read at
// run time, so a modified install is honoured; this is only the fallback.
constexpr std::int64_t kAttractIdleFallbackS = 30;
constexpr std::int64_t kAttractIdleMinS = 5;

struct MenuItem {
    AppInput action;  // resolved when Enter selects this row
    bool live;        // false = a documented stub row (no handler yet, inert)
};

// Seven rows in the ORIGINAL's selection-index order, so the cursor anchor lands
// on the labels baked into MAINMENU.PCX. Row 5's entry is unused: the help
// browser is dispatched inline, staying inside the loop like the original.
constexpr MenuItem kMenuItems[] = {
    {AppInput::StartMatch, true},   // 0 Play
    {AppInput::OpenNetHost, true},  // 1 START NET GAME
    {AppInput::OpenNetJoin, true},  // 2 JOIN NET GAME
    {AppInput::OpenOptions, true},  // 3 Options (sub_4080DC)
    {AppInput::OpenCredits, true},  // 4 Credits
    {AppInput::Advance, false},     // 5 Help browser (handled inline, entry unused)
    {AppInput::Quit, true},         // 6 Quit
};
constexpr int kMenuCount = static_cast<int>(std::size(kMenuItems));
constexpr int kOptionsRow = 3;
constexpr int kHelpRow = 5;
constexpr int kQuitRow = 6;

// Cursor anchor over MAINMENU.PCX — CONFIRMED getvalue(700/701/702), read live
// from VALUELST row 700's columns. These fallbacks are that install's values.
constexpr int kMenuCursorXFallback = 332;
constexpr int kMenuCursorYFallback = 140;
constexpr int kMenuCursorStepFallback = 38;

// The menu's frame loop as its own object, like the rest of this package. A
// method returning std::optional<AppInput> uses nullopt for "keep looping".
class MenuLoop {
public:
    MenuLoop(ScreenContext ctx, MenuState state)
        : ctx_(ctx),
          state_(state),
          idle_s_(ctx.values.at_or(92, kAttractIdleFallbackS)),
          attract_enabled_(idle_s_ > kAttractIdleMinS),
          frame_clock_(ctx.window) {}

    AppInput run();

private:
    std::optional<AppInput> pump_events();
    std::optional<AppInput> handle_event(const SDL_Event& ev);
    void inject_pad_key(const SDL_Event& ev);
    std::optional<AppInput> on_key(const SDL_Event& ev);
    std::optional<AppInput> on_quit_confirm_key(SDL_Keycode k);
    std::optional<AppInput> on_hotkey(const SDL_Event& ev);
    std::optional<AppInput> on_ctrl_e();
    std::optional<AppInput> on_nav_key(const SDL_Event& ev);
    std::optional<AppInput> on_accept();
    std::optional<AppInput> open_help_browser();
    std::optional<AppInput> check_attract_idle();
    void roll_attract_match();
    // Returning to the menu re-arms the music flag (MENU.RSS reloads from sample
    // 0) and reseeds the idle clock — time in a sub-screen is not idle time.
    void re_enter();

    void draw_frame();
    void draw_backdrop();
    void draw_cursor();
    void draw_quit_confirm();

    ScreenContext ctx_;
    MenuState state_;
    std::int64_t idle_s_;
    bool attract_enabled_;
    platform::FrameClock frame_clock_;
    std::uint64_t frame_ = 0;
    bool quit_confirm_ = false;
};

AppInput MenuLoop::run() {
    ctx_.audio.start_music(kMenuMusicId);
    state_.menu_idle_since_ms = SDL_GetTicks();
    while (true) {
        if (const std::optional<AppInput> exit = pump_events()) return *exit;
        if (const std::optional<AppInput> exit = check_attract_idle()) return *exit;
        ctx_.audio.update_music();
        draw_frame();
        SDL_RenderPresent(ctx_.sdl);
        frame_clock_.pace();
    }
}

std::optional<AppInput> MenuLoop::pump_events() {
    SDL_Event ev;
    while (SDL_PollEvent(&ev))
        if (const std::optional<AppInput> exit = handle_event(ev)) return *exit;
    return std::nullopt;
}

std::optional<AppInput> MenuLoop::handle_event(const SDL_Event& ev) {
    if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
    // ANY real input resets the idle clock — key, mouse button or pad button,
    // mirroring the original's "any real key" blip path plus this port's own
    // mouse/pad surfaces (sub_42B9CE only had a keyboard).
    if (ev.type == SDL_EVENT_KEY_DOWN || ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
        ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)
        state_.menu_idle_since_ms = SDL_GetTicks();
    if (ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) inject_pad_key(ev);
    if (ev.type != SDL_EVENT_KEY_DOWN) return std::nullopt;
    return on_key(ev);
}

// sub_4102B7's pad->key synthesis, re-injected as SDL events so every key path
// (blips, dialog, rows) is shared rather than duplicated.
void MenuLoop::inject_pad_key(const SDL_Event& ev) {
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

std::optional<AppInput> MenuLoop::on_key(const SDL_Event& ev) {
    // F10 (PORT-ONLY Video Settings) is ignored while the modal is up, but it
    // still blips — an unmapped key clicking is the original's behaviour.
    if (!quit_confirm_ && ev.key.key == SDLK_F10) {
        ctx_.audio.play(20);
        VideoSettingsScreen(ctx_, {.uncap_fps = &state_.uncap_fps,
                                   .native_cadence = &state_.native_cadence,
                                   .show_fps = &state_.show_fps,
                                   .soft_scaling = &state_.soft_scaling,
                                   .options_dirty = &state_.options_dirty})
            .run();
        return std::nullopt;
    }
    // Modal: no other input reaches the rows below.
    if (quit_confirm_) return on_quit_confirm_key(ev.key.key);
    if (ev.key.key == SDLK_E && (ev.key.mod & SDL_KMOD_CTRL) != 0) return on_ctrl_e();
    state_.editor_trigger_count = 0;  // any other key resets the Ctrl+E counter
    if (const std::optional<AppInput> exit = on_hotkey(ev)) return *exit;
    return on_nav_key(ev);
}

// sub_41456C's key loop: any real key blips; Y/y/Enter/Space accept, N/n/Q/q/Esc
// cancel, everything else leaves the dialog up. It resolves SILENTLY — no accept
// sting on either answer.
std::optional<AppInput> MenuLoop::on_quit_confirm_key(SDL_Keycode k) {
    ctx_.audio.play(20);
    const bool yes = k == SDLK_Y || k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE;
    if (yes) {
        // On Yes, FREE the music first — MENU.RSS must not keep looping under
        // the sting — then fire it and sleep so it is audible.
        ctx_.audio.stop_music();
        ctx_.audio.play_sting(kQuitStingLo);  // sub_427BFB(2600)
        SDL_Delay(kQuitStingSleepMs);
        return AppInput::Quit;
    }
    const bool no = k == SDLK_N || k == SDLK_Q || k == SDLK_ESCAPE;
    if (!no) return std::nullopt;
    // The cancel path exits through sub_42B9CE's OUTER loop: cursor home to row 0
    // and MENU.RSS reloaded from sample 0.
    quit_confirm_ = false;
    state_.menu_index = 0;
    ctx_.audio.start_music(kMenuMusicId);
    return std::nullopt;
}

// The hidden scheme-editor trigger (docs/re/results-and-options.md §5): the 6th
// CONSECUTIVE Ctrl+E fires. Ctrl+E itself never falls into the row switch.
std::optional<AppInput> MenuLoop::on_ctrl_e() {
    ctx_.audio.play(20);  // the any-real-key blip fires for each press (30787-30790)
    if (++state_.editor_trigger_count <= 5) return std::nullopt;
    state_.editor_trigger_count = 0;
    ctx_.audio.play(10);  // accept sting (SFX 10), §5
    EditorRunner(ctx_, EditorEditState{.setup_lcg = state_.setup_lcg,
                                       .scheme = state_.scheme,
                                       .game_dir = state_.game_dir,
                                       .scheme_path = state_.scheme_path})
        .run();
    re_enter();
    return std::nullopt;
}

// Menu hotkeys (sub_42B9CE's raw-code dispatch, every one riding the any-key
// blip 20 first) — docs/frontend-menu.md "Hidden triggers".
std::optional<AppInput> MenuLoop::on_hotkey(const SDL_Event& ev) {
    const bool alt = (ev.key.mod & SDL_KMOD_ALT) != 0;
    if (ev.key.key == SDLK_Q && (ev.key.mod & SDL_KMOD_CTRL) != 0) {
        ctx_.audio.play(20);
        ctx_.audio.play(10);
        state_.menu_index = kQuitRow;
        quit_confirm_ = true;
        return std::nullopt;
    }
    if (alt && ev.key.key == SDLK_O) {
        ctx_.audio.play(20);
        ctx_.audio.play(10);
        state_.menu_index = 0;  // case 3's fall-through homes the cursor (30910-30912)
        return kMenuItems[kOptionsRow].action;
    }
    if (ev.key.key == SDLK_F1) {
        ctx_.audio.play(20);
        ctx_.audio.play(10);
        state_.menu_index = kHelpRow;
        return open_help_browser();
    }
    if (alt && ev.key.key == SDLK_A) {
        ctx_.audio.play(20);
        state_.menu_index = 0;  // the attract path homes the selection too (30894)
        roll_attract_match();
        return AppInput::StartMatch;
    }
    if (alt && ev.key.key == SDLK_D) {
        ctx_.audio.play(20);
        if (DebugInfoScreen(ctx_).run() == AppInput::Quit) return AppInput::Quit;
        state_.menu_idle_since_ms = SDL_GetTicks();  // debug-window time is not idle
    }
    return std::nullopt;
}

std::optional<AppInput> MenuLoop::on_nav_key(const SDL_Event& ev) {
    const SDL_Keycode k = ev.key.key;
    if (k == SDLK_UP || k == SDLK_W) {
        state_.menu_index = (state_.menu_index + kMenuCount - 1) % kMenuCount;
        ctx_.audio.play(20);  // nav blip (SOUNDLST 20, sub_427961(20))
        return std::nullopt;
    }
    if (k == SDLK_DOWN || k == SDLK_S) {
        state_.menu_index = (state_.menu_index + 1) % kMenuCount;
        ctx_.audio.play(20);
        return std::nullopt;
    }
    if (k == SDLK_ESCAPE) {
        // Escape does NOT quit: it blips, plays the accept sting, selects row 6
        // and pops the confirm dialog — docs/frontend-menu.md.
        ctx_.audio.play(20);
        ctx_.audio.play(10);
        state_.menu_index = kQuitRow;
        quit_confirm_ = true;
        return std::nullopt;
    }
    if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) return on_accept();
    // The any-real-key blip fires for EVERY key sub_42B9CE reads, mapped or not
    // (30787-30790) — an unbound letter still clicks. Gated on the press edge so
    // SDL's key repeats don't buzz.
    if (!ev.key.repeat) ctx_.audio.play(20);
    return std::nullopt;
}

std::optional<AppInput> MenuLoop::on_accept() {
    ctx_.audio.play(20);  // any-real-key blip
    ctx_.audio.play(10);  // accept sting (SOUNDLST 10, menuexit)
    if (state_.menu_index == kHelpRow) return open_help_browser();
    // A row we have not built yet still plays the accept sting to stay faithful,
    // but has no leaf to jump to, so it stays put.
    if (!kMenuItems[state_.menu_index].live) return std::nullopt;
    const AppInput sel = kMenuItems[state_.menu_index].action;
    // Row 3 (Options) is one of the rows whose fall-through resets the cursor to
    // row 0 for the next menu visit (30910-30912).
    if (state_.menu_index == kOptionsRow) state_.menu_index = 0;
    // Quit from the row is the SAME sub_412987 dispatch Escape reaches, so it
    // pops the SAME confirm dialog rather than quitting outright.
    if (sel == AppInput::Quit) {
        quit_confirm_ = true;
        return std::nullopt;
    }
    return sel;  // a CUT, no wipe — HEADWIPE.ANI is dead art
}

std::optional<AppInput> MenuLoop::open_help_browser() {
    if (HelpBrowserScreen(ctx_).run() == AppInput::Quit) return AppInput::Quit;
    re_enter();
    return std::nullopt;
}

void MenuLoop::re_enter() {
    ctx_.audio.start_music(kMenuMusicId);
    state_.menu_idle_since_ms = SDL_GetTicks();
}

// Once the idle clock exceeds getvalue(92) seconds, fire the SAME Play dispatch a
// real Enter-on-row-0 would. Held off while the quit-confirm modal is up: it is
// modal in the original too, and a demo match must not yank it away mid-decision.
std::optional<AppInput> MenuLoop::check_attract_idle() {
    if (!attract_enabled_ || quit_confirm_) return std::nullopt;
    const std::uint64_t idle_ms = static_cast<std::uint64_t>(idle_s_) * 1000;
    if (SDL_GetTicks() - state_.menu_idle_since_ms < idle_ms) return std::nullopt;
    state_.menu_index = 0;  // the attract path homes the cursor (30894)
    roll_attract_match();
    return AppInput::StartMatch;
}

// Attract-mode entry — sub_4224E2's save + sub_410F81's attract branch
// (docs/frontend-menu.md "Attract").
void MenuLoop::roll_attract_match() {
    state_.attract_saved.type = state_.setup_type;
    state_.attract_saved.sub = state_.setup_sub;
    state_.attract_saved.team = state_.setup_team;
    state_.attract_saved.level = state_.selected_level;
    state_.attract_saved.team_play = state_.team_play;

    state_.attract = true;  // dword_464938 = 1
    state_.team_play = false;

    // Two presentation-LCG rolls (never State::rng), advanced in the doc's read
    // order — roster count first, the level right after (sub_410F81).
    state_.attract_lcg = state_.attract_lcg * 1664525u + 1013904223u;
    const int computer_count = attract_computer_count(state_.attract_lcg >> 16);
    fill_attract_roster(computer_count, state_.setup_type, state_.setup_sub, state_.setup_team);

    state_.attract_lcg = state_.attract_lcg * 1664525u + 1013904223u;
    const int level_count = static_cast<int>(ctx_.values.column_or(35, 0, 11));  // getvalue(35)
    state_.selected_level = attract_stage_pick(state_.attract_lcg >> 16, level_count);
}

void MenuLoop::draw_frame() {
    draw_backdrop();
    draw_cursor();
    draw_quit_confirm();
}

void MenuLoop::draw_backdrop() {
    SDL_SetRenderDrawColor(ctx_.sdl, 0, 0, 0, 255);
    SDL_RenderClear(ctx_.sdl);
    const Sprite& bg = ctx_.assets.frontend_pcx("MAINMENU");
    if (bg.tex) {
        SDL_FRect d{0, 0, static_cast<float>(bg.w), static_cast<float>(bg.h)};
        SDL_RenderTexture(ctx_.sdl, bg.tex, nullptr, &d);
    }
    // "V1.0" every menu frame (pseudo.c 30779) in the general grey — a literal
    // hardcoded in the binary (aV10), not a MESSAGES.TXT entry.
    ctx_.front_font.draw_outlined(ctx_.sdl, "V1.0", 0, 0, 168, 168, 164, 0, 0, 0, 50.0f);
}

// The "bomb trigger green" cursor at the CONFIRMED VALUELST 700 anchor. Its phase
// is HELD while the confirm dialog is up, because that dialog is modal in the
// original and the frame under it is frozen.
void MenuLoop::draw_cursor() {
    if (!quit_confirm_) ++frame_;
    const int cx = static_cast<int>(ctx_.values.column_or(700, 0, kMenuCursorXFallback));
    const int cy0 = static_cast<int>(ctx_.values.column_or(700, 1, kMenuCursorYFallback));
    const int cstep = static_cast<int>(ctx_.values.column_or(700, 2, kMenuCursorStepFallback));
    const int cy = cy0 + state_.menu_index * cstep;
    const Anim cur = resolve_sequence(ctx_.assets.trigbomb(-1), "bomb trigger green");
    if (cur.steps.empty()) {
        const Uint8 pulse = static_cast<Uint8>(90 + 60 * ((frame_ / 8) % 2));
        SDL_FRect bar{static_cast<float>(cx), static_cast<float>(cy), 240.0f, 22.0f};
        SDL_SetRenderDrawBlendMode(ctx_.sdl, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ctx_.sdl, 255, 220, 60, pulse);
        SDL_RenderFillRect(ctx_.sdl, &bar);
        return;
    }
    const Sprite& sp = cur.steps[anim_step_index(frame_, cur.steps.size())];
    if (sp.tex == nullptr) return;
    SDL_FRect d{static_cast<float>(cx - sp.hx), static_cast<float>(cy - sp.hy),
                static_cast<float>(sp.w), static_cast<float>(sp.h)};
    SDL_RenderTexture(ctx_.sdl, sp.tex, nullptr, &d);
}

// The Quit confirm modal (sub_412987 -> sub_41456C): the WINZ.PCX 9-patch chrome,
// dark-red prompt over a PHOTO-DERIVED gold outline — the one dialog that
// overrides the default black. docs/frontend-menu.md "The quit dialog's chrome".
void MenuLoop::draw_quit_confirm() {
    if (!quit_confirm_) return;
    const std::string prompt = ctx_.assets.getstring(10, "Are you sure you want to exit?");
    const std::string yes_label = ctx_.assets.getstring(26, " Yes ");
    const std::string no_label = ctx_.assets.getstring(25, " No ");
    draw_confirm_dialog(ctx_.sdl, ctx_.front_font, &ctx_.assets.frontend_pcx("WINZ"), prompt, "",
                        yes_label, no_label, 164, 0, 0, 252, 248, 88);
}

}  // namespace

AppInput MenuScreen::run() {
    return MenuLoop(ctx_, state_).run();
}

}  // namespace bomber::game
