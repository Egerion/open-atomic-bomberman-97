// Diarrhea (auto-drop) x grab/throw glove interaction (Gap 4). Faithful to
// sub_41F29B LABEL_246: the auto-drop disease forces the bomb-key edge every
// frame -- +56 set to 1, +54 cleared to 0, and the local "bomb pressed this
// frame" flag raised; that same flag also unconditionally RELEASES a carried
// bomb (the +37 throw block). With the grab glove this becomes a grab->throw->
// drop->grab loop = the "serial throwing" the user observed. This suite pins
// that the port reproduces it, and that plain diarrhea (no grab) only drops.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "helpers.hpp"

using namespace bomber::sim;
using namespace bomber::sim::test;

namespace {

int flying_bombs(const State& st) {
    int n = 0;
    for (const auto& b : st.bombs)
        if (b.active && b.flying) ++n;
    return n;
}

// Count all BombThrown events emitted this tick (unhashed, presentation).
int thrown_events(const Simulation& s) {
    int n = 0;
    for (const auto& e : s.state().events)
        if (e.type == Event::Type::BombThrown) ++n;
    return n;
}

}  // namespace

TEST_CASE("diarrhea + grab throws bombs serially (auto-drop forces the release)") {
    Simulation s(open_config());
    State& st = s.state();
    st.players[1].alive = false;   // solo: keep the arena to ourselves
    Player& q = st.players[0];
    q.grab = true;
    q.max_bombs = 5;               // room for several grab/throw cycles
    infect(q, Disease::Diarrhea);  // white-box: force the disease, not the roll

    // The player sits centred on its spawn. Diarrhea auto-drops a bomb underfoot,
    // the grab glove grabs it next tick, and the auto-drop forces the throw the
    // tick after — a grab->throw->drop loop. Over many ticks it throws MULTIPLE
    // times (the serial throwing the user observed).
    int throws = 0;
    bool ever_flew = false;
    for (int t = 0; t < 60; ++t) {
        s.tick(TickInputs{});  // no button input: diarrhea drives everything
        throws += thrown_events(s);
        if (flying_bombs(st) > 0) ever_flew = true;
    }
    CHECK(throws >= 2);  // carried bombs get thrown repeatedly (serial-throw)
    CHECK(ever_flew);    // and they fly (launched, not just dropped)
}

TEST_CASE("plain diarrhea (no grab) only drops — never throws") {
    Simulation s(open_config());
    State& st = s.state();
    Player& p = st.players[0];
    p.grab = false;                // no grab glove
    p.max_bombs = 5;               // room to drop several
    infect(p, Disease::Diarrhea);

    bool ever_thrown = false;
    bool ever_placed = false;
    for (int t = 0; t < 40; ++t) {
        s.tick(TickInputs{});
        if (thrown_events(s) > 0) ever_thrown = true;
        // Sample DURING the loop, not just after: the auto-drop places on tick 0
        // and its fuse (tuning.fuse_frames == 40) can expire on the loop's very
        // last iteration, clearing bombs_placed back to 0 right as the loop ends
        // (same same-tick place+fuse-tick ordering as elsewhere — see
        // simulation.cpp's step 1/3 and test_trigger_allowance.cpp).
        if (p.bombs_placed >= 1) ever_placed = true;
    }
    CHECK_FALSE(ever_thrown);      // without the glove there is nothing to throw
    // It did auto-drop: a bomb (or its blast aftermath) exists / was placed.
    CHECK(ever_placed);
}

TEST_CASE("constipation blocks the drop but still lets a carried bomb be thrown") {
    // The original gates only the DROP block on constipation (+134); the throw
    // block (+37) has no such gate. A player already carrying a bomb who then
    // gets constipation can still throw it (here via key release).
    Simulation s(open_config());
    State& st = s.state();
    Player& p = st.players[0];
    p.grab = true;

    // Press once to drop a bomb underfoot, then again (standing on it) to grab.
    s.tick(press1(0));             // drop at (0,0)
    s.tick(TickInputs{});          // release
    s.tick(press1(0));             // grab it -> carrying (+ pickup stun)
    REQUIRE(p.carrying);

    infect(p, Disease::Constipation);  // now constipated while carrying
    // The grab imposes a short pickup pause (stun); tick past it, keys released.
    run(s, 4, TickInputs{});       // key stays released -> throw block fires
    CHECK_FALSE(p.carrying);       // the held bomb was thrown despite constipation
    CHECK(flying_bombs(st) >= 1);
}

TEST_CASE("diarrhea auto-drop still respected without the glove edge semantics") {
    // Regression: auto-drop must fire every tick regardless of the real button,
    // exactly as the forced +56/+54 edge does. A no-input diarrhea player drops.
    Simulation s(open_config());
    State& st = s.state();
    Player& p = st.players[0];
    p.max_bombs = 3;
    infect(p, Disease::Diarrhea);

    s.tick(TickInputs{});          // no input at all
    CHECK(p.bombs_placed >= 1);    // a bomb was auto-dropped this very tick
}
