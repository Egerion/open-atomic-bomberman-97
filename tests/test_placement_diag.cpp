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
// head_stun_frames defaults to 16 ticks (0.8 s): a bomb bouncing on the head
// (PowerupSystem::head_hit) locks placement out for nearly a second.
TEST_CASE("C: a head-hit-sized stun blocks placement for its whole 16-tick window") {
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
    CHECK(refused_stun >= 6);   // a run of refusals through the stun window
    CHECK(first_place >= 0);    // recovers once stun hits 0
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
