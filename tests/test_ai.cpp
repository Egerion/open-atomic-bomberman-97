// Computer-player AI tests (ADR-0005 Stage 2). Covers the flee/danger-avoidance
// behaviour (an ai=true player next to a live bomb steps to safety and never
// onto flame), a deterministic-replay hash check with an AI player, and the
// "golden inert" guarantee: with NO ai players the AISystem draws zero RNG, so
// the rng stream is byte-identical to a run that never had the AI code.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>  // std::max in the Stage-5 pursuit tests

#include "bomber/sim/rng.hpp"
#include "bomber/sim/simulation.hpp"

using namespace bomber::sim;

namespace {

// Tile centre in the sim's 1/100-px units (tx*kTileWF + kTileWF/2).
Fixed cx(int tx) { return tx * kTileWF + kTileWF / 2; }
Fixed cy(int ty) { return ty * kTileHF + kTileHF / 2; }

// An open arena (all Blank) with a single alive player at (tx,ty). The empty
// Simulation() ctor does NOT run build_state, so the powerup layers default to
// PowerupType{} == ExtraBomb(0), not None(255) — mirror build_state and clear
// them to None, else every tile looks like it holds a powerup (blocking the AI
// and the danger rays). floor/hidden must be None on a bare arena.
Simulation open_arena(int tx, int ty, bool ai) {
    Simulation s;
    State& st = s.state();
    for (auto& row : st.cells) row.fill(Cell::Blank);
    for (auto& row : st.floor) row.fill(PowerupType::None);
    for (auto& row : st.hidden) row.fill(PowerupType::None);
    // The bare ctor also leaves ticks_left at its zero-init value. In a real
    // match that means "time's up" (sudden death: EnclosureSystem keeps the
    // wall spiral closing forever once armed — see docs/re/enclosure.md §2, the
    // original's remaining-seconds predicate clamps at 0 and never re-freezes).
    // These AI sandboxes were never meant to exercise the match clock at all,
    // so give them a real, generous countdown to keep the enclosure dormant.
    st.ticks_left = 9999 * kTicksPerSecond;
    Player& p = st.players[0];
    p.present = true;
    p.alive = true;
    p.ai = ai;
    p.x = cx(tx);
    p.y = cy(ty);
    p.speed = 923;      // start speed (id 42): ~9 px/tick, enough to cross a tile in ~4 ticks
    p.max_bombs = 1;
    p.flame = 2;
    return s;
}

// Drop a live, grounded bomb on (tx,ty) with the given flame reach and fuse.
void put_bomb(State& st, int tx, int ty, int flame, int fuse, std::uint8_t owner) {
    Bomb b;
    b.active = true;
    b.owner = owner;
    b.x = cx(tx);
    b.y = cy(ty);
    b.flame = flame;
    b.fuse = fuse;
    st.bombs.push_back(b);
}

int tile_x(const Player& p) { return static_cast<int>(p.x / kTileWF); }
int tile_y(const Player& p) { return static_cast<int>(p.y / kTileHF); }

// No-input tick set (everyone idle) — used to show a non-AI player stands still.
TickInputs idle() { return TickInputs{}; }

}  // namespace

TEST_CASE("Stage 2: an AI next to a live bomb flees to a safe tile and survives") {
    // Bomb at column 7, rows covered = 5..9 (flame 2 up/down/left/right). Put the
    // AI two tiles below the bomb, inside the blast column, with a short fuse so
    // the explosion arrives soon. The AI must step OFF column 7 (or far enough
    // along it) before the bomb goes off — and it must never walk onto a flaming
    // tile (the sub_40A76E veto).
    Simulation s = open_arena(/*tx=*/7, /*ty=*/9, /*ai=*/true);
    State& st = s.state();
    put_bomb(st, /*tx=*/7, /*ty=*/7, /*flame=*/2, /*fuse=*/24, /*owner=*/1);

    const int start_x = tile_x(st.players[0]), start_y = tile_y(st.players[0]);
    CHECK(start_x == 7);
    CHECK(start_y == 9);  // (7,9) is exactly the down-ray tip of the blast

    bool ever_left_blast_column = false;
    for (int t = 0; t < 24; ++t) {
        s.tick(idle());  // no external input; the AI fills its own
        // The AI must never occupy a flaming tile at the end of a tick.
        REQUIRE(st.flame[tile_y(st.players[0])][tile_x(st.players[0])] == 0);
        if (st.players[0].alive && tile_x(st.players[0]) != 7) ever_left_blast_column = true;
    }

    // The bomb has detonated by now (fuse 24 < 24 ticks + flame life). The AI
    // fled: it left the blast column at some point and is still alive.
    CHECK(ever_left_blast_column);
    CHECK(st.players[0].alive);

    // Control: the SAME setup with a non-AI idle player dies — proving the flee
    // was what saved the AI, not a benign board.
    Simulation dead = open_arena(7, 9, /*ai=*/false);
    put_bomb(dead.state(), 7, 7, 2, 24, 1);
    for (int t = 0; t < 40; ++t) dead.tick(idle());
    CHECK_FALSE(dead.state().players[0].alive);
}

TEST_CASE("Stage 2: the AI flees a blast ROW perpendicular and survives") {
    // Bomb directly west, one tile away, flame 2 → the AI stands at the east tip
    // of the blast row at (8,5). The only escape is perpendicular (row 4 or 6).
    // Because both perpendicular tiles are equally danger-free, the faithful ±1
    // BFS tie-break (sub_40970B, docs/re/ai.md §5.2) makes the AI wobble around
    // the tile centre for a few ticks before it strings enough same-direction
    // steps to cross into an off-row tile — exactly the original's flee wobble.
    // A long fuse gives that random walk time to resolve; the AI must leave
    // row 5, never touch flame, and be alive when the bomb finally goes off.
    Simulation s = open_arena(/*tx=*/8, /*ty=*/5, /*ai=*/true);
    put_bomb(s.state(), /*tx=*/6, /*ty=*/5, /*flame=*/2, /*fuse=*/60, /*owner=*/1);
    State& st = s.state();

    bool ever_left_row = false;
    for (int t = 0; t < 60; ++t) {
        s.tick(idle());
        REQUIRE(st.flame[tile_y(st.players[0])][tile_x(st.players[0])] == 0);
        if (st.players[0].alive && tile_y(st.players[0]) != 5) ever_left_row = true;
    }
    CHECK(ever_left_row);            // it fled the blast row
    CHECK(st.players[0].alive);      // and outran the fuse
}

TEST_CASE("Stage 2: deterministic replay with an AI player (same seed+inputs)") {
    // Two identical AI-bearing sims fed the same seed and inputs must produce
    // identical hashes and identical rng streams throughout (ADR-0003). The AI's
    // draws are part of the deterministic stream, so this exercises them.
    auto build = [] {
        Simulation s = open_arena(3, 3, /*ai=*/true);
        s.state().rng = 0xC0FFEEu;
        // A second AI player and a couple of bombs to make the AI actually decide
        // (flee some ticks, wander others) — more of the code path per tick.
        Player& q = s.state().players[1];
        q.present = true;
        q.alive = true;
        q.ai = true;
        q.x = cx(11);
        q.y = cy(7);
        q.speed = 923;
        q.max_bombs = 1;
        q.flame = 2;
        put_bomb(s.state(), 4, 3, 2, 60, 5);
        put_bomb(s.state(), 10, 7, 2, 70, 6);
        return s;
    };

    Simulation a = build(), b = build();
    for (int t = 0; t < 400; ++t) {
        TickInputs in = idle();
        a.tick(in);
        b.tick(in);
        if (t % 5 == 0) REQUIRE(a.hash() == b.hash());
        if (t % 3 == 0) REQUIRE(next_random(a.state()) == next_random(b.state()));
    }
    CHECK(a.hash() == b.hash());
    CHECK(a.state().rng == b.state().rng);
}

TEST_CASE("Stage 2: golden inert — no AI players draw zero AI RNG") {
    // The core golden guarantee (ADR-0005 §7): with NO ai player the AISystem
    // never runs, so it adds ZERO rng draws. We prove it by running the exact
    // same scenario twice — once through the normal path — and confirming the
    // rng advances ONLY by the non-AI mechanics. The tell: a run with all
    // players ai=false must leave rng identical to a hand-rolled reference that
    // performs the same non-AI draws. Simplest robust form: an all-idle board
    // with no bombs and no AI does NOT touch rng at all across many ticks
    // (nothing in the tick draws RNG for a static, AI-free board).
    Simulation s = open_arena(5, 5, /*ai=*/false);
    // A second, third idle human — still no AI, still no bombs.
    for (int i = 1; i < 3; ++i) {
        Player& p = s.state().players[i];
        p.present = true;
        p.alive = true;
        p.ai = false;
        p.x = cx(2 + i);
        p.y = cy(2);
        p.speed = 923;
    }
    const std::uint32_t rng0 = s.state().rng;
    for (int t = 0; t < 500; ++t) s.tick(idle());
    // No AI, no bombs, no diseases, no hurry ⇒ nothing draws RNG ⇒ stream frozen.
    CHECK(s.state().rng == rng0);

    // And flipping the SAME board to AI *does* consume RNG (the A/B scratch draws
    // fire every tick per present-alive AI player), so the two paths are cleanly
    // separated: AI-off is inert, AI-on is not.
    Simulation ai = open_arena(5, 5, /*ai=*/true);
    const std::uint32_t rng_ai0 = ai.state().rng;
    ai.tick(idle());
    CHECK(ai.state().rng != rng_ai0);  // draws A and B advanced the stream
}

// ---------------------------------------------------------------------------
// Stage 3 (ADR-0005 §8): directed pathfinding to targets + seek-a-powerup
// (behaviour 5, sub_40BAF5). The AI walks toward a chosen floor powerup and the
// normal Powerups pickup (field_vs_players) collects it when its centre tile
// reaches the powerup. No bomb/danger on the board, so behaviour 2 passes down
// and behaviour 5 drives movement.
// ---------------------------------------------------------------------------

TEST_CASE("Stage 3: a seeking AI walks to a nearby powerup and collects it") {
    // Directly latch the powerup-seek target (bypass the 1/50 acquire whim) so
    // the DIRECTED path-and-collect logic is exercised deterministically. Powerup
    // three tiles east of the AI, on an otherwise empty, danger-free board.
    Simulation s = open_arena(/*tx=*/3, /*ty=*/5, /*ai=*/true);
    State& st = s.state();
    const int pux = 6, puy = 5;
    st.floor[puy][pux] = PowerupType::ExtraBomb;  // a plain powerup (no RNG on pickup)

    const int bombs0 = st.players[0].max_bombs;

    // Latch behaviour 5 onto the powerup tile. The timer resets each acquire in
    // the real code; here we pin it so the ~10-tick timeout does not fire before
    // the AI arrives. (We re-arm it every few ticks to model a fresh whim, which
    // is exactly what the acquire branch would do if it kept re-triggering.)
    auto latch = [&] {
        Brain& br = st.brains[0];
        br.pow_seek.active = true;
        br.pow_seek.timer = 0;
        br.pow_seek.tile_x = static_cast<std::int16_t>(pux);
        br.pow_seek.tile_y = static_cast<std::int16_t>(puy);
    };
    latch();

    bool collected = false;
    for (int t = 0; t < 40 && !collected; ++t) {
        // Keep the pursuit alive across the ~10-tick timeout so a slow crossing
        // still reaches the goal (models continuous re-acquisition of the same
        // in-range powerup — the original re-rolls the whim every tick).
        if (!st.brains[0].pow_seek.active) latch();
        s.tick(idle());
        if (st.floor[puy][pux] == PowerupType::None) collected = true;
    }

    CHECK(collected);                              // the powerup is gone -> picked up
    CHECK(st.players[0].max_bombs == bombs0 + 1);  // ExtraBomb applied (it reached the tile)
    CHECK(st.players[0].alive);
}

TEST_CASE("Stage 3: seek-powerup acquires on its own and collects (emergent)") {
    // No hand-latching: a lone AI on a safe board with a reachable powerup within
    // getvalue(920)=4 tiles must, over enough ticks, hit the 1/50 acquire whim,
    // path to the powerup and collect it (or wander onto it — sub_40A59D now
    // permits stepping onto a powerup tile, the strict-1:1 predicate). Either way
    // the powerup ends up collected. A fixed seed makes this deterministic.
    Simulation s = open_arena(/*tx=*/5, /*ty=*/5, /*ai=*/true);
    State& st = s.state();
    // Seed refreshed for Stage 5: behaviour 6 (seek-enemy) now draws its 1/50
    // acquire roll every tick the chain reaches it, so the lone AI's per-tick RNG
    // stream shifted vs the Stage-3 stub. This is an EMERGENT (RNG-timed) AI test,
    // not golden — only AI-player draws moved (ADR-0005 §7), so a fresh seed that
    // still collects within the window is the correct update. The mechanic (whim-
    // acquire OR wander onto the powerup) is unchanged.
    st.rng = 0xDEADBEEFu;
    const int pux = 7, puy = 5;  // two tiles east, well within range 4
    st.floor[puy][pux] = PowerupType::Flame;

    bool collected = false;
    for (int t = 0; t < 600 && !collected; ++t) {
        s.tick(idle());
        if (st.floor[puy][pux] == PowerupType::None) collected = true;
    }
    CHECK(collected);
    CHECK(st.players[0].alive);
}

TEST_CASE("Stage 3: deterministic replay with a seeking AI (same seed+inputs)") {
    // Two identical AI-bearing sims with a powerup to chase must produce identical
    // hashes and rng streams throughout — the directed BFS + powerup-scan draws
    // are part of the deterministic stream (one ±1 tie-break per BFS call).
    auto build = [] {
        Simulation s = open_arena(4, 5, /*ai=*/true);
        s.state().rng = 0x5EED1234u;
        s.state().floor[5][8] = PowerupType::Kick;     // a powerup east
        s.state().floor[3][4] = PowerupType::Skate;    // another north
        // A second AI too, so both the seek and the flee/wander paths run.
        Player& q = s.state().players[1];
        q.present = true;
        q.alive = true;
        q.ai = true;
        q.x = cx(10);
        q.y = cy(7);
        q.speed = 923;
        q.max_bombs = 1;
        q.flame = 2;
        return s;
    };

    Simulation a = build(), b = build();
    for (int t = 0; t < 400; ++t) {
        TickInputs in = idle();
        a.tick(in);
        b.tick(in);
        if (t % 5 == 0) REQUIRE(a.hash() == b.hash());
        if (t % 3 == 0) REQUIRE(next_random(a.state()) == next_random(b.state()));
    }
    CHECK(a.hash() == b.hash());
    CHECK(a.state().rng == b.state().rng);
}

// ---------------------------------------------------------------------------
// Stage 4 (ADR-0005 §8): behaviour 3 blast-bricks (sub_40AD8D) + behaviour 0
// grab-glove (sub_40BD44). The AI drops a bomb to break an adjacent brick (gated
// on the 1-in-getvalue(915)=5 whim) and then flees its own blast (behaviour 2,
// higher priority). No new hashed brain field (state_flag was hashed in Stage 2),
// so golden is unaffected.
// ---------------------------------------------------------------------------

TEST_CASE("Stage 4: an AI beside a brick drops a bomb and flees its own blast") {
    // A brick directly WEST of the AI; open floor to the EAST to flee into. The
    // board is otherwise safe, so behaviour 2 passes down and behaviour 3 fires
    // the 1-in-5 drop. Once the bomb is placed the danger grid lights up and
    // behaviour 2 paths the AI away, out of the blast. It must survive.
    Simulation s = open_arena(/*tx=*/6, /*ty=*/5, /*ai=*/true);
    State& st = s.state();
    st.rng = 0x13572468u;               // fixed seed -> the 1-in-5 gate fires deterministically
    st.cells[5][5] = Cell::Brick;       // brick to the west (the AI's tile is (6,5))
    st.players[0].max_bombs = 1;
    st.players[0].flame = 2;

    bool dropped = false;
    bool survived_to_blast = true;
    for (int t = 0; t < 120; ++t) {
        s.tick(idle());
        if (!st.bombs.empty()) dropped = true;
        // The AI must never end a tick standing on flame (behaviour 2 + the veto).
        if (st.players[0].alive &&
            st.flame[tile_y(st.players[0])][tile_x(st.players[0])] > 0)
            survived_to_blast = false;
    }

    CHECK(dropped);                  // the blast-bricks behaviour placed a bomb
    CHECK(survived_to_blast);        // it never sat in flame
    CHECK(st.players[0].alive);      // and it fled its own bomb in time
    // The brick was broken by the AI's bomb (it burned away and cleared).
    CHECK(st.cells[5][5] != Cell::Brick);
}

TEST_CASE("Stage 4: no adjacent brick -> the AI never drops a blast-bricks bomb") {
    // Same seed, but NO brick anywhere near the AI. Behaviour 3's brick count is 0,
    // so it never rolls the 1-in-5 and never drops. (The AI just wanders on the
    // safe board.) Proves the drop is gated on an adjacent brick, not free.
    Simulation s = open_arena(/*tx=*/6, /*ty=*/5, /*ai=*/true);
    State& st = s.state();
    st.rng = 0x13572468u;
    for (int t = 0; t < 200; ++t) s.tick(idle());
    CHECK(st.bombs.empty());         // no brick adjacent -> no blast-bricks drop
    CHECK(st.players[0].alive);
}

TEST_CASE("Stage 4: a bomb already in the AI's column suppresses the blast drop") {
    // The column guard (sub_4245DA): if a live bomb already sits in the AI's
    // column, behaviour 3 bails before the brick count / 1-in-5 roll. Confine the
    // AI to a 1-wide vertical corridor in column 6 (solids on both sides) with a
    // brick capping the corridor directly SOUTH of it, and a foreign bomb up the
    // SAME column. The AI can only ever stand in column 6 (walled in), so the
    // column guard ALWAYS fires and it never adds a bomb of its own — regardless
    // of how it wanders up/down the corridor. This removes any way for the AI to
    // reach a brick-adjacent tile in a different (empty) column, so the test is
    // robust for any seed.
    Simulation s = open_arena(/*tx=*/6, /*ty=*/7, /*ai=*/true);
    State& st = s.state();
    st.rng = 0x13572468u;
    // Solid walls flanking column 6 for rows 2..8 -> a vertical corridor.
    for (int y = 2; y <= 8; ++y) {
        st.cells[y][5] = Cell::Solid;
        st.cells[y][7] = Cell::Solid;
    }
    st.cells[8][6] = Cell::Brick;   // brick capping the corridor just south of the AI
    st.cells[1][6] = Cell::Solid;   // cap the top so the AI stays in rows 2..7
    // A long-fuse foreign bomb up column 6 (row 2), flame 1 -> its danger (rows
    // 1..3) is up-corridor; the AI starts at row 7, out of that blast. The AI may
    // flee within the corridor but can never leave column 6.
    put_bomb(st, /*tx=*/6, /*ty=*/2, /*flame=*/1, /*fuse=*/1000000, /*owner=*/3);
    REQUIRE(st.bombs.size() == 1);

    for (int t = 0; t < 80; ++t) {
        s.tick(idle());
        REQUIRE(tile_x(st.players[0]) == 6);  // walled in: always column 6
    }
    // The AI never dropped: its column always held the foreign bomb, so behaviour
    // 3 bailed on the column guard every eligible tick.
    CHECK(st.bombs.size() == 1);
    CHECK(st.players[0].alive);
}

TEST_CASE("Stage 4: a grab-AI on its own bomb grabs it (behaviour 0)") {
    // Behaviour 0 (sub_40BD44): a player holding Grab, standing on its OWN resting
    // bomb, presses the bomb key on a 1/2 whim -> the mover's try_grab picks the
    // bomb up (Player::carrying). Seed the AI with grab and its own grounded bomb
    // underfoot on a safe board; over enough ticks the coin-flip fires and the AI
    // is carrying. A fixed seed makes it deterministic.
    Simulation s = open_arena(/*tx=*/6, /*ty=*/5, /*ai=*/true);
    State& st = s.state();
    st.rng = 0x0BADF00Du;
    Player& p = st.players[0];
    p.grab = true;
    p.max_bombs = 2;
    p.bombs_placed = 1;      // the seeded bomb below counts against the owner's slots
    // The AI's OWN resting bomb on its tile (owner 0 == the AI slot). Long fuse so
    // it does not blow up before the grab fires; safe board so behaviour 2 passes.
    put_bomb(st, /*tx=*/6, /*ty=*/5, /*flame=*/1, /*fuse=*/1000000, /*owner=*/0);

    bool grabbed = false;
    for (int t = 0; t < 60 && !grabbed; ++t) {
        s.tick(idle());
        if (st.players[0].carrying) grabbed = true;
    }
    CHECK(grabbed);                  // the grab-glove coin flip fired -> bomb carried
    CHECK(st.players[0].alive);
}

TEST_CASE("Stage 4: deterministic replay with a bombing (blast-bricks) AI") {
    // Two identical AI-bearing sims with bricks to blast must produce identical
    // hashes and rng streams throughout — behaviour 3's rand()%5 gate and the
    // resulting bomb-drop / flee draws are all part of the deterministic stream.
    auto build = [] {
        Simulation s = open_arena(6, 5, /*ai=*/true);
        State& st = s.state();
        st.rng = 0xBEEF7777u;
        st.cells[5][5] = Cell::Brick;   // brick west of AI #0
        st.cells[7][7] = Cell::Brick;   // brick west of AI #1 (below)
        Player& q = st.players[1];
        q.present = true;
        q.alive = true;
        q.ai = true;
        q.x = cx(8);
        q.y = cy(7);
        q.speed = 923;
        q.max_bombs = 1;
        q.flame = 2;
        return s;
    };

    Simulation a = build(), b = build();
    for (int t = 0; t < 300; ++t) {
        TickInputs in = idle();
        a.tick(in);
        b.tick(in);
        if (t % 5 == 0) REQUIRE(a.hash() == b.hash());
        if (t % 3 == 0) REQUIRE(next_random(a.state()) == next_random(b.state()));
    }
    CHECK(a.hash() == b.hash());
    CHECK(a.state().rng == b.state().rng);
}

// ---------------------------------------------------------------------------
// Stage 5 (ADR-0005 §8, FINAL): behaviour 1 punch (sub_40BE02), behaviour 4
// bomb-near-enemy (sub_40ABED), behaviour 6 seek-enemy (sub_40B8C2 + enemy
// finder sub_422718), and the safe-branch remote-detonation whim. All 8
// behaviours now live. TEAM reduces to slot != self in a no-team match (no
// Player::team field) -> NO new hashed field -> golden FROZEN.
// ---------------------------------------------------------------------------

// Add a live player (human by default) at (tx,ty). Returns the slot.
Player& add_player(State& st, int slot, int tx, int ty, bool ai) {
    Player& q = st.players[slot];
    q.present = true;
    q.alive = true;
    q.ai = ai;
    q.x = cx(tx);
    q.y = cy(ty);
    q.speed = 923;
    q.max_bombs = 1;
    q.flame = 2;
    return q;
}

TEST_CASE("Stage 5: a punch-glove AI punches a bomb sitting directly ahead") {
    // Behaviour 1 (sub_40BE02): the AI holds the punch glove and a bomb sits on an
    // orthogonally-adjacent tile. On the 1-in-4 whim it faces the bomb and swings;
    // try_punch launches the bomb 3 tiles away (it starts flying and leaves the
    // adjacent tile). Box the AI in on three sides so it cannot walk off, leaving
    // only the bomb tile "ahead" — over enough ticks the 1-in-4 fires and the bomb
    // is launched. Fixed seed makes the gate deterministic.
    Simulation s = open_arena(/*tx=*/6, /*ty=*/5, /*ai=*/true);
    State& st = s.state();
    st.rng = 0x51234567u;
    Player& p = st.players[0];
    p.punch = true;
    // A foreign resting bomb directly EAST of the AI (owner 3, long fuse so it does
    // not detonate before the punch). This is the "bomb ahead" once the AI faces E.
    put_bomb(st, /*tx=*/7, /*ty=*/5, /*flame=*/1, /*fuse=*/1000000, /*owner=*/3);
    // Wall the AI on N/S/W so its only non-wall neighbour is the bomb tile to the
    // E; it can never walk away, so it keeps facing the bomb and eventually punches.
    st.cells[4][6] = Cell::Solid;  // north
    st.cells[6][6] = Cell::Solid;  // south
    st.cells[5][5] = Cell::Solid;  // west
    REQUIRE(st.bombs.size() == 1);
    REQUIRE_FALSE(st.bombs[0].flying);

    bool launched = false;
    for (int t = 0; t < 80 && !launched; ++t) {
        s.tick(idle());
        // The punch sets the bomb flying (BombSystem::launch): once airborne it has
        // left the (7,5) tile — the tell that behaviour 1 swung and connected.
        for (const auto& b : st.bombs)
            if (b.flying) launched = true;
    }
    CHECK(launched);                 // the AI punched the adjacent bomb
    CHECK(st.players[0].alive);
}

TEST_CASE("Stage 5: an AI next to an enemy drops a bomb at it, then flees") {
    // Behaviour 4 (sub_40ABED): a live enemy sits on the AI's cross; with the
    // column clear, the standing tile clear, and abs(tileX)+abs(tileY) >= 3, the
    // AI drops a bomb on the 1-in-5 whim. Then the new bomb lights the danger grid
    // and behaviour 2 (higher priority) flees the AI out of its own blast. Keep the
    // pair in a small 3x3 room so the wandering AI stays within cross range of the
    // (idle, non-AI) enemy until the gate fires. Fixed seed -> deterministic.
    Simulation s = open_arena(/*tx=*/6, /*ty=*/6, /*ai=*/true);
    State& st = s.state();
    st.rng = 0x24681357u;
    // A 3x3 open room (rows 5..7, cols 5..7) walled off, so both players stay put.
    for (int y = 4; y <= 8; ++y)
        for (int x = 4; x <= 8; ++x)
            if (y == 4 || y == 8 || x == 4 || x == 8) st.cells[y][x] = Cell::Solid;
    // An idle HUMAN enemy in the same room (slot 1), adjacent to the AI's start.
    add_player(st, /*slot=*/1, /*tx=*/6, /*ty=*/5, /*ai=*/false);

    bool dropped = false;
    bool never_on_flame = true;
    for (int t = 0; t < 200; ++t) {
        s.tick(idle());  // slot-1 enemy gets idle input -> stands still
        if (!st.bombs.empty()) dropped = true;
        if (st.players[0].alive &&
            st.flame[tile_y(st.players[0])][tile_x(st.players[0])] > 0)
            never_on_flame = false;
    }
    CHECK(dropped);                  // behaviour 4 bombed the adjacent enemy
    CHECK(never_on_flame);           // behaviour 2 kept the AI out of its own blast
    CHECK(st.players[0].alive);      // and it survived (fled in time)
}

TEST_CASE("Stage 5: no enemy nearby -> the AI never drops a bomb-near-enemy bomb") {
    // Same room + seed, but the enemy is REMOVED. Behaviour 4's cross scan finds
    // no live enemy, so it never rolls the 1-in-5 and never drops (and there is no
    // brick, so behaviour 3 is silent too). Proves the drop is gated on an enemy.
    Simulation s = open_arena(/*tx=*/6, /*ty=*/6, /*ai=*/true);
    State& st = s.state();
    st.rng = 0x24681357u;
    for (int y = 4; y <= 8; ++y)
        for (int x = 4; x <= 8; ++x)
            if (y == 4 || y == 8 || x == 4 || x == 8) st.cells[y][x] = Cell::Solid;
    // No second player at all.
    for (int t = 0; t < 200; ++t) s.tick(idle());
    CHECK(st.bombs.empty());         // no enemy on the cross -> no bomb-near-enemy drop
    CHECK(st.players[0].alive);
}

TEST_CASE("Stage 5: a seek-enemy AI paths toward a distant live enemy") {
    // Behaviour 6 (sub_40B8C2): directly latch the enemy-seek target (bypass the
    // 1/50 acquire whim) so the DIRECTED path-toward-a-foe logic is exercised
    // deterministically. Enemy far to the east on an open, danger-free board; the
    // AI must close the gap (its tile-X strictly increases toward the enemy).
    Simulation s = open_arena(/*tx=*/3, /*ty=*/5, /*ai=*/true);
    State& st = s.state();
    add_player(st, /*slot=*/1, /*tx=*/11, /*ty=*/5, /*ai=*/false);  // distant enemy, idle

    const int start_x = tile_x(st.players[0]);
    // Latch behaviour 6 onto slot 1, re-arming across the ~10-tick timeout so the
    // pursuit persists while the AI crosses (models continuous re-acquisition).
    auto latch = [&] {
        Brain& br = st.brains[0];
        br.enemy_seek.active = true;
        br.enemy_seek.timer = 0;
        br.enemy_seek.target_slot = 1;
    };
    latch();

    int max_x = start_x;
    for (int t = 0; t < 60; ++t) {
        if (!st.brains[0].enemy_seek.active) latch();
        s.tick(idle());  // the enemy stays put (idle input)
        if (st.players[0].alive) max_x = std::max(max_x, tile_x(st.players[0]));
    }
    CHECK(max_x > start_x + 3);       // the AI advanced several tiles toward the foe
    CHECK(st.players[0].alive);
}

TEST_CASE("Stage 5: seek-enemy acquires on its own and closes on the foe (emergent)") {
    // No hand-latching: a lone AI and a reachable live enemy on a safe board. Over
    // enough ticks the AI must hit the 1/50 acquire whim (sub_40B8C2 -> the enemy
    // finder sub_422718 picks the only opponent) and path toward it. A fixed seed
    // makes this deterministic. The AI closes at least a couple of tiles.
    Simulation s = open_arena(/*tx=*/2, /*ty=*/5, /*ai=*/true);
    State& st = s.state();
    st.rng = 0x0FACE123u;
    add_player(st, /*slot=*/1, /*tx=*/10, /*ty=*/5, /*ai=*/false);  // the only opponent, idle

    const int start_x = tile_x(st.players[0]);
    int max_x = start_x;
    for (int t = 0; t < 800; ++t) {
        s.tick(idle());
        if (st.players[0].alive) max_x = std::max(max_x, tile_x(st.players[0]));
    }
    CHECK(max_x > start_x + 1);       // it acquired and moved toward the foe at least once
    CHECK(st.players[0].alive);
}

TEST_CASE("Stage 5: deterministic replay with two fully-live AIs fighting") {
    // Two identical sims each with TWO ai=true players and bombs in play must
    // produce identical hashes and rng streams throughout. This exercises the full
    // 8-behaviour chain (punch/bomb-enemy/seek-enemy included) under lockstep: the
    // Stage-5 draws (enemy finder rand()%10, %5 enemy-bomb, %4 punch, %50 acquire/
    // timeout, %2 give-up, %10 trigger whim) are all part of the deterministic
    // stream. Both AIs hold every glove so all behaviours are reachable.
    auto build = [] {
        Simulation s = open_arena(4, 5, /*ai=*/true);
        State& st = s.state();
        st.rng = 0xA11CE5EEu;
        Player& p0 = st.players[0];
        p0.punch = true;
        p0.grab = true;
        p0.trigger = true;
        p0.max_bombs = 2;
        p0.flame = 3;
        Player& q = add_player(st, /*slot=*/1, /*tx=*/9, /*ty=*/6, /*ai=*/true);
        q.punch = true;
        q.grab = true;
        q.trigger = true;
        q.max_bombs = 2;
        q.flame = 3;
        // A brick each so behaviour 3 can also fire, and a couple of live bombs so
        // the danger grid + flee run early.
        st.cells[5][5] = Cell::Brick;
        st.cells[6][8] = Cell::Brick;
        put_bomb(st, 4, 8, 2, 80, 4);
        put_bomb(st, 11, 6, 2, 90, 5);
        return s;
    };

    Simulation a = build(), b = build();
    for (int t = 0; t < 500; ++t) {
        TickInputs in = idle();
        a.tick(in);
        b.tick(in);
        if (t % 5 == 0) REQUIRE(a.hash() == b.hash());
        if (t % 3 == 0) REQUIRE(next_random(a.state()) == next_random(b.state()));
    }
    CHECK(a.hash() == b.hash());
    CHECK(a.state().rng == b.state().rng);
}
