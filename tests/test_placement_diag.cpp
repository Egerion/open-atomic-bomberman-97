// DIAGNOSTIC + REGRESSION harness for the user report "sometimes I can't place
// bombs, for no reason." Drives a player pressing action1 across scenarios and
// records, per tick, whether a bomb was placed and — when NOT — which sim gate
// refused it. The gate predicates below MIRROR the drop dispatch in
// simulation.cpp (player_turn) + BombSystem::drop, so every refusal is
// attributed to a named cause and compared against the original's gates
// (sub_41F29B LABEL_246 / sub_41EB13 / the sub_4245DA live-bomb scan).
//
// Findings pinned here (all FAITHFUL to the binary — see the investigation
// report): constipation and stun hard-block placement; a healthy player on an
// open tile is never refused; and a chained bomb frees its owner's slot on its
// OWN (deferred) explosion tick, exactly matching the original's sub_4245DA
// active-flag scan (no extra-tick inflation from the chain-queue).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <cstdio>
#include <string>

#include <doctest/doctest.h>

#include "helpers.hpp"

using namespace bomber::sim;
using namespace bomber::sim::test;

namespace {

bool tile_open_for(const State& s, int tx, int ty) { return !tile_blocked(s, tx, ty); }

// Teleport a player onto a tile centre (white-box, like other suites).
void put(Player& p, int tx, int ty) {
    p.x = tx * kTileWF + kTileWF / 2;
    p.y = ty * kTileHF + kTileHF / 2;
}

bool placed_this_tick(const Simulation& s, int owner) {
    for (const auto& e : s.state().events)
        if (e.type == Event::Type::BombPlaced && e.player == owner) return true;
    return false;
}

// Mirror of the placement gates, in the SAME order player_turn/drop apply them.
// Evaluated on the PRE-tick state. Returns nullptr if placement should succeed.
const char* refusal_reason(const State& s, int i) {
    const Player& p = s.players[i];
    if (!p.present || !p.alive) return "DEAD";
    if (p.stun > 0) return "STUN";
    if (p.bounce > 0) return "BOUNCE";
    if (p.warp > 0) return "WARP";
    if (p.sick(Disease::Constipation)) return "CONSTIPATION";
    bool own_under = false;
    for (const auto& b : s.bombs)
        if (b.active && !b.flying && b.tile_x() == p.tile_x() && b.tile_y() == p.tile_y() &&
            b.owner == static_cast<std::uint8_t>(i))
            own_under = true;
    if (p.grab && own_under) return "GRAB(edge eaten)";
    if (p.spooge && own_under) return "SPOOGE(edge eaten)";
    if (!tile_open_for(s, p.tile_x(), p.tile_y())) return "TILE_BLOCKED";
    if (tile_has_bomb(s, p.tile_x(), p.tile_y())) return "BOMB_HERE";
    if (p.bombs_placed >= p.max_bombs) return "AT_LIMIT";
    return nullptr;
}

}  // namespace

// --- A: solo refill timing after a normal explosion ----------------------
// Place a bomb, step OFF it (so the owner survives), and measure the gap
// between the explosion (bombs_placed 1->0) and the first successful re-place.
// Establishes the baseline dead-window a normal bomb costs: exactly one tick.
TEST_CASE("A: solo refill is one tick after a normal explosion") {
    Simulation s(open_config());
    State& st = s.state();
    st.players[1].alive = false;
    Player& p = st.players[0];
    p.max_bombs = 1;
    p.flame = 1;  // blast reaches only (1,0)/(0,1); a bystander tile stays safe

    put(p, 0, 0);
    s.tick(press1(0));  // drop A at (0,0)
    REQUIRE(p.bombs_placed == 1);
    put(p, 0, 4);  // step the owner far out of A's reach-1 blast

    long long exploded = -1, replaced = -1;
    for (int t = 0; t < 60 && replaced < 0; ++t) {
        const bool was_live = p.bombs_placed > 0;
        s.tick(TickInputs{});  // idle: let A's fuse run
        if (was_live && p.bombs_placed == 0) exploded = static_cast<long long>(st.tick) - 1;
        if (exploded >= 0) {
            // Try a fresh edge press at the safe (0,4) tile.
            s.tick(press1(0));
            if (placed_this_tick(s, 0)) replaced = static_cast<long long>(st.tick) - 1;
            s.tick(TickInputs{});
        }
    }
    std::printf("[A] exploded=%lld replaced=%lld gap=%lld\n", exploded, replaced,
                replaced - exploded);
    CHECK(exploded >= 0);
    CHECK(replaced >= 0);
    CHECK(replaced - exploded == 1);  // faithful: the tick after the slot frees
}

// --- B: constipation refuses every drop; flash cue live every tick -------
TEST_CASE("B: constipation refuses every drop, flash cue always renderable") {
    Simulation s(open_config());
    State& st = s.state();
    st.players[1].alive = false;
    Player& p = st.players[0];
    p.max_bombs = 5;
    infect(p, Disease::Constipation);

    // The renderer's disease strobe is `(disease_timer & 8) != 0` (the confirmed
    // sub_41F29B ~23252 mechanism). Pin that this cue actually PULSES (both on
    // and off phases occur) over the infection — a stuck-on or stuck-off value
    // would not read as a distinct diseased state. This verifies the render
    // cue's logic headlessly against the real hashed disease_timer, since
    // renderer.cpp is not built in the headless preset.
    int placed = 0;
    bool flash_live = true, flash_on = false, flash_off = false;
    for (int t = 0; t < 20; ++t) {
        if (p.disease_timer <= 0) flash_live = false;      // any-disease liveness
        if ((p.disease_timer & 8) != 0) flash_on = true;   // strobe ON phase
        else flash_off = true;                             // strobe OFF phase
        put(p, 4, 4);          // always a clear, safe tile
        s.tick(TickInputs{});  // release
        s.tick(press1(0));     // press
        if (placed_this_tick(s, 0)) ++placed;
    }
    std::printf("[B] constipation placed=%d flash_live=%d pulse(on=%d,off=%d)\n", placed,
                flash_live ? 1 : 0, flash_on ? 1 : 0, flash_off ? 1 : 0);
    CHECK(placed == 0);       // FAITHFUL: +134 blocks the drop block entirely
    CHECK(flash_live);        // a disease is active the whole time
    CHECK(flash_on);          // the strobe pulse fires...
    CHECK(flash_off);         // ...and rests — a legible 8-on/8-off cue
    CHECK(p.bombs_placed == 0);
}

// --- C: stun blocks placement for its full window, then recovers ---------
// head_stun_frames defaults to 16 FRAMES, burned 3 per 20 Hz tick (facts.md
// "Canonical frame cadence") — ~0.27 s: a bomb bouncing on the head
// (PowerupSystem::head_hit) locks placement out for about a quarter second.
TEST_CASE("C: a head-hit-sized stun blocks placement for its whole 16-frame window") {
    Simulation s(open_config());
    State& st = s.state();
    st.players[1].alive = false;
    Player& p = st.players[0];
    p.max_bombs = 5;
    put(p, 4, 4);
    p.stun = st.tuning.head_stun_frames;
    const int stun0 = p.stun;

    int refused_stun = 0;
    long long first_place = -1;
    for (int t = 0; t < stun0 + 6; ++t) {
        const char* r = refusal_reason(st, 0);
        put(p, 4, 4);
        s.tick(TickInputs{});
        s.tick(press1(0));
        if (r && std::string(r) == "STUN" && !placed_this_tick(s, 0)) ++refused_stun;
        if (placed_this_tick(s, 0) && first_place < 0) first_place = static_cast<long long>(st.tick);
    }
    std::printf("[C] stun0=%d refused_stun=%d first_place=%lld\n", stun0, refused_stun, first_place);
    // 16 frames span ceil(16/3) = 6 blocked ticks = >= 2 of this loop's
    // 2-tick (release+press) iterations still refused.
    CHECK(refused_stun >= 2);  // a run of refusals through the stun window
    CHECK(first_place >= 0);   // recovers once stun hits 0
}

// --- D / F: chain-queue does NOT inflate the live-bomb count -------------
// The decisive suspect-#2 test. Owner holds two bombs; A's blast chains B.
// Log bombs_placed per tick and assert B frees its slot on its OWN deferred
// explosion tick (one tick after A), exactly as the original's per-slot pass
// clears the active flag after that same tick's drain — NOT held an extra tick.
TEST_CASE("F: a chained bomb frees the owner's slot on its own deferred tick (no inflation)") {
    Simulation s(open_config());
    State& st = s.state();
    st.players[1].alive = false;
    Player& p = st.players[0];
    p.max_bombs = 2;
    p.flame = 3;  // A at (0,0) reaches (2,0)

    put(p, 0, 0);
    s.tick(press1(0));  // A placed at (0,0), fuse = fuse_frames
    REQUIRE(p.bombs_placed == 1);

    // Inject B as the owner's second bomb at (2,0) with a LONG fuse, so it can
    // only die by A's chain (isolating the chain path). Mirror a real place:
    // active, grounded, owned by player 0.
    Bomb b;
    b.active = true;
    b.id = st.next_bomb_id++;
    b.owner = 0;
    b.x = 2 * kTileWF + kTileWF / 2;
    b.y = 0 * kTileHF + kTileHF / 2;
    b.fuse_init = 999;
    b.fuse = 999;
    b.flame = 1;
    st.bombs.push_back(b);
    ++p.bombs_placed;  // keep the count honest (place() would have)
    REQUIRE(p.bombs_placed == 2);

    put(p, 0, 6);  // owner steps well clear of both blasts

    // Log bombs_placed around A's explosion. fuse_frames default 40; A placed
    // at tick 1 -> explodes ~tick 41; B chains one tick later.
    long long a_free = -1, b_free = -1;
    int prev = p.bombs_placed;
    for (int t = 0; t < 60; ++t) {
        s.tick(TickInputs{});
        const int now = p.bombs_placed;
        if (now != prev) {
            std::printf("[F] tick %llu: bombs_placed %d -> %d\n",
                        static_cast<unsigned long long>(st.tick), prev, now);
            if (prev == 2 && now == 1) a_free = static_cast<long long>(st.tick);
            if (prev == 1 && now == 0) b_free = static_cast<long long>(st.tick);
            prev = now;
        }
    }
    std::printf("[F] a_free=%lld b_free=%lld delta=%lld\n", a_free, b_free, b_free - a_free);
    CHECK(a_free >= 0);
    CHECK(b_free >= 0);
    // The chained bomb frees exactly ONE tick after the trigger bomb — the
    // faithful 1-link-per-tick deferral, not two ticks (which would be the
    // feared "held an extra tick" inflation).
    CHECK(b_free - a_free == 1);
}

// --- E: healthy player on an open tile is NEVER refused ------------------
// Control: teleport across many open tiles and confirm every first press
// places. A spurious refusal here means a bogus gate crept in.
TEST_CASE("E: healthy player on an open tile is never spuriously refused") {
    Simulation s(open_config());
    State& st = s.state();
    st.players[1].alive = false;
    Player& p = st.players[0];
    p.max_bombs = 20;  // never hit the limit within the test
    p.flame = 1;

    const int tiles[][2] = {{0, 0}, {2, 0}, {4, 0}, {6, 0}, {0, 2}, {2, 2},
                            {4, 2}, {6, 2}, {8, 0}, {10, 0}, {12, 0}, {0, 4}};
    int successes = 0, spurious = 0;
    for (auto& tl : tiles) {
        put(p, tl[0], tl[1]);
        const char* r = refusal_reason(st, 0);
        s.tick(TickInputs{});  // release (idle; teleport holds position)
        put(p, tl[0], tl[1]);
        s.tick(press1(0));
        if (placed_this_tick(s, 0)) {
            ++successes;
        } else {
            ++spurious;
            std::printf("[E] SPURIOUS refusal at (%d,%d): %s\n", tl[0], tl[1], r ? r : "unknown");
        }
    }
    std::printf("[E] successes=%d spurious=%d\n", successes, spurious);
    CHECK(spurious == 0);
    CHECK(successes == 12);
}

// ============================================================================
// REGRESSION HUNT for the user report "bomb placement sometimes just vanishes"
// (intermittent; suspected NEW since the recent sim merges). Everything above
// pins the individual gates; the cases below HAMMER the placement path across
// seeds, positions, and press timings to catch a non-obvious intermittent
// vanish deterministically. If the sim never fails here, the regression is
// presentation-side (draw order), not a lost drop in the sim.
// ============================================================================

namespace {

// Test-side xorshift (NOT the sim RNG — never touch State::rng from a test).
struct Rng32 {
    std::uint32_t v;
    explicit Rng32(std::uint32_t seed) : v(seed ? seed : 0x9e3779b9u) {}
    std::uint32_t next() {
        v ^= v << 13;
        v ^= v >> 17;
        v ^= v << 5;
        return v;
    }
    int below(int n) { return static_cast<int>(next() % static_cast<std::uint32_t>(n)); }
};

// An open (non-solid) tile in open_config's (odd,odd)-pillar arena.
bool arena_open_tile(int tx, int ty) {
    return tx >= 0 && tx < kGridWidth && ty >= 0 && ty < kGridHeight && !(tx % 2 == 1 && ty % 2 == 1);
}

}  // namespace

// --- G: exhaustive placement fuzzer, pure gate ---------------------------
// The decisive vanish hunt. Every tick: wipe the player back to a clean,
// healthy, stationary actor (so ONLY the core placement gate is under test),
// teleport it to a random open tile, and drive a random action1 level. Predict
// placement from the SAME gate the sim applies (rising edge + open + no-bomb +
// under-limit) and assert the sim agrees BYTE-for-byte, every tick, across many
// seeds. A single intermittent "legal press produced no bomb" would trip here.
TEST_CASE("G: placement never vanishes under a randomized press/position fuzz") {
    long long checked = 0, vanished = 0, phantom = 0;
    for (std::uint32_t seed = 1; seed <= 40; ++seed) {
        Simulation s(open_config());
        State& st = s.state();
        st.players[1].present = false;
        st.players[1].alive = false;
        Player& p = st.players[0];
        Rng32 rng(seed * 2654435761u + 1u);
        bool prev_press = false;

        for (int t = 0; t < 400; ++t) {
            // Scrub every modifier so this isolates the raw drop gate: healthy,
            // upright, no grab/spooge/trigger double-tap, generous slot budget.
            p.present = true;
            p.alive = true;
            p.stun = 0;
            p.bounce = 0;
            p.warp = 0;
            p.carrying = false;
            p.grab = p.spooge = p.trigger = p.kick = p.punch = false;
            p.goldflame = false;
            p.jelly = false;
            for (auto& d : p.disease) d = false;
            p.disease_timer = 0;
            p.max_bombs = 99;
            p.flame = 1;

            int tx, ty;
            do {
                tx = rng.below(kGridWidth);
                ty = rng.below(kGridHeight);
            } while (!arena_open_tile(tx, ty));
            put(p, tx, ty);

            const bool press = rng.below(3) != 0;  // ~2/3 held
            const bool rising = press && !prev_press;
            const bool should_place = rising && !tile_blocked(st, tx, ty) &&
                                      !tile_has_bomb(st, tx, ty) && p.bombs_placed < p.max_bombs;

            TickInputs in;
            in.players[0].action1 = press;
            s.tick(in);
            prev_press = press;

            const bool got = placed_this_tick(s, 0);
            ++checked;
            if (should_place && !got) {
                ++vanished;
                if (vanished <= 5)
                    std::printf("[G] VANISH seed=%u t=%d tile=(%d,%d) placed=%d limit=%d\n", seed, t,
                                tx, ty, p.bombs_placed, p.max_bombs);
            } else if (!should_place && got) {
                ++phantom;
                if (phantom <= 5)
                    std::printf("[G] PHANTOM seed=%u t=%d tile=(%d,%d)\n", seed, t, tx, ty);
            }
        }
    }
    std::printf("[G] checked=%lld vanished=%lld phantom=%lld\n", checked, vanished, phantom);
    CHECK(vanished == 0);
    CHECK(phantom == 0);
}

// --- H: walking fuzzer — drops adjacent to own bombs, at tile boundaries --
// The player actually WALKS (random directions) and presses action1 with random
// timing, never teleporting — so it drifts across tile centres and lays bombs
// right next to bombs it just placed. We assert the strong invariant only on
// ticks where the player stayed within one tile (post-move tile == pre-move
// tile), where the pre-tick gate cleanly predicts the outcome; movement ticks
// still run, building the adjacency/boundary states the assertion ticks probe.
TEST_CASE("H: placement never vanishes while walking among its own bombs") {
    long long checked = 0, vanished = 0;
    for (std::uint32_t seed = 1; seed <= 30; ++seed) {
        Simulation s(open_config());
        State& st = s.state();
        st.players[1].present = false;
        st.players[1].alive = false;
        Player& p = st.players[0];
        p.max_bombs = 8;
        p.flame = 1;
        p.grab = p.spooge = p.trigger = false;
        Rng32 rng(seed * 40503u + 7u);
        bool prev_press = false;

        for (int t = 0; t < 500; ++t) {
            // Keep the actor alive across its own blasts so the run doesn't stall
            // on death (we are fuzzing placement, not survival).
            if (!p.alive) {
                p.alive = true;
                p.bombs_placed = 0;
                put(p, 0, 0);
            }
            const int tx0 = p.tile_x(), ty0 = p.tile_y();
            const bool blocked_before = tile_blocked(st, tx0, ty0);
            const bool bomb_before = tile_has_bomb(st, tx0, ty0);
            const bool under_limit = p.bombs_placed < p.max_bombs;
            // "Not standing in flame" joined the gate 2026-07-11 (tick-order
            // audit, facts.md "Per-tick call order — END-TO-END" finding 1):
            // the fuzzer's artificial revive can resurrect the player INTO a
            // still-burning tile, and under the corrected order a moving
            // player dies at the first pixel step — before the drop block —
            // exactly like the original's turn-head flame check. That death
            // legitimately swallows the placement, so such ticks are no
            // longer "should place" ticks.
            const bool healthy = p.stun == 0 && p.bounce == 0 && p.warp == 0 && !p.carrying &&
                                 st.flame[ty0][tx0] == 0;

            const bool press = rng.below(3) != 0;
            const int mv = rng.below(6);  // 0..3 = walk a dir, 4/5 = stand still
            TickInputs in;
            in.players[0].action1 = press;
            if (mv == 0) in.players[0].up = true;
            else if (mv == 1) in.players[0].right = true;
            else if (mv == 2) in.players[0].down = true;
            else if (mv == 3) in.players[0].left = true;

            const bool rising = press && !prev_press;
            s.tick(in);
            prev_press = press;

            const int tx1 = p.tile_x(), ty1 = p.tile_y();
            const bool stayed = (tx1 == tx0 && ty1 == ty0);
            const bool got = placed_this_tick(s, 0);

            // Only assert on ticks where the tile did not change: then the
            // pre-tick gate is exactly the tile the drop targets.
            if (stayed && healthy) {
                const bool should = rising && !blocked_before && !bomb_before && under_limit;
                ++checked;
                if (should && !got) {
                    ++vanished;
                    if (vanished <= 5)
                        std::printf("[H] VANISH seed=%u t=%d tile=(%d,%d) placed=%d\n", seed, t, tx0,
                                    ty0, p.bombs_placed);
                }
            }
        }
    }
    std::printf("[H] checked=%lld vanished=%lld\n", checked, vanished);
    CHECK(vanished == 0);
}

// --- I: press held THROUGH a stun and into recovery ----------------------
// Suspect #1's specific window. A player holds action1 across a head-stun and
// keeps holding as it expires. This pins the current port's edge semantics at
// stun recovery deterministically (documents whatever the sim does — see the
// investigation notes for how it compares to the original's per-tick key-byte
// reset).
TEST_CASE("I: action1 behaviour across a stun boundary is deterministic") {
    Simulation s(open_config());
    State& st = s.state();
    st.players[1].present = false;
    st.players[1].alive = false;
    Player& p = st.players[0];
    p.max_bombs = 5;
    p.flame = 1;
    p.stun = 4;

    long long first_after_stun = -1;
    int stun_seen = 0;
    for (int t = 0; t < 12; ++t) {
        const bool was_stunned = p.stun > 0;
        put(p, 6, 6);            // hold a clean tile the whole time
        s.tick(press1(0));       // action1 HELD every tick, never released
        if (was_stunned) ++stun_seen;
        if (!was_stunned && placed_this_tick(s, 0) && first_after_stun < 0)
            first_after_stun = t;
    }
    std::printf("[I] stun_seen=%d first_place_after_stun_tick=%lld (held, never released)\n",
                stun_seen, first_after_stun);
    // With action1 HELD across the whole window, our edge-gated drop never sees
    // a rising edge (prev_action1 latches true during stun), so no drop fires —
    // recovery requires a release+re-press. Pin that as the CURRENT behaviour.
    CHECK(first_after_stun == -1);
}

// --- J: release+re-press right after a stun DOES place -------------------
// The recovery path that MUST work: once stun clears, a fresh press (after a
// release) always drops. Guards against a stun leaving a latched state that
// eats the first real post-stun press.
TEST_CASE("J: a fresh press after stun recovery always places") {
    for (int stun0 = 1; stun0 <= 6; ++stun0) {
        Simulation s(open_config());
        State& st = s.state();
        st.players[1].present = false;
        st.players[1].alive = false;
        Player& p = st.players[0];
        p.max_bombs = 5;
        p.flame = 1;
        p.stun = stun0;

        // Idle (released) until stun clears, then one clean release+press.
        for (int t = 0; t < stun0 + 1; ++t) {
            put(p, 6, 6);
            s.tick(TickInputs{});
        }
        CHECK(p.stun == 0);
        put(p, 6, 6);
        s.tick(press1(0));
        const bool placed = placed_this_tick(s, 0);
        std::printf("[J] stun0=%d post-stun fresh press placed=%d\n", stun0, placed ? 1 : 0);
        CHECK(placed);
    }
}

// --- K: press DURING a chain explosion frees-and-refills correctly -------
// Own bomb A chains own bomb B (deferred one tick). Across the whole explosion
// window the player, parked on a safe tile, releases+presses every other tick.
// Assert: whenever the pre-tick gate says a slot is free and the tile is clear,
// the press places — the slot-refill timing (suspect #3) never eats a drop.
TEST_CASE("K: pressing through a chain explosion never eats a legal drop") {
    Simulation s(open_config());
    State& st = s.state();
    st.players[1].present = false;
    st.players[1].alive = false;
    Player& p = st.players[0];
    p.max_bombs = 2;
    p.flame = 3;  // A at (0,0) reaches (2,0)

    put(p, 0, 0);
    s.tick(press1(0));  // A at (0,0)
    REQUIRE(p.bombs_placed == 1);
    Bomb b;
    b.active = true;
    b.id = st.next_bomb_id++;
    b.owner = 0;
    b.x = 2 * kTileWF + kTileWF / 2;
    b.y = 0 * kTileHF + kTileHF / 2;
    b.fuse_init = 999;
    b.fuse = 999;
    b.flame = 1;
    st.bombs.push_back(b);
    ++p.bombs_placed;  // B, chained only
    REQUIRE(p.bombs_placed == 2);

    // Park the owner on a safe far tile and drum action1 (release/press) while A
    // burns and chains B. The safe tile (6,8) is clear of both blasts.
    long long checked = 0, vanished = 0;
    bool prev_press = false;
    for (int t = 0; t < 60; ++t) {
        put(p, 6, 8);
        const int tx = p.tile_x(), ty = p.tile_y();
        const bool press = (t % 2 == 0);  // toggle: guarantees rising edges
        const bool rising = press && !prev_press;
        const bool should = rising && !tile_blocked(st, tx, ty) && !tile_has_bomb(st, tx, ty) &&
                            p.bombs_placed < p.max_bombs && p.stun == 0;
        TickInputs in;
        in.players[0].action1 = press;
        s.tick(in);
        prev_press = press;
        // Immediately step off any bomb we just laid so the next slot check is clean.
        put(p, 6, 8);
        const bool got = placed_this_tick(s, 0);
        ++checked;
        if (should && !got) {
            ++vanished;
            std::printf("[K] VANISH t=%d placed=%d bombs=%d\n", t, got, p.bombs_placed);
        }
    }
    std::printf("[K] checked=%lld vanished=%lld\n", checked, vanished);
    CHECK(vanished == 0);
}
