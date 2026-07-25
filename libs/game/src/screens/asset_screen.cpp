#include "bomber/game/screens/asset_screen.hpp"

#include <SDL3/SDL.h>

#include <cstdint>

#include "bomber/platform/frame_clock.hpp"

namespace bomber::game {

AppInput present_asset_screen(ScreenContext ctx, const ScreenDef& def, NetRoundGate* gate) {
    // Enter the screen (resets its clock/counter; music is NOT touched here —
    // the caller owns the continuous track, sub_42A088 only presents an image).
    ctx.asset_screen.enter(def, SDL_GetTicks());
    AppInput result = AppInput::Advance;
    bool waiting = true;
    // Refresh-boundary pacing: even a static screen spins this loop uncapped on
    // Windows without it — the same DWM non-blocking present as the animated loops.
    platform::FrameClock frame_clock(ctx.window);
    while (waiting) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
            if (ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                // Gamepad advance (mirrors present_menu's pad->synthetic-key
                // injection): on_key treats every accept key as advance, so a
                // controller button synthesizes Enter and walks the logo/title
                // chain the same as a keyboard accept.
                SDL_Event synth{};
                synth.type = SDL_EVENT_KEY_DOWN;
                synth.key.key = SDLK_RETURN;
                SDL_PushEvent(&synth);
                continue;
            }
            if (ev.type == SDL_EVENT_KEY_DOWN) {
                // ONLINE between-rounds (net_round_gate.hpp): a GUEST is the
                // original's `sub_40C06A() == 1` client — its key never dismisses
                // the DRAW screen, it just gets the SFX-40 "can't do that here"
                // buzz (docs/re/in-match-shell.md's DRAW wait loop). Escape still
                // leaves: abandoning the match is always the local player's own
                // call. The HOST's accept commits the next round but does NOT end
                // the screen here — it ends when the peer has the commitment
                // (gate->ready() below), so both leave together.
                if (gate != nullptr && ev.key.key != SDLK_ESCAPE) {
                    ctx.audio.play(20);
                    if (gate->readonly())
                        ctx.audio.play(40);
                    else
                        gate->accept();
                    continue;
                }
                // Feed every key to the screen: sub_42A088 blips (SFX 20) on any
                // key and, for the accept keys (Enter/Space/Escape), plays the
                // accept sting (SFX 10) and finishes. Escape additionally routes
                // us "back"; Enter/Space "advance". The blip/sting come from the
                // Screen, so the music track is untouched — only the screen ends.
                ctx.asset_screen.on_key(ev.key.key);
                if (ev.key.key == SDLK_ESCAPE) {
                    result = AppInput::Back;
                    waiting = false;
                }
            }
        }
        std::uint64_t now = SDL_GetTicks();
        ctx.asset_screen.update(now);
        // A gated screen's exit is a TWO-PEER event, so the Screen's own
        // dwell/accept verdict does not apply: only the gate says when to leave.
        if (gate != nullptr) {
            gate->pump();
            if (gate->ready())
                waiting = false;
            else if (gate->failed()) {
                result = AppInput::Back;
                waiting = false;
            }
        } else if (ctx.asset_screen.done()) {
            waiting = false;
        }

        ctx.audio.update_music();
        SDL_SetRenderDrawColor(ctx.sdl, 0, 0, 0, 255);
        SDL_RenderClear(ctx.sdl);
        ctx.asset_screen.draw(ctx.sdl);
        SDL_RenderPresent(ctx.sdl);
        frame_clock.pace();
    }

    // No transition out: sub_42A088 CUTS between screens — it sets the palette
    // (sub_41522D, instant; the >>2 is the 8->6-bit VGA palette conversion, NOT
    // a fade loop), blits (sub_429FF1), and flips (sub_41043C). There is no wipe
    // anywhere in the front end (docs/re/frontend-flow.md "HEADWIPE.ANI is dead
    // art"), so the next screen simply replaces this one.
    return result;
}

}  // namespace bomber::game
