#include "bomber/game/screens/setup_screen.hpp"

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "bomber/game/anim_pace.hpp"                 // anim_step_index
#include "bomber/game/bmscreen.hpp"                  // HelpBrowser
#include "bomber/game/dialog_chrome.hpp"             // draw_acknowledge_dialog
#include "bomber/game/frontend_util.hpp"             // pick_glue
#include "bomber/game/hud_format.hpp"                // fmt_u / fmt_us
#include "bomber/game/input.hpp"                     // cycle_slot_input_type / reset_setup_teams
#include "bomber/game/screens/campaign_screens.hpp"  // CampaignPickerScreen
#include "bomber/game/sprites.hpp"                   // Sprite, Anim, resolve_sequence

namespace bomber::game {

namespace {

// The Play-handler music (sub_42A3F6, docs/re/in-match-shell.md §2): id 1020
// (0x3FC, "win" in SOUNDLST) is actually the SETUP-SCREENS backdrop track, not
// victory music — started here at player-select entry and inherited by the
// LEVEL & ROUNDS screen; a looping track (start_music). game_app.cpp keeps its
// own copy for the goldman wheel / scoreboard (the fuller RE note lives there,
// beside kDrawMusicId for the outcome tier) until those screens extract too.
constexpr int kWinMusicId = 1020;  // 0x3FC — WIN.RSS, setup-screens backdrop (NOT victory)

}  // namespace

// Cycle a slot's input type FORWARD one step (sub_421E80 @0x421E80): 0 off ->
// 1 computer -> 2 keyboard sub 0 -> 2 keyboard sub 1 -> 3 joystick per present
// stick -> back to 0. The pure wrap-order logic lives in cycle_slot_input_type
// (input.hpp, unit-tested); this just supplies the live connected-gamepad
// count so the cycle offers exactly the sticks in ctx_.gamepads right now.
void SetupScreen::cycle_input_type(int slot) {
    cycle_slot_input_type(state_.setup_type[slot], state_.setup_sub[slot], ctx_.gamepads.count());
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
AppInput SetupScreen::run() {
    ctx_.audio.start_music(kWinMusicId);  // 1020, the Play-handler track (sub_42A3F6)
    // TEAM default — CORRECTED 2026-07-09 (docs/re/setup-screens.md "TEAM
    // default — CORRECTED"): sub_410F81 unconditionally calls sub_4046CC()
    // first thing, which (CD present) calls sub_403EEE(), which itself
    // unconditionally calls sub_4049C0() before anything else. sub_4049C0
    // sets `dword_46481C[12*j+8] = j & 1` for j in [0,10) (pseudo.c line
    // 6716) — i.e. every slot's TEAM byte resets to an ALTERNATING 0/1/0/1
    // pattern by slot parity every time this screen loads, not to a flat 0.
    // sub_403EEE's own file-parse loop only ever overwrites a slot's COLOUR
    // (dword_46481C+0/+4) from disk, never TEAM, unless a rare "-S
    // slot,x,y,team" 5-field profile line is present (pseudo.c line 6427) —
    // a hidden colour-profile file this port doesn't implement — so in
    // practice the alternating default always stands here. Getting this
    // wrong (old behaviour: every slot defaulted to 0) meant Team Play ON
    // without anyone pressing 'T' put every player on the SAME side: (a)
    // everybody got the team-1/WHITE 0.RMP override instead of half going
    // red (render_colour, docs/re/player-colour.md), and (b)
    // sides_remaining() read <=1 from tick 0, clinching the round instantly.
    reset_setup_teams(state_.setup_team);
    const std::string glue = pick_glue(state_.setup_lcg, ctx_.values);
    // Layout (VALUELST X,Y,YS,colour -> consecutive getvalue ids): header 705,
    // list 710, joystick pane heading 715, joystick pane list 720.
    const float hx = static_cast<float>(ctx_.values.column_or(705, 0, 40));
    const float hy = static_cast<float>(ctx_.values.column_or(705, 1, 140));
    const float lx = static_cast<float>(ctx_.values.column_or(710, 0, 70));
    const float ly = static_cast<float>(ctx_.values.column_or(710, 1, 170));
    const float lys = static_cast<float>(ctx_.values.column_or(710, 2, 24));
    const float jhx = static_cast<float>(ctx_.values.column_or(715, 0, 300));
    const float jhy = static_cast<float>(ctx_.values.column_or(715, 1, 140));
    const float jlx = static_cast<float>(ctx_.values.column_or(720, 0, 320));
    const float jly = static_cast<float>(ctx_.values.column_or(720, 1, 170));
    const float jlys = static_cast<float>(ctx_.values.column_or(720, 2, 24));
    // Column 3 of each layout row is the CLIP WIDTH handed to the text
    // primitive (sub_41696C's max-width arg; setup-screens.md's earlier
    // "colour" label for this column was wrong — colour never comes from
    // VALUELST on this screen): header 200, slot rows 150, joystick heading
    // 170, joystick rows 320.
    const float hw = static_cast<float>(ctx_.values.column_or(705, 3, 200));
    const float lw = static_cast<float>(ctx_.values.column_or(710, 3, 150));
    const float jhw = static_cast<float>(ctx_.values.column_or(715, 3, 170));
    const float jlw = static_cast<float>(ctx_.values.column_or(720, 3, 320));
    // Footer anchor (VALUELST 790 — the file's own note: "goes on a lot of
    // different screens"): getstring(330) "Press F1 for help", centred on x
    // via sub_4172BA's `x = cx - (w+2)/2`, cyan ink byte_497F8F (96,252,252).
    const float fcx = static_cast<float>(ctx_.values.column_or(790, 0, 320));
    const float ffy = static_cast<float>(ctx_.values.column_or(790, 1, 440));
    const float ffw = static_cast<float>(ctx_.values.column_or(790, 3, 300));
    // Bomber-dude cursor blink base + random spread, seconds (VALUELST 690 =
    // {2,2}; getvalue(691) is column 1 of the same row).
    const int blink_base = static_cast<int>(ctx_.values.column_or(690, 0, 2));
    const int blink_spread = static_cast<int>(ctx_.values.column_or(690, 1, 2));

    int cursor = 0;

    // One frame of the screen (sub_410F81's per-frame body, pseudo.c
    // 15146-15267): backdrop, header, slot rows, joystick pane, footer — all
    // text through the 4-pass-outline primitive (sub_41696C) — and the
    // bomber-dude cursor LAST (the original queues sprites and flushes them
    // after the text, so the cursor lands on top). A lambda so the F1 help
    // browser below composites over the identical frame.
    auto draw_frame = [&]() {
        SDL_SetRenderDrawColor(ctx_.sdl, 0, 0, 0, 255);
        SDL_RenderClear(ctx_.sdl);
        const Sprite& bg = ctx_.assets.frontend_pcx(glue);
        if (bg.tex) {
            SDL_FRect dst{0.0f, 0.0f, static_cast<float>(bg.w), static_cast<float>(bg.h)};
            SDL_RenderTexture(ctx_.sdl, bg.tex, nullptr, &dst);
        }
        // Header (msg 50): white ink / black outline (byte_49D38F over
        // byte_495390[0], pseudo.c 15154-15160).
        ctx_.front_font.draw_outlined(ctx_.sdl, ctx_.assets.getstring(50, "Available players:"),
                                  hx, hy, 255, 255, 255, 0, 0, 0, hw);
        for (int i = 0; i < 10; ++i) {
            const int t = state_.setup_type[i];
            std::string type;
            switch (t) {
                case 1: type = ctx_.assets.getstring(221, "COMPUTER"); break;
                case 2: type = fmt_u(ctx_.assets.getstring(222, "KEYBOARD %u"), state_.setup_sub[i]); break;
                case 3: type = fmt_u(ctx_.assets.getstring(223, "JOYSTICK %u"), state_.setup_sub[i]); break;
                case 4: type = ctx_.assets.getstring(224, "OTHER"); break;
                default: type = ctx_.assets.getstring(220, "OFF"); break;
            }
            // One combined "Player %u: %s" splice (msg 51) — the original
            // sprintf's the slot number and the type text in ONE call
            // (pseudo.c 15169-15195); the old two-piece concat left a literal
            // "%s" on screen with the install's real MESSAGES.TXT.
            const std::string line = fmt_us(ctx_.assets.getstring(51, "Player %u: %s"), i + 1, type);
            // Ink = the slot's authentic colour via sub_41672F(i) (the .RMP
            // tail quantised min(c/3,31) -> RGB555 -> LUT), which
            // AssetStore::slot_color reproduces. CONFIRMED never the team
            // red/white override: sub_410F81 saves+zeroes dword_464964 around
            // this lookup (pseudo.c 15191-15204) — only the separate TEAM
            // marker below is team-inked. And the ink is NEVER state-dimmed
            // or selection-boosted (no OFF/COM dimming exists; the old
            // selected-row +70 nudge was invented — the cursor sprite alone
            // marks the selection).
            std::uint8_t sc[3];
            ctx_.assets.slot_color(i, sc);
            // Outline: black for every slot EXCEPT index 1 — the BLACK
            // player's row gets a WHITE outline (sub_416867, pseudo.c
            // 18496-18503) so it stays legible over a dark glue backdrop.
            const Uint8 oc = i == 1 ? 255 : 0;
            float lx_end = ctx_.front_font.draw_outlined(ctx_.sdl, line, lx,
                                                     ly + lys * static_cast<float>(i), sc[0],
                                                     sc[1], sc[2], oc, oc, oc, lw);
            if (state_.team_play) {
                // Team marker: getstring(230), drawn for EVERY slot whenever
                // Team Play is on (gated on the GLOBAL dword_464964, pseudo.c
                // ~15212 — NOT on this slot's own team byte). CONFIRMED
                // unformatted (no sprintf before the two sub_4124A4(230)
                // reads at ~15221/15223) — the COLOUR alone tells the teams
                // apart, via sub_4141F8(team): team byte != 0 -> byte_49D0DA
                // red (252,80,80), else byte_49D38F white — the same split as
                // the in-match sprite override (docs/re/player-colour.md).
                std::string marker = "  " + ctx_.assets.getstring(230, "TEAM");
                const bool team1 = state_.setup_team[i] != 0;  // sub_4141F8's `a1 ?` branch
                ctx_.front_font.draw_outlined(
                    ctx_.sdl, marker, lx_end, ly + lys * static_cast<float>(i),
                    static_cast<Uint8>(team1 ? 252 : 255), static_cast<Uint8>(team1 ? 80 : 255),
                    static_cast<Uint8>(team1 ? 80 : 255), 0, 0, 0);
            }
        }
        // Joystick pane (getvalue 715/720): heading msg 40, then one line per
        // detected stick (msg 41 "Joy %u - %s", the stick's own name in the
        // %s — sub_429A61(i)) or, if none, the single msg-42 line. ALL of it
        // plain white ink / black outline (pseudo.c 15227-15263) — the old
        // grey (200,200,200)/(150,150,150) tints were invented.
        ctx_.front_font.draw_outlined(ctx_.sdl, ctx_.assets.getstring(40, "JOYSTICKS"), jhx,
                                  jhy, 255, 255, 255, 0, 0, 0, jhw);
        const int joy_count = ctx_.gamepads.count();
        if (joy_count == 0) {
            ctx_.front_font.draw_outlined(ctx_.sdl, ctx_.assets.getstring(42, "none"), jlx, jly,
                                      255, 255, 255, 0, 0, 0, jlw);
        } else {
            for (int j = 0; j < joy_count; ++j) {
                const std::string jline =
                    fmt_us(ctx_.assets.getstring(41, "Joy %u - %s"), j, ctx_.gamepads.name(j));
                ctx_.front_font.draw_outlined(ctx_.sdl, jline, jlx,
                                          jly + jlys * static_cast<float>(j), 255, 255, 255, 0, 0,
                                          0, jlw);
            }
        }
        // Footer (sub_413FB9 -> getstring(330), local play only): centred,
        // cyan/black. Replaces the invented key-legend line.
        const std::string help = ctx_.assets.getstring(330, "Press F1 for help");
        const float help_w = static_cast<float>(ctx_.front_font.measure(help));
        ctx_.front_font.draw_outlined(ctx_.sdl, help, fcx - (help_w + 2.0f) / 2.0f, ffy, 96,
                                  252, 252, 0, 0, 0, ffw);
        // The bomber-dude row cursor (sub_413BD6, called at pseudo.c
        // 15205-15211): MISC.ANI "cursor1", hotspot-anchored at
        // (getvalue(710) - 15, row_y + 16) — this screen alone uses -15; the
        // options/level screens use -20. The +16 y nudge is pinned
        // EMPIRICALLY from a 1:1 native capture of the level screen
        // (2026-07-12; VALUELST 736 = 170, measured sprite rows 155..186 →
        // anchor = row_y + 16): the dude's feet stand just under the row
        // text's baseline. The decompile loses the +16 to register mangling
        // at every call site, so the capture is the authority. Idle step 0 +
        // timed blink: cursor_indicator.hpp.
        Anim cur = resolve_sequence(ctx_.assets.misc(), "cursor1");
        if (!cur.steps.empty()) {
            const std::size_t st = ctx_.cursor_blink.step(SDL_GetTicks() / 1000ull, cur.steps.size(),
                                                      blink_base, blink_spread);
            const Sprite& sp = cur.steps[anim_step_index(st, cur.steps.size())];
            if (sp.tex) {
                SDL_FRect d{lx - 15.0f - static_cast<float>(sp.hx),
                            ly + lys * static_cast<float>(cursor) + 16.0f -
                                static_cast<float>(sp.hy),
                            static_cast<float>(sp.w), static_cast<float>(sp.h)};
                SDL_RenderTexture(ctx_.sdl, sp.tex, nullptr, &d);
            }
        }
    };

    // sub_414340 error modal (batch_0x410401.cpp ~1152/1176): the start
    // guards below pop a WINZ-9-patch acknowledge box — the reason line over
    // getstring(96), dark-red ink, dismissed by Enter/Space/Escape (nav-blip
    // on any key) — drawn over the frozen setup frame. Returns Quit if the
    // window closed while it was up, else Advance (the screen stays open).
    auto show_error = [&](const std::string& reason) -> AppInput {
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
                    return AppInput::Advance;
            }
            ctx_.audio.update_music();
            draw_frame();
            // Ink = byte_49A390 = DARK RED (164,0,0): the setup start-guard
            // errors are sub_414340(getstring(46/48/45)|getstring(96), color1,
            // byte_49A390) (batch_0x410401.cpp:1161/1175), and byte_49A390
            // resolves to (164,0,0) warning red (docs/re/frontend-flow.md), NOT
            // white. (Restores the correct red.)
            draw_acknowledge_dialog(ctx_.sdl, ctx_.front_font, &ctx_.assets.frontend_pcx("WINZ"),
                                    reason, sub, ok, 164, 0, 0);
            SDL_RenderPresent(ctx_.sdl);
            SDL_Delay(2);
        }
    };
    // Start guard 1 (sub_42223E, batch_0x410401.cpp 1148-1168): at least two
    // ACTIVE slots, or in TEAM mode at least two DISTINCT team values among
    // the active slots — else the game refuses to start.
    auto count_ok = [&]() {
        if (state_.team_play) {
            bool seen0 = false, seen1 = false;
            for (int i = 0; i < 10; ++i) {
                if (state_.setup_type[i] == 0) continue;  // OFF slots don't count
                (state_.setup_team[i] ? seen1 : seen0) = true;
            }
            return seen0 && seen1;
        }
        int active = 0;
        for (int i = 0; i < 10; ++i)
            if (state_.setup_type[i] != 0) ++active;
        return active >= 2;
    };
    // Start guard 2 (sub_422085, batch_0x410401.cpp 1172): two ACTIVE HUMAN
    // slots (KEYBOARD=2 or JOYSTICK=3) may not share the same input type AND
    // sub-index — same keyboard set or same stick. CPU (1)/OTHER (4)/OFF (0)
    // are excluded.
    auto dup_controller = [&]() {
        for (int i = 0; i < 10; ++i) {
            if (state_.setup_type[i] != 2 && state_.setup_type[i] != 3) continue;
            for (int j = i + 1; j < 10; ++j) {
                if (state_.setup_type[j] != 2 && state_.setup_type[j] != 3) continue;
                if (state_.setup_type[i] == state_.setup_type[j] && state_.setup_sub[i] == state_.setup_sub[j]) return true;
            }
        }
        return false;
    };

    bool waiting = true;
    // Enter/Space accept debounce (mirrors present_map_select's 1 s
    // accept_after_ms, 8100/8220-8228): ignore an accept for 1 s after entry
    // so a held Enter carried from the previous screen can't blast the start.
    std::uint64_t accept_after_ms = SDL_GetTicks() + 1000;
    while (waiting) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
            // Hotplug (sub_429628 "joystick present" is polled live in the
            // original; SDL3 gives us an event instead): rescan so the pane
            // and the Right-cycle's joystick count reflect what's plugged in
            // right now, without needing a restart.
            if (ev.type == SDL_EVENT_GAMEPAD_ADDED || ev.type == SDL_EVENT_GAMEPAD_REMOVED) {
                ctx_.gamepads.refresh();
                continue;
            }
            if (ev.type != SDL_EVENT_KEY_DOWN) continue;
            const SDL_Keycode k = ev.key.key;

            // Hidden campaign-mode trigger (docs/re/campaign.md §4,
            // sub_410F81 pseudo.c 15357-15365): 5 CONSECUTIVE 'C' presses
            // (any other key resets the counter — same same-key-repeat
            // pattern as present_menu's Ctrl+E x6) opens the *.cam picker.
            // Local-only in the original (sub_40C06A() guard); this port has
            // no netplay (ADR-0003), so that guard is always-true and
            // omitted. Checked BEFORE the general dispatch below so 'C'
            // itself never falls into the row-navigation switch.
            if (k == SDLK_C) {
                // CUMULATIVE, not consecutive (sub_410F81 pseudo.c 840,
                // 1193-1197): ONLY the 'C' handler touches this counter — no
                // other key resets it — so 5 total 'C' presses across the
                // visit arm the picker. (The old any-other-key reset below
                // required 5 CONSECUTIVE presses, which the original never
                // demanded.)
                if (++state_.campaign_trigger_count == 5) {
                    state_.campaign_trigger_count = 0;
                    ctx_.audio.play(10);  // accept sting (SFX 10), mirrors the editor trigger
                    CampaignPickerScreen(ctx_, campaign_, backdrop_).run();
                }
                continue;
            }

            if (k == SDLK_ESCAPE) {
                ctx_.audio.play(20);
                ctx_.audio.play(10);
                // doc §2: "Cleared to -1 by: ... Esc on the player-setup
                // screen" — cancelling the whole Play flow here also forfeits
                // any gold player pending from an earlier match.
                state_.gold_player = -1;
                // Campaign quit semantics — CONFIRMED negative, docs/re/
                // campaign.md "Campaign-exit key": grepped every read/write
                // of dword_46489C in the binary; it is written in exactly
                // TWO places total (sub_4015C6's `=1` and sub_42A3F6's own
                // entry `=0`, pseudo.c 29692) — there is NO key anywhere,
                // Escape or otherwise, that explicitly clears it. The
                // original's own Escape-on-setup just aborts the current
                // sub_42A3F6 call to the menu (dword_464A68=2); dword_46489C
                // is left stale until the NEXT "Play" click resets it at
                // entry, which is behaviourally invisible (that stale value
                // is never read before being overwritten). Our explicit
                // clear here produces the identical observable outcome
                // (back at the menu, campaign not running) via an immediate
                // reset instead of an implicit one — a faithful convenience,
                // not a guess. This only fires if a *.cam pick from THIS
                // visit to present_setup hasn't been confirmed into a
                // running match yet; an in-progress campaign is abandoned
                // via run_match's own Esc/Ctrl+Q (below), which — matching
                // the original — doesn't touch state_.campaign_active either;
                // it only clears on the NEXT Menu->StartMatch transition
                // (see that path's own comment).
                state_.campaign_active = false;
                state_.campaign_stages.clear();
                state_.campaign_stage_index = 0;
                return AppInput::Back;
            }
            // Enter/Space (batch_0x410401.cpp 1148-1182: Space, 0x20, routes
            // through the SAME accept path as Enter) leaves this screen and
            // proceeds to match init / the LEVEL screen — but only past the
            // debounce and the two start guards.
            if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) {
                ctx_.audio.play(20);  // any-key blip first (sub_427961(20))
                if (SDL_GetTicks() < accept_after_ms) continue;  // held-Enter debounce
                // Guard 2 first (sub_422085): same-controller humans -> error
                // getstring(45) over getstring(96).
                if (dup_controller()) {
                    if (show_error(ctx_.assets.getstring(
                            45, "Two players cannot use the same controls!")) == AppInput::Quit)
                        return AppInput::Quit;
                    continue;
                }
                // Guard 1 (sub_42223E): too few players/teams -> error
                // getstring(46) (solo) or getstring(48) (team) over
                // getstring(96).
                if (!count_ok()) {
                    const std::string reason =
                        state_.team_play ? ctx_.assets.getstring(48, "You need at least two teams!")
                                   : ctx_.assets.getstring(46, "You need at least two players!");
                    if (show_error(reason) == AppInput::Quit) return AppInput::Quit;
                    continue;
                }
                ctx_.audio.play(10);  // accept sting
                waiting = false;
                break;
            }
            ctx_.audio.play(20);  // any real key blips first (sub_427961(20))
            if (k == SDLK_UP)
                cursor = (cursor + 9) % 10;  // 328
            else if (k == SDLK_DOWN)
                cursor = (cursor + 1) % 10;                    // 336
            else if (k == SDLK_RIGHT)
                cycle_input_type(cursor);                      // 333 sub_421E80
            else if (k == SDLK_LEFT || k == SDLK_0 || k == SDLK_O) {  // 331 / '0' / 'o' (111)
                state_.setup_type[cursor] = 0;                       // sub_421E33(i,0,0)
                state_.setup_sub[cursor] = 0;
            } else if (k == SDLK_T) {  // 'T' team toggle (+84)
                // batch_0x410401.cpp 1206-1223: only an ACTIVE slot toggles;
                // an OFF slot buzzes (SFX 40) and does nothing.
                if (state_.setup_type[cursor] != 0)
                    state_.setup_team[cursor] = state_.setup_team[cursor] ? 0 : 1;
                else
                    ctx_.audio.play(40);
            } else if (k == SDLK_F1) {
                // sub_410F81 15432-15436: key 0x13B (F1) dispatches the SAME
                // generic *.BM help browser as menu row 5 / the options
                // screen / the in-round key (one routine, sub_41431C),
                // composited over this screen like every sub_41431C site.
                HelpBrowser browser(ctx_.assets, ctx_.front_font);
                browser.enter(ctx_.values.at_or(15, 1) != 0);
                while (!browser.done()) {
                    SDL_Event hev;
                    while (SDL_PollEvent(&hev)) {
                        if (hev.type == SDL_EVENT_QUIT) return AppInput::Quit;
                        if (hev.type == SDL_EVENT_KEY_DOWN) browser.on_key(hev.key.key, ctx_.audio);
                    }
                    if (browser.viewing() && browser.viewer().done()) browser.close_viewer();
                    ctx_.audio.update_music();
                    draw_frame();
                    browser.draw(ctx_.sdl);
                    SDL_RenderPresent(ctx_.sdl);
                    SDL_Delay(2);
                }
            }
        }
        ctx_.audio.update_music();
        draw_frame();
        SDL_RenderPresent(ctx_.sdl);
        SDL_Delay(2);
    }
    return AppInput::Advance;
}

}  // namespace bomber::game
