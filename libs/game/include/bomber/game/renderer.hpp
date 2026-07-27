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
    //
    // `alpha` is the inter-tick interpolation fraction (ADR-0003's "rendering
    // interpolates"): the ORIGINAL runs its whole gameplay driver once per
    // DISPLAYED frame with an ms frame delta (sub_42A191; movement budget =
    // baseSpeed × frameDelta / 50, docs/re/facts.md "Speed = a spent budget"),
    // so at 60-70 fps positions advance a few px every ~16 ms. Our sim is a
    // fixed 20 Hz step (determinism contract), so to reproduce that fluidity
    // the renderer blends the moving entities (players, bombs, rovers)
    // between the previous and current tick by `alpha` = the match loop's
    // accumulator fraction in [0,1). Purely cosmetic — the sim state is never
    // touched. The default 1.0 draws raw current-tick positions (demo
    // screenshots, frozen end-of-round frames, visual goldens).
    void draw_frame(const sim::State& s, float alpha = 1.0f);

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

    // F9 native-cadence live path (game_app.cpp): when set, sample_movement
    // advances the walk/carry/fidget phases every DISPLAYED frame instead of
    // once per 20 Hz sim tick, so the walk cycle animates at the real frame rate
    // (the sim's PlayerWalking events now arrive per frame). Matches the
    // original's per-frame sub_41F29B animation clock. No effect on the
    // deterministic path (default false → the once-per-tick gate stays).
    void set_native_cadence(bool v) { native_cadence_ = v; }

    // F9 only: interpolation fraction [0,1) for 50 ms-stepped entities (flying/
    // sliding bombs, rovers) = the sim's systems accumulator / 50 ms. Players
    // are drawn direct (they moved this frame); this lets the slower-cadence
    // entities glide between their 20 Hz steps instead of stuttering.
    void set_entity_interp(float a) { entity_alpha_ = a; }

    // Consumes this tick's events for the visual effects: death animations
    // and the HURRY! banner. `tick_advanced` (default true = the deterministic
    // once-per-tick call) gates the per-tick pose countdowns; the F9 per-frame
    // path passes false on frames that didn't cross a sim tick.
    void on_events(const sim::State& s, bool tick_advanced = true);

    // Rolls the per-tick render snapshots forward for ONE freshly-simulated
    // tick — the inter-tick interpolation baseline (capture_interp) and the
    // walk/fidget/carry/gold-sparkle bookkeeping (sample_movement). The match
    // loop MUST call this once per sim_.tick() inside its catch-up loop, so
    // that when a slow displayed frame advances the sim two ticks at once the
    // interpolation `prev` is the PENULTIMATE tick (a 1-tick lerp span), not
    // two ticks back (a 2-tick span that snaps/double-speeds for ~3 frames —
    // the "jump/hitch" jitter). Both callees are tick-keyed no-ops on repeat,
    // so draw_frame's own calls below stay correct for the demo/screenshot
    // path that ticks-then-draws without this loop. See draw_frame's doc.
    void advance_tick(const sim::State& s) {
        capture_interp(s);
        sample_movement(s);
    }

    // Forgets per-match cosmetic state (call when a new match starts). `untimed`
    // is the PRESENTATION-side "no time limit" flag (options_.playtime_seconds
    // == 1001, docs/re/in-match-shell.md §3's dword_4601A8 == 1001) — the sim's
    // ticks_left carries no such sentinel (a very long but finite countdown
    // stands in for it there, game_app.cpp's start_match comment), so the HUD
    // needs this told to it directly rather than inferring it from ticks_left.
    void reset_match(bool untimed = false);

    // True while at least one death animation still has frames left to play at
    // tick `s.tick`. The ROUND-OVER gate depends on this, so it is a query and
    // not just cosmetic bookkeeping: the original's round loop keeps running
    // while a corpse is mid-animation (its per-player pass sub_41F29B still
    // counts a dying slot, and only clears the slot's in-play flag when the
    // death sequence's own step count is reached, sub_41DA5C), so the round is
    // over exactly when the LAST death animation finishes — see MatchRunner's
    // advance_round_end. Computed from the same start tick + sequence length
    // draw_world retires each effect on, rather than reading deaths_.empty(),
    // so the answer is exact at any tick even on a frame that has not drawn yet.
    bool death_fx_active(const sim::State& s) const {
        for (const DeathFx& fx : deaths_) {
            const std::vector<Anim>& pool = assets_->deaths_for(fx.player % kLocalPlayers);
            if (pool.empty()) continue;
            if (s.tick - fx.start < pool[fx.anim % pool.size()].steps.size()) return true;
        }
        return false;
    }

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

    void draw_cells(const sim::State& s);   // static solid/brick tiles (background layer)
    void draw_actors(const sim::State& s);  // conveyor/trampoline floor tiles
    void draw_bombs(const sim::State& s);
    void draw_powerups(const sim::State& s);
    void draw_world(const sim::State& s);
    void draw_hud(const sim::State& s);
    void sample_movement(const sim::State& s);

    // Rolls the inter-tick snapshots forward when a new sim tick is observed
    // (previous ← last seen, last seen ← current). Keyed on s.tick like
    // sample_movement, so extra draw calls within one tick are no-ops.
    void capture_interp(const sim::State& s);

    // On-screen FIELD-pixel position for a moving entity: lerps prev→curr by
    // interp_alpha_ unless the snapshot is unusable (`prev_ok` false) or the
    // entity jumped further than any legit per-tick travel (warp/trampoline/
    // flight wrap) — then it snaps to the current position on BOTH axes.
    struct Posf {
        float x = 0, y = 0;
    };
    Posf interp_pos(sim::Fixed prev_x, sim::Fixed prev_y, sim::Fixed x, sim::Fixed y,
                    bool prev_ok) const;

    // On-screen FIELD-pixel position + facing for PLAYER i: plays back the
    // sim's per-sub-frame trace (State::sub_trace) across the tick interval
    // instead of lerping the two 20 Hz endpoints. The original draws every
    // displayed frame at the player's live per-frame position, so its
    // ~180 fps micro-zigzag (AI direction flips up to kSubFrames× per tick)
    // is visible; an endpoint lerp filters all of it out. `out_dir` receives
    // the active segment's facing so the sprite flips mid-tick too. Falls
    // back to the endpoint (and p.facing) when no previous tick exists or
    // alpha >= 1; each SEGMENT applies the same snap threshold interp_pos
    // uses, so warps/teleports still snap instead of smearing.
    Posf player_interp(const sim::State& s, int i, int& out_dir) const;

    void draw_sprite(const Sprite& sp, float x, float y, Uint8 r = 255, Uint8 g = 255,
                     Uint8 b = 255);
    void draw_anim(const Anim& a, std::size_t step, float x, float y, Uint8 r = 255, Uint8 g = 255,
                   Uint8 b = 255);

    // Cosmetic render-side RNG for the disease colour strobe (never the sim's).
    // Returns 0..kLocalPlayers-1 — the original's `rand() % 10` frame pick
    // (sub_41F29B ~23252), i.e. which of the ten real player-colour sprite
    // sets to redraw the body in this flash tick.
    int disease_flash_colour();

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

    // The art piece for one flame cell's FlameKind (docs/re/facts.md "Flame
    // arm-shape selection") — a plain 1:1 table matching the original's
    // off_45BEA0 name order, not a live neighbour scan.
    static const Anim& flame_piece(const FlameSet& fset, sim::FlameKind kind);

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
    // Action-pose countdowns (ticks): a recent kick/punch shows KICK.ANI/
    // PUNBOMB*.ANI instead of walk/stand. Driven by the (unhashed)
    // BombKicked/BombPunched events, so this is purely cosmetic and never
    // touches the sim.
    std::array<int, sim::kMaxPlayers> kick_pose_{}, punch_pose_{};
    // "Picking up a bomb" transitional pose (PUP*.ANI "pickup <dir>",
    // sub_41F29B action-state 4): counts down from the sequence length after
    // a BombGrabbed event, overriding the carry pose while it runs. Cosmetic,
    // event-driven like kick/punch above.
    std::array<int, sim::kMaxPlayers> pickup_pose_{};
    // Idle "cornerhead" fidget: while a player is boxed in and standing still it
    // cycles random fidgets (sub_41F29B). `panic_active_` marks the fidget as
    // running; `panic_elapsed_` is the elapsed-frame counter (the ANI phase),
    // which re-rolls a fresh variant once it reaches the current variant's own
    // frame count — the original re-rolls on ANI-cycle completion, NOT on a
    // fixed tick spread (see sample_movement). Purely cosmetic — reads the sim
    // state, never mutates it, and rolls off the panic LCG below (never
    // State::rng).
    std::array<bool, sim::kMaxPlayers> panic_active_{};
    std::array<int, sim::kMaxPlayers> panic_elapsed_{};
    std::array<int, sim::kMaxPlayers> panic_variant_{};
    // Bomb-pickup carry arc (docs/re/id-audit.md item 4, VALUELST 500/502/
    // 504/506): ticks elapsed since this player started carrying a bomb,
    // clamped 0..3 (see update_carry_arc / the carried-bomb draw in
    // draw_world). Cosmetic-only — never touches the sim.
    std::array<int, sim::kMaxPlayers> carry_ticks_{};
    std::array<bool, sim::kMaxPlayers> carrying_prev_{};

    // Inter-tick interpolation snapshots (see draw_frame's doc comment).
    // `seen_*` mirrors the entity positions of the latest tick drawn;
    // `prev_*` the tick before it. Bombs/rovers are matched by slot index —
    // a slot reused by a NEW entity between two ticks lerps from stale data
    // at worst one frame, and only if the jump is under the snap threshold.
    struct EntSnap {
        bool active = false;
        sim::Fixed x = 0, y = 0;
    };
    std::uint64_t interp_tick_ = ~0ull;  // tick the seen_* arrays hold
    bool interp_valid_ = false;          // prev_* holds a real earlier tick
    float interp_alpha_ = 1.0f;          // this frame's fraction (set by draw_frame)
    std::array<sim::Fixed, sim::kMaxPlayers> prev_px_{}, prev_py_{}, seen_px_{}, seen_py_{};
    // The latest tick's per-sub-frame player trace (State::sub_trace copy,
    // rolled by capture_interp): describes the motion prev_p*_ -> seen_p*_
    // that player_interp plays back.
    std::array<std::array<sim::State::SubSample, sim::kSubFrames>, sim::kMaxPlayers> trace_{};
    std::vector<EntSnap> prev_bombs_, seen_bombs_;
    std::vector<EntSnap> prev_rovers_, seen_rovers_;

    std::vector<DeathFx> deaths_;
    std::uint64_t hurry_until_ = 0;  // HURRY! banner flashes until this tick
    bool untimed_ = false;           // draw the KFONT 'infinity' glyph instead of MM:SS
    std::uint32_t flash_lcg_ = 0x2545F491u;
    std::uint32_t panic_lcg_ = 0x9E3779B9u;

    // Gold Bomberman "twinkle" (docs/re/goldman-roulette.md §6): a fixed
    // 100-slot particle pool, matching the original's `dword_4621CC` array.
    // Each entry is an independent floating spark, screen-position fixed at
    // spawn time (NOT re-anchored to the player every frame), aged once per
    // RENDER frame (in draw_world, matching sub_420E39's per-engine-frame
    // cadence) and retired after `goldman_anim_`'s own frame count.
    struct GoldSparkle {
        bool active = false;
        float x = 0, y = 0;
        int age = 0;
    };
    static constexpr int kGoldSparkleSlots = 100;
    std::array<GoldSparkle, kGoldSparkleSlots> gold_sparkles_{};
    Anim goldman_anim_;     // MISC.ANI "goldman" sequence
    int gold_player_ = -1;  // -1 = no pending gold player (see set_gold_player)
    bool gold_team_mode_ = false;
    bool native_cadence_ = false;  // F9 live path — see set_native_cadence
    float entity_alpha_ = 1.0f;    // F9 bomb/rover interp fraction — set_entity_interp
    std::uint32_t gold_lcg_ = 0xB16B00B5u;
};

}  // namespace bomber::game
