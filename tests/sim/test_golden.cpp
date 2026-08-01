// Golden-hash regression tests: whole scenarios whose EXACT behaviour is pinned
// — state hash, RNG stream position, everything.
//
// A FAILURE HERE MEANS YOU CHANGED GAMEPLAY. That is either a bug (fix it) or a
// deliberate faithfulness improvement from a new RE fact — in that case update
// the constants IN THE SAME COMMIT as the change and cite the docs/re/facts.md
// entry that justifies it (CLAUDE.md's determinism contract, rule 5).
//
// The scenarios themselves live in tests/common/golden_scenarios.hpp, because
// tests/net/test_build_hash.cpp's rule-7 detector needs the same six runs to
// answer "did sim behaviour change?" from a source OTHER than build_hash. This
// file keeps every pinned constant and every non-hash assertion; the header
// holds only the setup and the per-tick keys.
//
// HOW TO RECAPTURE, AND THE PROOF THAT MUST COME WITH IT. A hash is opaque, so a
// recapture can hide a second, unintended change inside the one you meant. Run
// BOTH revisions and check the NON-HASH assertions first: golden A's final rng,
// golden D's kExpectedRng at all four checkpoints, golden E's bounce count and
// final rng. Those pin the RNG STREAM rather than the board, so if they are
// byte-identical across the two builds then the change added, removed and
// reordered no draws — and only then is moving the hashes alone honest. Record
// in the commit message which scenarios moved and which stayed byte-identical.
// A recapture also moves the golden FINGERPRINT pinned in
// tests/net/test_build_hash.cpp; that suite is where you find out whether
// build_hash moved with it, which rule 7 requires.
//
// WHAT EACH SCENARIO DISCRIMINATES. They are not interchangeable, and a scenario
// that stops REACHING its mechanic silently retires that coverage:
//   A  no players, no bombs, no rovers, 10000 ticks. The control: nothing
//      behavioural reaches it, so if A moves at all the hash LAYOUT changed.
//      It does NOT follow that an always-zero field leaves A unmoved, which is
//      what this line used to claim — corrected 2026-08-01, when adding
//      State::enclose_interval (always 0 in A, since A never arms the spiral)
//      moved it anyway. FNV folds the eight zero bytes of a new mix() word like
//      any other input; only packing into an existing word's SPARE BITS is free.
//      So: A moved + B-F moved => layout growth. A still + B-F moved =>
//      behaviour, in state A cannot reach.
//   B  4 players, all-brick board, every ability granted at baseline — the
//      broadest behavioural net, and the first scenario a change usually moves.
//   C  trigger duel on a short 70 s clock.
//   D  disease gauntlet with the action keys forced off: the only scenario with
//      active diseases, and the only one that pins the RNG stream per checkpoint.
//   E  scripted jelly choreography — a kicked ping-pong plus a punched flight
//      whose veer roll draws RNG. The only non-pattern (hand-scripted) run.
//   F  a hurry phase that runs to completion. B and C used to be the only
//      scenarios that reached the wall spiral at all, and after the round-end
//      freeze landed (enclosure F2) neither does; F exists so that coverage did
//      not leave with them.
//
// The per-recapture history — twenty-odd dated entries, each naming the audit
// that moved which constant — is where this repo keeps history:
// `git log -p tests/sim/test_golden.cpp`.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "bomber/sim/simulation.hpp"
#include "golden_scenarios.hpp"

using namespace bomber::sim;
namespace golden = bomber::testing::golden;

TEST_CASE("golden A: empty state, 10000 ticks") {
    Simulation a = golden::make_a();
    for (std::uint64_t t = 0; t < golden::kTicksA; ++t) a.tick(golden::input_a(t));
    CHECK(a.hash() == 0xd220967578a0d145ull);
    CHECK(a.state().rng == 0x0000002au);
}

TEST_CASE("golden B: 4-player brick match with all abilities") {
    Simulation s = golden::make_b();
    CHECK(s.hash() == 0xb97f8753728e6233ull);  // setup alone is pinned, before any tick

    // B is the only roster that both starts with the grab glove and drives the
    // bomb key, so it is the only scenario that can reach the grab pause window
    // or a drop whose tile depends on which sub-frame resolved the edge. The
    // mechanics the other scenarios cannot reach are pinned at the digest
    // instead, by build_hash.cpp's scenarios.
    static constexpr std::uint64_t kExpected[6] = {
        0xbfa51c335b60afaeull,  // tick 500
        0x4978c8e1aba1188eull,  // tick 1000
        0x7e69dda620a4e346ull,  // tick 1500
        0x13078aebb1e94facull,  // tick 2000
        0x462455175efacff9ull,  // tick 2500
        0xc4287cb46255ecfdull,  // tick 3000
    };
    for (std::uint64_t t = 0; t < golden::kTicksB; ++t) {
        s.tick(golden::input_b(t));
        if ((t + 1) % 500 == 0) CHECK(s.hash() == kExpected[(t + 1) / 500 - 1]);
    }
}

TEST_CASE("golden C: trigger duel on a short 70 s clock") {
    Simulation s = golden::make_c();
    for (std::uint64_t t = 0; t < golden::kTicksC; ++t) s.tick(golden::input_c(t));
    CHECK(s.hash() == 0x0f6cced5cb6934a3ull);
    // Legible companions to the digest, so a stepper regression names itself
    // instead of only moving an opaque hash. The round decides at tick 11 of
    // 1500, and the round-end freeze (enclosure F2) then holds the stepper: this
    // scenario USED to arm at tick 281 and close all 96 spiral tiles.
    CHECK(sides_remaining(s.state()) == 1);  // decided at tick 11 of 1500...
    CHECK(s.state().enclose_interval == 0);  // ...so the walls never armed...
    CHECK(s.state().enclose_index == 0);     // ...and not one tile ever dropped.
}

TEST_CASE("golden D: the disease gauntlet") {
    Simulation s = golden::make_d();

    static constexpr std::uint64_t kExpectedHash[4] = {
        0x75310962f429a817ull,  // tick 200
        0x807d0b240e6a7052ull,  // tick 400
        0xd5319ee16ac197f0ull,  // tick 600
        0xb885a756ad019ed4ull,  // tick 800
    };
    // The RNG stream beside the board, and the reason D is the scenario every
    // recapture is proved against: it pins the DRAW COUNT independently of the
    // hash, so a change that only reorders state shows up here as "unchanged"
    // while a change that draws differently cannot hide. The series goes quiet
    // after tick 200 because the gauntlet's pickups are all consumed by then.
    static constexpr std::uint32_t kExpectedRng[4] = {0x49cffff6u, 0xf1401d55u, 0xf1401d55u,
                                                      0xf1401d55u};
    for (std::uint64_t t = 0; t < golden::kTicksD; ++t) {
        s.tick(golden::input_d(t));
        if ((t + 1) % 200 == 0) {
            CHECK(s.hash() == kExpectedHash[(t + 1) / 200 - 1]);
            CHECK(s.state().rng == kExpectedRng[(t + 1) / 200 - 1]);
        }
    }
}

TEST_CASE("golden E: jelly ping-pong and a veering punched flight") {
    Simulation s = golden::make_e();

    static constexpr std::uint64_t kExpected[4] = {
        0x4e11a09ce588db56ull,  // tick 75
        0x181750dc1a399221ull,  // tick 150
        0xd09630ed746df4f9ull,  // tick 225
        0xc527e8cf81ba5d96ull,  // tick 300
    };
    int bounces = 0;
    for (std::uint64_t t = 0; t < golden::kTicksE; ++t) {
        s.tick(golden::input_e(t));
        for (const auto& e : s.state().events)
            if (e.type == Event::Type::JellyBounced) ++bounces;
        if ((t + 1) % 75 == 0) CHECK(s.hash() == kExpected[(t + 1) / 75 - 1]);
    }
    // 10 legs, and the count is a RETRACTION worth keeping: an audit once folded
    // a flat +100*kSubFrames "ground bonus" into the kicked slide, which read as
    // ~19 px/tick and 21 legs. The native oracle showed that +100 is cancelled by
    // a paired one-step position backoff, so the faithful slide is the BASE speed
    // (~0.25 tile/tick, cadence-invariant) and the bonus was withdrawn as a false
    // positive (bombs F2, docs/re/audit/bombs.md finding 2). A bounce draws no
    // RNG itself, so the rng below pins the detonation tile, not the ping-pong.
    CHECK(bounces == 10);                 // the ping-pong really happened
    CHECK(s.state().rng == 0x405862fbu);  // base kicked speed: detonation on its pre-F2 tile
}

TEST_CASE("golden F: a full hurry phase with the round still undecided") {
    Simulation s = golden::make_f();
    // The walls arm at tick 180 (30 s clock, hurry 25 ⇒ the arm predicate
    // `remaining <= hurry - 5` first holds with 419 ticks left) and the two-ring
    // spiral's 96th and last tile lands around tick 660.
    static constexpr std::uint64_t kExpected[4] = {
        0xd0a7a87582ccc5fdull,  // tick 250  (armed at 180; drop index 13)
        0xbb32accff09772b7ull,  // tick 500  (index 63)
        0x0b52431bb67422d7ull,  // tick 750  (index 96 — past TimeUp at tick 600)
        0x29426b6cfd46a0daull,  // tick 1000 (spiral exhausted, board static)
    };
    for (std::uint64_t t = 0; t < golden::kTicksF; ++t) {
        s.tick(golden::input_f(t));
        if ((t + 1) % 250 == 0) CHECK(s.hash() == kExpected[(t + 1) / 250 - 1]);
    }
    // Legible assertions alongside the opaque digests, so a regression in the
    // stepper says WHAT broke and not just "some hash moved".
    CHECK(sides_remaining(s.state()) == 2);  // never decided -> never frozen
    CHECK(s.state().ticks_left == 0);        // the clock ran out at tick 600...
    CHECK(s.state().enclose_index == 96);    // ...and the spiral finished anyway (§2):
                                             // rings 0-1 = 96 drop events, all landed
    CHECK(s.state().actor_type[4][4] == ActorType::None);       // warphole swept (§5.1)
    CHECK(s.state().actor_type[6][10] == ActorType::None);      // warphole swept
    CHECK(s.state().actor_type[6][4] == ActorType::None);       // trampoline swept
    CHECK(s.state().actor_type[4][10] == ActorType::Conveyor);  // belt survives
    CHECK(s.state().actor_type[6][6] == ActorType::DirArrow);   // arrow survives
    CHECK(s.state().warp_dest_x[4][4] == 10);                   // only the ACTIVE flag is cleared
}
