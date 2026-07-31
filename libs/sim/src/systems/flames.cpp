#include "systems/flames.hpp"

#include <algorithm>
#include <optional>

#include "bomber/sim/rng.hpp"
#include "grid.hpp"
#include "systems/powerups.hpp"

namespace bomber::sim {

namespace {

// off_45BE50 indices 5/6/11 — the shipped VALUELST.RES's own id-102 comment
// calls these the "over-powerful" powers (facts.md "Overpowered-powerup
// relocation").
bool is_overpowered(PowerupType k) {
    return k == PowerupType::Punch || k == PowerupType::Grab || k == PowerupType::SuperDisease;
}

// The LAST push's skip_dir for this bomb, or nullopt if it was never queued —
// mirroring the original's unconditional per-entry overwrite of bomb+56.
std::optional<int> queued_skip_for(const std::vector<State::PendingChain>& pending,
                                   std::uint32_t bomb_id) {
    std::optional<int> skip;
    for (const auto& entry : pending)
        // bugprone-signed-char-misuse (NOLINT): the widening sign-extension IS
        // the intent — skip_dir's -1 sentinel ("no restriction") must stay -1 as
        // an int, not become 255.
        if (entry.bomb_id == bomb_id) skip = entry.skip_dir;  // NOLINT(bugprone-signed-char-misuse)
    return skip;
}

}  // namespace

void FlameSystem::burn_powerup_here(int tx, int ty) {
    State& s = s_;
    if (s.floor[ty][tx] == PowerupType::None) return;
    const PowerupType burned = s.floor[ty][tx];
    s.events.push_back({Event::Type::PowerupBurned, -1, static_cast<std::int8_t>(tx),
                        static_cast<std::int8_t>(ty), static_cast<std::int8_t>(burned)});
    s.floor[ty][tx] = PowerupType::None;
    // "Diseases Can Be Destroyed" OFF (dword_464990 = 0, VALUELST 120): a burned
    // skull is not lost, a fresh one relocates. The flame walk's powerup branch
    // (sub_42331C ~25626/25653) calls sub_4255B2 for kind 2 whenever
    // dword_464990 is clear, right after the destruction; scatter() IS our
    // sub_4255B2, so the RNG order and count mirror the original. Destroying the
    // token itself is unconditional.
    if (burned == PowerupType::Disease && !s.tuning.diseases_destroyable)
        powerups_.scatter(PowerupType::Disease);
}

bool FlameSystem::ignite_epicentre(int tx, int ty, std::uint8_t owner, std::uint8_t colour) {
    // The bomb's own tile (sub_42331C epicentre block, pseudo.c 25619-25636) is
    // ALWAYS ignited — it is inherently blank, since a bomb cannot rest on
    // solid/brick — and any powerup there is then destroyed. No stop/occupancy
    // test here; that applies only to the extending arm. No bomb check either:
    // placement is gated on sub_422E48, so no two bombs ever share a tile.
    State& s = s_;
    if (!grid::in_grid(tx, ty)) return false;
    s.flame[ty][tx] =
        static_cast<std::uint8_t>(std::clamp<std::int32_t>(s.tuning.flame_frames, 1, 255));
    s.flame_owner[ty][tx] = owner;
    s.flame_colour[ty][tx] = colour;
    s.flame_kind[ty][tx] = FlameKind::Center;  // off_45BEA0[8], pseudo.c 25625
    burn_powerup_here(tx, ty);
    return true;
}

bool FlameSystem::spread_to(int tx, int ty, std::uint8_t owner, std::uint8_t colour,
                            Direction from_dir, bool is_last_of_reach) {
    // The extending arm (sub_42331C's per-direction loop, pseudo.c 25637-25678),
    // per tile step and IN THIS ORDER: a grounded bomb stops the arm and queues
    // it (the tile is NOT ignited by this arm — there is no sub_426FCC call on
    // that branch; the chained bomb's own explosion flames it next tick via ITS
    // epicentre); a VISIBLE floor powerup (sub_42542D @ 25653, state == 2) stops
    // the arm and is destroyed, also without igniting; only past both does the
    // cell-type verdict run — solid stops with no ignite, brick ignites as
    // "brick burning" and stops, blank ignites and the arm continues.
    State& s = s_;
    if (!grid::in_grid(tx, ty)) return false;

    if (Bomb* hit = grid::bomb_at(s, tx, ty)) {
        // Ownership transfers to the TRIGGERING bomb right now (pseudo.c 25644
        // copies the detonating bomb's +62 owner word into the bomb it ignites,
        // before the sub_423209 push), so kill attribution for the eventual chain
        // explosion credits whoever's blast actually set it off.
        //
        // The PLACEMENT SLOT moves with that word, because the original keeps no
        // per-player counter: capacity is a live scan (sub_4245DA counts active
        // slots whose +62 == player) against max_bombs in the drop gates. So the
        // victim's capacity frees IMMEDIATELY at transfer time and the chained
        // bomb counts against the CHAINER until it explodes. bombs_placed is that
        // scan's running equivalent; without this move the victim's counter
        // leaked one slot per cross-owner chained bomb, permanently — the
        // reported "5 max bombs, suddenly one placeable" collapse. facts.md
        // "Bomb capacity is a derived live-bomb count".
        if (hit->owner != owner) {
            if (s.players[hit->owner].bombs_placed > 0) --s.players[hit->owner].bombs_placed;
            ++s.players[owner].bombs_placed;
        }
        // ONLY the owner word moves: the chained bomb's COLOUR byte — the other
        // half of the original's +60 dword — is untouched by the 25644 transfer,
        // so its explosion still flames in the original placer's colour.
        hit->owner = owner;
        // The chained bomb skips a re-blast back along this arm: its +56 skip
        // field takes the arm's godir rotated by two.
        const int skip = (grid::to_godir(from_dir) + 2) & 3;
        queue_chain(hit->id, skip);
        return false;
    }
    if (s.floor[ty][tx] != PowerupType::None) {
        burn_powerup_here(tx, ty);
        return false;
    }

    Cell& c = s.cells[ty][tx];
    if (c == Cell::Solid) return false;
    if (c == Cell::Brick) {
        // The brick stays Brick (blocking) for the WHOLE crumble: sub_425EFC's
        // cell-type write nets to a no-op at ignition (blank, then reverted in
        // the same call, pseudo.c 26826-26846), and age_flames_and_bricks is the
        // only place the cell actually flips. The hidden powerup, however,
        // reveals RIGHT NOW — sub_425107 runs immediately after ignition (the
        // reveal block at 26274-26343) — so it fades in over the still-burning
        // brick instead of popping in once the brick is gone. Re-hitting an
        // already-crumbling brick re-enters this branch, since `c` is still
        // Brick, and simply resets the timer, matching sub_426FCC's
        // unconditional reinit per ignite call. facts.md "Brick crumble timing".
        s.burning[ty][tx] =
            static_cast<std::uint8_t>(std::clamp<std::int32_t>(s.tuning.brick_burn_frames, 1, 255));
        // sub_425107's very FIRST statement draws rand() % 30 and calls
        // sub_42BE0B on zero (pseudo.c 26288-26291) — but sub_42BE0B has an EMPTY
        // body (30942, a stubbed debug hook), so the roll has no gameplay effect
        // and only CONSUMES a draw. Reproducing it keeps the stream byte-aligned:
        // without it the port ran one draw short per brick reveal, so every
        // downstream random outcome drifted on any brick-bearing match.
        // Unconditional and BEFORE the relocate/reveal, exactly where sub_425107
        // does it. facts.md "Brick-reveal cure roll (empty hook, RNG-count only)".
        (void)random_below(s, 30);
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
        return false;
    }

    s.flame[ty][tx] =
        static_cast<std::uint8_t>(std::clamp<std::int32_t>(s.tuning.flame_frames, 1, 255));
    s.flame_owner[ty][tx] = owner;
    s.flame_colour[ty][tx] = colour;
    // kind = godir (a TIP) only at the arm's FULL configured reach, else godir+4
    // (a MID) — pseudo.c 25673-25677 — decided here, on the "arm continues" path,
    // exactly like the original. FlameKind's tip/mid pairs are declared in the
    // same compass order as godir, so godir+4 lands on the matching mid piece.
    s.flame_kind[ty][tx] =
        static_cast<FlameKind>(grid::to_godir(from_dir) + (is_last_of_reach ? 0 : 4));
    return true;
}

// sub_425107's early gated branch (pseudo.c 26295-26336), disassembly-pinned
// 2026-07-10 (facts.md "Overpowered-powerup relocation"): a hidden Punch/Grab/
// SuperDisease token does NOT reveal the first time its brick burns, for the
// opening `overpowered_relocate_seconds` of the match. It swaps with a different
// kind's record elsewhere on a still-standing brick, or failing that moves to an
// empty brick with no reveal at all.
//
// The original additionally requires a non-networked game (!sub_40C06A). This
// port has no netplay concept inside the sim (ADR-0003), so every match here IS
// the original's "local" case and that half of the gate is always-true.
void FlameSystem::relocate_overpowered_here(int tx, int ty) {
    State& s = s_;
    const PowerupType kind = s.hidden[ty][tx];
    if (!is_overpowered(kind)) return;
    const auto deadline =
        static_cast<std::uint64_t>(s.tuning.overpowered_relocate_seconds) * kTicksPerSecond;
    if (s.tick >= deadline) return;

    // Pass 1 (pseudo.c 26304-26322): up to 200 tries, ALWAYS drawing x then y
    // even when the candidate is rejected — the rand_() calls sit before the
    // guards in the original, so a miss still burns its 2 draws. Accepts the
    // first still-standing brick holding ANY other kind's record, hidden or
    // already-visible-but-still-crumbling (sub_42542D tests the record's presence
    // regardless of state, and the swap is a raw struct copy that moves the state
    // byte with the kind), and swaps the two records whole.
    for (int i = 0; i < 200; ++i) {
        const int rx = static_cast<int>(random_below(s, kGridWidth));
        const int ry = static_cast<int>(random_below(s, kGridHeight));
        if (s.cells[ry][rx] != Cell::Brick) continue;
        const bool cand_hidden = s.hidden[ry][rx] != PowerupType::None;
        const PowerupType cand_kind = cand_hidden ? s.hidden[ry][rx] : s.floor[ry][rx];
        if (cand_kind == PowerupType::None || is_overpowered(cand_kind)) continue;
        if (cand_hidden) {
            s.hidden[ry][rx] = PowerupType::None;
            s.hidden[ty][tx] = cand_kind;  // this tile stays "hidden"
        } else {
            s.floor[ry][rx] = PowerupType::None;
            s.hidden[ty][tx] = PowerupType::None;
            s.floor[ty][tx] = cand_kind;  // this tile becomes "floor" (already visible)
        }
        s.hidden[ry][rx] = kind;  // the candidate tile takes the source's own state
        return;
    }
    // Pass 2 (26323-26334), reached only if pass 1 exhausted all 200 tries: a
    // second, independent 200-try search for a completely EMPTY brick — a plain
    // MOVE, not a swap. This tile ends up with nothing, so the caller's reveal
    // check fires on NOTHING, matching the original, which returns early before
    // reaching its own reveal block.
    for (int i = 0; i < 200; ++i) {
        const int rx = static_cast<int>(random_below(s, kGridWidth));
        const int ry = static_cast<int>(random_below(s, kGridHeight));
        if (s.cells[ry][rx] != Cell::Brick) continue;
        if (s.hidden[ry][rx] != PowerupType::None || s.floor[ry][rx] != PowerupType::None) continue;
        s.hidden[ry][rx] = kind;
        s.hidden[ty][tx] = PowerupType::None;
        return;
    }
    // Both searches exhausted (only plausible on an almost fully-cleared board):
    // leave the record untouched — the caller reveals it as-is.
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
    ignite_epicentre(cx, cy, b.owner, b.colour);
    // Cast the four arms in ASCENDING GODIR order 0,1,2,3 = Up,Right,Down,Left
    // (flames.md finding 1): sub_42331C's arm loop runs k from 0 to 3 and indexes
    // the delta tables dword_45BECC/45BEDC directly by k, so k IS the godir. The
    // arms are otherwise independent, so the order is inert EXCEPT where an arm
    // draws State::rng (relocate_overpowered_here / burn_powerup_here ->
    // scatter): with two such draws in different directions in one explosion, the
    // wrong order desyncs the stream for the rest of the match. The prior
    // enum-declaration braced list {Up,Down,Left,Right} visited godir 0,2,3,1.
    for (int g = 0; g < 4; ++g) {
        // A chain-triggered bomb never re-casts an arm back toward the flame that
        // triggered it (the +56 skip field, pseudo.c 25621); every OTHER
        // direction still gets its normal full-reach arm.
        if (skip_dir >= 0 && g == skip_dir) continue;
        const Direction d = grid::from_godir(g);
        for (int i = 1; i <= reach; ++i) {
            if (!spread_to(cx + grid::dir_dx(d) * i, cy + grid::dir_dy(d) * i, b.owner, b.colour, d,
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
    // Pull this tick's queue out BEFORE exploding anything: explode() (via
    // spread_to) may push NEW entries for the tick after this one, and those must
    // not be visited by the loop below.
    std::vector<State::PendingChain> pending;
    pending.swap(s.pending_chain);
    // Ascending bomb order, not push order: the original's drain loop only stamps
    // flags (forces fuse-elapsed = duration, stashes bomb+56) and the actual
    // explosions happen later, as each bomb's OWN slot comes up in the main
    // per-slot pass (0..99 ascending, pseudo.c 25335-25350). Our append-only
    // vector has no slot reuse, so position is creation order among active bombs
    // — not bit-identical to the original's slot indices, but the same idea: a
    // well-defined deterministic order for any RNG draw a chained arm makes.
    for (std::size_t i = 0; i < s.bombs.size(); ++i) {
        if (!s.bombs[i].active) continue;
        if (const std::optional<int> skip = queued_skip_for(pending, s.bombs[i].id))
            explode(i, *skip);
    }
}

void FlameSystem::age_flames_and_bricks() {
    State& s = s_;
    grid::for_each_cell([&](int x, int y) {
        if (s.flame[y][x] > 0) --s.flame[y][x];
        if (s.burning[y][x] > 0 && --s.burning[y][x] == 0) {
            // The powerup already revealed at ignition; this is only the deferred
            // cell-type flip. sub_426D06 asks sub_425FB9 for the cell type and
            // calls sub_425E9B(x,y,0) only when it answers 2 (brick) — guard the
            // same way, in case the enclosure or regen system already overwrote
            // the tile.
            if (s.cells[y][x] == Cell::Brick) s.cells[y][x] = Cell::Blank;
        }
    });
}

}  // namespace bomber::sim
