// Golden-hash regression tests: whole scenarios whose EXACT behaviour is pinned
// — state hash, RNG stream position, everything.
//
// A FAILURE HERE MEANS YOU CHANGED GAMEPLAY. That is either a bug (fix it) or a
// deliberate faithfulness improvement from a new RE fact — in that case update
// the constants IN THE SAME COMMIT as the change and cite the docs/re/facts.md
// entry that justifies it (CLAUDE.md's determinism contract, rule 5).
//
// HOW TO RECAPTURE, AND THE PROOF THAT MUST COME WITH IT. A hash is opaque, so a
// recapture can hide a second, unintended change inside the one you meant. Run
// BOTH revisions and check the NON-HASH assertions first: golden A's final rng,
// golden D's kExpectedRng at all four checkpoints, golden E's bounce count and
// final rng. Those pin the RNG STREAM rather than the board, so if they are
// byte-identical across the two builds then the change added, removed and
// reordered no draws — and only then is moving the hashes alone honest. Record
// in the commit message which scenarios moved and which stayed byte-identical.
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

using namespace bomber::sim;

namespace {

TickInputs pattern(std::uint64_t t) {
    TickInputs in{};
    for (int p = 0; p < kMaxPlayers; ++p) {
        auto& pi = in.players[static_cast<std::size_t>(p)];
        pi.up = (t + static_cast<std::uint64_t>(p)) % 7 == 0;
        pi.down = (t + static_cast<std::uint64_t>(p)) % 11 == 1;
        pi.left = (t * 3 + static_cast<std::uint64_t>(p)) % 5 == 2;
        pi.right = (t * 5 + static_cast<std::uint64_t>(p)) % 9 == 3;
        pi.action1 = (t * 31 + static_cast<std::uint64_t>(p)) % 13 == 0;
        pi.action2 = (t * 17 + static_cast<std::uint64_t>(p)) % 23 == 0;
    }
    return in;
}

MatchConfig pillars_config() {
    MatchConfig cfg;
    for (int y = 0; y < kGridHeight; ++y)
        for (int x = 0; x < kGridWidth; ++x)
            cfg.cells[y][x] = (x % 2 == 1 && y % 2 == 1) ? Cell::Solid : Cell::Blank;
    // Disarm the round-start input freeze (facts.md "Round-start input freeze",
    // VALUELST id 30 ≈ 1 s of dead input): these scenarios were captured acting
    // from tick 0 and golden E's choreography depends on it. test_freeze.cpp
    // pins the freeze itself.
    cfg.tuning.input_freeze_ticks = 0;
    return cfg;
}

}  // namespace

TEST_CASE("golden A: empty state, 10000 ticks") {
    Simulation a;
    a.state().rng = 42u;
    // The bare ctor zero-inits ticks_left, which the enclosure reads as "time's
    // up" and would close walls from tick 0. A was never meant to exercise the
    // spiral (docs/re/enclosure.md §2/§6 documents it as having no clock), so
    // seed a countdown generous enough to keep the stepper dormant.
    a.state().ticks_left = 9999 * kTicksPerSecond;
    for (std::uint64_t t = 0; t < 10000; ++t) a.tick(pattern(t));
    CHECK(a.hash() == 0xd220967578a0d145ull);
    CHECK(a.state().rng == 0x0000002au);
}

TEST_CASE("golden B: 4-player brick match with all abilities") {
    MatchConfig cfg;
    for (int y = 0; y < kGridHeight; ++y)
        for (int x = 0; x < kGridWidth; ++x)
            cfg.cells[y][x] = (x % 2 == 1 && y % 2 == 1) ? Cell::Solid : Cell::Brick;
    cfg.spawns = {{0, 0}, {14, 10}, {14, 0}, {0, 10}};
    cfg.player_count = 4;
    cfg.seed = 0xB0BB1E5;
    cfg.tuning.input_freeze_ticks = 0;  // see pillars_config's disarm note
    // "Born with" is a starting-inventory BASELINE, not a grant replayed through
    // PowerupSystem::apply (facts.md "The .SCH -P row's 2nd field is a COUNT
    // that REPLACES the starting inventory"): the scheme field writes VALUELST
    // id 50+kind, which IS Tuning::start_with[].
    cfg.tuning.start_with[static_cast<int>(PowerupType::Kick)] = 1;
    cfg.tuning.start_with[static_cast<int>(PowerupType::Punch)] = 1;
    cfg.tuning.start_with[static_cast<int>(PowerupType::Grab)] = 1;
    cfg.tuning.start_with[static_cast<int>(PowerupType::Spooger)] = 1;
    cfg.tuning.start_with[static_cast<int>(PowerupType::Jelly)] = 1;
    Simulation s(cfg);
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
    for (std::uint64_t t = 0; t < 3000; ++t) {
        s.tick(pattern(t));
        if ((t + 1) % 500 == 0) CHECK(s.hash() == kExpected[(t + 1) / 500 - 1]);
    }
}

TEST_CASE("golden C: trigger bombs and a fast hurry phase") {
    MatchConfig cfg = pillars_config();
    cfg.cells[0][2] = Cell::Brick;
    cfg.spawns = {{0, 0}, {14, 10}};
    cfg.player_count = 2;
    cfg.seed = 99;
    cfg.tuning.game_seconds = 70;
    cfg.tuning.start_with[static_cast<int>(PowerupType::Trigger)] = 1;  // baseline, see B
    Simulation s(cfg);
    for (std::uint64_t t = 0; t < 1500; ++t) s.tick(pattern(t * 7 + 3));
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
    MatchConfig cfg = pillars_config();
    cfg.spawns = {{0, 0}, {14, 10}, {7, 0}};
    cfg.player_count = 3;
    cfg.seed = 1234;
    for (auto& c : cfg.tuning.spawn_counts) c = 0;
    Simulation s(cfg);
    int k = 0;
    for (int y = 0; y < kGridHeight; ++y)
        for (int x = 0; x < kGridWidth; ++x) {
            if (s.state().cells[y][x] != Cell::Blank) continue;
            int m = k++ % 5;
            if (m == 0)
                s.state().floor[y][x] = PowerupType::Disease;
            else if (m == 1)
                s.state().floor[y][x] = PowerupType::SuperDisease;
            else if (m == 2)
                s.state().floor[y][x] = PowerupType::Skate;
            else if (m == 3)
                s.state().floor[y][x] = PowerupType::Flame;
        }

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
    for (std::uint64_t t = 0; t < 800; ++t) {
        TickInputs in = pattern(t);
        for (int p = 0; p < kMaxPlayers; ++p) {
            in.players[p].action1 = false;
            in.players[p].action2 = false;
        }
        s.tick(in);
        if ((t + 1) % 200 == 0) {
            CHECK(s.hash() == kExpectedHash[(t + 1) / 200 - 1]);
            CHECK(s.state().rng == kExpectedRng[(t + 1) / 200 - 1]);
        }
    }
}

TEST_CASE("golden E: jelly ping-pong and a veering punched flight") {
    // Jelly mechanics per docs/re/facts.md "Bomb machine" (sub_42331C): a kicked
    // jelly reverses off obstacles, a flying jelly rolls the 1-in-getvalue(667)
    // veer at each landing boundary. Choreography: drop a jelly bomb, kick it
    // into a wall so it ping-pongs between the wall and the player, then drop a
    // second bomb on a free row and punch it east (the veer roll consumes RNG).
    MatchConfig cfg = pillars_config();
    cfg.cells[0][7] = Cell::Solid;  // kick wall
    cfg.spawns = {{2, 0}, {14, 10}};
    cfg.player_count = 2;
    cfg.seed = 4242;
    for (auto& c : cfg.tuning.spawn_counts) c = 0;
    cfg.tuning.fuse_frames = 200;  // long fuse: room for the ping-pong
    cfg.tuning.start_with[0] = 3;  // three bombs
    cfg.tuning.start_with[static_cast<int>(PowerupType::Kick)] = 1;   // baseline, see B
    cfg.tuning.start_with[static_cast<int>(PowerupType::Punch)] = 1;  // baseline, see B
    cfg.tuning.start_with[static_cast<int>(PowerupType::Jelly)] = 1;  // baseline, see B
    Simulation s(cfg);

    auto script = [](std::uint64_t t) {
        TickInputs in{};
        auto& p = in.players[0];
        if (t == 0)
            p.action1 = true;  // drop jelly bomb at (2,0)
        else if (t >= 1 && t <= 10)
            p.left = true;  // step off westward
        else if (t >= 11 && t <= 18)
            p.right = true;  // walk back -> kick east
        else if (t >= 19 && t <= 26)
            p.down = true;  // leave row 0 to the ping-pong
        else if (t == 32)
            p.action1 = true;  // drop bomb #2 at (2,2)
        else if (t >= 33 && t <= 36)
            p.left = true;  // one tile west of it
        else if (t == 40)
            p.right = true;  // face east (no contact)
        else if (t == 44)
            p.action2 = true;  // punch #2 -> flight + veer RNG
        return in;
    };

    static constexpr std::uint64_t kExpected[4] = {
        0x4e11a09ce588db56ull,  // tick 75
        0x181750dc1a399221ull,  // tick 150
        0xd09630ed746df4f9ull,  // tick 225
        0xc527e8cf81ba5d96ull,  // tick 300
    };
    int bounces = 0;
    for (std::uint64_t t = 0; t < 300; ++t) {
        s.tick(script(t));
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
    MatchConfig cfg = pillars_config();
    // Both spawns are ring-4 tiles (min(x, y, 14-x, 10-y) == 4); with
    // enclosement_depth = 1 the walls close rings 0-1 only, so neither player is
    // ever crushed and the round stays undecided to the last tick. That is the
    // whole point: nobody presses a key, nobody dies, sides_remaining stays 2,
    // and the stepper's round-end gate never trips.
    cfg.spawns = {{6, 4}, {8, 6}};
    cfg.player_count = 2;
    cfg.seed = 0xEC105u;
    cfg.tuning.game_seconds = 30;
    cfg.tuning.hurry_seconds = 25;
    cfg.tuning.enclosement_depth = 1;
    // One of each actor type, all on ring-4 tiles the spiral never reaches and
    // none under a player, so the ONLY thing that can change them is the arm
    // sweep: the warphole pair and the trampoline must go, the belt and the
    // arrow must stay (sub_405D0C, docs/re/enclosure.md §5.1).
    cfg.actor_type[4][4] = ActorType::Warphole;
    cfg.warp_dest_x[4][4] = 10;
    cfg.warp_dest_y[4][4] = 6;
    cfg.actor_type[6][10] = ActorType::Warphole;
    cfg.warp_dest_x[6][10] = 4;
    cfg.warp_dest_y[6][10] = 4;
    cfg.actor_type[6][4] = ActorType::Trampoline;
    cfg.actor_type[4][10] = ActorType::Conveyor;
    cfg.actor_dir[4][10] = 1;
    cfg.actor_type[6][6] = ActorType::DirArrow;
    cfg.actor_dir[6][6] = 2;

    Simulation s(cfg);
    // The walls arm at tick 180 (30 s clock, hurry 25 ⇒ the arm predicate
    // `remaining <= hurry - 5` first holds with 419 ticks left) and the two-ring
    // spiral's 96th and last tile lands around tick 660.
    static constexpr std::uint64_t kExpected[4] = {
        0xd0a7a87582ccc5fdull,  // tick 250  (armed at 180; drop index 13)
        0xbb32accff09772b7ull,  // tick 500  (index 63)
        0x0b52431bb67422d7ull,  // tick 750  (index 96 — past TimeUp at tick 600)
        0x29426b6cfd46a0daull,  // tick 1000 (spiral exhausted, board static)
    };
    for (std::uint64_t t = 0; t < 1000; ++t) {
        s.tick(TickInputs{});
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
