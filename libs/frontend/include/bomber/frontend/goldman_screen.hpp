#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>

#include "bomber/audio/audio_engine.hpp"
#include "bomber/game_util/anim_pace.hpp"
#include "bomber/game_util/goldman_wheel.hpp"
#include "bomber/render/asset_store.hpp"
#include "bomber/render/sequences.hpp"
#include "bomber/ui/bmscreen.hpp"

// The Goldman Roulette wheel screen — `sub_4034BC` @0x4034BC
// (docs/re/goldman-roulette.md), mirroring OptionsScreen's structure: an
// enter()/on_key()/draw() trio the caller (present_setup's trigger site in
// game_app.cpp) drives once per Play entry when a gold player is pending.
// ROULETTE.PCX + "roulette.plt" backdrop, the "ring" ANI pointer, the 6 prize
// icons (the shared "power <name>" ANI icons via SequenceSet::powerup_anim),
// and the pure spin math (goldman_wheel.hpp) advanced one frame per draw().
namespace bomber::game {

class GoldmanScreen {
public:
    GoldmanScreen(const AssetStore& assets, const SequenceSet& seqs, const FontTextures& font)
        : assets_(&assets), seqs_(&seqs), font_(&font) {}

    // (Re)enter the screen: runs the 5 setup rand() draws (doc §3) off the
    // presentation LCG seeded from `seed` (caller supplies a fresh value each
    // spin, e.g. an advancing counter — never bomber::sim::State::rng).
    // `circle_steps`/`segment_steps` default to the VALUELST 1004 fallback
    // (70) but the caller may pass the live getvalue(1004) value.
    void enter(std::uint32_t seed, int segment_steps = kWheelSegmentSteps) {
        segment_steps_ = segment_steps > 0 ? segment_steps : kWheelSegmentSteps;
        circle_steps_ = wheel_circle_steps(segment_steps_);
        WheelRng rng{seed};
        wheel_ = spin_setup(rng, circle_steps_);
        frame_ = 0;
        done_ = false;
        aborted_ = false;
        ring_anim_ = resolve_sequence(assets_->ring(), "ring");
    }

    // Feed one SDL keycode (doc §5's input table). Enter/Space: phase 0 ->
    // phase 1 (start the wind-down), or in phase 2 dismiss the screen. Esc:
    // abort (caller reads aborted() to forfeit the gold player). Any other
    // real key just blips.
    void on_key(SDL_Keycode key, AudioEngine& audio) {
        switch (key) {
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
            case SDLK_SPACE:
                audio.play(20);  // nav blip fires first for any real key (doc §5)
                if (wheel_.phase == WheelPhase::FreeSpin) {
                    wheel_.phase = WheelPhase::WindingDown;
                } else if (wheel_.phase == WheelPhase::Settled) {
                    done_ = true;
                }
                break;
            case SDLK_ESCAPE:
                audio.play(20);
                aborted_ = true;
                done_ = true;
                break;
            default:
                audio.play(20);
                break;
        }
    }

    // Advances the spin by one frame (doc §3 "Per frame") and plays the
    // matching SFX. Call once per drawn frame while !done().
    void tick(AudioEngine& audio) {
        if (done_ || wheel_.phase == WheelPhase::Settled) return;
        ++frame_;
        // Ring: +direction, ticks SFX 1300 on every boundary crossing.
        bool ring_boundary = step_mover(wheel_.ring, wheel_.phase, wheel_.direction, circle_steps_,
                                        segment_steps_);
        if (ring_boundary) audio.play(1300);
        // Wheel: -direction (opposite the ring), no tick sound (doc §3).
        step_mover(wheel_.wheel, wheel_.phase, -wheel_.direction, circle_steps_, segment_steps_);

        if (wheel_.phase == WheelPhase::WindingDown && wheel_.wheel.budget == 0 &&
            wheel_.ring.budget == 0) {
            wheel_.result = resolve_prize(wheel_, circle_steps_, segment_steps_);
            audio.play(wheel_.result == kClogsPrizeId ? 1320 : 1310);
            wheel_.phase = WheelPhase::Settled;
        }
    }

    // Backdrop + ring pointer + 6 prize icons + (phase 2) result text. Centre/
    // radii/Lissajous frequency default to the doc's VALUELST fallbacks
    // (1000,320,240 / 1002,200,150 / 1006,1,1); pass live getvalue() results
    // when available.
    void draw(SDL_Renderer* ren, int cx = 320, int cy = 240, int rx = 200, int ry = 150,
             int freq_x = 1, int freq_y = 1) const {
        if (!ren) return;
        const Sprite& bg = assets_->frontend_pcx("ROULETTE");
        if (bg.tex) {
            SDL_FRect d{0, 0, static_cast<float>(bg.w), static_cast<float>(bg.h)};
            SDL_RenderTexture(ren, bg.tex, nullptr, &d);
        } else {
            SDL_SetRenderDrawColor(ren, 10, 10, 20, 255);
            SDL_RenderClear(ren);
        }

        // The 6 prize icons at angles wheel + k*segment_steps (doc §3), drawn
        // with the shared floor-powerup ANI (SequenceSet::powerup_anim) — the
        // wheel reuses the normal in-game icons, no roulette-specific art.
        // Clogs (13) draws its OWN icon (POWERS.ANI "power clog",
        // SequenceSet::clogs_anim) — CONFIRMED the original draws all 6 slots
        // uniformly via sub_425C7F(x,y,kind), no special-case skip for slot 13
        // (docs/re/goldman-roulette.md §9.4); it is simply not a
        // sim::PowerupType (never a sim inventory kind, §8).
        for (int k = 0; k < kWheelSegments; ++k) {
            int angle = wheel_.wheel.pos + k * segment_steps_;
            float x = 0, y = 0;
            lissajous_xy(angle, circle_steps_, cx, cy, rx, ry, freq_x, freq_y, x, y);
            int prize_id = kWheelPrizeIds[static_cast<std::size_t>(k)];
            if (prize_id == kClogsPrizeId) {
                draw_anim_step(ren, seqs_->clogs_anim, x, y);
                continue;
            }
            sim::PowerupType pt = wheel_prize_to_powerup(prize_id);
            if (pt == sim::PowerupType::None) continue;
            const Anim& a = seqs_->powerup_anim[static_cast<int>(pt)];
            draw_anim_step(ren, a, x, y);
        }

        // Pointer: ANI sequence "ring" at the ring position.
        {
            float x = 0, y = 0;
            lissajous_xy(wheel_.ring.pos, circle_steps_, cx, cy, rx, ry, freq_x, freq_y, x, y);
            draw_anim_step(ren, ring_anim_, x, y);
        }

        if (wheel_.phase == WheelPhase::Settled && font_ && font_->loaded()) {
            std::string header = assets_->getstring(790, "YOU HAVE WON");
            std::string prize = assets_->getstring(800 + wheel_.result, "A PRIZE");
            std::string footer = assets_->getstring(791, "");
            float ty = static_cast<float>(cy) - 30.0f;
            draw_centered(ren, header, static_cast<float>(cx), ty);
            draw_centered(ren, prize, static_cast<float>(cx), ty + 20.0f);
            if (!footer.empty()) draw_centered(ren, footer, static_cast<float>(cx), ty + 40.0f);
        }
    }

    bool done() const { return done_; }
    bool aborted() const { return aborted_; }
    int prize() const { return wheel_.result; }  // -1 while spinning/aborted
    const WheelState& wheel() const { return wheel_; }

private:
    static void lissajous_xy(int a, int circle_steps, int cx, int cy, int rx, int ry, int freq_x,
                             int freq_y, float& out_x, float& out_y);
    void draw_anim_step(SDL_Renderer* ren, const Anim& a, float x, float y) const;
    void draw_centered(SDL_Renderer* ren, const std::string& s, float cx, float y) const;

    const AssetStore* assets_ = nullptr;
    const SequenceSet* seqs_ = nullptr;
    const FontTextures* font_ = nullptr;

    Anim ring_anim_;  // resolved in enter() from assets_->ring(); empty draws nothing

    WheelState wheel_;
    int segment_steps_ = kWheelSegmentSteps;
    int circle_steps_ = wheel_circle_steps();
    std::uint64_t frame_ = 0;
    bool done_ = false;
    bool aborted_ = false;
};

}  // namespace bomber::game
