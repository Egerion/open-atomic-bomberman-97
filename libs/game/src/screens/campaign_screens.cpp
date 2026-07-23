#include "bomber/game/screens/campaign_screens.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <string>
#include <utility>

#include "bomber/assets/campaign.hpp"      // assets::res::Campaign / load_campaign
#include "bomber/game/asset_store.hpp"     // Sprite (frontend_pcx)
#include "bomber/game/bmscreen.hpp"        // HelpBrowser
#include "bomber/game/campaign_screen.hpp"  // CampaignFilePicker
#include "bomber/game/dialog_chrome.hpp"   // DialogRect / draw_dialog_* / kDialogInk*
#include "bomber/game/frontend_util.hpp"   // pick_glue

namespace bomber::game {

AppInput HelpBrowserModal::run() {
    // The in-round F1 opening of the SAME browser (docs/re/in-match-shell.md
    // §1's sub_42A16F(1)/(0) bracket): this loop never calls sim_.tick — the
    // match is genuinely frozen for its duration, exactly like the menu-row
    // browser never advances anything either. The backdrop is the live
    // (frozen) match render rather than MAINMENU, since the original
    // overlays the list dialog on whatever screen was already current.
    HelpBrowser browser(ctx_.assets, ctx_.front_font);
    // getvalue(15) ("is the online manual enabled?", default 1, §4): gate
    // BEFORE the glob, matching sub_414235's own order — the SAME gate the
    // menu-row browser above applies, since sub_41431C is one routine.
    browser.enter(ctx_.values.at_or(15, 1) != 0);
    while (!browser.done()) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
            if (ev.type != SDL_EVENT_KEY_DOWN) continue;
            browser.on_key(ev.key.key, ctx_.audio);
        }
        if (browser.viewing() && browser.viewer().done()) browser.close_viewer();
        ctx_.audio.update_music();
        SDL_SetRenderDrawColor(ctx_.sdl, 0, 0, 0, 255);
        SDL_RenderClear(ctx_.sdl);
        backdrop_.renderer.draw_frame(backdrop_.state);
        browser.draw(ctx_.sdl);
        SDL_RenderPresent(ctx_.sdl);
        SDL_Delay(2);
    }
    return AppInput::Advance;
}

void CampaignPickerScreen::run() {
    // sub_4015C6 (docs/re/campaign.md "Trace: string refs -> loader ->
    // trigger -> entry point"): glob "*.cam" in the install root,
    // list, pick, parse, arm campaign mode. Runs its own nested loop exactly
    // like present_editor's chooser/picker loops — no AppState/AppInput slot,
    // since there is no menu row for this screen either.
    CampaignFilePicker picker(ctx_.assets, ctx_.front_font);
    picker.enter(state_.game_dir, pick_glue(state_.setup_lcg, ctx_.values));
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
    if (picker.cancelled() || picker.empty())
        return;  // sub_4015C6's error-dialog path (port: silent)

    try {
        assets::res::Campaign parsed = assets::res::load_campaign(picker.selected());
        if (parsed.stages.empty())
            return;  // "Couldn't open..." / zero-stage file: leave state untouched
        state_.campaign_stages = std::move(parsed.stages);
        state_.campaign_stage_index = 0;  // dword_4648B0 = 0
        if (!load_campaign_stage(0, state_)) {
            // The stage's scheme couldn't be resolved (e.g. a hand-authored
            // .CAM naming a scheme the install doesn't ship) — bail out of
            // arming campaign mode rather than starting a match against a
            // stale/mismatched board (port convenience; unpinned by the RE).
            state_.campaign_stages.clear();
            return;
        }
        state_.campaign_active = true;  // dword_46489C = 1
        // sub_4015C6's own confirmation overlay (getstring 1210 + 95) —
        // PORTED 2026-07-09 (present_campaign_confirm, above), replacing the
        // former accept-sting stand-in. The SEPARATE stage-start banner
        // (sub_40133F, getstring 1235/1230 — docs/re/campaign.md "Stage
        // banner") follows right after, same as the original's sub_410B6E
        // showing it for the freshly-armed stage 0.
        if (CampaignConfirmScreen(ctx_, backdrop_).run() == AppInput::Quit) {
            state_.campaign_active = false;
            state_.campaign_stages.clear();
            return;
        }
        if (CampaignBannerScreen(ctx_, state_, backdrop_).run() == AppInput::Quit) {
            state_.campaign_active = false;
            state_.campaign_stages.clear();
            return;
        }
    } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch)
        // aCouldnTOpenCam path (§1/§3): unreadable/corrupt file. Leave
        // campaign mode untouched, same as a cancelled picker.
    }
}

// The campaign-activation confirmation dialog (sub_4015C6, docs/re/
// campaign.md "Campaign-activation confirmation dialog") — PORTED
// 2026-07-09, replacing the accept-sting stand-in
// (formerly a coverage-audit.md crumb, now closed). Uses the SAME sub_43C734 chrome
// primitive (DialogRect/draw_dialog_chrome, above) as the quit-confirm
// dialog, sized from BOTH lines' text extents (sub_414340's own v24 =
// max(measure(top), measure(bottom)), traced from the raw disassembly at
// 0x41436c-0x4143a1: it measures LODWORD's text, then HIDWORD's, and keeps
// the wider) — width = max(that, 80)+64, height = 4*fontheight+64+2*
// fontheight (two lines).
//
// Line order/content — CONFIRMED via raw disassembly (BM95.EXE, imagebase
// 0x400000, capstone; see docs/re/campaign.md "Round pacing" provenance note
// for the same disassembly method), NOT guessed:
//   sub_4015C6 @ 0x401653-0x401669: `mov eax,0x4ba(1210); call getstring;
//   mov edx,eax; mov eax,0x5f(95); call getstring; call sub_414340` — so at
//   the call, EDX=getstring(1210)="Campaign Mode Activated!", EAX=
//   getstring(95)="NOTE!".
//   sub_414340 @ 0x414471-0x4144bb: draws the caller's EAX-sourced text
//   FIRST at the top y (fontheight+32), then the EDX-sourced text SECOND,
//   fontheight+2 further down — i.e. LODWORD/EAX is the TOP line, HIDWORD/
//   EDX is the BOTTOM line. So "NOTE!" (95) is on top, "Campaign Mode
//   Activated!" (1210) is below it — matching the SAME header-word-on-top
//   pattern the sibling error dialog uses (getstring(97)="Warning!" over
//   getstring(1215)="Campaigns not available!...", identical EAX/EDX
//   assignment at 0x4016b6-0x4016cc).
//   Both lines draw in the general white ink (byte_49D38F): the pushed
//   stack args at the sub_4172BA call sites are [byte_495390[0]=black,
//   byte_49D38F=white], and the register/ink wiring matches every other
//   sub_414340 call site already pinned in this file.
//   Position: y=(480-height)/2, x=(640-width)/2 — BOTH axes explicitly
//   computed by sub_414340 itself (0x4143fe-0x414425, against
//   dword_464A70=640/dword_464A6C=480), matching (and confirming, not just
//   approximating) the port's existing horizontal-centering convention for
//   this whole dialog family (dialog_rect/dialog_rect_vcentered, above).
//
// Dismiss behaviour — traced from sub_414340's own key loop
// (0x414510-0x414548, pseudo.c 17085-17106): every real key event plays the
// nav-blip (sub_427961(20)); only Enter(13)/Space(32)/Escape(27) close the
// dialog (v33=1 branch) — any OTHER key (arrows, letters, extended codes)
// just loops, waiting for another key. No Yes/No choice — it is a plain
// acknowledgement modal.
AppInput CampaignConfirmScreen::run() {
    const float h = static_cast<float>(ctx_.front_font.loaded() ? ctx_.front_font.line_height() : 12);
    std::string top_line = ctx_.assets.getstring(95, "NOTE!");
    std::string bottom_line = ctx_.assets.getstring(1210, "Campaign Mode Activated!");
    float top_w = ctx_.front_font.loaded() ? static_cast<float>(ctx_.front_font.measure(top_line)) : 0.0f;
    float bottom_w =
        ctx_.front_font.loaded() ? static_cast<float>(ctx_.front_font.measure(bottom_line)) : 0.0f;
    float win_w = std::max(std::max(top_w, bottom_w), 80.0f) + 64.0f;
    float win_h = 4.0f * h + 64.0f + 2.0f * h;
    DialogRect win = dialog_rect_vcentered(win_h, win_w);
    while (true) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
            if (ev.type != SDL_EVENT_KEY_DOWN) continue;
            const SDL_Keycode k = ev.key.key;
            ctx_.audio.play(20);  // nav blip, EVERY key (sub_427961(20))
            if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE || k == SDLK_ESCAPE) {
                return AppInput::Advance;
            }
        }
        ctx_.audio.update_music();
        SDL_SetRenderDrawColor(ctx_.sdl, 0, 0, 0, 255);
        SDL_RenderClear(ctx_.sdl);
        backdrop_.renderer.draw_frame(backdrop_.state);
        // sub_414340 paints the WINZ 9-patch too (its sub_41726B call @
        // pseudo.c 17070) and draws its lines via sub_41696C (outlined).
        draw_dialog_chrome(ctx_.sdl, win, &ctx_.assets.frontend_pcx("WINZ"));
        draw_dialog_text(ctx_.sdl, ctx_.front_font, top_line,
                         win.x + (win.w - top_w) / 2.0f, win.y + h + 32.0f, kDialogInkR,
                         kDialogInkG, kDialogInkB);  // byte_49D38F
        draw_dialog_text(ctx_.sdl, ctx_.front_font, bottom_line,
                         win.x + (win.w - bottom_w) / 2.0f, win.y + h + 32.0f + h + 2.0f,
                         kDialogInkR, kDialogInkG, kDialogInkB);
        SDL_RenderPresent(ctx_.sdl);
        SDL_Delay(2);
    }
}

// The stage-start banner (sub_40133F, docs/re/campaign.md "Stage banner"):
// getstring(1235)="(%s)" formatted with the stage name, over getstring(1230)
// ="Prepare to begin Campaign!". The original's dialog (sub_414340) blocks
// for a keypress; this port additionally dwells a couple seconds so an
// unattended auto-advance (stage-clear -> next stage) doesn't stall forever.
AppInput CampaignBannerScreen::run() {
    if (state_.campaign_banner.empty()) return AppInput::Advance;
    constexpr std::uint64_t kDwellMs = 2000;
    const std::uint64_t start = SDL_GetTicks();
    const std::string prepare = ctx_.assets.getstring(1230, "Prepare to begin Campaign!");
    while (true) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
            if (ev.type == SDL_EVENT_KEY_DOWN &&
                (ev.key.key == SDLK_RETURN || ev.key.key == SDLK_KP_ENTER ||
                 ev.key.key == SDLK_SPACE || ev.key.key == SDLK_ESCAPE)) {
                ctx_.audio.play(10);  // accept sting, sub_427961(10)
                return AppInput::Advance;
            }
        }
        if (SDL_GetTicks() - start >= kDwellMs) return AppInput::Advance;
        ctx_.audio.update_music();
        SDL_SetRenderDrawColor(ctx_.sdl, 0, 0, 0, 255);
        SDL_RenderClear(ctx_.sdl);
        backdrop_.renderer.draw_frame(backdrop_.state);
        ctx_.front_font.draw(ctx_.sdl, state_.campaign_banner, 220.0f, 200.0f, 255, 255, 255);
        ctx_.front_font.draw(ctx_.sdl, prepare, 220.0f, 224.0f, 255, 220, 80);
        SDL_RenderPresent(ctx_.sdl);
        SDL_Delay(2);
    }
}

AppInput CampaignCompleteScreen::run() {
    // sub_40133F's stage-exhausted branch (batch_0x401010.cpp:288-296): when
    // `++dword_4648B0 >= dword_45E014` the original pops a blocking sub_414340
    // acknowledge modal — getstring(1220) "Congratulations!" over getstring(1225)
    // "You made it through the whole campaign!", ink byte_49A390 = (164,0,0) dark
    // red (batch_0x401010.cpp:289/294, a3/foreground; frontend-flow.md) — then
    // returns to the menu. The port used to
    // clear campaign state silently. Waits for Enter/Space/Escape (nav blip on
    // any key), like every other sub_414340 modal; Quit if the window closed.
    const std::string top = ctx_.assets.getstring(1220, "Congratulations!");
    const std::string bottom = ctx_.assets.getstring(1225, "You made it through the whole campaign!");
    const std::string ok = ctx_.assets.getstring(27, " Ok ");
    while (true) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
            if (ev.type != SDL_EVENT_KEY_DOWN) continue;
            ctx_.audio.play(20);  // nav blip on any key
            if (ev.key.key == SDLK_RETURN || ev.key.key == SDLK_KP_ENTER ||
                ev.key.key == SDLK_SPACE || ev.key.key == SDLK_ESCAPE)
                return AppInput::Advance;
        }
        ctx_.audio.update_music();
        SDL_SetRenderDrawColor(ctx_.sdl, 0, 0, 0, 255);
        SDL_RenderClear(ctx_.sdl);
        backdrop_.renderer.draw_frame(backdrop_.state);
        draw_acknowledge_dialog(ctx_.sdl, ctx_.front_font, &ctx_.assets.frontend_pcx("WINZ"), top,
                                bottom, ok, 164, 0, 0);  // byte_49A390 = (164,0,0) dark red
        SDL_RenderPresent(ctx_.sdl);
        SDL_Delay(2);
    }
}

}  // namespace bomber::game
