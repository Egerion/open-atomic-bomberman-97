#include "bomber/frontend/setup_screen.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "bomber/core/cast.hpp"                  // core::to_underlying
#include "bomber/frontend/campaign_screens.hpp"  // CampaignPickerScreen
#include "bomber/game_util/anim_pace.hpp"        // anim_step_index
#include "bomber/game_util/frontend_util.hpp"    // pick_glue
#include "bomber/game_util/hud_format.hpp"       // fmt_u / fmt_us
#include "bomber/input/input.hpp"                // cycle_slot_input_type / reset_setup_teams
#include "bomber/match/match_factory.hpp"        // scheme_setup_teams
#include "bomber/render/sprites.hpp"             // Sprite, Anim, resolve_sequence
#include "bomber/ui/help_screens.hpp"            // run_help_browser
#include "bomber/ui/dialog_chrome.hpp"           // draw_acknowledge_dialog

namespace bomber::game {

// The PLAYER INPUT TYPE SELECTION screen — sub_410F81 @0x410F81. The RE pins,
// the two corrected TEAM defaults, the campaign trigger and the start guards are
// all in docs/frontend-setup-screens.md.

namespace {

// The screen's VALUELST geometry, read once on entry — a parameter object (§3)
// rather than eighteen consecutive `const float` locals. Column 3 of each id is a
// CLIP WIDTH, never a colour.
struct SetupLayout {
    float hx, hy, hw;              // header 705
    float lx, ly, lys, lw;         // slot rows 710
    float jhx, jhy, jhw;           // joystick heading 715
    float jlx, jly, jlys, jlw;     // joystick rows 720
    float fcx, ffy, ffw;           // footer 790 ("goes on a lot of different screens")
    int blink_base, blink_spread;  // cursor blink base + spread, seconds (690 = {2,2})
};

float column_f(const assets::res::ValueList& values, int id, int column, int fallback) {
    return static_cast<float>(values.column_or(id, column, fallback));
}

SetupLayout read_layout(const assets::res::ValueList& v) {
    return SetupLayout{column_f(v, 705, 0, 40),
                       column_f(v, 705, 1, 140),
                       column_f(v, 705, 3, 200),
                       column_f(v, 710, 0, 70),
                       column_f(v, 710, 1, 170),
                       column_f(v, 710, 2, 24),
                       column_f(v, 710, 3, 150),
                       column_f(v, 715, 0, 300),
                       column_f(v, 715, 1, 140),
                       column_f(v, 715, 3, 170),
                       column_f(v, 720, 0, 320),
                       column_f(v, 720, 1, 170),
                       column_f(v, 720, 2, 24),
                       column_f(v, 720, 3, 320),
                       column_f(v, 790, 0, 320),
                       column_f(v, 790, 1, 440),
                       column_f(v, 790, 3, 300),
                       static_cast<int>(v.column_or(690, 0, 2)),
                       static_cast<int>(v.column_or(690, 1, 2))};
}

// The four injected seams, bundled so the frame loop takes three constructor
// arguments rather than six (§3). `chat` is BORROWED and may be null.
struct SetupSeams {
    CampaignState campaign;  // the 'C'x5 picker's state...
    MatchBackdrop backdrop;  // ...and the frame it composites over
    NetSetupLink net;
    ChatOverlay* chat;
};

// The screen's ten roster rows (dword_46481C).
constexpr int kSetupSlots = 10;

// Start guard 1 (sub_42223E, batch_0x410401.cpp 1148-1168): at least two ACTIVE
// slots, or in TEAM mode at least two DISTINCT team values among the active
// slots — else the game refuses to start.
bool count_ok(const SetupState& s) {
    if (!s.team_play) {
        int active = 0;
        for (int i = 0; i < kSetupSlots; ++i)
            if (s.setup_type[i] != 0) ++active;
        return active >= 2;
    }
    bool seen0 = false, seen1 = false;
    for (int i = 0; i < kSetupSlots; ++i) {
        if (s.setup_type[i] == 0) continue;  // OFF slots don't count
        (s.setup_team[i] ? seen1 : seen0) = true;
    }
    return seen0 && seen1;
}

// A slot the start guards treat as HUMAN: KEYBOARD (2) or JOYSTICK (3). CPU (1),
// OTHER (4) and OFF (0) are excluded.
bool is_human_slot(int type) {
    return type == core::to_underlying(SlotInputType::Keyboard) ||
           type == core::to_underlying(SlotInputType::Joystick);
}

// A human slot's (input type, sub-index) pair, packed into one comparable id.
int controller_id(const SetupState& s, int slot) {
    return s.setup_type[slot] * 256 + s.setup_sub[slot];
}

// Start guard 2 (sub_422085, batch_0x410401.cpp 1172): two ACTIVE HUMAN slots may
// not share the same input type AND sub-index — same keyboard set or same stick.
bool dup_controller(const SetupState& s) {
    std::array<int, kSetupSlots> seen{};
    std::size_t count = 0;
    for (int i = 0; i < kSetupSlots; ++i) {
        if (!is_human_slot(s.setup_type[i])) continue;
        const int id = controller_id(s, i);
        const auto end = seen.begin() + static_cast<std::ptrdiff_t>(count);
        if (std::find(seen.begin(), end, id) != end) return true;
        seen[count++] = id;
    }
    return false;
}

// The screen's frame loop as its own object. A method returning
// std::optional<AppInput> uses nullopt for "keep looping" and a value for
// "run() returns this now".
class SetupLoop {
public:
    SetupLoop(ScreenContext ctx, SetupState state, SetupSeams seams)
        : ctx_(ctx),
          state_(state),
          seams_(seams),
          layout_(read_layout(ctx.values)),
          // pick_glue advances the shared presentation LCG and its draw
          // order/count is observable, so it stays FIRST, before any other work.
          glue_(pick_glue(state.setup_lcg, ctx.values)),
          // Accept debounce (8100/8220-8228): ignore an accept for 1 s after
          // entry so a held Enter from the previous screen can't blast the start.
          accept_after_ms_(SDL_GetTicks() + 1000),
          net_mode_(net_setup_active(seams.net)),
          net_guest_(net_setup_readonly(seams.net)) {}

    AppInput run();

private:
    // --- the three phases of one frame ---
    std::optional<AppInput> pump_events();
    std::optional<AppInput> pump_link();
    void present_frame();

    // --- event dispatch ---
    std::optional<AppInput> handle_event(const SDL_Event& ev);
    std::optional<AppInput> on_key(SDL_Keycode k);
    std::optional<AppInput> on_campaign_key();
    AppInput on_escape();
    std::optional<AppInput> on_accept();
    void on_edit_key(SDL_Keycode k);
    bool edit_denied() const;
    void cycle_slot_right();
    void clear_slot();
    void toggle_slot_team();

    // --- nested loops that composite over this screen's own frame ---
    std::optional<AppInput> run_help_browser();
    std::optional<AppInput> show_error(const std::string& reason);

    // --- drawing ---
    void draw_frame();
    void draw_backdrop();
    void draw_slot_rows();
    void draw_slot_row(int i);
    std::string slot_type_text(int i) const;
    void draw_joystick_pane();
    void draw_footer();
    void draw_cursor();

    ScreenContext ctx_;
    SetupState state_;
    SetupSeams seams_;
    SetupLayout layout_;
    std::string glue_;
    std::uint64_t accept_after_ms_;
    unsigned net_spin_ = 0;  // the [WAIT] prompt's frame-paced spinner phase
    int cursor_ = 0;
    bool net_mode_;
    bool net_guest_;
    bool net_dirty_ = false;  // an edit happened this frame -> re-publish the preview
    bool accepted_ = false;
};

AppInput SetupLoop::run() {
    while (true) {
        if (const std::optional<AppInput> exit = pump_events()) return *exit;
        // The accept path deliberately falls THROUGH one more frame rather than
        // returning from the event pump: the original breaks out of the poll loop
        // and still runs the publish/pump/draw tail below before it leaves.
        if (const std::optional<AppInput> exit = pump_link()) return *exit;
        present_frame();
        if (accepted_) return AppInput::Advance;
    }
}

std::optional<AppInput> SetupLoop::pump_events() {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (const std::optional<AppInput> exit = handle_event(ev)) return *exit;
        if (accepted_) break;
    }
    return std::nullopt;
}

std::optional<AppInput> SetupLoop::handle_event(const SDL_Event& ev) {
    if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
    // The F2 chat overlay gets first refusal (PORT-ONLY): open, it consumes every
    // key; closed, it takes only F2.
    if (seams_.chat != nullptr && seams_.chat->handle_event(ev, ctx_)) return std::nullopt;
    // Hotplug (sub_429628 polls "joystick present" live; SDL3 gives us an event):
    // rescan so the pane and the Right-cycle reflect what is plugged in now.
    if (ev.type == SDL_EVENT_GAMEPAD_ADDED || ev.type == SDL_EVENT_GAMEPAD_REMOVED) {
        ctx_.gamepads.refresh();
        return std::nullopt;
    }
    if (ev.type != SDL_EVENT_KEY_DOWN) return std::nullopt;
    return on_key(ev.key.key);
}

std::optional<AppInput> SetupLoop::on_key(SDL_Keycode k) {
    // 'C' is checked BEFORE the general dispatch so it never also falls into the
    // row-navigation keys below.
    if (k == SDLK_C) return on_campaign_key();
    if (k == SDLK_ESCAPE) return on_escape();
    // Space routes through the SAME accept path as Enter (batch_0x410401.cpp
    // 1148-1182), past the debounce and the two start guards.
    if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) return on_accept();
    ctx_.audio.play(20);  // any real key blips first (sub_427961(20))
    // 0x13B (F1) dispatches the SAME generic *.BM browser every other F1 site
    // opens — one routine, sub_41431C (sub_410F81 15432-15436).
    if (k == SDLK_F1) return run_help_browser();
    on_edit_key(k);
    return std::nullopt;
}

// The hidden campaign-mode trigger (docs/re/campaign.md §4, sub_410F81 pseudo.c
// 15357-15365): five 'C' presses open the *.cam picker. Three things here are
// counter-intuitive and all three were port bugs — the blip comes FIRST, the
// count is CUMULATIVE rather than consecutive, and there is NO accept sting.
// docs/frontend-setup-screens.md "The hidden 'C'x5 campaign trigger".
std::optional<AppInput> SetupLoop::on_campaign_key() {
    ctx_.audio.play(20);
    if (net_mode_) return std::nullopt;  // local only, and SILENTLY so
    if (++state_.campaign_trigger_count != 5) return std::nullopt;
    state_.campaign_trigger_count = 0;
    CampaignPickerScreen(ctx_, seams_.campaign, seams_.backdrop).run();
    return std::nullopt;
}

// Cancelling the Play flow also forfeits any pending gold player. The campaign
// teardown is a faithful CONVENIENCE, not a citation: NO key in the binary clears
// dword_46489C, which is left stale until the next "Play" resets it at entry, so
// clearing it now reaches the identical observable outcome
// (docs/frontend-setup-screens.md "Escape and campaign state").
AppInput SetupLoop::on_escape() {
    ctx_.audio.play(20);
    ctx_.audio.play(10);
    state_.gold_player = -1;
    state_.campaign_active = false;
    state_.campaign_stages.clear();
    state_.campaign_stage_index = 0;
    return AppInput::Back;
}

std::optional<AppInput> SetupLoop::on_accept() {
    ctx_.audio.play(20);  // any-key blip first (sub_427961(20))
    // §7: the HOST owns the screen advance (`sub_40F064(901)`, kind 32); a guest
    // pressing Enter lands on LABEL_159's SFX 40.
    if (net_guest_) {
        ctx_.audio.play(40);
        return std::nullopt;
    }
    if (SDL_GetTicks() < accept_after_ms_) return std::nullopt;  // held-Enter debounce
    // Guard 2 (sub_422085) is checked FIRST, then guard 1 (sub_42223E).
    if (dup_controller(state_))
        return show_error(ctx_.assets.getstring(45, "Two players cannot use the same controls!"));
    if (!count_ok(state_))
        return show_error(state_.team_play
                              ? ctx_.assets.getstring(48, "You need at least two teams!")
                              : ctx_.assets.getstring(46, "You need at least two players!"));
    ctx_.audio.play(10);  // accept sting
    accepted_ = true;
    return std::nullopt;
}

// §7's read-only gate: on THIS screen Up/Down and F1 stay live on a guest and
// every EDIT key buzzes — the LEVEL screen has no such carve-out
// (docs/frontend-setup-screens.md). A slot the wire seat assignment owns is
// likewise frozen (net_setup_link.hpp "SEAT LOCKING").
bool SetupLoop::edit_denied() const {
    return net_guest_ || (net_mode_ && net_setup_slot_locked(seams_.net, cursor_));
}

void SetupLoop::on_edit_key(SDL_Keycode k) {
    if (k == SDLK_UP) {
        cursor_ = (cursor_ + 9) % 10;  // 328
        return;
    }
    if (k == SDLK_DOWN) {
        cursor_ = (cursor_ + 1) % 10;  // 336
        return;
    }
    // 'T' has its OWN gate and must not go through edit_denied(): a LOCKED slot
    // still toggles online, because the team byte travels in the confirmed config
    // and only the input TYPE of a wire seat is fixed.
    if (k == SDLK_T) {  // 'T' team toggle (+84)
        toggle_slot_team();
        return;
    }
    if (k == SDLK_RIGHT) {  // 333 sub_421E80
        cycle_slot_right();
        return;
    }
    if (k == SDLK_LEFT || k == SDLK_0 || k == SDLK_O) clear_slot();  // 331 / '0' / 'o' (111)
}

void SetupLoop::cycle_slot_right() {
    if (edit_denied()) {
        ctx_.audio.play(40);
        return;
    }
    if (!net_mode_) {
        // sub_421E80's wrap order (off -> computer -> keyboard 0/1 -> one step per
        // present stick -> off) is the unit-tested cycle_slot_input_type; this only
        // supplies the live connected-gamepad count.
        cycle_slot_input_type(state_.setup_type[cursor_], state_.setup_sub[cursor_],
                              ctx_.gamepads.count());
        return;
    }
    // Online, a non-seat slot can only be an AI (net_setup_link.hpp "SEAT
    // LOCKING"), so the cycle collapses to the two states that ARE simulable on
    // both peers.
    const int computer = core::to_underlying(SlotInputType::Computer);
    state_.setup_type[cursor_] =
        state_.setup_type[cursor_] == computer ? core::to_underlying(SlotInputType::Off) : computer;
    state_.setup_sub[cursor_] = 0;
    net_dirty_ = true;
}

void SetupLoop::clear_slot() {
    if (edit_denied()) {
        ctx_.audio.play(40);
        return;
    }
    state_.setup_type[cursor_] = core::to_underlying(SlotInputType::Off);  // sub_421E33(i,0,0)
    state_.setup_sub[cursor_] = 0;
    net_dirty_ = true;
}

// batch_0x410401.cpp 1206-1223: only an ACTIVE slot toggles; an OFF slot buzzes
// (SFX 40) and does nothing.
void SetupLoop::toggle_slot_team() {
    if (net_guest_ || state_.setup_type[cursor_] == 0) {
        ctx_.audio.play(40);  // OFF slot, or a guest that may not edit
        return;
    }
    state_.setup_team[cursor_] = state_.setup_team[cursor_] ? 0 : 1;
    net_dirty_ = true;
}

std::optional<AppInput> SetupLoop::run_help_browser() {
    // The shared sub_41431C loop over this screen's own live frame, with the
    // net link + lobby heartbeat pumped every iteration (they must not go
    // silent under the browser). Routing through the template rather than a
    // hand copy is what gives THIS F1 the mouse half back: the local fork
    // never called dispatch_list_mouse, so help from Setup was keyboard-only
    // against sub_42DBCC's mouse-first input model.
    const AppInput r = ::bomber::game::run_help_browser(
        ctx_, [this] { draw_frame(); },
        [this] {
            net_setup_pump(seams_.net);
            if (seams_.chat != nullptr) seams_.chat->pump();
        });
    if (r == AppInput::Quit) return AppInput::Quit;
    return std::nullopt;
}

// The start guards' sub_414340 acknowledge box over the frozen setup frame — the
// reason line over getstring(96) in byte_49A390 DARK RED (164,0,0), NOT white.
// Quit if the window closed while it was up, else nullopt (the screen stays up).
std::optional<AppInput> SetupLoop::show_error(const std::string& reason) {
    const std::string sub = ctx_.assets.getstring(96, "Cannot start the game!");
    const std::string ok = ctx_.assets.getstring(27, " Ok ");
    while (true) {
        SDL_Event mev;
        while (SDL_PollEvent(&mev)) {
            if (mev.type == SDL_EVENT_QUIT) return AppInput::Quit;
            if (mev.type != SDL_EVENT_KEY_DOWN) continue;
            ctx_.audio.play(20);  // nav blip on any key
            if (mev.key.key == SDLK_RETURN || mev.key.key == SDLK_KP_ENTER ||
                mev.key.key == SDLK_SPACE || mev.key.key == SDLK_ESCAPE)
                return std::nullopt;
        }
        // Keep the setup link alive under the modal: the guest fails after
        // timeout_ms of silence and the matchmaker reaps a silent member, so a
        // host sitting on a start-guard error must still be re-broadcasting.
        net_setup_pump(seams_.net);
        if (seams_.chat != nullptr) seams_.chat->pump();
        ctx_.audio.update_music();
        draw_frame();
        draw_acknowledge_dialog(DialogPen{ctx_.sdl, ctx_.front_font},
                                &ctx_.assets.frontend_pcx("WINZ"),
                                AcknowledgeLabels{reason, sub, ok}, AcknowledgeStyle{{164, 0, 0}});
        SDL_RenderPresent(ctx_.sdl);
        SDL_Delay(2);
    }
}

// The HOST broadcasts each change (§7: roster kind 40, team kind 58) — on the
// edit, not every frame, so `revision` only moves when something actually did.
// `rounds = 0` is the "still on the roster screen" sentinel (net_setup_link.hpp);
// the level travels from the LEVEL & ROUNDS screen, where the host picks it.
std::optional<AppInput> SetupLoop::pump_link() {
    const LocalRoster roster{state_.setup_type, state_.setup_sub, state_.setup_team,
                             state_.team_play};
    if (net_mode_ && !net_guest_ && (net_dirty_ || !net_setup_has_preview(seams_.net))) {
        net_setup_publish(seams_.net, roster,
                          LevelPreview{/*level=*/-1, /*name=*/std::string(), /*rounds=*/0});
        net_dirty_ = false;
    }
    net_setup_pump(seams_.net);
    if (seams_.chat != nullptr) seams_.chat->pump();
    if (!net_guest_) return std::nullopt;
    // Read-only: the displayed roster IS the host's newest preview.
    net_setup_apply_roster(seams_.net, roster);
    // The host moved on, or confirmed outright and we already hold the config.
    if (net_setup_final(seams_.net) || net_setup_on_level_screen(seams_.net))
        return AppInput::Advance;
    // Timed out / the host vanished. The caller reads net_setup_failed() to tell
    // this Back from an Esc.
    if (net_setup_failed(seams_.net)) return AppInput::Back;
    return std::nullopt;
}

void SetupLoop::present_frame() {
    ctx_.audio.update_music();
    draw_frame();
    // Before the first preview lands there is nothing of the host's to show, so
    // the guest sits on `sub_42B47D`'s own [WAIT] prompt (§6) over this screen's
    // backdrop — the closest RE'd composition to `sub_410F81`'s head, where a
    // guest likewise BLOCKS in a pump loop until the host speaks (§7). Nothing new
    // is drawn: the pinned prompt, the pinned anchor, the real getstring(80).
    if (net_guest_ && !net_setup_has_preview(seams_.net)) draw_net_wait_prompt(ctx_, net_spin_);
    if (seams_.chat != nullptr) seams_.chat->draw(ctx_);  // last: the panel sits on top
    SDL_RenderPresent(ctx_.sdl);
    SDL_Delay(2);
}

// One frame of the screen (sub_410F81's per-frame body, pseudo.c 15146-15267):
// backdrop, header, slot rows, joystick pane, footer — all text through the
// 4-pass-outline primitive (sub_41696C) — and the bomber-dude cursor LAST (the
// original queues sprites and flushes them after the text, so the cursor lands on
// top).
void SetupLoop::draw_frame() {
    draw_backdrop();
    // Header (msg 50): white ink / black outline (byte_49D38F over byte_495390[0],
    // pseudo.c 15154-15160).
    ctx_.front_font.draw_outlined(ctx_.sdl, ctx_.assets.getstring(50, "Available players:"),
                                  SDL_FPoint{layout_.hx, layout_.hy},
                                  OutlinedTextStyle{{255, 255, 255}, {}, layout_.hw});
    draw_slot_rows();
    draw_joystick_pane();
    draw_footer();
    draw_cursor();
}

void SetupLoop::draw_backdrop() {
    SDL_SetRenderDrawColor(ctx_.sdl, 0, 0, 0, 255);
    SDL_RenderClear(ctx_.sdl);
    const Sprite& bg = ctx_.assets.frontend_pcx(glue_);
    if (bg.tex == nullptr) return;
    SDL_FRect dst{0.0f, 0.0f, static_cast<float>(bg.w), static_cast<float>(bg.h)};
    SDL_RenderTexture(ctx_.sdl, bg.tex, nullptr, &dst);
}

void SetupLoop::draw_slot_rows() {
    for (int i = 0; i < 10; ++i) draw_slot_row(i);
}

// Each slot's input type via getstring(220..224). A flat dispatch over the
// original's own five type codes — §8's "leave the switches the 1997 binary
// invented alone".
std::string SetupLoop::slot_type_text(int i) const {
    switch (state_.setup_type[i]) {
        case 1: return ctx_.assets.getstring(221, "COMPUTER");
        case 2: return fmt_u(ctx_.assets.getstring(222, "KEYBOARD %u"), state_.setup_sub[i]);
        case 3: return fmt_u(ctx_.assets.getstring(223, "JOYSTICK %u"), state_.setup_sub[i]);
        case 4: return ctx_.assets.getstring(224, "OTHER");
        default: return ctx_.assets.getstring(220, "OFF");
    }
}

// One combined splice (msg 51) in the slot's authentic colour, NEVER the team
// override, black-outlined except index 1 (the BLACK player, which gets white).
// The team marker is drawn for EVERY slot whenever Team Play is on — gated on the
// GLOBAL dword_464964, not this slot's team byte — and is CONFIRMED unformatted,
// because the COLOUR alone tells the teams apart
// (docs/frontend-setup-screens.md "Slot rows").
void SetupLoop::draw_slot_row(int i) {
    const std::string line =
        fmt_us(ctx_.assets.getstring(51, "Player %u: %s"), i + 1, slot_type_text(i));
    std::uint8_t sc[3];
    ctx_.assets.slot_color(i, sc);
    const Uint8 oc = i == 1 ? 255 : 0;
    const float row_y = layout_.ly + layout_.lys * static_cast<float>(i);
    const float lx_end = ctx_.front_font.draw_outlined(
        ctx_.sdl, line, SDL_FPoint{layout_.lx, row_y},
        OutlinedTextStyle{{sc[0], sc[1], sc[2]}, {oc, oc, oc}, layout_.lw});
    if (!state_.team_play) return;
    const std::string marker = "  " + ctx_.assets.getstring(230, "TEAM");
    const bool team1 = state_.setup_team[i] != 0;  // sub_4141F8's `a1 ?` branch
    ctx_.front_font.draw_outlined(ctx_.sdl, marker, SDL_FPoint{lx_end, row_y},
                                  OutlinedTextStyle{team1 ? Rgb{252, 80, 80} : Rgb{255, 255, 255}});
}

// Joystick pane (getvalue 715/720): heading msg 40, then one line per detected
// stick (msg 41, the stick's own name from sub_429A61(i)) or, if none, the single
// msg-42 line. ALL of it plain white ink over a black outline (pseudo.c
// 15227-15263) — the old grey tints were invented.
void SetupLoop::draw_joystick_pane() {
    ctx_.front_font.draw_outlined(ctx_.sdl, ctx_.assets.getstring(40, "JOYSTICKS"),
                                  SDL_FPoint{layout_.jhx, layout_.jhy},
                                  OutlinedTextStyle{{255, 255, 255}, {}, layout_.jhw});
    const int joy_count = ctx_.gamepads.count();
    if (joy_count == 0) {
        ctx_.front_font.draw_outlined(ctx_.sdl, ctx_.assets.getstring(42, "none"),
                                      SDL_FPoint{layout_.jlx, layout_.jly},
                                      OutlinedTextStyle{{255, 255, 255}, {}, layout_.jlw});
        return;
    }
    for (int j = 0; j < joy_count; ++j) {
        const std::string jline =
            fmt_us(ctx_.assets.getstring(41, "Joy %u - %s"), j, ctx_.gamepads.name(j));
        ctx_.front_font.draw_outlined(
            ctx_.sdl, jline,
            SDL_FPoint{layout_.jlx, layout_.jly + layout_.jlys * static_cast<float>(j)},
            OutlinedTextStyle{{255, 255, 255}, {}, layout_.jlw});
    }
}

// Footer (sub_413FB9 -> getstring(330), local play only): centred on x via
// sub_4172BA's `x = cx - (w+2)/2`, cyan ink byte_497F8F (96,252,252).
void SetupLoop::draw_footer() {
    const std::string help = ctx_.assets.getstring(330, "Press F1 for help");
    const float help_w = static_cast<float>(ctx_.front_font.measure(help));
    ctx_.front_font.draw_outlined(ctx_.sdl, help,
                                  SDL_FPoint{layout_.fcx - (help_w + 2.0f) / 2.0f, layout_.ffy},
                                  OutlinedTextStyle{{96, 252, 252}, {}, layout_.ffw});
}

// The row cursor (sub_413BD6): MISC.ANI "cursor1" at (getvalue(710) - 15,
// row_y + 16). This screen alone uses -15; the options/level screens use -20. The
// +16 is pinned EMPIRICALLY from a native capture, because the decompile loses it
// to register mangling at every call site (docs/frontend-setup-screens.md).
void SetupLoop::draw_cursor() {
    const Anim cur = resolve_sequence(ctx_.assets.misc(), "cursor1");
    if (cur.steps.empty()) return;
    const std::size_t st = ctx_.cursor_blink.step(SDL_GetTicks() / 1000ull, cur.steps.size(),
                                                  layout_.blink_base, layout_.blink_spread);
    const Sprite& sp = cur.steps[anim_step_index(st, cur.steps.size())];
    if (sp.tex == nullptr) return;
    SDL_FRect d{
        layout_.lx - 15.0f - static_cast<float>(sp.hx),
        layout_.ly + layout_.lys * static_cast<float>(cursor_) + 16.0f - static_cast<float>(sp.hy),
        static_cast<float>(sp.w), static_cast<float>(sp.h)};
    SDL_RenderTexture(ctx_.sdl, sp.tex, nullptr, &d);
}

}  // namespace

// Screen 1 of the pre-match flow (sub_410F81 @0x410F81). Presentation only.
//
// NO music call here, deliberately: the 1020 track is already running. Restarting
// it restarted WIN.RSS from the top whenever the goldman wheel ran first — the
// wheel had the identical bug (docs/re/sound-engine.md §9).
//
// The TWO team seeds below run in the original's own order and both were
// corrected against earlier readings: getting the first wrong made Team Play put
// everyone on the same side and clinch on tick 0. docs/frontend-setup-screens.md
// "The TEAM default, corrected twice".
AppInput SetupScreen::run() {
    reset_setup_teams(state_.setup_team);
    match::scheme_setup_teams(state_.scheme, state_.setup_team);
    return SetupLoop(ctx_, state_, SetupSeams{campaign_, backdrop_, net_, chat_}).run();
}

}  // namespace bomber::game
