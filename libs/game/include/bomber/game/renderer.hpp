#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <vector>

#include "bomber/assets/reslist.hpp"
#include "bomber/game/asset_store.hpp"
#include "bomber/game/sequences.hpp"
#include "bomber/sim/simulation.hpp"

// Draws one frame of the match from the sim state. Owns only cosmetic,
// render-side memory (walk phases, death effects, the HURRY banner timer) —
// it reads the sim state and NEVER mutates it.

namespace bomber::game {

// Field placement — CONFIRMED against the binary (sub_42647A): the play grid
// origin is ((screenW-600)/2, screenH-412) = (20, 68) for 640x480, tiles 40x36,
// grid 15x11. Sprites anchor at (tile-centre-x, tile-bottom-y), which the draw
// code reproduces by adding kTileH/2 to the stored tile-centre position.
inline constexpr int kFieldOriginX = 20;
inline constexpr int kFieldOriginY = 68;  // was 64 (4 px too high) — see sub_42647A
inline constexpr int kScreenW = 640;
inline constexpr int kScreenH = 480;

class Renderer {
public:
    Renderer(SDL_Renderer* ren, const AssetStore& assets, const SequenceSet& seqs,
             const assets::res::ValueList& values)
        : ren_(ren), assets_(&assets), seqs_(&seqs), values_(&values) {}

    // Clears, then draws background, powerups, world, and HUD.
    void draw_frame(const sim::State& s);

    // Consumes this tick's events for the visual effects: death animations
    // and the HURRY! banner.
    void on_events(const sim::State& s);

    // Forgets per-match cosmetic state (call when a new match starts). `untimed`
    // is the PRESENTATION-side "no time limit" flag (options_.playtime_seconds
    // == 1001, docs/re/in-match-shell.md §3's dword_4601A8 == 1001) — the sim's
    // ticks_left carries no such sentinel (a very long but finite countdown
    // stands in for it there, game_app.cpp's start_match comment), so the HUD
    // needs this told to it directly rather than inferring it from ticks_left.
    void reset_match(bool untimed = false);

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

    void draw_actors(const sim::State& s);  // conveyor/trampoline floor tiles
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

    // True when all four orthogonal neighbours of (tx,ty) are impassable
    // (wall/brick/burning brick or a resting bomb); out-of-grid counts blocked.
    static bool boxed_in(const sim::State& s, int tx, int ty);

    // Cosmetic render-side LCG for the idle-fidget rolls (never the sim's).
    std::uint32_t panic_roll();

    SDL_Renderer* ren_ = nullptr;
    const AssetStore* assets_ = nullptr;
    const SequenceSet* seqs_ = nullptr;
    const assets::res::ValueList* values_ = nullptr;

    // Walk animation memory: movement is sampled once per sim tick — the
    // render loop runs faster than the 20 Hz sim, so a per-frame position
    // diff would read "idle".
    std::uint64_t last_tick_ = ~0ull;
    std::array<sim::Fixed, sim::kMaxPlayers> last_x_{}, last_y_{};
    std::array<bool, sim::kMaxPlayers> moving_{};
    std::array<std::uint32_t, sim::kMaxPlayers> walk_phase_{};
    // Action-pose countdowns (ticks): a recent kick/punch shows KICK/PUNCH.ANI
    // instead of walk/stand. Driven by the (unhashed) BombKicked/BombPunched
    // events, so this is purely cosmetic and never touches the sim.
    std::array<int, sim::kMaxPlayers> kick_pose_{}, punch_pose_{};
    // Idle "cornerhead" fidget: while a player is boxed in and standing still it
    // cycles random fidgets (sub_41F29B). Purely cosmetic — reads the sim state,
    // never mutates it, and rolls off the panic LCG below (never State::rng).
    std::array<int, sim::kMaxPlayers> panic_ticks_{};
    std::array<int, sim::kMaxPlayers> panic_variant_{};

    std::vector<DeathFx> deaths_;
    std::uint64_t hurry_until_ = 0;  // HURRY! banner flashes until this tick
    bool untimed_ = false;  // draw the KFONT 'infinity' glyph instead of MM:SS
    std::uint32_t flash_lcg_ = 0x2545F491u;
    std::uint32_t panic_lcg_ = 0x9E3779B9u;
};

}  // namespace bomber::game
