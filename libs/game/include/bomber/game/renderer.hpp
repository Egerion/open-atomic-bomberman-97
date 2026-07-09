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
        : ren_(ren), assets_(&assets), seqs_(&seqs), values_(&values) {
        // Gold Bomberman "twinkle" sparkle sprite (docs/re/goldman-roulette.md
        // §6): MISC.ANI's "goldman" sequence (pseudo.c aGoldman_0 = "goldman",
        // sub_420E39). Resolved once; an empty Anim (missing MISC.ANI/sequence)
        // just draws nothing — see draw_anim's own empty-steps guard.
        goldman_anim_ = resolve_sequence(assets.misc(), "goldman");
    }

    // Clears, then draws background, powerups, world, and HUD.
    void draw_frame(const sim::State& s);

    // Tells the renderer which player is the pending Goldman-wheel winner,
    // for the twinkle overlay (docs/re/goldman-roulette.md §6). `who` is -1
    // for none; otherwise a player SLOT index in solo play or a TEAM id in
    // team play (mirrors dword_46492C's dual encoding — see
    // bomber::game::assign_gold_player's doc comment). Call every frame from
    // the live match loop; cheap, and gold_player_ changes only between
    // rounds.
    void set_gold_player(int who, bool team_mode) {
        gold_player_ = who;
        gold_team_mode_ = team_mode;
    }

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

    // The colour-set index (0-9) to draw player `slot` with — i.e. which of
    // AssetStore's per-slot recoloured sprite sets (built 1:1 off the ten
    // .RMP files) to index into. Bounds-checks `slot` then delegates to the
    // SDL-free `bomber::match::team_render_colour` (libs/match/include/bomber/
    // match/team_colour.hpp — see its doc comment for the full sub_4214BC RE
    // citation and doctest coverage) for the actual team-vs-slot-index rule.
    // docs/re/player-colour.md "Team Play colour override".
    static int render_colour(const sim::State& s, int slot);

    // True when all four orthogonal neighbours of (tx,ty) are impassable
    // (wall/brick/burning brick or a resting bomb); out-of-grid counts blocked.
    static bool boxed_in(const sim::State& s, int tx, int ty);

    // Cosmetic render-side LCG for the idle-fidget rolls (never the sim's).
    std::uint32_t panic_roll();

    // Cosmetic render-side LCG for the gold-twinkle sparkle rolls (never the
    // sim's). See update_gold_sparkles.
    std::uint32_t gold_roll();
    // Spawns/ages the gold-player twinkle particle pool (docs/re/
    // goldman-roulette.md §6, sub_420D4E/sub_420E39/sub_420F07). Called once
    // per sim tick from sample_movement.
    void update_gold_sparkles(const sim::State& s);

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
    // Bomb-pickup carry arc (docs/re/id-audit.md item 4, VALUELST 500/502/
    // 504/506): ticks elapsed since this player started carrying a bomb,
    // clamped 0..3 (see update_carry_arc / the carried-bomb draw in
    // draw_world). Cosmetic-only — never touches the sim.
    std::array<int, sim::kMaxPlayers> carry_ticks_{};
    std::array<bool, sim::kMaxPlayers> carrying_prev_{};

    std::vector<DeathFx> deaths_;
    std::uint64_t hurry_until_ = 0;  // HURRY! banner flashes until this tick
    bool untimed_ = false;  // draw the KFONT 'infinity' glyph instead of MM:SS
    std::uint32_t flash_lcg_ = 0x2545F491u;
    std::uint32_t panic_lcg_ = 0x9E3779B9u;

    // Gold Bomberman "twinkle" (docs/re/goldman-roulette.md §6): a fixed
    // 100-slot particle pool, matching the original's `dword_4621CC` array.
    // Each entry is an independent floating spark, screen-position fixed at
    // spawn time (NOT re-anchored to the player every frame), aged once per
    // sim tick and retired after `goldman_anim_`'s own frame count.
    struct GoldSparkle {
        bool active = false;
        float x = 0, y = 0;
        int age = 0;
    };
    static constexpr int kGoldSparkleSlots = 100;
    std::array<GoldSparkle, kGoldSparkleSlots> gold_sparkles_{};
    Anim goldman_anim_;            // MISC.ANI "goldman" sequence
    int gold_player_ = -1;         // -1 = no pending gold player (see set_gold_player)
    bool gold_team_mode_ = false;
    std::uint32_t gold_lcg_ = 0xB16B00B5u;
};

}  // namespace bomber::game
