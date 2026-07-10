#include "systems/flames.hpp"

#include <algorithm>

#include "bomber/sim/rng.hpp"
#include "grid.hpp"
#include "systems/powerups.hpp"

namespace bomber::sim {

void FlameSystem::burn_powerup_here(int tx, int ty) {
    State& s = s_;
    if (s.floor[ty][tx] == PowerupType::None) return;
    const PowerupType burned = s.floor[ty][tx];
    s.events.push_back({Event::Type::PowerupBurned, -1, static_cast<std::int8_t>(tx),
                        static_cast<std::int8_t>(ty), static_cast<std::int8_t>(burned)});
    s.floor[ty][tx] = PowerupType::None;
    // "Diseases Can Be Destroyed" OFF (dword_464990=0, options.ini
    // diseases_destroyable= / VALUELST 120): a burned skull is not lost —
    // a fresh one relocates to a random free tile. The flame walk's
    // powerup branch (sub_42331C ~25626/25653) runs `if (kind == 2 &&
    // !dword_464990) sub_4255B2(2)` right after the destruction;
    // scatter() IS our sub_4255B2, so order and count of the RNG draws
    // mirror the original. Destroying the token itself is unconditional.
    if (burned == PowerupType::Disease && !s.tuning.diseases_destroyable)
        powerups_.scatter(PowerupType::Disease);
}

bool FlameSystem::ignite_epicentre(int tx, int ty, std::uint8_t owner) {
    // The bomb's own tile (sub_42331C epicentre block, pseudo.c 25619-25636):
    // ALWAYS ignited (it is inherently blank — a bomb cannot rest on
    // solid/brick), THEN any powerup there is destroyed. No stop/occupancy
    // test here; that only applies to the extending arm below. No bomb check
    // either: placement itself is gated on sub_422E48 (no two bombs ever
    // share a tile), so a second bomb can never be sitting on the epicentre.
    State& s = s_;
    if (!grid::in_grid(tx, ty)) return false;
    s.flame[ty][tx] = static_cast<std::uint8_t>(
        std::clamp<std::int32_t>(s.tuning.flame_frames, 1, 255));
    s.flame_owner[ty][tx] = owner;
    s.flame_kind[ty][tx] = FlameKind::Center;  // off_45BEA0[8], pseudo.c 25625
    burn_powerup_here(tx, ty);
    return true;
}

bool FlameSystem::spread_to(int tx, int ty, std::uint8_t owner, Direction from_dir,
                            bool is_last_of_reach) {
    // The extending arm (sub_42331C per-direction loop, pseudo.c 25637-25678).
    // Per tile step, in order: a GROUNDED bomb here stops the arm and QUEUES
    // it for a forced detonation next tick (sub_423209 @ 25645 — CONFIRMED a
    // deferred queue, not a synchronous chain: see docs/re/facts.md "Chain-
    // reaction timing") — the tile is NOT ignited by this arm at all (no
    // sub_426FCC call on that branch); the chained bomb's OWN explosion
    // (next tick) flames it via ITS epicentre instead. A VISIBLE floor
    // powerup here (sub_42542D @ 25653, state==2) stops the arm and is
    // destroyed — also not ignited. Only past both checks does the cell-type
    // verdict run: solid stops with no ignite; brick ignites (as "brick
    // burning") and stops; blank ignites and the arm continues. This is
    // facts.md's flagged fidelity gap: our previous port ignited every
    // non-solid/non-brick tile unconditionally, so a flame arm burned
    // straight through bombs and powerups instead of stopping at them
    // (docs/re/facts.md "Options toggles" §"Known remaining fidelity gaps").
    State& s = s_;
    if (!grid::in_grid(tx, ty)) return false;

    if (Bomb* hit = grid::bomb_at(s, tx, ty)) {
        // Ownership transfers to the triggering bomb RIGHT NOW (pseudo.c
        // 25644, `v48[+62] = v75[+62]`, executed before the sub_423209
        // push) — so flame_owner/kill attribution for the eventual chain
        // explosion credits whoever's blast actually set it off, not the
        // chained bomb's original owner. The bomb itself detonates next
        // tick (queue_chain), skipping a re-blast back toward this arm's
        // direction (bomb+56 = opposite(from_dir), pseudo.c
        // `((k+2)&3)+1`).
        hit->owner = owner;
        const int skip = (grid::to_godir(from_dir) + 2) & 3;
        queue_chain(hit->id, skip);
        return false;  // arm stops at the bomb it chain-queues
    }
    if (s.floor[ty][tx] != PowerupType::None) {
        burn_powerup_here(tx, ty);
        return false;  // arm stops at the powerup it burns
    }

    Cell& c = s.cells[ty][tx];
    if (c == Cell::Solid) return false;
    if (c == Cell::Brick) {
        // The brick stays Brick (blocking) for the WHOLE crumble —
        // sub_425EFC's cell-type write nets to a no-op at ignition (blank,
        // then reverted to brick in the very same call, pseudo.c
        // 26826-26846/26782-26797); age_flames_and_bricks() is the ONLY
        // place the cell actually flips, once `burning` hits 0, mirroring
        // sub_426D06's later, conditional `sub_425E9B(x,y,0)`. The hidden
        // powerup, however, reveals RIGHT NOW (sub_425107, called
        // immediately after ignition, pseudo.c 26274-26343 `LABEL_33`) —
        // well before the tile opens up, so it visibly fades in over the
        // still-burning brick instead of popping in only once the brick is
        // fully gone. Re-hitting an already-crumbling brick (another arm
        // reaching this tile before `burning` expires) re-enters this same
        // branch — since `c` is still Brick — and simply resets the timer,
        // matching sub_426FCC's unconditional reinit on every ignite call.
        // docs/re/facts.md "Brick crumble timing".
        s.burning[ty][tx] = static_cast<std::uint8_t>(
            std::clamp<std::int32_t>(s.tuning.brick_burn_frames, 1, 255));
        // Punch/Grab/SuperDisease may relocate instead of revealing here —
        // see relocate_overpowered_here. Runs BEFORE the reveal check below,
        // exactly where sub_425107 sits relative to LABEL_33 in the original
        // (both are part of the SAME ignition call, relocate-then-reveal).
        relocate_overpowered_here(tx, ty);
        if (s.hidden[ty][tx] != PowerupType::None) {
            s.floor[ty][tx] = s.hidden[ty][tx];
            s.hidden[ty][tx] = PowerupType::None;
            s.events.push_back({Event::Type::PowerupRevealed, -1, static_cast<std::int8_t>(tx),
                                static_cast<std::int8_t>(ty),
                                static_cast<std::int8_t>(s.floor[ty][tx])});
        }
        s.events.push_back({Event::Type::BrickDestroyed, -1, static_cast<std::int8_t>(tx),
                            static_cast<std::int8_t>(ty), 0});
        return false;  // flame stops at the brick it destroys
    }

    s.flame[ty][tx] = static_cast<std::uint8_t>(
        std::clamp<std::int32_t>(s.tuning.flame_frames, 1, 255));
    s.flame_owner[ty][tx] = owner;
    // kind = godir (a TIP) only at the arm's FULL configured reach, else
    // godir+4 (a MID) — pseudo.c 25673-25677 `if (reach-1==m) v45=k; else
    // v45=k+4;`, decided here (only on the "arm continues" path) exactly
    // like the original. FlameKind's tip/mid pairs are declared in the same
    // compass order as godir, so godir+4 lands on the matching mid piece.
    s.flame_kind[ty][tx] = static_cast<FlameKind>(
        grid::to_godir(from_dir) + (is_last_of_reach ? 0 : 4));
    return true;
}

// sub_425107's early gated branch (pseudo.c 26295-26336), disassembly-pinned
// 2026-07-10 (docs/re/facts.md "Overpowered-powerup relocation"): a hidden
// token whose kind is Punch/Grab/SuperDisease (off_45BE50 indices 5/6/11 —
// the shipped VALUELST.RES's own id-102 comment calls these "over-powerful"
// powers) doesn't reveal the first time its brick burns, for the opening
// `overpowered_relocate_seconds` of the match — it swaps with a DIFFERENT
// kind's record elsewhere on a still-standing brick, or (failing that) moves
// to an empty brick with no reveal at all. No network-role gate: the
// original also requires a non-networked game (!sub_40C06A()), but this
// port has no netplay concept yet (ADR-0003 defers it) — every match here
// IS the original's "local" case, so that half of the binary's gate is
// always-true and simply omitted.
void FlameSystem::relocate_overpowered_here(int tx, int ty) {
    State& s = s_;
    const PowerupType kind = s.hidden[ty][tx];
    if (kind != PowerupType::Punch && kind != PowerupType::Grab &&
        kind != PowerupType::SuperDisease)
        return;
    const auto deadline = static_cast<std::uint64_t>(s.tuning.overpowered_relocate_seconds) *
                          kTicksPerSecond;
    if (s.tick >= deadline) return;

    // Pass 1 (pseudo.c 26304-26322): up to 200 tries, ALWAYS drawing x then y
    // even when the candidate is rejected (the rand_() calls sit before the
    // guard/checks in the original, so a miss still burns its 2 draws).
    // Accepts the first still-standing brick holding ANY other kind's record
    // (hidden OR already-visible-but-still-crumbling — sub_42542D tests the
    // record's presence regardless of state, and the original's swap is a
    // raw struct copy that moves the state byte along with the kind) and
    // swaps the two records whole.
    for (int i = 0; i < 200; ++i) {
        const int rx = static_cast<int>(random_below(s, kGridWidth));
        const int ry = static_cast<int>(random_below(s, kGridHeight));
        if (s.cells[ry][rx] != Cell::Brick) continue;
        const bool cand_hidden = s.hidden[ry][rx] != PowerupType::None;
        const PowerupType cand_kind = cand_hidden ? s.hidden[ry][rx] : s.floor[ry][rx];
        if (cand_kind == PowerupType::None) continue;
        if (cand_kind == PowerupType::Punch || cand_kind == PowerupType::Grab ||
            cand_kind == PowerupType::SuperDisease)
            continue;
        if (cand_hidden) {
            s.hidden[ry][rx] = PowerupType::None;
            s.hidden[ty][tx] = cand_kind;  // this tile stays "hidden"
        } else {
            s.floor[ry][rx] = PowerupType::None;
            s.hidden[ty][tx] = PowerupType::None;
            s.floor[ty][tx] = cand_kind;  // this tile becomes "floor" (already visible)
        }
        s.hidden[ry][rx] = kind;  // candidate tile becomes "hidden" (the source's own state)
        return;
    }
    // Pass 2 (pseudo.c 26323-26334): only reached if pass 1 exhausted all 200
    // tries. A second, independent 200-try search for a completely EMPTY
    // brick (no record at all) — a plain MOVE, not a swap: this tile ends up
    // with nothing, so the caller's reveal check below fires on NOTHING
    // (matching the original's early `return` before its own LABEL_33).
    for (int i = 0; i < 200; ++i) {
        const int rx = static_cast<int>(random_below(s, kGridWidth));
        const int ry = static_cast<int>(random_below(s, kGridHeight));
        if (s.cells[ry][rx] != Cell::Brick) continue;
        if (s.hidden[ry][rx] != PowerupType::None || s.floor[ry][rx] != PowerupType::None) continue;
        s.hidden[ry][rx] = kind;
        s.hidden[ty][tx] = PowerupType::None;
        return;
    }
    // Both searches exhausted (only plausible on an almost fully-cleared
    // board): leave the record untouched — the caller reveals it as-is.
}

void FlameSystem::explode(std::size_t bomb_index, int skip_dir) {
    State& s = s_;
    Bomb& b = s.bombs[bomb_index];
    if (!b.active) return;
    b.active = false;
    if (s.players[b.owner].bombs_placed > 0) --s.players[b.owner].bombs_placed;

    int cx = b.tile_x(), cy = b.tile_y();
    int reach = b.flame;
    s.events.push_back({Event::Type::Explosion, static_cast<std::int8_t>(b.owner),
                        static_cast<std::int8_t>(cx), static_cast<std::int8_t>(cy), 0});
    ignite_epicentre(cx, cy, b.owner);
    for (Direction d : {Direction::Up, Direction::Down, Direction::Left, Direction::Right}) {
        // A chain-triggered bomb (skip_dir >= 0) never re-casts an arm back
        // toward the flame that triggered it (bomb+56, pseudo.c 25621:
        // `if (!field56 || k+1 != field56)`) — every OTHER direction still
        // gets its normal full-reach arm.
        if (skip_dir >= 0 && grid::to_godir(d) == skip_dir) continue;
        for (int i = 1; i <= reach; ++i) {
            if (!spread_to(cx + grid::dir_dx(d) * i, cy + grid::dir_dy(d) * i, b.owner, d,
                           i == reach))
                break;
        }
    }
}

void FlameSystem::queue_chain(std::uint32_t bomb_id, int skip_dir) {
    s_.pending_chain.push_back({bomb_id, static_cast<std::int8_t>(skip_dir)});
}

void FlameSystem::drain_chain_queue() {
    State& s = s_;
    if (s.pending_chain.empty()) return;
    // Pull this tick's queue out before exploding anything: explode() (via
    // spread_to) may itself push NEW entries for the tick AFTER this one, and
    // those must not be visited by the loop below.
    std::vector<State::PendingChain> pending;
    pending.swap(s.pending_chain);
    // Ascending bomb order, not push order: the original's drain loop only
    // stamps flags (forces fuse-elapsed = fuse-duration, stashes bomb+56) —
    // the actual explosions happen later, as each bomb's OWN slot comes up
    // in the main per-slot pass (0..99 ascending, pseudo.c 25335-25350). Our
    // append-only `bombs` vector has no slot reuse, so "vector position" is
    // simply creation order among currently-active bombs — not bit-identical
    // to the original's slot indices, but the same idea (a well-defined,
    // deterministic order for any RNG draws a chained explosion's arm makes,
    // e.g. a scatter() on a Disease tile). A bomb queued more than once
    // keeps its LAST push's skip_dir (mirrors the original's unconditional
    // per-entry overwrite of bomb+56).
    for (std::size_t i = 0; i < s.bombs.size(); ++i) {
        Bomb& b = s.bombs[i];
        if (!b.active) continue;
        bool queued = false;
        int skip = -1;
        for (const auto& entry : pending) {
            if (entry.bomb_id == b.id) {
                queued = true;
                // bugprone-signed-char-misuse (NOLINT below): the widening
                // sign-extension IS the intent here — skip_dir's -1 sentinel
                // ("no restriction") must stay -1 as an int, not become 255.
                skip = entry.skip_dir;  // NOLINT(bugprone-signed-char-misuse)
            }
        }
        if (queued) explode(i, skip);
    }
}

void FlameSystem::age_flames_and_bricks() {
    State& s = s_;
    for (int y = 0; y < kGridHeight; ++y) {
        for (int x = 0; x < kGridWidth; ++x) {
            if (s.flame[y][x] > 0) --s.flame[y][x];
            if (s.burning[y][x] > 0 && --s.burning[y][x] == 0) {
                // The powerup already revealed at ignition (spread_to); this
                // is only the deferred cell-type flip (sub_426D06's
                // `if (sub_425FB9(x,y)==2) sub_425E9B(x,y,0)`), guarded the
                // same way — only clear it if it is still Brick (in case
                // something else, e.g. the enclosure/regen systems,
                // already overwrote the tile).
                if (s.cells[y][x] == Cell::Brick) s.cells[y][x] = Cell::Blank;
            }
        }
    }
}

}  // namespace bomber::sim
