#include "bomber/game/screens/net_setup_link.hpp"

#include <SDL3/SDL.h>

#include <cstddef>

#include "bomber/game/dialog_chrome.hpp"     // draw_acknowledge_dialog / the ink constants
#include "bomber/game/net_setup_roster.hpp"  // the pure roster/level mapping (unit-tested)
#include "bomber/game/sprites.hpp"           // Sprite
#include "bomber/net/protocol.hpp"           // net::SetupPreviewFrame
#include "bomber/net/setup_session.hpp"      // net::SetupSession
#include "bomber/platform/frame_clock.hpp"

namespace bomber::game {

namespace {

// The [WAIT] spinner table — `dword_45BFB4[4] = { 47, 45, 92, 124 }`
// (docs/re/network-screens.md §6, "a classic 4-phase spinner").
constexpr char kSpinner[] = {'/', '-', '\\', '|'};
// The prompt's pinned anchor: x = 150, y = 200, clip w = 400 (§6, correcting
// setup-screens.md's "(150,400)" reading).
constexpr float kWaitPromptX = 150.0f;
constexpr float kWaitPromptY = 200.0f;
constexpr float kWaitPromptW = 400.0f;

// A MESSAGES.TXT `%c` splice, the same crash-proof shape as hud_format.hpp's
// fmt_u/fmt_s: the format string is the user's own file, so substitute the first
// %c and leave anything else literal rather than risk a wrong-type sprintf.
std::string fmt_c(const std::string& f, char v) {
    const std::size_t p = f.find('%');
    if (p == std::string::npos || p + 1 >= f.size()) return f;
    if (f[p + 1] != 'c') return f;
    return f.substr(0, p) + std::string(1, v) + f.substr(p + 2);
}

}  // namespace

void net_setup_pump(const NetSetupLink& l) {
    if (l.session == nullptr) return;
    l.session->step(static_cast<std::int64_t>(SDL_GetTicks()));
}

bool net_setup_final(const NetSetupLink& l) {
    // Phase::Final, NOT has_final_config(): the latter is true on the host the
    // instant it confirms, while Final means BOTH peers hold the same bytes
    // (setup_session.hpp's first caller obligation).
    return l.session != nullptr && l.session->phase() == net::SetupSession::Phase::Final;
}

bool net_setup_failed(const NetSetupLink& l) {
    return l.session != nullptr && l.session->failed();
}

bool net_setup_has_preview(const NetSetupLink& l) {
    return l.session != nullptr && l.session->has_preview();
}

bool net_setup_on_level_screen(const NetSetupLink& l) {
    return net_setup_has_preview(l) && l.session->preview().rounds != 0;
}

void net_setup_seed_host_roster(const NetSetupLink& l, std::array<int, sim::kMaxPlayers>& type,
                                std::array<int, sim::kMaxPlayers>& sub) {
    if (l.session == nullptr) return;
    seed_net_host_roster(l.local_seats, l.remote_seats, type, sub);
}

void net_setup_publish(const NetSetupLink& l, const std::array<int, sim::kMaxPlayers>& type,
                       const std::array<int, sim::kMaxPlayers>& team, bool team_play, int level,
                       const std::string& level_name, int rounds) {
    if (l.session == nullptr || !l.host) return;
    net::SetupPreviewFrame p;
    fill_preview_roster(type, team, team_play, p);
    p.level_index = index_to_preview_level(level);
    p.level_name = wire_safe_level_name(level_name);
    p.rounds = rounds < 0 ? std::uint8_t{0} : static_cast<std::uint8_t>(rounds & 0xFF);
    l.session->publish(p);
}

void net_setup_publish_level(const NetSetupLink& l, int level, const std::string& level_name,
                             int rounds) {
    if (l.session == nullptr || !l.host) return;
    // The host's own last frame IS the roster half; only the level fields move,
    // so the level screen — which holds no roster state — cannot blank the
    // roster the guest is showing.
    net::SetupPreviewFrame p =
        l.session->has_preview() ? l.session->preview() : net::SetupPreviewFrame{};
    p.level_index = index_to_preview_level(level);
    p.level_name = wire_safe_level_name(level_name);
    p.rounds = rounds < 0 ? std::uint8_t{0} : static_cast<std::uint8_t>(rounds & 0xFF);
    l.session->publish(p);
}

void net_setup_apply_roster(const NetSetupLink& l, std::array<int, sim::kMaxPlayers>& type,
                            std::array<int, sim::kMaxPlayers>& sub,
                            std::array<int, sim::kMaxPlayers>& team, bool& team_play) {
    if (!net_setup_has_preview(l)) return;
    apply_preview_roster(l.session->preview(), l.local_seats, type, sub, team, team_play);
}

void net_setup_apply_level(const NetSetupLink& l, int level_count, int& level, int& rounds,
                           std::string& level_name) {
    if (!net_setup_has_preview(l)) return;
    const net::SetupPreviewFrame& p = l.session->preview();
    level = preview_level_to_index(p.level_index, level_count);
    rounds = p.rounds != 0 ? static_cast<int>(p.rounds) : rounds;
    if (!p.level_name.empty()) level_name = p.level_name;
}

void draw_net_wait_prompt(ScreenContext ctx, unsigned& spinner) {
    const char c = kSpinner[spinner % (sizeof(kSpinner) / sizeof(kSpinner[0]))];
    ++spinner;  // dword_464AFC advances once per RENDERED frame, not per ms
    const std::string line = fmt_c(ctx.assets.getstring(80, "Waiting for the server... %c"), c);
    ctx.front_font.draw_outlined(ctx.sdl, line, kWaitPromptX, kWaitPromptY, kDialogInkR,
                                 kDialogInkG, kDialogInkB, 0, 0, 0, kWaitPromptW);
}

AppInput run_net_notice(ScreenContext ctx, const std::string& top, const std::string& body) {
    platform::FrameClock frame_clock(ctx.window);
    const Sprite* winz = &ctx.assets.frontend_pcx("WINZ");
    const std::string ok_label = ctx.assets.getstring(27, " Ok ");
    while (true) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
            if (ev.type != SDL_EVENT_KEY_DOWN || ev.key.repeat) continue;
            if (ev.key.key == SDLK_RETURN || ev.key.key == SDLK_KP_ENTER ||
                ev.key.key == SDLK_SPACE || ev.key.key == SDLK_ESCAPE)
                return AppInput::Advance;
            ctx.audio.play(20);  // sub_414340's own loop: the nav blip on any real key
        }
        ctx.audio.update_music();
        SDL_SetRenderDrawColor(ctx.sdl, 0, 0, 0, 255);
        SDL_RenderClear(ctx.sdl);
        const Sprite& bg = ctx.assets.frontend_pcx("MAINMENU");
        if (bg.tex) {
            SDL_FRect d{0, 0, static_cast<float>(bg.w), static_cast<float>(bg.h)};
            SDL_RenderTexture(ctx.sdl, bg.tex, nullptr, &d);
        }
        draw_acknowledge_dialog(ctx.sdl, ctx.front_font, winz, top, body, ok_label, kDialogInkR,
                                kDialogInkG, kDialogInkB);
        SDL_RenderPresent(ctx.sdl);
        frame_clock.pace();
    }
}

}  // namespace bomber::game
