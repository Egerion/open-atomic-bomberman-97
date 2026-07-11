// Player state machine (+78) parity — pins the transitions and, above all,
// the FORBIDDEN combinations of docs/re/facts.md "Player state machine (+78)
// — COMPLETE" (2026-07-11 audit). The original holds each player's mode in
// ONE word (0 normal, 1 kick anim, 2 punch anim, 3 head-stun, 4 pickup-pause,
// 5 trampoline hop, 6/7 warp out/in, 20-39 cornerhead), so combinations like
// "carrying while warping" or "bouncing while head-stunned" are structurally
// unrepresentable there; our multi-flag port must forbid them by explicit
// transition guards, which these cases pin.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "helpers.hpp"

using namespace bomber::sim;
using namespace bomber::sim::test;

namespace {

Fixed centre_x(int tx) { return tx * kTileWF + kTileWF / 2; }
Fixed centre_y(int ty) { return ty * kTileHF + kTileHF / 2; }

// Link a warphole pair the way apply_actors would (no RNG at trigger time).
void add_warp_pair(State& st, int ax, int ay, int bx, int by) {
    st.actor_type[ay][ax] = ActorType::Warphole;
    st.warp_dest_x[ay][ax] = static_cast<std::uint8_t>(bx);
    st.warp_dest_y[ay][ax] = static_cast<std::uint8_t>(by);
    st.actor_type[by][bx] = ActorType::Warphole;
    st.warp_dest_x[by][bx] = static_cast<std::uint8_t>(ax);
    st.warp_dest_y[by][bx] = static_cast<std::uint8_t>(ay);
}

// Queue a flying bomb one tick from landing on (tx,ty) — the sub_42331C
// flight-landing path that dispatches the head hit (sub_421F7E).
void land_bomb_on(State& st, int tx, int ty) {
    Bomb b;
    b.active = true;
    b.owner = 1;
    b.fuse = 100000;  // never detonates during these tests
    b.flying = true;
    b.from_x = centre_x(tx);
    b.from_y = centre_y(ty + 4);
    b.x = b.from_x;
    b.y = b.from_y;
    b.to_x = centre_x(tx);
    b.to_y = centre_y(ty);
    b.dir = Direction::Up;
    b.fly_total = 1;
    b.fly_ticks = 1;
    st.bombs.push_back(b);
}

}  // namespace

// ---- stun (+58) vs pickup-pause (state 4) are independent counters --------

TEST_CASE("a grab starts pickup_pause, not the head-hit stun counter") {
    // sub_424AF4 (the grab-attach primitive, pseudo.c 26018-26025) never
    // touches the +58 word — the pause is state 4's own +80-vs-getvalue(665)
    // window (sub_41F29B ~23017-23025). sub_421F7E (head hit) is the ONLY
    // +58 writer.
    Simulation s(open_config());
    Player& p = s.state().players[0];
    p.grab = true;
    s.tick(press1(0));     // drop own bomb underfoot
    s.tick(TickInputs{});  // release for a fresh edge
    s.tick(press1(0));     // grab it
    REQUIRE(p.carrying);
    CHECK(p.pickup_pause == s.state().tuning.pickup_pause);
    CHECK(p.stun == 0);
}

TEST_CASE("pickup_pause blocks new input like a stun, then releases") {
    Simulation s(open_config());
    Player& p = s.state().players[0];
    p.pickup_pause = 3;  // white-box: mid grab-pause
    const Fixed x0 = p.x, y0 = p.y;

    TickInputs down;
    down.players[0].down = true;
    run(s, 3, down);
    CHECK(p.x == x0);  // input blocked for the whole pause
    CHECK(p.y == y0);
    CHECK(p.pickup_pause == 0);  // the countdown ran

    run(s, 4, down);
    CHECK(p.y > y0);  // input resumed
}

// ---- forbidden combo: carrying && (bounce || warp) -------------------------

TEST_CASE("entering a warp releases a carried bomb (no carrying-through-a-warp)") {
    // LABEL_246's release check `if (+37) { if (v112 || !+56) throw }` runs in
    // ALL +78 states (state 5 goto at pseudo.c 23198; 6/7 fall through
    // LABEL_239 into 23277), and input-blocked states leave +56 at its
    // per-tick 0 reset — so the original throws the carried bomb on the first
    // flight tick. facts.md "Player state machine (+78)" port-parity fix 2.
    Simulation s(open_config());
    State& st = s.state();
    add_warp_pair(st, 4, 0, 8, 0);

    Player& p = st.players[0];
    p.grab = true;
    s.tick(press1(0));     // drop own bomb at (0,0)
    s.tick(TickInputs{});
    s.tick(press1(0));     // grab it
    REQUIRE(p.carrying);
    run(s, s.state().tuning.pickup_pause + 1, press1(0));  // hold through pause

    // Walk east onto the warphole at (4,0), holding the key so the carried
    // bomb is not released by the ordinary key-up path first.
    TickInputs east_hold;
    east_hold.players[0].right = true;
    east_hold.players[0].action1 = true;
    int guard = 0;
    while (p.warp == 0 && ++guard < 400) s.tick(east_hold);
    REQUIRE(p.warp > 0);  // reached and triggered the warphole

    // The bomb must be released (thrown) by the first blocked warp tick,
    // never held through the flight.
    s.tick(east_hold);
    CHECK(!p.carrying);
    REQUIRE(!st.bombs.empty());
}

TEST_CASE("entering a trampoline hop releases a carried bomb") {
    Simulation s(open_config());
    State& st = s.state();
    st.actor_type[0][4] = ActorType::Trampoline;

    Player& p = st.players[0];
    p.grab = true;
    s.tick(press1(0));
    s.tick(TickInputs{});
    s.tick(press1(0));
    REQUIRE(p.carrying);
    run(s, s.state().tuning.pickup_pause + 1, press1(0));

    TickInputs east_hold;
    east_hold.players[0].right = true;
    east_hold.players[0].action1 = true;
    int guard = 0;
    while (p.bounce == 0 && ++guard < 400) s.tick(east_hold);
    REQUIRE(p.bounce > 0);

    s.tick(east_hold);
    CHECK(!p.carrying);
    REQUIRE(!st.bombs.empty());
}

// ---- forbidden combo: (bounce || warp || pickup-pause) && head-stun --------

TEST_CASE("a head hit cancels an in-flight bounce, warp and pickup-pause") {
    // sub_421F7E writes `a1[39] = 3; a1[40] = 0` UNCONDITIONALLY and its
    // victim probe sub_421CB5 (pseudo.c 24207) has no +78 guard — the single
    // state word means a head hit CLOBBERS states 4/5/6/7. facts.md
    // port-parity fix 3.
    Simulation s(open_config());
    State& st = s.state();
    Player& p = st.players[0];

    SUBCASE("bounce cancelled in place") {
        p.bounce = 10;  // mid-hop, away from the len/2 apex (no relocation RNG)
        land_bomb_on(st, 0, 0);
        s.tick(TickInputs{});
        CHECK(p.stun > 0);     // head-hit landed
        CHECK(p.bounce == 0);  // flight cancelled
    }

    SUBCASE("warp cancelled without relocating (hit during warp-out)") {
        add_warp_pair(st, 0, 0, 8, 0);
        p.x = centre_x(0);
        p.y = centre_y(0);
        s.tick(TickInputs{});  // step-on: warp starts (warp-out, still at entry)
        REQUIRE(p.warp > 0);
        REQUIRE(p.tile_x() == 0);

        land_bomb_on(st, 0, 0);
        s.tick(TickInputs{});
        CHECK(p.stun > 0);
        CHECK(p.warp == 0);      // warp clobbered by state 3
        CHECK(p.tile_x() == 0);  // never relocated: the +28/+32 <- +20/+24
                                 // write lives inside the state-6 branch
    }

    SUBCASE("pickup-pause cancelled") {
        p.pickup_pause = 2;
        land_bomb_on(st, 0, 0);
        s.tick(TickInputs{});
        CHECK(p.stun > 0);
        CHECK(p.pickup_pause == 0);  // state 4 -> 3: the pause is gone
    }
}

// ---- states 5/6/7 are death-exempt (sub_41DE63 21999-22006) ----------------

TEST_CASE("flame does not kill a bouncing or warping player") {
    Simulation s(open_config());
    State& st = s.state();
    Player& p = st.players[0];

    SUBCASE("bouncing") {
        p.bounce = 10;
        st.flame[p.tile_y()][p.tile_x()] = 200;
        s.tick(TickInputs{});
        CHECK(p.alive);  // sub_41DE63 early-out for +78 == 5
    }
    SUBCASE("warping") {
        p.warp = 10;
        st.flame[p.tile_y()][p.tile_x()] = 200;
        s.tick(TickInputs{});
        CHECK(p.alive);  // sub_41DE63 early-out for +78 == 6/7
    }
    SUBCASE("control: a normal player on the same flame dies") {
        st.flame[p.tile_y()][p.tile_x()] = 200;
        s.tick(TickInputs{});
        CHECK(!p.alive);
    }
}

// ---- landing head-hit runs BEFORE the warphole probe ------------------------

TEST_CASE("a bomb landing on a warphole tile still head-hits the player standing there") {
    // pseudo.c 25443-25452: the victim scan (sub_421CB5 -> sub_421F7E) is the
    // FIRST statement inside the clear-tile verdict; the warphole probe only
    // runs in the victim-less else. Our old port folded the warphole into the
    // blocked verdict, skipping the head-hit entirely. facts.md fix 5.
    Simulation s(open_config());
    State& st = s.state();
    add_warp_pair(st, 0, 0, 8, 0);
    Player& p = st.players[0];
    p.x = centre_x(0);
    p.y = centre_y(0);
    p.warp_latch = true;  // already warped here: stranded on the exit warphole

    land_bomb_on(st, 0, 0);
    s.tick(TickInputs{});
    CHECK(p.stun > 0);  // head-hit fired despite the warphole underfoot
    // ...and the bomb hopped onward (a head hit never settles the bomb).
    for (const auto& bb : st.bombs)
        if (bb.active && !bb.flying)
            CHECK(!(bb.tile_x() == 0 && bb.tile_y() == 0));
}

TEST_CASE("a bomb landing on an empty warphole tile hops onward without settling") {
    // The warphole-blocks-settling half of the same control flow (pseudo.c
    // 25452-25453, the resolved dead `exp_` term).
    Simulation s(open_config());
    State& st = s.state();
    add_warp_pair(st, 4, 0, 8, 4);
    // Move the players off row 0 so no head-hit interferes.
    st.players[0].y = centre_y(2);
    st.players[1].y = centre_y(8);

    Bomb b;
    b.active = true;
    b.owner = 0;
    b.fuse = 100000;
    b.flying = true;
    b.from_x = centre_x(0);
    b.from_y = centre_y(0);
    b.x = b.from_x;
    b.y = b.from_y;
    b.to_x = centre_x(4);
    b.to_y = centre_y(0);
    b.dir = Direction::Right;
    b.fly_total = 1;
    b.fly_ticks = 1;
    st.bombs.push_back(b);

    s.tick(TickInputs{});
    REQUIRE(!st.bombs.empty());
    CHECK(st.bombs[0].flying);  // hopped onward, did not settle on the warphole
}

// ---- LABEL_246 runs in EVERY alive state, incl. a standing head-stun -------
// (2026-07-11 follow-up: the state-machine and tick-order audits both
// deferred this. The bounce/warp cases above were already fixed by an
// unconditional release-on-entry; this section pins the STANDING stun case
// (+58 > 0, no bounce/warp) that previously fully skipped the bomb-action
// block, plus that the disease auto-drop keeps firing through a stun/bounce/
// warp exactly like the bounce/warp release above. facts.md "Player state
// machine (+78) — COMPLETE" NOT-changed note; simulation.cpp `bomb_actions`.)

TEST_CASE("a standing head-hit stun releases a carried bomb (release-throw while stunned)") {
    // Unlike bounce/warp (which release via the SAME LABEL_246 fall-through),
    // a plain head-stun previously left player_turn's whole bomb-action block
    // skipped, holding a carried bomb frozen through the stun. The original
    // reaches LABEL_246 every alive tick regardless of +78 == 3, and with
    // input blocked (+56 stuck at 0) the throw's `!+56` check fires on the
    // very first stunned tick.
    Simulation s(open_config());
    State& st = s.state();
    Player& p = st.players[0];
    p.grab = true;
    s.tick(press1(0));     // drop own bomb underfoot
    s.tick(TickInputs{});  // release for a fresh edge
    s.tick(press1(0));     // grab it
    REQUIRE(p.carrying);
    run(s, s.state().tuning.pickup_pause + 1, press1(0));  // clear the pause, still holding
    REQUIRE(p.carrying);
    REQUIRE(p.pickup_pause == 0);

    p.stun = 5;  // white-box: a standing head-hit stun (sub_421F7E), no bounce/warp
    s.tick(TickInputs{});  // no input this tick: blocked key defaults to 0 -> !+56 throws
    CHECK(!p.carrying);
    REQUIRE(!st.bombs.empty());
    bool any_flying = false;
    for (const auto& b : st.bombs)
        if (b.active && b.flying) any_flying = true;
    CHECK(any_flying);
}

TEST_CASE("diarrhea auto-drop still fires every tick during a standing head-hit stun") {
    // The auto-drop force (+135/+137 -> +56=1;+54=0) lives INSIDE LABEL_246,
    // unconditional on the v113/`blocked` gate that only affects the RAW key
    // read — so it keeps firing even while a stun blocks every other action.
    Simulation s(open_config());
    State& st = s.state();
    Player& p = st.players[0];
    p.max_bombs = 5;
    infect(p, Disease::Diarrhea);

    p.stun = 8;  // white-box: standing stun, no bounce/warp/pickup-pause
    s.tick(TickInputs{});
    CHECK(p.bombs_placed >= 1);  // auto-dropped despite being fully input-blocked
    CHECK(p.stun == 7);          // the stun countdown itself is untouched
}

TEST_CASE("diarrhea + grab keeps cycling grab/throw/drop through a whole trampoline flight") {
    // Not just the single release-on-entry: LABEL_246's auto-drop force and
    // drop block both keep running every tick of the bounce, so a diseased
    // carrier throws repeatedly mid-flight, the same "serial throw" loop
    // test_diarrhea_throw.cpp pins on the ground.
    Simulation s(open_config());
    State& st = s.state();
    st.players[1].alive = false;  // solo: keep the arena to ourselves
    Player& p = st.players[0];
    p.grab = true;
    p.max_bombs = 5;
    infect(p, Disease::Diarrhea);

    p.bounce = st.tuning.trampoline_bounce_frames;  // white-box: mid-flight, away from the apex
    int throws = 0;
    for (int t = 0; t < static_cast<int>(st.tuning.trampoline_bounce_frames) && p.bounce > 0; ++t) {
        s.tick(TickInputs{});
        for (const auto& e : s.state().events)
            if (e.type == Event::Type::BombThrown) ++throws;
    }
    CHECK(throws >= 2);  // released more than once across the flight, not just at entry
}

// ---- AI silence in blocked states (v113 == 0) -------------------------------

TEST_CASE("an AI mid-bounce or mid-warp draws no RNG this tick") {
    // v113 is forced 0 for +78 == 5/6/7 (sub_41F29B 23015-23016), skipping the
    // whole input/AI dispatch — same contract as the stun/pickup-pause skips
    // pinned in test_ai.cpp. The bounce apex is kept out of this tick's
    // window (it draws relocation RNG by design, tested in
    // test_trampoline.cpp).
    Simulation s(open_config());
    State& st = s.state();
    st.players[0].ai = true;

    SUBCASE("bouncing") {
        st.players[0].bounce = 3;  // far from the len/2 apex draw
        const std::uint32_t rng0 = st.rng;
        s.tick(TickInputs{});
        CHECK(st.rng == rng0);
    }
    SUBCASE("warping") {
        st.players[0].warp = 3;  // past the midpoint: pure countdown, no RNG
        const std::uint32_t rng0 = st.rng;
        s.tick(TickInputs{});
        CHECK(st.rng == rng0);
    }
    SUBCASE("control: unblocked AI draws") {
        const std::uint32_t rng0 = st.rng;
        s.tick(TickInputs{});
        CHECK(st.rng != rng0);
    }
}
