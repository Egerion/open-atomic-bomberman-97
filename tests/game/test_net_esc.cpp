// net_esc.hpp — the ONLINE Esc rule: one press stops the round (host only), a
// second one LEAVES (everyone).
//
// The bug this exists for: the wire-v8 match shell made Esc "ask the host to end
// the round", which is right for a working match and useless for a broken one.
// With the peer stalled the answer never came, so Esc looked dead and there was
// no way out of a broken match at all — the exact complaint that started this
// thread ("oyun takılınca esc'e basıp mp'den çıkamıyoruz").
//
// The dead-peer case is the load-bearing one and it is the reason this rule is a
// pure object rather than an `if` in MatchRunner's event loop: everything around
// it there is SDL, so the one situation that matters could otherwise only be
// reproduced with two machines and a broken network.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "bomber/game/net_esc.hpp"

using bomber::game::EscPress;
using bomber::game::kEscLeaveWindowMs;
using bomber::game::NetEscState;

TEST_CASE("esc: the first press arms, the second leaves") {
    NetEscState e;
    CHECK(e.press(1000, /*stop_outstanding=*/false) == EscPress::Arm);
    // ...and the prompt is up for exactly the window in which that is true, so
    // the screen cannot promise something the key handler will not honour.
    CHECK(e.armed(1000, false));
    CHECK(e.armed(1000 + kEscLeaveWindowMs - 1, false));
    CHECK(e.press(1500, false) == EscPress::Leave);
}

TEST_CASE("esc: a stray press does not leave the player one keystroke from quitting") {
    NetEscState e;
    REQUIRE(e.press(1000, false) == EscPress::Arm);
    CHECK_FALSE(e.armed(1000 + kEscLeaveWindowMs, false));
    // Minutes later, the next press arms again rather than leaving.
    CHECK(e.press(400000, false) == EscPress::Arm);
    CHECK(e.armed(400000, false));
}

TEST_CASE("esc: A PEER THAT NEVER ANSWERS — the way out stays open indefinitely") {
    // THE LOAD-BEARING CASE. The host pressed Esc, so a stop is scheduled; but a
    // stop only completes when EVERY peer reaches the agreed tick, and a peer
    // that has stopped responding never will. So `stop_outstanding` is true
    // forever, and the window must not close under the player — arming on the
    // timer alone would shut the escape hatch three seconds into precisely the
    // situation it exists for.
    NetEscState e;
    REQUIRE(e.press(1000, /*stop_outstanding=*/false) == EscPress::Arm);

    // Half an hour of a dead peer. Still armed, still leaves on a press.
    for (const std::uint64_t t : {5000ull, 60000ull, 600000ull, 1800000ull}) {
        CHECK(e.armed(t, /*stop_outstanding=*/true));
    }
    CHECK(e.press(1800000, /*stop_outstanding=*/true) == EscPress::Leave);
}

TEST_CASE("esc: a GUEST gets the same way out, on the same two presses") {
    // A guest's press never sets `stop_outstanding` — it does nothing to the
    // match at all, by the session's own rule — so this is the plain timed
    // window. It must still LEAVE on the second press: a guest who cannot leave a
    // stalled match is the bug the owner started from, and stopping the match
    // being host-only must not reach leaving.
    NetEscState e;
    CHECK(e.press(0, /*stop_outstanding=*/false) == EscPress::Arm);
    CHECK(e.press(200, /*stop_outstanding=*/false) == EscPress::Leave);
}

TEST_CASE("esc: leaving does not need the round to be stoppable, or stopped") {
    // Every combination reaches Leave on the second press. Whatever the session
    // is doing — or refusing to do — the second press is the player's own.
    for (const bool outstanding : {false, true}) {
        NetEscState e;
        REQUIRE(e.press(0, outstanding) == (outstanding ? EscPress::Leave : EscPress::Arm));
        CHECK(e.press(100, outstanding) == EscPress::Leave);
    }
}
