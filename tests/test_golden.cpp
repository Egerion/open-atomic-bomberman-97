// Golden-hash regression tests. These scenarios were captured from the
// pre-refactor simulation (2026-07-03) and pin the EXACT behaviour: state
// hash, RNG stream position, everything.
//
// If one of these fails you changed gameplay behaviour. That is either a bug
// (fix it) or a deliberate faithfulness improvement from new RE facts — in
// that case update the constants IN THE SAME COMMIT as the change and cite
// the docs/re/facts.md entry that justifies it. See CLAUDE.md.

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
    return cfg;
}

}  // namespace

// NOTE 2026-07-08: every constant below was recaptured. Root-cause: commit
// 21f6187 ("Phase 1 mechanic-fidelity sweep") shipped this file's constants
// ALREADY WRONG — even checked out at that exact commit, none of the five
// scenarios reproduce their own pinned hashes (verified byte-for-byte; the
// "tests green" claim in that commit's message was never true). Two genuine
// sim bugs rode along on top of that broken baseline and are fixed here:
//   1. Conveyor "no input" push (StageActorSystem::move_on_actor case (a))
//      added the player's own speed on top of conveyor_speed, contradicting
//      sub_41F29B's case (a) pseudocode (docs/re/stage-actors.md §3), which
//      never reads the player's speed when there is no input. Fixed by a new
//      MovementSystem::move(..., use_player_speed) parameter, false only for
//      the belt-forced/no-input case.
//   2. EnclosureSystem::update() gated `warn`/`closing` on `ticks_left > 0`,
//      so the wall spiral froze solid the instant the match clock hit zero.
//      The original's remaining-seconds predicate (sub_410578, clamped >= 0)
//      never re-freezes once armed — the walls keep closing through sudden
//      death (docs/re/enclosure.md §2, which already documented golden A/D/E
//      as relying on never reaching the clock at all). Fixed by dropping the
//      guard; our ticks_left is likewise clamped at 0 so the predicate stays
//      monotonic.
// Also, commit b0dc533 (AI port) added a `brains` hash block (hash.cpp) that
// mixes kMaxPlayers Brain-sized zero words into EVERY scenario's digest —
// including golden A, which has no AI and no players at all — without
// recapturing this file, growing the hash layout out from under the pinned
// constants a second time.
//
// UPDATE 2026-07-08 (TEAM wiring, docs/re/ai.md TEAM follow-up /
// docs/re/setup-screens.md +84 byte): hash.cpp now mixes a `Player::team` word
// per present player (one new mix() call per player, right after
// trigger_placed). Every golden scenario's players default team=0 (no config
// sets MatchConfig::team[]), so gameplay is byte-identical — this is a
// one-time HASH-LAYOUT recapture only (CLAUDE.md determinism contract rule 5),
// same shape as the b0dc533 brains-block growth above. Golden A is unaffected
// (0 players -> 0 new mix words). Golden D's pinned RNG-stream values
// (kExpectedRng) are UNCHANGED by this commit — verified byte-for-byte before
// recapturing the hashes below — confirming the team wiring adds no new RNG
// draws on the untamed (all-zero-team) path.
// Two test-fixture-only fixes ride along (no sim behaviour change, just
// removing an accidental dependency on the ticks_left==0 "no clock" edge
// case that the enclosure fix above turned into "sudden death from tick 0"):
// golden A and tests/test_ai.cpp's open_arena() now set an explicit, large
// ticks_left so these clock-free scratch scenarios stay clock-free, matching
// this file's own "empty state"/"no clock" framing.

TEST_CASE("golden A: empty state, 10000 ticks") {
    Simulation a;
    a.state().rng = 42u;
    // The bare ctor also zero-inits ticks_left, which the enclosure system reads
    // as "time's up" (sudden death: EnclosureSystem now correctly keeps closing
    // walls past TimeUp instead of freezing — see docs/re/enclosure.md §2/§6,
    // which already documents golden A as having "no clock (empty sim)").
    // Without a real match clock this scenario was never meant to exercise the
    // wall spiral at all, so give it a generous countdown to keep it dormant,
    // matching that documented intent.
    a.state().ticks_left = 9999 * kTicksPerSecond;
    for (std::uint64_t t = 0; t < 10000; ++t) a.tick(pattern(t));
    CHECK(a.hash() == 0xa99c2999b7246564ull);
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
    cfg.born_with[static_cast<int>(PowerupType::Kick)] = true;
    cfg.born_with[static_cast<int>(PowerupType::Punch)] = true;
    cfg.born_with[static_cast<int>(PowerupType::Grab)] = true;
    cfg.born_with[static_cast<int>(PowerupType::Spooger)] = true;
    cfg.born_with[static_cast<int>(PowerupType::Jelly)] = true;
    Simulation s(cfg);
    CHECK(s.hash() == 0x7bb2faa299a9dfbdull);  // setup itself is pinned

    static constexpr std::uint64_t kExpected[6] = {
        0xda15a41550e9063cull,  // tick 500
        0x6403965535b92850ull,  // tick 1000
        0x04d0c964cba38454ull,  // tick 1500
        0x9d807c0fa62005dbull,  // tick 2000
        0x6ac54d08725422acull,  // tick 2500
        0xd757156294d60149ull,  // tick 3000
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
    cfg.born_with[static_cast<int>(PowerupType::Trigger)] = true;
    Simulation s(cfg);
    for (std::uint64_t t = 0; t < 1500; ++t) s.tick(pattern(t * 7 + 3));
    CHECK(s.hash() == 0xeb7ccb190e3122ecull);
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
            if (m == 0) s.state().floor[y][x] = PowerupType::Disease;
            else if (m == 1) s.state().floor[y][x] = PowerupType::SuperDisease;
            else if (m == 2) s.state().floor[y][x] = PowerupType::Skate;
            else if (m == 3) s.state().floor[y][x] = PowerupType::Flame;
        }

    static constexpr std::uint64_t kExpectedHash[4] = {
        0x917fd0daca6a23baull,  // tick 200
        0x0e7f1c307c8235c0ull,  // tick 400
        0xd6651053635e6029ull,  // tick 600
        0xba564b2cc526e62cull,  // tick 800
    };
    static constexpr std::uint32_t kExpectedRng[4] = {0xca47489cu, 0x49cffff6u, 0x2abb3268u,
                                                      0xd72904d8u};
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
    // Captured 2026-07-03 with the jelly mechanics from docs/re/facts.md
    // "Bomb machine" (sub_42331C): kicked jelly reverses off obstacles, flying
    // jelly rolls the 1-in-getvalue(667) veer at each landing boundary.
    // Choreography: drop a jelly bomb, kick it into a wall so it ping-pongs
    // between the wall and the player, then drop a second bomb on a free row
    // and punch it east (the veer roll consumes sim RNG).
    MatchConfig cfg = pillars_config();
    cfg.cells[0][7] = Cell::Solid;  // kick wall
    cfg.spawns = {{2, 0}, {14, 10}};
    cfg.player_count = 2;
    cfg.seed = 4242;
    for (auto& c : cfg.tuning.spawn_counts) c = 0;
    cfg.tuning.fuse_frames = 200;   // long fuse: room for the ping-pong
    cfg.tuning.start_with[0] = 3;   // three bombs
    cfg.born_with[static_cast<int>(PowerupType::Kick)] = true;
    cfg.born_with[static_cast<int>(PowerupType::Punch)] = true;
    cfg.born_with[static_cast<int>(PowerupType::Jelly)] = true;
    Simulation s(cfg);

    auto script = [](std::uint64_t t) {
        TickInputs in{};
        auto& p = in.players[0];
        if (t == 0) p.action1 = true;                 // drop jelly bomb at (2,0)
        else if (t >= 1 && t <= 10) p.left = true;    // step off westward
        else if (t >= 11 && t <= 18) p.right = true;  // walk back -> kick east
        else if (t >= 19 && t <= 26) p.down = true;   // leave row 0 to the ping-pong
        else if (t == 32) p.action1 = true;           // drop bomb #2 at (2,2)
        else if (t >= 33 && t <= 36) p.left = true;   // one tile west of it
        else if (t == 40) p.right = true;             // face east (no contact)
        else if (t == 44) p.action2 = true;           // punch #2 -> flight + veer RNG
        return in;
    };

    static constexpr std::uint64_t kExpected[4] = {
        0x9f35de7774cdb72aull,  // tick 75
        0xcf0451a465a2833full,  // tick 150
        0xac5945f3c6d87d01ull,  // tick 225
        0x08b0705d6b25cfa8ull,  // tick 300
    };
    int bounces = 0;
    for (std::uint64_t t = 0; t < 300; ++t) {
        s.tick(script(t));
        for (const auto& e : s.state().events)
            if (e.type == Event::Type::JellyBounced) ++bounces;
        if ((t + 1) % 75 == 0) CHECK(s.hash() == kExpected[(t + 1) / 75 - 1]);
    }
    CHECK(bounces == 7);                    // the ping-pong really happened
    CHECK(s.state().rng == 0xcce3bbf8u);    // the veer roll really consumed RNG
}
