#include "global_hotkeys.hpp"

#include "bomber/game_util/log.hpp"
#include "bomber/netui/net_overlay.hpp"  // kNetOverlayToggleKey

namespace bomber::game {

namespace {

// SDL3's borderless "desktop" fullscreen (no explicit SDL_DisplayMode) resizes
// the OS window/output only; kScreenW/kScreenH and the sim are untouched, since
// SDL_LOGICAL_PRESENTATION_STRETCH fills any output size edge to edge.
void toggle_fullscreen(const HotkeySlots& s) {
    s.fullscreen = !s.fullscreen;
    SDL_SetWindowFullscreen(s.window, s.fullscreen);
    s.options_dirty = true;  // persist the choice; flush_settings is the writer
}

// Selects optional DATA_HD artwork without changing the fixed gameplay
// coordinate system or the simulation.
void toggle_hd_artwork(const HotkeySlots& s) {
    const bool enabling = !s.assets.hd_enabled();
    // The per-player HD sprite sets are built lazily the first time HD is turned
    // on (build_player_sets skips them at boot to save the ~16x-heavier HD memory
    // while HD is off). When ensure_player_hd_sets() reports that it built them,
    // re-resolve the SequenceSet so the per-player Sprites pick up their fresh
    // tex_hd — the shared sets' HD was already resolved at boot. resolve() only
    // rebuilds the stage-independent sequences, leaving the current stage's tiles
    // intact.
    if (enabling && s.assets.ensure_player_hd_sets()) s.seqs.resolve(s.assets);
    s.assets.set_hd_enabled(enabling);
    SDL_SetWindowTitle(
        s.window, s.assets.hd_enabled() ? "Atomic Bomberman [HD]" : "Atomic Bomberman [Classic]");
    log_info("artwork mode: %s", s.assets.hd_enabled() ? "HD" : "classic");
}

// Flip the flag and the renderer's vsync in lockstep. OFF = vsync on (present
// blocks on vblank, the refresh-boundary pacer caps at 60); ON = vsync off
// (present returns immediately, the sub-frame pacer free-runs to ~180). The match
// pacer reads the flag every iteration, so this takes effect on the next frame.
void toggle_uncapped_fps(const HotkeySlots& s) {
    s.uncap_fps = !s.uncap_fps;
    SDL_SetRenderVSync(s.sdl, s.uncap_fps ? 0 : 1);
    log_info("framerate: %s",
             s.uncap_fps ? "uncapped (~180 fps, native feel)" : "vsync (60 fps, smooth)");
}

// The sim runs per displayed frame on the real wall-clock delta, drawn without
// interpolation. Read by MatchRunner on the next frame.
void toggle_native_cadence(const HotkeySlots& s) {
    s.native_cadence = !s.native_cadence;
    log_info("cadence: %s", s.native_cadence ? "native per-frame wall-clock (non-deterministic)"
                                             : "fixed 20 Hz + interpolation (deterministic)");
}

}  // namespace

bool handle_global_hotkey(const HotkeySlots& s, const SDL_Event& ev) {
    if (ev.type != SDL_EVENT_KEY_DOWN || ev.key.repeat) return true;  // ignore key-repeat spam
    switch (ev.key.key) {
        case SDLK_TAB: toggle_hd_artwork(s); break;
        case SDLK_F7:
            s.show_fps = !s.show_fps;
            log_info("fps indicator: %s", s.show_fps ? "on" : "off");
            break;
        case SDLK_F8: toggle_uncapped_fps(s); break;
        case SDLK_F9: toggle_native_cadence(s); break;
        case SDLK_F11: toggle_fullscreen(s); break;
        // MatchRunner only draws the panel when a netplay session is actually
        // running, so pressing this in a local match is a harmless no-op rather
        // than an empty panel.
        case kNetOverlayToggleKey:
            s.show_netstats = !s.show_netstats;
            log_info("netplay overlay: %s", s.show_netstats ? "on" : "off");
            break;
        case SDLK_RETURN:
            if ((ev.key.mod & SDL_KMOD_ALT) == 0) return true;  // a plain Enter is the screen's
            toggle_fullscreen(s);
            break;
        default: return true;  // not ours
    }
    return false;
}

}  // namespace bomber::game
