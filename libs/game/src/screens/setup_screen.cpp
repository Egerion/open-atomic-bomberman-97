#include "bomber/game/screens/setup_screen.hpp"

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "bomber/core/cast.hpp"                      // core::to_underlying
#include "bomber/game/anim_pace.hpp"                 // anim_step_index
#include "bomber/game/bmscreen.hpp"                  // HelpBrowser
#include "bomber/game/dialog_chrome.hpp"             // draw_acknowledge_dialog
#include "bomber/game/frontend_util.hpp"             // pick_glue
#include "bomber/game/hud_format.hpp"                // fmt_u / fmt_us
#include "bomber/game/input.hpp"                     // cycle_slot_input_type / reset_setup_teams
#include "bomber/game/screens/campaign_screens.hpp"  // CampaignPickerScreen
#include "bomber/game/sprites.hpp"                   // Sprite, Anim, resolve_sequence
#include "bomber/match/match_factory.hpp"            // scheme_setup_teams

namespace bomber::game {

// The Play-handler music (sub_42A3F6, docs/re/in-match-shell.md §2): id 1020
// (0x3FC, "win" in SOUNDLST) is the SETUP-SCREENS backdrop track, not victory
// music. This screen and the LEVEL & ROUNDS screen both merely INHERIT it, so
// the constant is not needed here at all — game_app.cpp owns kWinMusicId and
// starts it once per Play entry (docs/re/sound-engine.md §9).

namespace {

// The screen's VALUELST-driven geometry, read once on entry. A parameter object
// (§3) rather than the eighteen consecutive `const float` locals that used to
// open run(): each draw helper below takes the whole layout instead of growing a
// six-anchor signature.
//
// Layout ids (VALUELST X,Y,YS,clip -> consecutive getvalue columns): header 705,
// slot list 710, joystick pane heading 715, joystick pane list 720, footer 790,
// cursor blink 690. Column 3 of each row is the CLIP WIDTH handed to the text
// primitive (sub_41696C's max-width arg; setup-screens.md's earlier "colour"
// label for this column was wrong — colour never comes from VALUELST here).
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

// The four injected seams SetupScreen's header names, bundled so the frame loop
// below takes three constructor arguments rather than six (§3). All four are
// cheap reference/pointer value types; `chat` is BORROWED and may be null.
struct SetupSeams {
    CampaignState campaign;  // the 'C'x5 picker's state...
    MatchBackdrop backdrop;  // ...and the frame it composites over
    NetSetupLink net;
    ChatOverlay* chat;
};

// Start guard 1 (sub_42223E, batch_0x410401.cpp 1148-1168): at least two ACTIVE
// slots, or in TEAM mode at least two DISTINCT team values among the active
// slots — else the game refuses to start.
bool count_ok(const SetupState& s) {
    if (!s.team_play) {
        int active = 0;
        for (int i = 0; i < 10; ++i)
            if (s.setup_type[i] != 0) ++active;
        return active >= 2;
    }
    bool seen0 = false, seen1 = false;
    for (int i = 0; i < 10; ++i) {
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

// Start guard 2 (sub_422085, batch_0x410401.cpp 1172): two ACTIVE HUMAN slots may
// not share the same input type AND sub-index — same keyboard set or same stick.
bool dup_controller(const SetupState& s) {
    for (int i = 0; i < 10; ++i) {
        if (!is_human_slot(s.setup_type[i])) continue;
        for (int j = i + 1; j < 10; ++j) {
            if (!is_human_slot(s.setup_type[j])) continue;
            if (s.setup_type[i] == s.setup_type[j] && s.setup_sub[i] == s.setup_sub[j]) return true;
        }
    }
    return false;
}

// The screen's frame loop, as its own object — the shape the netplay extraction
// established. The per-run state that used to be captured by two 100-line
// lambdas (the layout, the cursor row, the debounce deadline, the publish flag)
// becomes members, so each step below is a named method a reader takes one at a
// time. A method returning `std::optional<AppInput>` uses nullopt for "keep
// looping" and a value for "run() returns this now".
class SetupLoop {
public:
    SetupLoop(ScreenContext ctx, SetupState state, SetupSeams seams)
        : ctx_(ctx),
          state_(state),
          seams_(seams),
          layout_(read_layout(ctx.values)),
          // pick_glue advances the shared presentation LCG and its draw
          // order/count is observable (setup_state.hpp), so it stays exactly
          // where the old run() had it — first, before any other work.
          glue_(pick_glue(state.setup_lcg, ctx.values)),
          // Enter/Space accept debounce (mirrors present_map_select's 1 s
          // accept_after_ms, 8100/8220-8228): ignore an accept for 1 s after
          // entry so a held Enter carried from the previous screen can't blast
          // the start.
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
    // The F2 lobby-chat overlay gets first refusal (chat_overlay.hpp — PORT-ONLY,
    // not RE'd). While it is open it consumes every key, so typing a message never
    // also cycles a slot or toggles teams behind it; while it is closed it takes
    // only F2 and this screen behaves exactly as it did before chat existed.
    if (seams_.chat != nullptr && seams_.chat->handle_event(ev, ctx_)) return std::nullopt;
    // Hotplug (sub_429628 "joystick present" is polled live in the original; SDL3
    // gives us an event instead): rescan so the pane and the Right-cycle's
    // joystick count reflect what's plugged in right now, without a restart.
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
    // Enter/Space (batch_0x410401.cpp 1148-1182: Space, 0x20, routes through the
    // SAME accept path as Enter) leaves this screen for match init / the LEVEL
    // screen — but only past the debounce and the two start guards.
    if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) return on_accept();
    ctx_.audio.play(20);  // any real key blips first (sub_427961(20))
    // sub_410F81 15432-15436: key 0x13B (F1) dispatches the SAME generic *.BM
    // help browser as menu row 5 / the options screen / the in-round key (one
    // routine, sub_41431C), composited over this screen like every sub_41431C
    // site.
    if (k == SDLK_F1) return run_help_browser();
    on_edit_key(k);
    return std::nullopt;
}

// Hidden campaign-mode trigger (docs/re/campaign.md §4, sub_410F81 pseudo.c
// 15357-15365): five 'C' presses open the *.cam picker.
std::optional<AppInput> SetupLoop::on_campaign_key() {
    // THE BLIP COMES FIRST, ALWAYS. sub_410F81's key loop fires sub_427961(20)
    // @0x411724 for every real key BEFORE its dispatch switch at 0x411729 — 'c'
    // (0x63) is just another case in that switch (it lands at 0x41186D). The
    // port's early exit used to jump the queue and swallow the blip.
    ctx_.audio.play(20);
    // LOCAL ONLY — the original gates the whole campaign trigger on `sub_40C06A()`
    // (docs/re/campaign.md §4). That guard used to be omitted here because the
    // port had no netplay; now it does, so it is real again: a campaign
    // roster/stage pick is a local-only concept that would never reach the peer.
    //
    // The guard is SILENT: 0x41186D tests sub_40C06A() and, when it is non-zero,
    // jumps straight to the loop tail (0x41188E) with no sound at all. The SFX-40
    // buzz that used to be here was invented — there is no "you can't do that"
    // for this key.
    if (net_mode_) return std::nullopt;
    // CUMULATIVE, not consecutive (sub_410F81 pseudo.c 840, 1193-1197): ONLY the
    // 'C' handler touches this counter — no other key resets it — so 5 total 'C'
    // presses across the visit arm the picker. (The old any-other-key reset
    // required 5 CONSECUTIVE presses, which the original never demanded.)
    if (++state_.campaign_trigger_count != 5) return std::nullopt;
    state_.campaign_trigger_count = 0;
    // NO accept sting. This trigger is NOT the menu's Ctrl+E x6 editor trigger it
    // was written to "mirror": that one really does play 10 (0x42BD50, right
    // before sub_40330E), but the campaign arm at 0x411882 calls sub_4015C6 and
    // zeroes its counter with nothing in between. The 20 blip above is the only
    // sound five C presses make.
    CampaignPickerScreen(ctx_, seams_.campaign, seams_.backdrop).run();
    return std::nullopt;
}

AppInput SetupLoop::on_escape() {
    ctx_.audio.play(20);
    ctx_.audio.play(10);
    // doc §2: "Cleared to -1 by: ... Esc on the player-setup screen" — cancelling
    // the whole Play flow here also forfeits any gold player pending from an
    // earlier match.
    state_.gold_player = -1;
    // Campaign quit semantics — CONFIRMED negative, docs/re/campaign.md
    // "Campaign-exit key": grepped every read/write of dword_46489C in the binary;
    // it is written in exactly TWO places total (sub_4015C6's `=1` and
    // sub_42A3F6's own entry `=0`, pseudo.c 29692) — there is NO key anywhere,
    // Escape or otherwise, that explicitly clears it. The original's own
    // Escape-on-setup just aborts the current sub_42A3F6 call to the menu
    // (dword_464A68=2); dword_46489C is left stale until the NEXT "Play" click
    // resets it at entry, which is behaviourally invisible (that stale value is
    // never read before being overwritten). Our explicit clear here produces the
    // identical observable outcome (back at the menu, campaign not running) via an
    // immediate reset instead of an implicit one — a faithful convenience, not a
    // guess. This only fires if a *.cam pick from THIS visit hasn't been confirmed
    // into a running match yet; an in-progress campaign is abandoned via
    // run_match's own Esc/Ctrl+Q, which — matching the original — doesn't touch
    // state_.campaign_active either; it only clears on the NEXT Menu->StartMatch
    // transition (see that path's own comment).
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
    // Guard 2 first (sub_422085): same-controller humans -> error getstring(45)
    // over getstring(96).
    if (dup_controller(state_))
        return show_error(ctx_.assets.getstring(45, "Two players cannot use the same controls!"));
    // Guard 1 (sub_42223E): too few players/teams -> getstring(46) (solo) or
    // getstring(48) (team) over getstring(96).
    if (!count_ok(state_))
        return show_error(state_.team_play
                              ? ctx_.assets.getstring(48, "You need at least two teams!")
                              : ctx_.assets.getstring(46, "You need at least two players!"));
    ctx_.audio.play(10);  // accept sting
    accepted_ = true;
    return std::nullopt;
}

// §7's read-only gate: Up/Down (pure navigation) and F1 stay live on a guest;
// every EDIT key buzzes SFX 40, the same shape as `sub_410F81`'s
// `sub_40C06A() != 1` guards. A slot the wire seat assignment owns is likewise
// frozen (net_setup_link.hpp "SEAT LOCKING").
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
    // 'T' has its own gate: a LOCKED slot still toggles online, because the team
    // byte travels in the confirmed config — only the input TYPE of a wire seat is
    // fixed. So it must not go through edit_denied() with the others.
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
        // Cycle a slot's input type FORWARD one step (sub_421E80 @0x421E80): 0 off
        // -> 1 computer -> 2 keyboard sub 0 -> 2 keyboard sub 1 -> 3 joystick per
        // present stick -> back to 0. The pure wrap-order logic lives in
        // cycle_slot_input_type (input.hpp, unit-tested); this just supplies the
        // live connected-gamepad count so the cycle offers exactly the sticks in
        // ctx_.gamepads right now.
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
    HelpBrowser browser(ctx_.assets, ctx_.front_font);
    browser.enter(ctx_.values.at_or(15, 1) != 0);
    while (!browser.done()) {
        SDL_Event hev;
        while (SDL_PollEvent(&hev)) {
            if (hev.type == SDL_EVENT_QUIT) return AppInput::Quit;
            if (hev.type == SDL_EVENT_KEY_DOWN) browser.on_key(hev.key.key, ctx_.audio);
        }
        if (browser.viewing() && browser.viewer().done()) browser.close_viewer();
        net_setup_pump(seams_.net);  // the link must not go silent under the browser
        if (seams_.chat != nullptr) seams_.chat->pump();  // nor the lobby's heartbeat
        ctx_.audio.update_music();
        draw_frame();
        browser.draw(ctx_.sdl);
        SDL_RenderPresent(ctx_.sdl);
        SDL_Delay(2);
    }
    return std::nullopt;
}

// sub_414340 error modal (batch_0x410401.cpp ~1152/1176): the start guards pop a
// WINZ-9-patch acknowledge box — the reason line over getstring(96), dark-red ink,
// dismissed by Enter/Space/Escape (nav-blip on any key) — drawn over the frozen
// setup frame. Returns Quit if the window closed while it was up, else nullopt
// (the screen stays open).
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
        // `timeout_ms` of silence, so a host sitting on a start-guard error must
        // still be re-broadcasting. Same for the lobby link — the matchmaker reaps
        // a member that stops heart-beating. The chat OVERLAY is not driven here:
        // it can only be reached from the main loop, which swallows Enter while it
        // is open, so it cannot be up.
        net_setup_pump(seams_.net);
        if (seams_.chat != nullptr) seams_.chat->pump();
        ctx_.audio.update_music();
        draw_frame();
        // Ink = byte_49A390 = DARK RED (164,0,0): the setup start-guard errors are
        // sub_414340(getstring(46/48/45)|getstring(96), color1, byte_49A390)
        // (batch_0x410401.cpp:1161/1175), and byte_49A390 resolves to (164,0,0)
        // warning red (docs/re/frontend-flow.md), NOT white.
        draw_acknowledge_dialog(ctx_.sdl, ctx_.front_font, &ctx_.assets.frontend_pcx("WINZ"),
                                reason, sub, ok, 164, 0, 0);
        SDL_RenderPresent(ctx_.sdl);
        SDL_Delay(2);
    }
}

// The HOST broadcasts each change (§7: roster kind 40, team kind 58) — on the
// edit, not every frame, so `revision` only moves when something actually did.
// `rounds = 0` is the "still on the roster screen" sentinel (net_setup_link.hpp);
// the level travels from the LEVEL & ROUNDS screen, where the host picks it.
std::optional<AppInput> SetupLoop::pump_link() {
    if (net_mode_ && !net_guest_ && (net_dirty_ || !net_setup_has_preview(seams_.net))) {
        net_setup_publish(seams_.net, state_.setup_type, state_.setup_team, state_.team_play,
                          /*level=*/-1, /*level_name=*/std::string(), /*rounds=*/0);
        net_dirty_ = false;
    }
    net_setup_pump(seams_.net);
    if (seams_.chat != nullptr) seams_.chat->pump();
    if (!net_guest_) return std::nullopt;
    // Read-only: the displayed roster IS the host's newest preview.
    net_setup_apply_roster(seams_.net, state_.setup_type, state_.setup_sub, state_.setup_team,
                           state_.team_play);
    // The host moved on (its preview now carries a real win target), or it
    // confirmed outright and we already hold the config: follow it.
    if (net_setup_final(seams_.net) || net_setup_on_level_screen(seams_.net))
        return AppInput::Advance;
    // Timed out / the host vanished. Back returns to the caller, which reads
    // net_setup_failed() to tell this from an Esc.
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
                                  layout_.hx, layout_.hy, 255, 255, 255, 0, 0, 0, layout_.hw);
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

void SetupLoop::draw_slot_row(int i) {
    // One combined "Player %u: %s" splice (msg 51) — the original sprintf's the
    // slot number and the type text in ONE call (pseudo.c 15169-15195); the old
    // two-piece concat left a literal "%s" on screen with the install's real
    // MESSAGES.TXT.
    const std::string line =
        fmt_us(ctx_.assets.getstring(51, "Player %u: %s"), i + 1, slot_type_text(i));
    // Ink = the slot's authentic colour via sub_41672F(i) (the .RMP tail quantised
    // min(c/3,31) -> RGB555 -> LUT), which AssetStore::slot_color reproduces.
    // CONFIRMED never the team red/white override: sub_410F81 saves+zeroes
    // dword_464964 around this lookup (pseudo.c 15191-15204) — only the separate
    // TEAM marker below is team-inked. And the ink is NEVER state-dimmed or
    // selection-boosted (no OFF/COM dimming exists; the old selected-row +70 nudge
    // was invented — the cursor sprite alone marks the selection).
    std::uint8_t sc[3];
    ctx_.assets.slot_color(i, sc);
    // Outline: black for every slot EXCEPT index 1 — the BLACK player's row gets a
    // WHITE outline (sub_416867, pseudo.c 18496-18503) so it stays legible over a
    // dark glue backdrop.
    const Uint8 oc = i == 1 ? 255 : 0;
    const float row_y = layout_.ly + layout_.lys * static_cast<float>(i);
    const float lx_end = ctx_.front_font.draw_outlined(ctx_.sdl, line, layout_.lx, row_y, sc[0],
                                                       sc[1], sc[2], oc, oc, oc, layout_.lw);
    if (!state_.team_play) return;
    // Team marker: getstring(230), drawn for EVERY slot whenever Team Play is on
    // (gated on the GLOBAL dword_464964, pseudo.c ~15212 — NOT on this slot's own
    // team byte). CONFIRMED unformatted (no sprintf before the two sub_4124A4(230)
    // reads at ~15221/15223) — the COLOUR alone tells the teams apart, via
    // sub_4141F8(team): team byte != 0 -> byte_49D0DA red (252,80,80), else
    // byte_49D38F white — the same split as the in-match sprite override
    // (docs/re/player-colour.md).
    const std::string marker = "  " + ctx_.assets.getstring(230, "TEAM");
    const bool team1 = state_.setup_team[i] != 0;  // sub_4141F8's `a1 ?` branch
    ctx_.front_font.draw_outlined(
        ctx_.sdl, marker, lx_end, row_y, static_cast<Uint8>(team1 ? 252 : 255),
        static_cast<Uint8>(team1 ? 80 : 255), static_cast<Uint8>(team1 ? 80 : 255), 0, 0, 0);
}

// Joystick pane (getvalue 715/720): heading msg 40, then one line per detected
// stick (msg 41 "Joy %u - %s", the stick's own name in the %s — sub_429A61(i))
// or, if none, the single msg-42 line. ALL of it plain white ink / black outline
// (pseudo.c 15227-15263) — the old grey (200,200,200)/(150,150,150) tints were
// invented.
void SetupLoop::draw_joystick_pane() {
    ctx_.front_font.draw_outlined(ctx_.sdl, ctx_.assets.getstring(40, "JOYSTICKS"), layout_.jhx,
                                  layout_.jhy, 255, 255, 255, 0, 0, 0, layout_.jhw);
    const int joy_count = ctx_.gamepads.count();
    if (joy_count == 0) {
        ctx_.front_font.draw_outlined(ctx_.sdl, ctx_.assets.getstring(42, "none"), layout_.jlx,
                                      layout_.jly, 255, 255, 255, 0, 0, 0, layout_.jlw);
        return;
    }
    for (int j = 0; j < joy_count; ++j) {
        const std::string jline =
            fmt_us(ctx_.assets.getstring(41, "Joy %u - %s"), j, ctx_.gamepads.name(j));
        ctx_.front_font.draw_outlined(ctx_.sdl, jline, layout_.jlx,
                                      layout_.jly + layout_.jlys * static_cast<float>(j), 255, 255,
                                      255, 0, 0, 0, layout_.jlw);
    }
}

// Footer (sub_413FB9 -> getstring(330), local play only): centred on x via
// sub_4172BA's `x = cx - (w+2)/2`, cyan ink byte_497F8F (96,252,252).
void SetupLoop::draw_footer() {
    const std::string help = ctx_.assets.getstring(330, "Press F1 for help");
    const float help_w = static_cast<float>(ctx_.front_font.measure(help));
    ctx_.front_font.draw_outlined(ctx_.sdl, help, layout_.fcx - (help_w + 2.0f) / 2.0f, layout_.ffy,
                                  96, 252, 252, 0, 0, 0, layout_.ffw);
}

// The bomber-dude row cursor (sub_413BD6, called at pseudo.c 15205-15211):
// MISC.ANI "cursor1", hotspot-anchored at (getvalue(710) - 15, row_y + 16) — this
// screen alone uses -15; the options/level screens use -20. The +16 y nudge is
// pinned EMPIRICALLY from a 1:1 native capture of the level screen (2026-07-12;
// VALUELST 736 = 170, measured sprite rows 155..186 -> anchor = row_y + 16): the
// dude's feet stand just under the row text's baseline. The decompile loses the
// +16 to register mangling at every call site, so the capture is the authority.
// Idle step 0 + timed blink: cursor_indicator.hpp.
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

// The PLAYER INPUT TYPE SELECTION screen (sub_410F81 @0x410F81, VALUELST "PLAYER
// INPUT TYPE SELECTION" getvalue 705-713). Screen 1 of the pre-match flow reached
// from Play. A random GLUE<n> backdrop under the 1020 track (inherited from the
// Play handler sub_42A3F6 — this screen starts no music), header getstring(50),
// and the 10-slot list: each slot's input type via getstring(220..224), PREFIXED
// by getstring(51) (Player %u) and TINTED with the slot's intrinsic colour
// (VALUELST 200-247 = Tuning::color_rgb — there is no colour picker; colour is
// fixed per slot index, applied in-game via i.rmp), plus a team marker when the
// slot's team flag is set. Keys mirror the confirmed table
// (docs/re/setup-screens.md): Up/Down pick a slot, Right cycles its type
// (OFF->CPU->KBD0->KBD1->OFF), Left/'0' set it OFF, 'T' toggles its team, Enter
// goes on to the LEVEL screen, Escape cancels to the menu. Presentation only.
AppInput SetupScreen::run() {
    // NO music call here, deliberately. sub_410F81 starts none: the 1020 track is
    // already running, started by its caller sub_42A3F6 at 0x42A436 (the port's
    // run_app StartMatch handler does the same). This screen used to restart it,
    // which restarted WIN.RSS from the top whenever the goldman wheel ran first —
    // the wheel had the identical bug. docs/re/sound-engine.md §9.
    //
    // TEAM default — CORRECTED 2026-07-09 (docs/re/setup-screens.md "TEAM default
    // — CORRECTED"): sub_410F81 unconditionally calls sub_4046CC() first thing,
    // which (CD present) calls sub_403EEE(), which itself unconditionally calls
    // sub_4049C0() before anything else. sub_4049C0 sets `dword_46481C[12*j+8] =
    // j & 1` for j in [0,10) — i.e. every slot's TEAM byte resets to an ALTERNATING
    // 0/1/0/1 pattern by slot parity every time this screen loads, not to a flat 0.
    // Getting this wrong (old behaviour: every slot defaulted to 0) meant Team Play
    // ON without anyone pressing 'T' put every player on the SAME side: (a)
    // everybody got the team-1/WHITE 0.RMP override instead of half going red
    // (render_colour, docs/re/player-colour.md), and (b) sides_remaining() read <=1
    // from tick 0, clinching the round instantly.
    //
    // ...and then THE SCHEME OVERRIDES IT, per slot — CORRECTED AGAIN 2026-07-28
    // (docs/re/facts.md "The .SCH -S row's 4th field is the per-slot TEAM"). The
    // paragraph that stood here claimed sub_403EEE's parse loop "only ever
    // overwrites a slot's COLOUR from disk, never TEAM, unless a rare 5-field
    // profile line is present — a hidden colour-profile file this port doesn't
    // implement". All three parts are wrong: the dwords it called colour are the
    // spawn X and Y, the file being parsed is the scheme itself, and a four-field
    // "-S slot,x,y,team" row appears in 19 of the 67 shipped schemes (E_VS_W,
    // N_VS_S, TENNIS, VOLLEY, … — the two-sided maps). The reader stores that field
    // and its tail loop pushes every slot into the player record's +84 byte via
    // sub_422437, so the map author's layout is already in place before this screen
    // draws a frame. The 'T' key still overrides it, exactly as in the original,
    // where the key loop runs after the load.
    reset_setup_teams(state_.setup_team);
    match::scheme_setup_teams(state_.scheme, state_.setup_team);
    return SetupLoop(ctx_, state_, SetupSeams{campaign_, backdrop_, net_, chat_}).run();
}

}  // namespace bomber::game
