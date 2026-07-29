#include "bomber/game/screens/net_overlay.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "bomber/game/dialog_chrome.hpp"
#include "bomber/game/renderer.hpp"  // kScreenW / kScreenH
#include "bomber/net/net_stats.hpp"

namespace bomber::game {

namespace {

// Drawn at the same reduced scale the F7 fps indicator uses, via FontTextures::
// draw's `scale` (a dst-rect scale only — never SDL_SetRenderScale, which
// perturbs the whole render transform).
constexpr float kScale = 0.7f;
constexpr float kMargin = 3.0f;
constexpr float kPad = 4.0f;
constexpr float kPanelW = 214.0f;
constexpr Uint8 kPanelAlpha = 150;  // the field reads through, like the chat panel

// Three inks: the ordinary dialog white, an amber for "worth noticing", and a
// red for "this is the problem". Deliberately only three — a legend nobody has
// to learn is the difference between a diagnostic and decoration.
constexpr Uint8 kWarnR = 255, kWarnG = 200, kWarnB = 80;
constexpr Uint8 kBadR = 255, kBadG = 96, kBadB = 96;

// Above this the sparkline stops rescaling, so a 40 ms link and a 60 ms link do
// not both render as a full-height sawtooth. Spikes past it clip at the top,
// which is the correct reading: "off the scale".
constexpr int kSparkFloorMs = 150;
constexpr float kSparkH = 12.0f;

struct Ink {
    Uint8 r = kDialogInkR, g = kDialogInkG, b = kDialogInkB;
};

constexpr Ink kOk{};
constexpr Ink kWarn{kWarnR, kWarnG, kWarnB};
constexpr Ink kBad{kBadR, kBadG, kBadB};

std::string fmt(const char* f, ...) {  // NOLINT(cert-dcl50-cpp) — local, fixed buffer
    char buf[96];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return std::string(buf);
}

// -1 is "never measured", not "zero milliseconds", and the two must not look
// alike on a screen someone is about to draw a conclusion from.
std::string ms(int v) {
    return v < 0 ? std::string("--") : fmt("%d", v);
}

// "ME s0" / "ME s0,s2" — which seats at this machine, so a >2-seat match's
// per-peer rows can be read against the right player.
std::string local_seat_label(std::uint16_t local_seats) {
    std::string out;
    for (int i = 0; i < sim::kMaxPlayers; ++i) {
        if ((local_seats & static_cast<std::uint16_t>(1U << i)) == 0) continue;
        out += out.empty() ? "" : ",";
        out += fmt("%d", i);
    }
    return out.empty() ? std::string() : (" ME s" + out);
}

// The translucent slab the chat overlay uses, in the same chrome colours, so the
// two port-only overlays look like siblings rather than two inventions.
void draw_slab(SDL_Renderer* ren, const SDL_FRect& r) {
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, kDialogFillR, kDialogFillG, kDialogFillB, kPanelAlpha);
    SDL_RenderFillRect(ren, &r);
    SDL_SetRenderDrawColor(ren, kDialogDimR, kDialogDimG, kDialogDimB, kPanelAlpha);
    SDL_RenderRect(ren, &r);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
}

// A 1-px black outline under the ink, the same manual four-pass the fps overlay
// uses — the panel is translucent, so text over a bright field needs it.
void line_at(SDL_Renderer* ren, const FontTextures& font, const std::string& s, float x, float y,
             Ink ink) {
    font.draw(ren, s, x - 1, y, 0, 0, 0, kScale);
    font.draw(ren, s, x + 1, y, 0, 0, 0, kScale);
    font.draw(ren, s, x, y - 1, 0, 0, 0, kScale);
    font.draw(ren, s, x, y + 1, 0, 0, 0, kScale);
    font.draw(ren, s, x, y, ink.r, ink.g, ink.b, kScale);
}

// The RTT sparkline. Each bucket is 100 ms of history and holds that bucket's
// WORST sample, so a spike cannot be smoothed away by decimation; a bucket with
// no sample at all is stored as 0 and drawn as a GAP, which is exactly what
// "no acknowledgement came back in that tenth of a second" should look like.
void draw_spark(SDL_Renderer* ren, const net::PeerStats& p, float x, float y, float w) {
    const int top = std::max(kSparkFloorMs, p.rtt_recent_max_ms);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    // A dim baseline so the gaps are legible as gaps rather than as nothing.
    SDL_SetRenderDrawColor(ren, kDialogDimR, kDialogDimG, kDialogDimB, 90);
    SDL_FRect base{x, y + kSparkH, w, 1.0f};
    SDL_RenderFillRect(ren, &base);
    if (p.rtt_history_len == 0) {
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
        return;
    }
    const float bw = w / static_cast<float>(net::kRttHistory);
    for (std::size_t i = 0; i < p.rtt_history_len; ++i) {
        const int v = p.rtt_history[i];
        if (v == 0) continue;  // a gap: draw nothing, so the hole is visible
        const float h = std::min(1.0f, static_cast<float>(v) / static_cast<float>(top)) * kSparkH;
        const bool hot = v >= top && top > kSparkFloorMs;
        SDL_SetRenderDrawColor(ren, hot ? kBadR : kDialogInkR, hot ? kBadG : kDialogInkG,
                               hot ? kBadB : kDialogInkB, 220);
        SDL_FRect bar{x + static_cast<float>(i) * bw, y + kSparkH - h, std::max(1.0f, bw - 1.0f),
                      std::max(1.0f, h)};
        SDL_RenderFillRect(ren, &bar);
    }
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
}

// How a path should READ. Relayed and star are not faults, but they are the
// first thing to know when something goes wrong, so they are not drawn in the
// same ink as a direct match.
Ink path_ink(net::NetPath p) {
    switch (p) {
        case net::NetPath::Direct: return kOk;
        case net::NetPath::Relayed:
        case net::NetPath::StarHub: return kWarn;
        case net::NetPath::Loopback:
        case net::NetPath::Unknown: break;
    }
    return kBad;  // Unknown in a live match means something did not identify itself
}

std::string path_label(net::NetPath p) {
    switch (p) {
        case net::NetPath::Direct: return "DIRECT P2P";
        case net::NetPath::Relayed: return "RELAYED (SERVER IN PATH)";
        case net::NetPath::StarHub: return "STAR HUB (WE FAN OUT)";
        case net::NetPath::Loopback: return "LOOPBACK";
        case net::NetPath::Unknown: break;
    }
    return "UNKNOWN PATH";
}

}  // namespace

void draw_net_overlay(SDL_Renderer* ren, const FontTextures& font, const net::NetStats& s,
                      std::uint16_t local_seats) {
    if (ren == nullptr || !font.loaded()) return;

    const float lh = static_cast<float>(font.line_height()) * kScale + 1.0f;
    // Count the rows first so the slab is exactly as tall as its contents: a
    // panel with dead space at the bottom reads as a panel with missing data.
    int peer_count = 0;  // each tracked peer costs two text rows plus a sparkline
    bool any_offset_bound = false;
    for (const net::PeerStats& p : s.peers) {
        if (!p.tracked) continue;
        ++peer_count;
        if (p.rtt_offset_bound) any_offset_bound = true;
    }
    const int footer_rows = any_offset_bound ? 1 : 0;
    const float h = kPad * 2 + lh * static_cast<float>(6 + peer_count * 2 + footer_rows) +
                    static_cast<float>(peer_count) * (kSparkH + 3.0f);
    const SDL_FRect panel{kMargin, kMargin, kPanelW, h};
    draw_slab(ren, panel);

    float x = panel.x + kPad;
    float y = panel.y + kPad;
    const auto row = [&](const std::string& t, Ink ink) {
        line_at(ren, font, t, x, y, ink);
        y += lh;
    };

    // 1. THE PATH — first, biggest decision-changer, and the one field the
    //    client could never answer before. A direct match does not touch the
    //    matchmaker after tick 0, so this alone rules half the causes in or out.
    row(path_label(s.path) + local_seat_label(local_seats), path_ink(s.path));

    // 2. The loud states, if any. Shown here rather than buried at the bottom:
    //    a desynced or aborted session makes every number below it historical.
    if (s.desynced)
        row(fmt("DESYNC @ TICK %u", static_cast<unsigned>(s.desync_tick)), kBad);
    else if (s.aborted)
        row("PEER LOST - MATCH ENDING", kBad);
    else if (s.dropped_seats != 0)
        row(fmt("SEAT(S) 0x%03X -> AI", static_cast<unsigned>(s.dropped_seats)), kWarn);
    else
        row(fmt("TICK %u  CONF %u", static_cast<unsigned>(s.tick),
                static_cast<unsigned>(s.confirmed)),
            kOk);

    // 3. Prediction depth == how far the confirmed frontier trails the head.
    //    Amber once it is within one tick of the cap: that is the point past
    //    which the display stops being allowed to move.
    const bool deep = s.max_prediction > 0 && s.prediction_depth >= s.max_prediction - 1;
    row(fmt("PRED %d/%d  PEAK %d", s.prediction_depth, s.max_prediction, s.worst_prediction_depth),
        deep ? kWarn : kOk);

    // 4. THE STUTTER LINE. A pump the session was not allowed to simulate is
    //    what the player feels as a hitch; if this is zero the stutter is not
    //    the netcode, and that alone rules out half a day of guessing.
    row(fmt("STALL %d/s  (%u)", s.stalls_per_sec, static_cast<unsigned>(s.stall_pumps)),
        s.stalls_per_sec > 0 ? kBad : kOk);

    // 5. The correction workload.
    row(fmt("RB %d/s  RESIM %d/s  (%u)", s.rollbacks_per_sec, s.resim_ticks_per_sec,
            static_cast<unsigned>(s.rollbacks)),
        s.resim_ticks_per_sec > net::kPumpHz ? kWarn : kOk);

    // 6. Raw traffic. A non-zero BAD count means something on the path is
    //    corrupting or injecting — nothing else here would show that.
    row(fmt("RX %d/s  BAD %u", s.rx_per_sec, static_cast<unsigned>(s.rx_malformed)),
        s.rx_malformed > 0 ? kBad : kOk);

    for (int i = 0; i < sim::kMaxPlayers; ++i) {
        const net::PeerStats& p = s.peers[static_cast<std::size_t>(i)];
        if (!p.tracked) continue;
        if (!p.live) {
            row(fmt("s%d  DROPPED -> AI", i), kWarn);
            y += lh + kSparkH + 3.0f;  // keep the layout the height calc reserved
            continue;
        }
        const Ink lag_ink = (s.max_prediction > 0 && p.lag_ticks >= s.max_prediction) ? kBad
                            : (p.lag_ticks > s.max_prediction / 2)                    ? kWarn
                                                                                      : kOk;
        // The "*" is the whole honesty of this line: while the peer is running
        // behind us, the ack-RTT is measuring that tick offset and not the wire,
        // so it must not be read as a path latency (net_stats.hpp).
        const char* star = p.rtt_offset_bound ? "*" : "";
        row(fmt("s%d LAG %dt  RTT %sms%s  J%d", i, p.lag_ticks, ms(p.rtt_smooth_ms).c_str(), star,
                p.jitter_ms),
            lag_ink);
        // The loss figure is an ESTIMATE and is marked "~" on screen for the same
        // reason it is documented as one: it cannot separate datagrams lost on
        // the path from the peer's own loop stalling. MIN is the tightest
        // reading of the real path — the ack-RTT is an upper bound.
        row(fmt("   %d/s  ~%d%% LOSS  MIN %sms%s", p.recv_per_sec, p.loss_pct_est,
                ms(p.rtt_min_ms).c_str(), star),
            p.loss_pct_est >= 20 ? kBad : (p.loss_pct_est >= 5 ? kWarn : kOk));
        draw_spark(ren, p, x, y, kPanelW - kPad * 2);
        y += kSparkH + 3.0f;
    }
    if (any_offset_bound) row("* RTT = TICK OFFSET, NOT PATH", kWarn);
}

std::string net_log_timestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    char buf[32];
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv) == 0) return "-";
    return std::string(buf);
}

void append_net_session_log(const net::SessionSummary& summary) {
    // NEXT TO THE EXECUTABLE, on purpose: the player can find it without being
    // told a path, and it travels with the build it describes. SDL_GetBasePath
    // returns null on the platforms that have no such notion — fall back to the
    // working directory rather than dropping the record.
    std::filesystem::path dir;
    if (const char* base = SDL_GetBasePath(); base != nullptr) dir = base;
    const std::filesystem::path file = dir / "netdiag.log";
    // Silent on every failure. A read-only install directory must not be able to
    // take a match down, and there is nowhere useful to report it to anyway.
    std::ofstream out(file, std::ios::app | std::ios::binary);
    if (!out) return;
    out << net::format_session_log_line(summary);
}

AppInput present_net_session_end(ScreenContext ctx, const net::SessionSummary& summary) {
    // The on-screen half of "say WHY". Modelled on DebugInfoScreen's modal (the
    // RE'd Alt+D window): a WINZ 9-patch panel, Enter or Escape dismisses. Drawn
    // over black rather than over the frozen field, because by the time this is
    // shown the match's own renderer state is finished with.
    const net::NetStats& s = summary.stats;
    std::vector<std::pair<std::string, Ink>> rows;
    rows.emplace_back(std::string("NETWORK SESSION ENDED"), kBad);
    rows.emplace_back(std::string("REASON: ") + net::end_reason_name(summary.reason), kWarn);
    rows.emplace_back(std::string("PATH: ") + path_label(s.path), path_ink(s.path));
    if (s.desynced)
        rows.emplace_back(fmt("DESYNC AT TICK %u", static_cast<unsigned>(s.desync_tick)), kBad);
    rows.emplace_back(fmt("TICK %u   CONFIRMED %u", static_cast<unsigned>(s.tick),
                          static_cast<unsigned>(s.confirmed)),
                      kOk);
    rows.emplace_back(
        fmt("STALLS %u   ROLLBACKS %u   RESIM %u", static_cast<unsigned>(s.stall_pumps),
            static_cast<unsigned>(s.rollbacks), static_cast<unsigned>(s.resim_ticks)),
        kOk);
    rows.emplace_back(fmt("PACKETS %u   MALFORMED %u", static_cast<unsigned>(s.rx_packets),
                          static_cast<unsigned>(s.rx_malformed)),
                      s.rx_malformed > 0 ? kBad : kOk);
    for (int i = 0; i < sim::kMaxPlayers; ++i) {
        const net::PeerStats& p = s.peers[static_cast<std::size_t>(i)];
        if (!p.tracked) continue;
        rows.emplace_back(fmt("SEAT %d  LAG %dt (PEAK %dt)  RTT %d/%d/%d MS%s", i, p.lag_ticks,
                              p.worst_lag_ticks, p.rtt_ms, p.rtt_min_ms, p.rtt_max_ms,
                              p.rtt_offset_bound ? " (TICK OFFSET, NOT PATH)" : ""),
                          p.live ? kOk : kWarn);
    }
    rows.emplace_back(std::string("WRITTEN TO netdiag.log NEXT TO THE GAME"), kOk);

    const float lh = static_cast<float>(ctx.front_font.line_height());
    const float panel_h = std::max(180.0f, lh * static_cast<float>(rows.size() + 4) + 32.0f);
    const DialogRect win{(static_cast<float>(kScreenW) - 460.0f) / 2.0f,
                         (static_cast<float>(kScreenH) - panel_h) / 2.0f, 460.0f, panel_h};

    while (true) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
            if (ev.type != SDL_EVENT_KEY_DOWN || ev.key.repeat) continue;
            if (ev.key.key == SDLK_RETURN || ev.key.key == SDLK_KP_ENTER ||
                ev.key.key == SDLK_ESCAPE || ev.key.key == SDLK_SPACE)
                return AppInput::Advance;
        }
        ctx.audio.update_music();
        SDL_SetRenderDrawColor(ctx.sdl, 0, 0, 0, 255);
        SDL_RenderClear(ctx.sdl);
        draw_dialog_chrome(ctx.sdl, win, &ctx.assets.frontend_pcx("WINZ"));
        float ty = win.y + 16.0f;
        for (const auto& [text, ink] : rows) {
            draw_dialog_text(ctx.sdl, ctx.front_font, text, win.x + 20.0f, ty, ink.r, ink.g, ink.b);
            ty += lh + 2.0f;
        }
        draw_dialog_text(ctx.sdl, ctx.front_font, "PRESS [ENTER] OR [ESC]", win.x + 20.0f,
                         win.y + win.h - 16.0f - lh, kDialogInkR, kDialogInkG, kDialogInkB);
        SDL_RenderPresent(ctx.sdl);
        SDL_Delay(2);
    }
}

}  // namespace bomber::game
