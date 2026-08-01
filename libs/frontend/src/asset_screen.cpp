#include "bomber/frontend/asset_screen.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <optional>

#include "bomber/platform/frame_clock.hpp"

namespace bomber::game {

namespace {

// One asset-driven full-screen image (logo / title / results / draw / victory)
// as its own loop object. `gate` is null on every local path; non-null it turns
// this into an ONLINE between-rounds screen (net_round_gate.hpp).
class AssetScreenLoop {
public:
    AssetScreenLoop(ScreenContext ctx, NetRoundGate* gate)
        : ctx_(ctx), gate_(gate), frame_clock_(ctx.window) {}

    AppInput run(const ScreenDef& def) {
        // Music is NOT touched here — the caller owns the continuous track;
        // sub_42A088 only presents an image.
        ctx_.asset_screen.enter(def, SDL_GetTicks());
        while (waiting_) {
            if (const std::optional<AppInput> exit = pump_events()) return *exit;
            ctx_.asset_screen.update(SDL_GetTicks());
            check_exit();
            ctx_.audio.update_music();
            draw();
            SDL_RenderPresent(ctx_.sdl);
            frame_clock_.pace();
        }
        // No transition out: sub_42A088 CUTS. Its palette set is instant (the >>2
        // is the 8->6-bit VGA conversion, NOT a fade loop), and there is no wipe
        // anywhere in the front end ("HEADWIPE.ANI is dead art").
        return result_;
    }

private:
    std::optional<AppInput> pump_events() {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
            // Gamepad advance, mirroring the menu's pad->synthetic-key injection.
            if (ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                push_synthetic_return();
                continue;
            }
            if (ev.type == SDL_EVENT_KEY_DOWN) on_key(ev.key.key);
        }
        return std::nullopt;
    }

    static void push_synthetic_return() {
        SDL_Event synth{};
        synth.type = SDL_EVENT_KEY_DOWN;
        synth.key.key = SDLK_RETURN;
        SDL_PushEvent(&synth);
    }

    void on_key(SDL_Keycode k) {
        // ONLINE between-rounds: a GUEST's key never dismisses the screen, it just
        // gets the SFX-40 buzz. Escape still leaves, because abandoning the match
        // is always the local player's own call. The HOST's accept commits the
        // next round but does NOT end the screen — it ends when the peer has the
        // commitment, so both leave together.
        if (gate_ != nullptr && k != SDLK_ESCAPE) {
            ctx_.audio.play(20);
            const bool readonly = gate_->readonly();
            if (readonly) ctx_.audio.play(40);
            if (!readonly) gate_->accept();
            return;
        }
        // sub_42A088 blips (SFX 20) on any key and, for the accept keys
        // (Enter/Space/Escape), plays the accept sting (SFX 10) and finishes. The
        // blip/sting come from the Screen, so the music track is untouched.
        ctx_.asset_screen.on_key(k);
        if (k != SDLK_ESCAPE) return;
        result_ = AppInput::Back;
        waiting_ = false;
    }

    // A gated screen's exit is a TWO-PEER event, so the Screen's own dwell/accept
    // verdict does not apply: only the gate says when to leave.
    void check_exit() {
        if (gate_ == nullptr) {
            if (ctx_.asset_screen.done()) waiting_ = false;
            return;
        }
        // The gate owns the ONLY pump of the transport for this screen's
        // duration (setup_session.hpp's one-pump-at-a-time rule).
        gate_->pump();
        // ready() is tested FIRST and WINS OUTRIGHT: a gate that has both held
        // the peer and then seen it drop still advances, which is what keeps the
        // two peers leaving on the same round.
        if (gate_->ready()) {
            waiting_ = false;
            return;
        }
        if (!gate_->failed()) return;
        result_ = AppInput::Back;
        waiting_ = false;
    }

    void draw() {
        SDL_SetRenderDrawColor(ctx_.sdl, 0, 0, 0, 255);
        SDL_RenderClear(ctx_.sdl);
        ctx_.asset_screen.draw(ctx_.sdl);
    }

    ScreenContext ctx_;
    NetRoundGate* gate_;
    // Refresh-boundary pacing: even a static screen spins this loop uncapped on
    // Windows without it — the same DWM non-blocking present as the animated ones.
    platform::FrameClock frame_clock_;
    AppInput result_ = AppInput::Advance;
    bool waiting_ = true;
};

}  // namespace

AppInput present_asset_screen(ScreenContext ctx, const ScreenDef& def, NetRoundGate* gate) {
    return AssetScreenLoop(ctx, gate).run(def);
}

}  // namespace bomber::game
