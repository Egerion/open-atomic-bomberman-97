#include "boot_loading.hpp"

#include <algorithm>  // std::clamp
#include <cmath>      // std::lround
#include <string>

#include "bomber/game_util/log.hpp"
#include "bomber/match/match_factory.hpp"
#include "bomber/render/sprites.hpp"
#include "bomber/ui/dialog_chrome.hpp"

namespace bomber::game {

namespace {

// The "Loading data..." bar spans TWO port phases that together are the equivalent
// of the original's single MASTER.ALI read (sub_41D695): the ANI/PCX DECODE and the
// per-player RECOLOR (the port bakes recoloured sprite sets where the original
// remapped at blit time, so it is extra work with no dialog of its own). The split
// is a rough work estimate, not RE'd — the only contract is that the bar stays
// monotonic and the window is pumped throughout.
constexpr float kBootDataDecodeShare = 0.55f;

// The boot LOADING dialog — RE-PINNED 2026-07-10 (docs/re/frontend-flow.md "The
// percent-bar dialog, sub_412E33"). NOT the IPLOGO/HSLOGO/TITLE chain (sub_42B060, a
// separate later step): a small modal progress window sub_42BE22 shows TWICE before
// sub_42B060 runs — "Loading data..." (getstring 201, sub_41D695's MASTER.ALI read)
// and "Loading sound..." (getstring 200, sub_4287B9's SOUNDLST preload). It is drawn
// programmatically, NOT a full-screen PCX: no LOADING*.PCX exists anywhere.
//
// Pinned geometry/colours (sub_412E33, pseudo.c 16157-16211 + the raw-byte passes):
// window y=200 (CONFIRMED literal, not centered), height=8*fontheight, width=360 (x
// auto-centered), painted with the WINZ.PCX 9-patch via sub_41726B @16181 — the BLUE
// textured window, correcting an earlier reading of a flat (82,82,82) grey. Caption
// centered at y=1.5*fontheight in the general white ink (byte_49D38F ->
// (240,248,252)); its STRING is the buffer at 0x45BC5C, whose "Completion" initial
// value IS IDA's `aCompletion` symbol — but sub_412E0C strcpy's the caller's
// getstring text over it before the dialog shows, so the earlier "captioned
// Completion" reading was wrong. "%d" readout centered at y=3.5*fontheight in YELLOW
// (byte_49D37A -> LUT idx 182 -> (252,248,88)); a 1-px WHITE FRAME around the bar
// band (sub_43D080 @16191, y from 5.5 to 6.5*fontheight — its x extent is a
// decompiler-lost window field, reconstructed as one px around the track); two-tone
// bar at x=31, y=5.5*fontheight+1, height=fontheight-1, width 300 split at 3*pct,
// filled portion byte_49A624 -> idx 178 -> (168,168,164), unfilled black. All text
// via sub_41696C = ink over a 4-pass 1-px black outline.
void draw_boot_loading_dialog(SDL_Renderer* ren, const FontTextures& font, const Sprite* winz,
                              const char* caption, float fraction) {
    fraction = std::clamp(fraction, 0.0f, 1.0f);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);

    const float h = static_cast<float>(font.loaded() ? font.line_height() : 12);
    const DialogRect win = dialog_rect(200.0f, 8.0f * h, 360.0f);
    draw_dialog_chrome(ren, win, winz);

    std::string cap_str = caption;
    float cap_w = font.loaded() ? static_cast<float>(font.measure(cap_str)) : 0.0f;
    draw_dialog_text(ren, font, cap_str, win.x + (win.w - cap_w) / 2, win.y + 1.5f * h, kDialogInkR,
                     kDialogInkG, kDialogInkB);

    SDL_FRect frame{win.x + 30.0f, win.y + 5.5f * h, 302.0f, h + 1.0f};
    SDL_SetRenderDrawColor(ren, kDialogInkR, kDialogInkG, kDialogInkB, 255);
    SDL_RenderRect(ren, &frame);

    // Two-tone bar (sub_43D1C0 x2): paint the whole 300-px track black, then the
    // filled prefix (width 3*pct == 300*fraction) on top.
    const float track_x = win.x + 31.0f;
    const float track_y = win.y + 5.5f * h + 1.0f;
    SDL_FRect track{track_x, track_y, 300.0f, h - 1.0f};
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderFillRect(ren, &track);
    SDL_FRect bar{track_x, track_y, 300.0f * fraction, h - 1.0f};
    SDL_SetRenderDrawColor(ren, 168, 168, 164, 255);  // byte_49A624 -> idx 178
    SDL_RenderFillRect(ren, &bar);

    std::string pct_str = std::to_string(static_cast<int>(std::lround(fraction * 100.0f)));
    float pct_w = font.loaded() ? static_cast<float>(font.measure(pct_str)) : 0.0f;
    draw_dialog_text(ren, font, pct_str, win.x + (win.w - pct_w) / 2, win.y + 3.5f * h, 252, 248,
                     88);

    SDL_RenderPresent(ren);
}

// One object so the progress callbacks the three phases hand to AssetStore and
// AudioEngine all reach the same dialog without threading a renderer and a font
// through every one of them.
class BootLoader {
public:
    explicit BootLoader(const DataLoadSlots& slots) : s_(slots) {}

    bool run() {
        if (!decode_assets()) return false;
        build_presentation();
        load_sound();
        return true;
    }

private:
    // Pump the OS queue, then repaint the dialog at `fraction` (0..1). A capture
    // runs scripted with no interactive window and hashes frames drawn later, so the
    // dialog is pure overhead there and never reaches the hashed output; skipping it
    // makes the progress callbacks no-ops and the load silent.
    void progress(const char* caption, float fraction) {
        if (is_capture_run(s_.opts)) return;
        // Draining the message queue is what stops the window entering "not
        // responding" during the (multi-second, on a DATA_HD install) synchronous
        // preload — the port's equivalent of the original pumping Windows messages
        // between its sub_412E33 repaints. The global SDL_EventFilter runs inside
        // this pump, so Alt+Enter/F11 still works mid-load; everything else is
        // discarded, since boot has no interactive screen yet.
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
        }
        draw_boot_loading_dialog(s_.ren, s_.front_font, &s_.assets.frontend_pcx("WINZ"), caption,
                                 fraction);
    }

    // FONT6 and WINZ.PCX load standalone BEFORE the dialogs, matching the real init
    // order (docs/re/frontend-flow.md "FONT6 timing", CONFIRMED): sub_41095A calls
    // sub_414DF4 — which pins FONT6 via sub_431E9C(6) and loads "winz.plt" — before
    // it calls sub_41D695/sub_42896E. A missing font leaves the glyph set empty and
    // the dialog's text silently no-ops, the fallback the .BM viewer relies on too.
    bool decode_assets() {
        const std::filesystem::path& game = s_.opts.game_dir;
        s_.assets.load_frontend_font(game);
        s_.front_font.build(s_.ren, s_.assets.frontend_font());
        s_.assets.load_frontend_winz(s_.ren, game);
        // MESSAGES.TXT is not loaded yet at this first flash (it lives inside
        // AssetStore::load), so the caption is the literal fallback; the second
        // flash below reads the real string once it is available.
        progress("Loading data...", 0.0f);
        if (!s_.assets.load(s_.ren, game, [this](float f) {
                progress("Loading data...", f * kBootDataDecodeShare);
            }))
            return false;
        s_.seqs.resolve(s_.assets);
        s_.gamepads.refresh();  // sub_429628; hotplug refreshes it again later
        return true;
    }

    // Seed setup-screen slot colours from VALUELST for any colour without a .RMP tail
    // (a loaded .RMP keeps its own authoritative tail), then build the per-player
    // recoloured sprite sets — the second half of the "Loading data..." work.
    void build_presentation() {
        s_.base_tuning = match::build_match_config(s_.scheme, 2, 0, &s_.values).tuning;
        s_.assets.set_color_fallbacks(s_.base_tuning.color_rgb, 10);
        s_.assets.build_player_sets(s_.base_tuning.color_rgb, [this](float f) {
            progress("Loading data...", kBootDataDecodeShare + f * (1.0f - kBootDataDecodeShare));
        });
        s_.seqs.resolve(s_.assets);  // re-resolve: player sprite sets exist now
        s_.renderer.emplace(s_.ren, s_.assets, s_.seqs, s_.values);
        s_.screen.emplace(s_.assets, s_.audio);
    }

    // The second flash, "Loading sound..." (getstring 200, now that MESSAGES.TXT is
    // loaded), before sub_42896E/sub_4287B9's preload. RUNS LAST, after the whole
    // "Loading data..." bar — decode AND recolour — has completed: that is sub_41D695
    // preceding sub_42896E. Skipped in demo mode, which never touches audio.
    void load_sound() {
        if (s_.opts.demo) return;
        const std::string cap = s_.assets.getstring(200, "Loading sound...");
        progress(cap.c_str(), 0.0f);
        if (!s_.audio.init(s_.opts.game_dir, [this, &cap](float f) { progress(cap.c_str(), f); }))
            log_warn("audio unavailable, continuing silent");
    }

    const DataLoadSlots& s_;
};

}  // namespace

bool load_game_data(const DataLoadSlots& s) {
    return BootLoader(s).run();
}

}  // namespace bomber::game
