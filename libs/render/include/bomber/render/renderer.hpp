#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <vector>

#include "bomber/assets/reslist.hpp"
#include "bomber/game_util/carry_pose.hpp"  // PoseFlags (PoseView below)
#include "bomber/render/asset_store.hpp"
#include "bomber/render/sequences.hpp"
#include "bomber/sim/simulation.hpp"

// Draws one frame of the match from the sim state. Owns only cosmetic,
// render-side memory (walk phases, death effects, the HURRY banner timer) — it
// reads the sim state and NEVER mutates it. The long-form rationale is
// docs/render-notes.md, cited below by section.

namespace bomber::game {

// Field placement — CONFIRMED against the binary (sub_42647A): the play grid
// origin is ((screenW-600)/2, screenH-412) = (20, 68) for 640x480, tiles 40x36,
// grid 15x11. Sprites anchor at (tile-centre-x, tile-bottom-y), which the draw
// code reproduces by adding kTileH/2 to the stored tile-centre position.
inline constexpr int kFieldOriginX = 20;
inline constexpr int kFieldOriginY = 68;  // was 64 (4 px too high) — see sub_42647A
inline constexpr int kScreenW = 640;
inline constexpr int kScreenH = 480;

struct FieldCell {
    int x = 0, y = 0;
};

// The traversal every grid pass uses: row-major, y ascending then x ascending.
// The ORDER is load-bearing and its violation would be silent — overlapping
// sprites composite in visit order, so tests/visual pins the traversal as surely
// as it pins the pixels. Stating it once, here, is the point of the iterator.
class FieldCells {
public:
    class Iterator {
    public:
        explicit constexpr Iterator(int i) : i_(i) {}
        constexpr FieldCell operator*() const {
            return {i_ % sim::kGridWidth, i_ / sim::kGridWidth};
        }
        constexpr Iterator& operator++() {
            ++i_;
            return *this;
        }
        constexpr bool operator!=(const Iterator& o) const { return i_ != o.i_; }

    private:
        int i_;
    };
    static constexpr Iterator begin() { return Iterator{0}; }
    static constexpr Iterator end() { return Iterator{sim::kGridWidth * sim::kGridHeight}; }
};
inline constexpr FieldCells field_cells{};

// SIZE EXCEPTION (coding-standards §3, "class length 200 lines"): this body is
// 282 lines, of which 98 are comment and ~163 code. The code half is under the
// target; the miss is on the raw span, and closing it would mean deleting
// sub_XXXX citations §10 says to keep. The structural fix, if it is ever worth
// it, is to lift the per-player animation bookkeeping (walk/fidget/carry/pose
// timers, sample_movement, on_events) into its own class — that half is
// cohesive and would halve this one. Deliberately NOT attempted in a pass whose
// only regression pin is tests/visual.
class Renderer {
public:
    Renderer(SDL_Renderer* ren, const AssetStore& assets, const SequenceSet& seqs,
             const assets::res::ValueList& values)
        : ren_(ren), assets_(&assets), seqs_(&seqs), values_(&values) {
        // MISC.ANI "goldman" — the twinkle spark (sub_420E39). Resolved once; an
        // empty Anim (missing file) just draws nothing.
        goldman_anim_ = resolve_sequence(assets.misc(), "goldman");
    }

    // `alpha` is the inter-tick interpolation fraction; 1.0 draws raw
    // current-tick positions (demo screenshots, frozen end-of-round frames, the
    // visual goldens). docs/render-notes.md §1.
    void draw_frame(const sim::State& s, float alpha = 1.0f);

    // The pending Goldman-wheel winner. `who` is -1 for none, otherwise a player
    // SLOT in solo play or a RAW 0/1 team id in team play — dword_46492C's dual
    // encoding, kept raw here; gold_twinkle_matches (results.hpp) aligns it with
    // the +1-shifted Player::team at the seeding site.
    void set_gold_player(int who, bool team_mode) {
        gold_player_ = who;
        gold_team_mode_ = team_mode;
    }

    // F9 native-cadence live path: advance the walk/carry/fidget phases every
    // DISPLAYED frame instead of once per 20 Hz tick. docs/render-notes.md §2.
    void set_native_cadence(bool v) { native_cadence_ = v; }

    // F9 only: interpolation fraction [0,1) for the 50 ms-stepped entities
    // (flying/sliding bombs, rovers). docs/render-notes.md §2.
    void set_entity_interp(float a) { entity_alpha_ = a; }

    // Consumes this tick's events for the visual effects. `tick_advanced` gates
    // the per-tick pose countdowns; the F9 path passes false on frames that did
    // not cross a sim tick.
    void on_events(const sim::State& s, bool tick_advanced = true);

    // Rolls the render snapshots forward for ONE freshly-simulated tick. The
    // match loop MUST call this once per sim_.tick() INSIDE its catch-up loop:
    // a slow frame that advances two ticks at once would otherwise interpolate
    // across a 2-tick span and hitch for ~3 frames. Both callees are tick-keyed
    // no-ops on repeat, so draw_frame's own calls stay correct for the demo
    // path that ticks-then-draws without such a loop.
    void advance_tick(const sim::State& s) {
        capture_interp(s);
        sample_movement(s);
    }

    // Forgets per-match cosmetic state. `untimed` is the PRESENTATION-side "no
    // time limit" flag (dword_4601A8 == 1001): the sim's ticks_left carries no
    // such sentinel, so the HUD must be told.
    void reset_match(bool untimed = false);

    // The ROUND-OVER gate depends on this, so it is a query and not just
    // cosmetic bookkeeping: the original's round ends exactly when the LAST
    // death sequence reaches its own step count (sub_41F29B/sub_41DA5C) — see
    // MatchRunner::advance_round_end. Computed from the start tick rather than
    // deaths_.empty(), so it is exact on a frame that has not drawn yet.
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
    struct Posf {
        float x = 0, y = 0;
    };
    // The SDL colour-mod on a blit. Only the HUD clock's <=30 s warning uses
    // anything but kNoTint. (No default member initialisers: a nested class's
    // NSDMIs are not usable in the enclosing class's own declarations, and the
    // draw helpers below default this argument.)
    struct Tint {
        Uint8 r, g, b;
    };
    static constexpr Tint kNoTint{255, 255, 255};
    // One bomb/rover slot as of a captured tick.
    struct EntSnap {
        bool active = false;
        sim::Fixed x = 0, y = 0;
    };
    // A bomb/rover's previous-tick position, plus whether that snapshot is
    // usable at all — a slot that was not an active entity last tick draws
    // unsmoothed.
    struct PrevPos {
        sim::Fixed x = 0, y = 0;
        bool ok = false;
    };
    static PrevPos prev_entity(const std::vector<EntSnap>& prev, std::size_t index) {
        if (index >= prev.size() || !prev[index].active) return {};
        return {prev[index].x, prev[index].y, true};
    }
    // Everything the pose pick reads about ONE player on ONE frame — a
    // parameter object rather than eight positional arguments (§3).
    struct PoseView {
        PoseFlags flags;
        int slot = 0;
        int dir = 0;
        int colour = 0;
        std::size_t walk_phase = 0;
        std::size_t body_phase = 0;
    };
    struct PosedAnim {
        const Anim* anim = nullptr;
        std::size_t step = 0;
    };
    // The +78 action-state word. It is ONE word, so entering a state ends
    // whichever was running (sub_41EC84 22617-22624) — set_action_pose
    // reproduces that clobber over our three independent countdowns.
    enum class ActionPose : std::uint8_t { Kick, Punch, Pickup };

    // Per-pass draw stages, in sub_42A191's own call order (§4 of the notes).
    void draw_cells(const sim::State& s);
    void draw_actors(const sim::State& s);
    void draw_bombs(const sim::State& s);
    void draw_powerups(const sim::State& s);
    void draw_world(const sim::State& s);
    void draw_hud(const sim::State& s);

    // One cell / one entity of the pass above it.
    void draw_cell_tile(const sim::State& s, FieldCell c);
    void draw_actor_cell(const sim::State& s, FieldCell c);
    void draw_powerup_cell(const sim::State& s, FieldCell c);
    void draw_burning_cell(const sim::State& s, FieldCell c);
    void draw_flame_cell(const sim::State& s, FieldCell c);
    void draw_bomb(const sim::State& s, std::size_t index);
    const Anim& bomb_anim(const sim::Bomb& b, int colour) const;
    void draw_rover(const sim::State& s, std::size_t index);
    void draw_player(const sim::State& s, int slot);
    void draw_carried_bomb(const sim::State& s, const PoseView& v, Posf at);
    void draw_burning_bricks(const sim::State& s);
    void draw_flames(const sim::State& s);
    void draw_players(const sim::State& s);
    void draw_rovers(const sim::State& s);
    void draw_death_fx(const sim::State& s);
    void draw_gold_sparkles();
    void draw_hurry_banner(const sim::State& s);
    void draw_clock(const sim::State& s);

    // The bottom-centre anchor every tile-aligned sprite is drawn at: tile
    // centre x, tile bottom y (docs/re/facts.md "Screen geometry").
    static Posf tile_anchor(FieldCell c) {
        return {tile_screen_x(c.x) + sim::kTileW / 2.0f, tile_screen_y(c.y) + sim::kTileH - 1.0f};
    }

    // How far into its hop the trampoline at `c` is, driven by the hashed
    // Player::bounce of whoever is centred on it — the mat rests until (and only
    // while) it is actually bounced.
    std::size_t tramp_frame(const sim::State& s, FieldCell c) const;

    // The pose family and frame for one player, running the original's
    // name-build fallback over pose_attempt. docs/render-notes.md §3.
    PosedAnim pick_pose(const sim::State& s, PoseView v) const;
    PosedAnim pose_attempt(const sim::State& s, PoseView& v, bool*& drop) const;

    // The pose INPUTS for one player this frame — flags, body colour and the
    // two animation phases — split out so draw_player stays a sequence of blits.
    PoseView pose_view(const sim::State& s, int slot, int dir);

    void sample_movement(const sim::State& s);
    void update_fidget(const sim::State& s, int slot, bool displaced);
    void update_carry(const sim::Player& p, int slot, std::int8_t walk_px);

    void start_action_pose(const sim::State& s, const sim::Event& ev, ActionPose which);
    void set_action_pose(int slot, ActionPose which, int frames);
    void on_player_died(const sim::State& s, const sim::Event& ev);

    // Rolls the inter-tick snapshots forward when a new sim tick is observed.
    // Keyed on s.tick, so extra draw calls within one tick are no-ops.
    void capture_interp(const sim::State& s);

    // On-screen FIELD-pixel position for a moving entity: lerps prev→curr unless
    // the snapshot is unusable or the entity jumped further than any legit
    // per-tick travel — then it snaps on BOTH axes. docs/render-notes.md §1.
    Posf interp_pos(PrevPos prev, sim::Fixed x, sim::Fixed y) const;

    // Position + facing for PLAYER `i`, played back from the sim's
    // per-sub-frame trace rather than lerped between the 20 Hz endpoints.
    Posf player_interp(const sim::State& s, int i, int& out_dir) const;

    void draw_sprite(const Sprite& sp, Posf at, Tint tint = kNoTint);
    void draw_anim(const Anim& a, std::size_t step, Posf at, Tint tint = kNoTint);

    // Cosmetic render-side RNGs — NEVER State::rng (determinism rule 6).
    // disease_flash_colour is the original's `rand() % 10` frame pick
    // (sub_41F29B ~23252): which of the ten real player-colour sets to use.
    int disease_flash_colour();
    std::uint32_t panic_roll();
    std::uint32_t gold_roll();
    void update_gold_sparkles(const sim::State& s);
    void spawn_gold_sparkle(const sim::Player& p);

    // Which of AssetStore's per-slot recoloured sets to draw `slot` with.
    // Bounds-checks, then delegates to the SDL-free match::team_render_colour,
    // whose doc comment carries the sub_4214BC citation and the doctests.
    static int render_colour(const sim::State& s, int slot);

    // True when all four orthogonal neighbours of (tx,ty) are impassable
    // (wall/brick/burning brick or a resting bomb); out-of-grid counts blocked.
    static bool boxed_in(const sim::State& s, int tx, int ty);

    // A plain 1:1 table matching off_45BEA0's name order, not a live neighbour
    // scan (docs/re/facts.md "Flame arm-shape selection").
    static const Anim& flame_piece(const FlameSet& fset, sim::FlameKind kind);

    SDL_Renderer* ren_ = nullptr;
    const AssetStore* assets_ = nullptr;
    const SequenceSet* seqs_ = nullptr;
    const assets::res::ValueList* values_ = nullptr;

    // Walk memory, sampled once per sim tick: the render loop runs faster than
    // the 20 Hz sim, so a per-frame position diff would read "idle".
    std::uint64_t last_tick_ = ~0ull;
    std::array<sim::Fixed, sim::kMaxPlayers> last_x_{}, last_y_{};
    std::array<bool, sim::kMaxPlayers> moving_{};
    std::array<std::uint32_t, sim::kMaxPlayers> walk_phase_{};
    // The original's +48 body anim counter (carry_pose.hpp). Only the carry and
    // pickup poses read it; walk/stand keep walk_phase_, which is what the
    // visual pins were captured against.
    std::array<std::uint32_t, sim::kMaxPlayers> body_phase_{};
    // Action-pose countdowns (ticks), driven by the unhashed BombKicked/
    // BombPunched/BombGrabbed events. Cosmetic; never touches the sim.
    std::array<int, sim::kMaxPlayers> kick_pose_{}, punch_pose_{};
    std::array<int, sim::kMaxPlayers> pickup_pose_{};
    // Idle fidget (sub_41F29B): `panic_elapsed_` is the ANI phase, and reaching
    // the current variant's own frame count is what re-rolls the next — the
    // original re-rolls on ANI-cycle completion, NOT on a fixed tick spread.
    std::array<bool, sim::kMaxPlayers> panic_active_{};
    std::array<int, sim::kMaxPlayers> panic_elapsed_{};
    std::array<int, sim::kMaxPlayers> panic_variant_{};
    // Ticks since this player started carrying, 0 on the grab tick — our +80
    // equivalent. Clamped at 4 because carry_arc_index saturates at 3.
    std::array<int, sim::kMaxPlayers> carry_ticks_{};
    std::array<bool, sim::kMaxPlayers> carrying_prev_{};

    // Inter-tick snapshots (docs/render-notes.md §1). Bombs/rovers match by slot
    // index — a slot reused by a NEW entity lerps from stale data at worst one
    // frame, and only if the jump is under the snap threshold.
    std::uint64_t interp_tick_ = ~0ull;  // tick the seen_* arrays hold
    bool interp_valid_ = false;          // prev_* holds a real earlier tick
    float interp_alpha_ = 1.0f;          // this frame's fraction (set by draw_frame)
    std::array<sim::Fixed, sim::kMaxPlayers> prev_px_{}, prev_py_{}, seen_px_{}, seen_py_{};
    std::array<std::array<sim::State::SubSample, sim::kSubFrames>, sim::kMaxPlayers> trace_{};
    std::vector<EntSnap> prev_bombs_, seen_bombs_;
    std::vector<EntSnap> prev_rovers_, seen_rovers_;

    std::vector<DeathFx> deaths_;
    std::uint64_t hurry_until_ = 0;  // HURRY! banner flashes until this tick
    bool untimed_ = false;           // draw the KFONT 'infinity' glyph instead of MM:SS
    std::uint32_t flash_lcg_ = 0x2545F491u;
    std::uint32_t panic_lcg_ = 0x9E3779B9u;

    // The gold twinkle pool — a fixed 100 slots, matching dword_4621CC; each
    // spark is positioned at spawn and ages once per RENDER frame (§5).
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
