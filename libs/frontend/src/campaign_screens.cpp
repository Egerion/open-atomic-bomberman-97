#include "bomber/frontend/campaign_screens.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

#include "bomber/assets/campaign.hpp"           // assets::res::Campaign / load_campaign
#include "bomber/frontend/campaign_screen.hpp"  // CampaignFilePicker
#include "bomber/game_util/frontend_util.hpp"   // pick_glue
#include "bomber/render/asset_store.hpp"        // Sprite (frontend_pcx)
#include "bomber/ui/dialog_chrome.hpp"          // DialogRect / draw_dialog_* / kDialogInk
#include "bomber/ui/help_screens.hpp"           // run_help_browser

namespace bomber::game {

namespace {

// sub_414340's key loop (0x414510-0x414548, pseudo.c 17085-17106): every real key
// plays the nav blip (sub_427961(20)); only Enter/Space/Escape close the dialog —
// any OTHER key just loops. There is no accept sting anywhere in the routine.
bool is_dismiss_key(SDL_Keycode k) {
    return k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE || k == SDLK_ESCAPE;
}

// The shared pump for every sub_414340 acknowledge modal below: Quit on window
// close, Advance once dismissed, nullopt to keep the dialog up.
std::optional<AppInput> pump_acknowledge(ScreenContext& ctx) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
        if (ev.type != SDL_EVENT_KEY_DOWN) continue;
        ctx.audio.play(20);
        if (is_dismiss_key(ev.key.key)) return AppInput::Advance;
    }
    return std::nullopt;
}

// The two-line acknowledge modal both campaign end-states share, over the live
// match frame. Ink byte_49A390 = (164,0,0) dark red.
AppInput run_acknowledge(ScreenContext& ctx, MatchBackdrop& backdrop, const std::string& top,
                         const std::string& bottom) {
    const std::string ok = ctx.assets.getstring(27, " Ok ");
    while (true) {
        if (const std::optional<AppInput> exit = pump_acknowledge(ctx)) return *exit;
        ctx.audio.update_music();
        SDL_SetRenderDrawColor(ctx.sdl, 0, 0, 0, 255);
        SDL_RenderClear(ctx.sdl);
        backdrop.renderer.draw_frame(backdrop.state);
        draw_acknowledge_dialog(DialogPen{ctx.sdl, ctx.front_font},
                                &ctx.assets.frontend_pcx("WINZ"),
                                AcknowledgeLabels{top, bottom, ok}, AcknowledgeStyle{{164, 0, 0}});
        SDL_RenderPresent(ctx.sdl);
        SDL_Delay(2);
    }
}

}  // namespace

// The in-round F1 browser (the sub_42A16F(1)/(0) bracket): the shared
// run_help_browser loop — this never ticks the sim, the match is genuinely
// frozen for its duration — with the live frozen match render as the backdrop
// rather than MAINMENU, since the original overlays the list dialog on whatever
// screen was current.
AppInput HelpBrowserModal::run() {
    return run_help_browser(ctx_, [this] { backdrop_.renderer.draw_frame(backdrop_.state); });
}

// sub_4015C6: glob "*.cam" in the install root, list, pick, parse, arm campaign
// mode. Runs its own nested loop — there is no menu row for this screen, so no
// AppState/AppInput slot either.
void CampaignPickerScreen::run() {
    CampaignFilePicker picker(ctx_.assets, ctx_.front_font);
    picker.enter(state_.game_dir, pick_glue(state_.setup_lcg, ctx_.values));
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
    if (picker.cancelled() || picker.empty()) return;  // sub_4015C6's error path
    try {
        arm_campaign(picker.selected());
    } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch)
        // aCouldnTOpenCam path (§1/§3): unreadable/corrupt file. Leave campaign
        // mode untouched, same as a cancelled picker.
    }
}

// Parse the picked .CAM, seed stage 0 and show the confirm + banner dialogs.
// Anything that fails leaves campaign mode untouched rather than starting a match
// against a stale board.
void CampaignPickerScreen::arm_campaign(const std::filesystem::path& file) {
    assets::res::Campaign parsed = assets::res::load_campaign(file);
    if (parsed.stages.empty()) return;  // "Couldn't open..." / zero-stage file
    state_.campaign_stages = std::move(parsed.stages);
    state_.campaign_stage_index = 0;  // dword_4648B0 = 0
    // The stage's scheme couldn't be resolved (e.g. a hand-authored .CAM naming a
    // scheme the install doesn't ship) — port convenience, unpinned by the RE.
    if (!load_campaign_stage(0, state_)) {
        state_.campaign_stages.clear();
        return;
    }
    state_.campaign_active = true;  // dword_46489C = 1
    // sub_4015C6's own confirmation overlay, then the SEPARATE stage-start banner
    // (sub_40133F), same as the original's sub_410B6E showing it for the
    // freshly-armed stage 0.
    if (CampaignConfirmScreen(ctx_, backdrop_).run() == AppInput::Quit) return disarm_campaign();
    if (CampaignBannerScreen(ctx_, state_, backdrop_).run() == AppInput::Quit)
        return disarm_campaign();
}

void CampaignPickerScreen::disarm_campaign() {
    state_.campaign_active = false;
    state_.campaign_stages.clear();
}

// The campaign-activation confirmation dialog (sub_4015C6) — the SAME sub_43C734
// chrome as the quit-confirm dialog, sized from BOTH lines' extents
// (0x41436c-0x4143a1) and centred on both axes by sub_414340 itself
// (0x4143fe-0x414425).
//
// LINE ORDER — CONFIRMED via raw disassembly, not guessed: sub_4015C6 resolves id
// 1210 into EDX then id 95 into EAX, and sub_414340 draws the EAX text FIRST, so
// "NOTE!" is the TOP line. Same header-word-on-top pattern as the sibling error
// dialog (getstring(97) over getstring(1215), identical register assignment).
AppInput CampaignConfirmScreen::run() {
    const float h =
        static_cast<float>(ctx_.front_font.loaded() ? ctx_.front_font.line_height() : 12);
    const std::string top_line = ctx_.assets.getstring(95, "NOTE!");
    const std::string bottom_line = ctx_.assets.getstring(1210, "Campaign Mode Activated!");
    const float top_w = measure(top_line);
    const float bottom_w = measure(bottom_line);
    const float win_w = std::max(std::max(top_w, bottom_w), 80.0f) + 64.0f;
    const DialogRect win = dialog_rect_vcentered(4.0f * h + 64.0f + 2.0f * h, win_w);
    while (true) {
        if (const std::optional<AppInput> exit = pump_acknowledge(ctx_)) return *exit;
        ctx_.audio.update_music();
        SDL_SetRenderDrawColor(ctx_.sdl, 0, 0, 0, 255);
        SDL_RenderClear(ctx_.sdl);
        backdrop_.renderer.draw_frame(backdrop_.state);
        // sub_414340 paints the WINZ 9-patch too (its sub_41726B call @ pseudo.c
        // 17070) and draws its lines via sub_41696C (outlined).
        draw_dialog_chrome(ctx_.sdl, win, &ctx_.assets.frontend_pcx("WINZ"));
        const DialogPen pen{ctx_.sdl, ctx_.front_font};
        draw_dialog_text(pen, top_line,
                         SDL_FPoint{win.x + (win.w - top_w) / 2.0f, win.y + h + 32.0f},
                         DialogInk{kDialogInk});
        draw_dialog_text(
            pen, bottom_line,
            SDL_FPoint{win.x + (win.w - bottom_w) / 2.0f, win.y + h + 32.0f + h + 2.0f},
            DialogInk{kDialogInk});
        SDL_RenderPresent(ctx_.sdl);
        SDL_Delay(2);
    }
}

float CampaignConfirmScreen::measure(const std::string& s) const {
    return ctx_.front_font.loaded() ? static_cast<float>(ctx_.front_font.measure(s)) : 0.0f;
}

// The stage-start banner (sub_40133F, docs/re/campaign.md "Stage banner"). The
// original's dialog blocks for a keypress; this port additionally dwells a couple
// of seconds so an unattended stage auto-advance doesn't stall forever.
AppInput CampaignBannerScreen::run() {
    if (state_.campaign_banner.empty()) return AppInput::Advance;
    constexpr std::uint64_t kDwellMs = 2000;
    const std::uint64_t start = SDL_GetTicks();
    const std::string prepare = ctx_.assets.getstring(1230, "Prepare to begin Campaign!");
    while (true) {
        if (const std::optional<AppInput> exit = pump_acknowledge(ctx_)) return *exit;
        if (SDL_GetTicks() - start >= kDwellMs) return AppInput::Advance;
        ctx_.audio.update_music();
        SDL_SetRenderDrawColor(ctx_.sdl, 0, 0, 0, 255);
        SDL_RenderClear(ctx_.sdl);
        backdrop_.renderer.draw_frame(backdrop_.state);
        ctx_.front_font.draw(ctx_.sdl, state_.campaign_banner, SDL_FPoint{220.0f, 200.0f},
                             TextStyle{{255, 255, 255}});
        ctx_.front_font.draw(ctx_.sdl, prepare, SDL_FPoint{220.0f, 224.0f},
                             TextStyle{{255, 220, 80}});
        SDL_RenderPresent(ctx_.sdl);
        SDL_Delay(2);
    }
}

// sub_42A3F6's campaign arm, 0x42A660-0x42A686 (docs/re/campaign.md "Round end"),
// with the same ink pair as "Congratulations!" below — instruction for
// instruction what sub_40133F does at 0x401374/0x40137C.
AppInput CampaignUnsuccessfulScreen::run() {
    return run_acknowledge(ctx_, backdrop_, ctx_.assets.getstring(1240, "Oh Well!"),
                           ctx_.assets.getstring(1245, "Campaign unsuccessful!"));
}

// sub_40133F's stage-exhausted branch: when `++dword_4648B0 >= dword_45E014` the
// original pops a blocking acknowledge modal before returning to the menu. The
// port used to clear campaign state silently.
AppInput CampaignCompleteScreen::run() {
    const std::string top = ctx_.assets.getstring(1220, "Congratulations!");
    const std::string bottom =
        ctx_.assets.getstring(1225, "You made it through the whole campaign!");
    return run_acknowledge(ctx_, backdrop_, top, bottom);
}

}  // namespace bomber::game
