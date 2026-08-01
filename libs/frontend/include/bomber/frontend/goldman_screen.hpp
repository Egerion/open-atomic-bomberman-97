#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>

#include "bomber/audio/audio_engine.hpp"
#include "bomber/game_util/goldman_wheel.hpp"
#include "bomber/render/asset_store.hpp"
#include "bomber/render/sequences.hpp"
#include "bomber/ui/bmscreen.hpp"

// The Goldman Roulette wheel screen — sub_4034BC @0x4034BC
// (docs/re/goldman-roulette.md), mirroring OptionsScreen's enter()/on_key()/
// draw() shape. ROULETTE.PCX + "roulette.plt" backdrop, the "ring" ANI pointer,
// the 6 prize icons (the shared "power <name>" ANI icons), and the pure spin
// math (goldman_wheel.hpp) advanced one frame per draw().

namespace bomber::game {

// The wheel's placement, a parameter object rather than six trailing ints on
// every draw. Defaults are the doc's VALUELST fallbacks (1000, 1002, 1006); the
// caller passes the live getvalue() results.
struct WheelGeometry {
    int cx = 320, cy = 240;      // getvalue(1000)
    int rx = 200, ry = 150;      // getvalue(1002)
    int freq_x = 1, freq_y = 1;  // getvalue(1006)
};

class GoldmanScreen {
public:
    GoldmanScreen(const AssetStore& assets, const SequenceSet& seqs, const FontTextures& font)
        : assets_(&assets), seqs_(&seqs), font_(&font) {}

    // (Re)enter: runs the 5 setup rand() draws (doc §3) off the presentation LCG
    // seeded from `seed` — the caller supplies a fresh value each spin and it is
    // NEVER sim::State::rng.
    void enter(std::uint32_t seed, int segment_steps = kWheelSegmentSteps) {
        segment_steps_ = segment_steps > 0 ? segment_steps : kWheelSegmentSteps;
        circle_steps_ = wheel_circle_steps(segment_steps_);
        WheelRng rng{seed};
        wheel_ = spin_setup(rng, circle_steps_);
        done_ = false;
        aborted_ = false;
        ring_anim_ = resolve_sequence(assets_->ring(), "ring");
    }

    // Doc §5's input table. Enter/Space: phase 0 -> phase 1 (start the wind-down),
    // or in phase 2 dismiss. Esc: abort (the caller reads aborted() to forfeit the
    // gold player). The nav blip fires first for ANY real key.
    void on_key(SDL_Keycode key, AudioEngine& audio) {
        audio.play(20);
        if (key == SDLK_ESCAPE) {
            aborted_ = true;
            done_ = true;
            return;
        }
        if (key != SDLK_RETURN && key != SDLK_KP_ENTER && key != SDLK_SPACE) return;
        if (wheel_.phase == WheelPhase::FreeSpin) wheel_.phase = WheelPhase::WindingDown;
        if (wheel_.phase == WheelPhase::Settled) done_ = true;
    }

    // Advances the spin by one frame (doc §3 "Per frame") and plays the matching
    // SFX. Call once per drawn frame while !done().
    void tick(AudioEngine& audio) {
        if (done_ || wheel_.phase == WheelPhase::Settled) return;
        // Ring: +direction, ticks SFX 1300 on every boundary crossing. Wheel:
        // -direction (opposite the ring), no tick sound (doc §3).
        if (step_mover(wheel_.ring, wheel_.phase, wheel_.direction, circle_steps_, segment_steps_))
            audio.play(1300);
        step_mover(wheel_.wheel, wheel_.phase, -wheel_.direction, circle_steps_, segment_steps_);
        if (wheel_.phase != WheelPhase::WindingDown) return;
        if (wheel_.wheel.budget != 0 || wheel_.ring.budget != 0) return;
        wheel_.result = resolve_prize(wheel_, circle_steps_, segment_steps_);
        audio.play(wheel_.result == kClogsPrizeId ? 1320 : 1310);
        wheel_.phase = WheelPhase::Settled;
    }

    // Backdrop + ring pointer + 6 prize icons + (phase 2) result text.
    void draw(SDL_Renderer* ren, const WheelGeometry& geo = {}) const;

    bool done() const { return done_; }
    bool aborted() const { return aborted_; }
    int prize() const { return wheel_.result; }  // -1 while spinning/aborted
    const WheelState& wheel() const { return wheel_; }

private:
    void draw_backdrop(SDL_Renderer* ren) const;
    void draw_prize_icons(SDL_Renderer* ren, const WheelGeometry& geo) const;
    void draw_result_text(SDL_Renderer* ren, const WheelGeometry& geo) const;
    static SDL_FPoint lissajous_xy(int angle, int circle_steps, const WheelGeometry& geo);
    void draw_anim_step(SDL_Renderer* ren, const Anim& a, SDL_FPoint at) const;
    void draw_centered(SDL_Renderer* ren, const std::string& s, float cx, float y) const;

    const AssetStore* assets_ = nullptr;
    const SequenceSet* seqs_ = nullptr;
    const FontTextures* font_ = nullptr;

    Anim ring_anim_;  // resolved in enter() from assets_->ring(); empty draws nothing

    WheelState wheel_;
    int segment_steps_ = kWheelSegmentSteps;
    int circle_steps_ = wheel_circle_steps();
    bool done_ = false;
    bool aborted_ = false;
};

}  // namespace bomber::game
