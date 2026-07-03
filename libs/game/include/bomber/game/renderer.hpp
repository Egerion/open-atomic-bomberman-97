#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <vector>

#include "bomber/game/asset_store.hpp"
#include "bomber/game/sequences.hpp"
#include "bomber/sim/simulation.hpp"

// Draws one frame of the match from the sim state. Owns only cosmetic,
// render-side memory (walk phases, death effects, the HURRY banner timer) —
// it reads the sim state and NEVER mutates it.

namespace bomber::game {

// Field placement verified against FIELD0.PCX and the original HUD.
inline constexpr int kFieldOriginX = 20;
inline constexpr int kFieldOriginY = 64;
inline constexpr int kScreenW = 640;
inline constexpr int kScreenH = 480;

class Renderer {
public:
    Renderer(SDL_Renderer* ren, const AssetStore& assets, const SequenceSet& seqs)
        : ren_(ren), assets_(&assets), seqs_(&seqs) {}

    // Clears, then draws background, powerups, world, and HUD.
    void draw_frame(const sim::State& s);

    // Consumes this tick's events for the visual effects: death animations
    // and the HURRY! banner.
    void on_events(const sim::State& s);

    // Forgets per-match cosmetic state (call when a new match starts).
    void reset_match();

    static float tile_screen_x(int tx) {
        return static_cast<float>(kFieldOriginX + tx * sim::kTileW);
    }
    static float tile_screen_y(int ty) {
        return static_cast<float>(kFieldOriginY + ty * sim::kTileH);
    }

private:
    struct DeathFx {
        std::size_t anim = 0;
        int player = 0;
        float x = 0, y = 0;
        std::uint64_t start = 0;
    };

    void draw_powerups(const sim::State& s);
    void draw_world(const sim::State& s);
    void draw_hud(const sim::State& s);
    void sample_movement(const sim::State& s);

    void draw_sprite(const Sprite& sp, float x, float y, Uint8 r = 255, Uint8 g = 255,
                     Uint8 b = 255);
    void draw_anim(const Anim& a, std::size_t step, float x, float y, Uint8 r = 255,
                   Uint8 g = 255, Uint8 b = 255);

    // Maps a countdown timer onto a play-once sequence.
    static std::size_t timed_step(const Anim& a, int remaining, int total);

    // Cosmetic render-side RNG for the disease colour strobe (never the sim's).
    Uint8 disease_flash_channel();

    SDL_Renderer* ren_ = nullptr;
    const AssetStore* assets_ = nullptr;
    const SequenceSet* seqs_ = nullptr;

    // Walk animation memory: movement is sampled once per sim tick — the
    // render loop runs faster than the 20 Hz sim, so a per-frame position
    // diff would read "idle".
    std::uint64_t last_tick_ = ~0ull;
    std::array<sim::Fixed, sim::kMaxPlayers> last_x_{}, last_y_{};
    std::array<bool, sim::kMaxPlayers> moving_{};
    std::array<std::uint32_t, sim::kMaxPlayers> walk_phase_{};

    std::vector<DeathFx> deaths_;
    std::uint64_t hurry_until_ = 0;  // HURRY! banner flashes until this tick
    std::uint32_t flash_lcg_ = 0x2545F491u;
};

}  // namespace bomber::game
