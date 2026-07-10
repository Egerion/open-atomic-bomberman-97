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
//      death (docs/re/enclosure.md §2, which already documents golden A/D/E
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
//
// UPDATE 2026-07-09 (Goldman wheel clogs, docs/re/goldman-roulette.md §9):
// hash.cpp now mixes a `Player::clogs` word per present player (right after
// `Player::team`) — a speed-penalty count fed ONLY by the new
// MatchConfig::born_with_clogs, which every golden config leaves at its
// default 0 (none of these scenarios use the Goldman wheel). One-time HASH-
// LAYOUT recapture only (CLAUDE.md determinism contract rule 5), same shape
// as the team-wiring update above. Golden A is unaffected (0 players -> 0
// new mix words; its hash constant below is UNCHANGED, verified). Golden D's
// kExpectedRng and golden E's final rng/bounces are UNCHANGED (verified
// byte-for-byte before recapturing the hashes below) — confirming clogs adds
// no new RNG draws and no gameplay change on the zero-clogs path.
//
// UPDATE 2026-07-09 (campaign rover/ghost hazards, docs/re/campaign.md
// "Rover/ghost/AI roster", "Per-tick mover"): hash.cpp now mixes
// State::rovers (a count word + per-entry words) and
// State::campaign_hazards_active/hazard_clear_timer (one packed word) —
// three new hash TERMS, all after the bombs block. Every golden scenario's
// MatchConfig leaves campaign_rovers/campaign_ghosts at their default 0, so
// `rovers` stays empty (mix(0) for the count word, zero per-entry words) and
// campaign_hazards_active/hazard_clear_timer stay false/0 (mix(0)) for the
// ENTIRE run — RoverSystem::tick's first action is an early return on
// `!s.campaign_hazards_active`, so it draws no RNG and touches no other
// state. One-time HASH-LAYOUT recapture only (CLAUDE.md determinism contract
// rule 5), same shape as the two updates above. Golden D's kExpectedRng and
// golden E's final rng/bounces are UNCHANGED (verified byte-for-byte before
// recapturing the hashes below) — confirming rovers/ghosts add no new RNG
// draws and no gameplay change on every existing (zero-hazard) scenario.
//
// UPDATE 2026-07-09 (per-level tile regeneration + ice/input-lag, docs/re/
// facts.md "Per-level tile regeneration" / "Ice / input-lag"): hash.cpp now
// mixes State::regen_timer (one word, right after dud_gate) and, per present
// player, Player::ice_history (a 30-entry ring buffer, 4 packed words, right
// after the disease word) — TWO new hash TERMS. Every golden MatchConfig
// leaves Tuning::level_index at its default 0 ("new traditionalist"), whose
// regen_seconds/ice_delay_ms are both 0 — TileRegenSystem::update() and
// MovementSystem::ice_delay() both take their very first early-return branch
// every tick for every scenario, so regen_timer never moves off 0 and
// ice_history is never written. One-time HASH-LAYOUT recapture only (CLAUDE.md
// determinism contract rule 5), same shape as the updates above. Verified by
// running the full suite before/after this change: every non-hash assertion
// in this file — golden A's final rng, golden D's kExpectedRng at all four
// checkpoints, golden E's bounce count (7) and final rng — is BYTE-IDENTICAL
// (17 of 24 assertions in this file are the hash checks that moved; the
// other 7 all passed unchanged), proving zero extra RNG draws and zero
// gameplay change on every existing scenario.
//
// UPDATE 2026-07-10 (core-feel fidelity audit, docs/re/facts.md "Core-feel
// audit 2026-07-10" + "Dud bombs" units correction): a DELIBERATE behaviour
// recapture, not a layout-only one. The changes and their per-scenario reach:
//   1. Dud gate units seconds->ticks x20 (VALUELST 320/321's own legend) and
//      literal `+=` re-arm: every build_state() digest shifts because the
//      hashed dud_gate VALUE grew x20 (same single setup RNG draw). Within
//      3000/1500-tick runs the gate (>= 3600 ticks) never opens, so B's and
//      C's in-run dud re-arm+roll draws VANISH.
//   2. Kick fidelity (sub_41EC84 `!v35` in-loop probe): the kick now fires on
//      the ARRIVAL tick (was one tick later), and a bomb sliding in another
//      direction is snapped + REDIRECTED (sub_42464B). Kick+action2 stops own
//      sliding bombs (sub_4247C5). Reaches B (kick players) and E (the
//      choreography kicks earlier and redirects the returning jelly bomb:
//      bounce count 7 -> 10, veer-roll RNG stream shifts).
//   3. Eviction scatter (sub_41E16A) + trigger downgrade (sub_424C47), spooge
//      owner-gate/player-stop/fuse-stagger, throw fuse restart, grab-while-
//      sliding: reach B (all gloves born_with) and C (born_with trigger).
//   4. Reversed disease applied to the RESOLVED dir, humans only (sub_41F29B
//      ~23049): reaches D (disease gauntlet) — movement-only, RNG-neutral.
// Hash-layout growth rides along: per-bomb fuse_init + stop_pending words
// (zero-bomb scenarios unaffected). PROOFS run before recapture: golden A
// passes BYTE-IDENTICAL (old constant kept — no players, no bombs, bare-ctor
// state has dud_gate 0). Golden D's kExpectedRng is byte-identical at all
// four checkpoints, and with the x20 temporarily reverted D's ticks 200-600
// hashes reproduce the OLD constants exactly — isolating D's delta to the
// dud_gate value plus one RNG-neutral reversal divergence in ticks 600-800.
//
// UPDATE 2026-07-10 (disease-system fidelity audit, docs/re/facts.md
// "Disease system fidelity audit 2026-07-10"): a DELIBERATE behaviour
// recapture, reaching D ONLY (the sole scenario with active diseases).
//   1. DiseaseSystem::spread_and_age() now ages/expires each player BEFORE
//      scanning them as a contagion source, not after (sub_41F29B: per
//      player, freshness--, then age+=frameDelta/cure, THEN that SAME
//      player's own contagion scan — all before the next player's slot). A
//      disease that expires this tick no longer spreads on its last tick,
//      and a surviving disease transmits its post-age value, not last
//      tick's.
//   2. Disease aging and contagion (both source and target eligibility) are
//      now frozen for stunned players (`if (!+8)` wraps the whole block in
//      the original), matching the "present && alive && stun==0" valid-
//      other-player test used everywhere else in this codebase (ai.cpp
//      etc.).
//   3. DiseaseSystem::give()'s Swap no longer swaps move_budget —
//      sub_41DFB6's XOR trick only ever swaps the two integer-pixel
//      position fields (+0x1c/+0x20, our x/y).
// None of the three add or remove an RNG draw: kExpectedRng below is
// BYTE-IDENTICAL at all four checkpoints (verified before recapturing the
// hashes) — pure state/ordering fixes, no new randomness. Golden A/B/C/E are
// unaffected (full suite run before/after: every one of their assertions is
// byte-identical). Golden D's OWN tick 200/400 checkpoints are ALSO
// byte-identical — the fix only bites once a disease is actually contagious,
// aging past expiry, or adjacent to a stun in the 400-600 tick window — so
// only kExpectedHash[2]/[3] (ticks 600/800) move below.
//
// CORRECTION 2026-07-10 (offset +8/+58 mislabel, docs/re/facts.md "Stun does
// NOT gate flame-death or pickup" + "Disease system fidelity audit" point 3
// CORRECTED): the audit above got point 2 WRONG. The `if (!+8)` block that
// wraps disease aging/contagion is the ALIVE gate (+8 = died-this-round flag),
// NOT a "not stunned" gate — the +58 head-hit stun is a separate WORD,
// decremented INSIDE that same block (~22982). The spurious `stun == 0` /
// `stun > 0` guards point 2 added to DiseaseSystem (and the matching ones in
// ai.cpp) have been REMOVED: a merely-stunned-but-alive player now ages,
// spreads/catches, and is a valid swap target, exactly as the original.
// This revert is INERT in golden — scenario D never produces a stunned player
// (no punch/grab gloves, action keys forced off below → no flying bombs → no
// head-hits → Player::stun stays 0 for the whole run), so the removed guards
// were never exercised. Every constant below (hashes AND kExpectedRng at all
// four checkpoints) is UNCHANGED and verified byte-identical before/after —
// NO recapture. Points 1 (age-then-spread) and 3 (no move_budget swap) stand.

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
    CHECK(a.hash() == 0x5189198a15a7c8e4ull);
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
    CHECK(s.hash() == 0x57a58cd7591a0885ull);  // setup itself is pinned

    static constexpr std::uint64_t kExpected[6] = {
        0x57c354d638c66743ull,  // tick 500
        0xf78a6117e3a966c7ull,  // tick 1000
        0xac762307668aeff3ull,  // tick 1500
        0x8b7d40da72ac06b5ull,  // tick 2000
        0x678b77f30499a1ebull,  // tick 2500
        0xd26e903bfb21b042ull,  // tick 3000
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
    CHECK(s.hash() == 0x183f800195a7b93dull);
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

    // Ticks 600/800 recaptured (docs/re/facts.md "Flame-arm stops"): the
    // scenario's flame reaches a floor powerup between tick 400 and 600. The
    // arm now stops there instead of burning through, so the state — and
    // hence the hash — diverges from that point on. Ticks 200/400 are BYTE-
    // IDENTICAL to before the fix (proved by running both revisions), and the
    // RNG stream (kExpectedRng below) is completely unaffected at every
    // checkpoint: the fix adds no RNG draws, it only changes which tile the
    // arm's blank-tile ignite loop reaches next.
    //
    // Ticks 600/800 recaptured AGAIN 2026-07-10 (disease-system fidelity
    // audit, see the file-level UPDATE note above): age-before-spread
    // ordering + stun-frozen aging/contagion + the move_budget-swap removal.
    // Ticks 200/400 stay byte-identical to the "Flame-arm stops" constants
    // directly above (unchanged by this audit); kExpectedRng is unchanged.
    static constexpr std::uint64_t kExpectedHash[4] = {
        0x725cfee1548c97c7ull,  // tick 200
        0xdf043d8f1c91bfd1ull,  // tick 400
        0x76c4a6e9bff308b1ull,  // tick 600
        0x1421cc71c2a8a55eull,  // tick 800
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
    cfg.tuning.fuse_frames = 200;  // long fuse: room for the ping-pong
    cfg.tuning.start_with[0] = 3;  // three bombs
    cfg.born_with[static_cast<int>(PowerupType::Kick)] = true;
    cfg.born_with[static_cast<int>(PowerupType::Punch)] = true;
    cfg.born_with[static_cast<int>(PowerupType::Jelly)] = true;
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
        0x2127eecc535b0d1dull,  // tick 75
        0x2b023abf13ff90baull,  // tick 150
        0x47ed56eafc466f39ull,  // tick 225
        0xd897750a91e2131bull,  // tick 300
    };
    int bounces = 0;
    for (std::uint64_t t = 0; t < 300; ++t) {
        s.tick(script(t));
        for (const auto& e : s.state().events)
            if (e.type == Event::Type::JellyBounced) ++bounces;
        if ((t + 1) % 75 == 0) CHECK(s.hash() == kExpected[(t + 1) / 75 - 1]);
    }
    // 10 with the audit's kick fidelity (was 7): the kick lands on the walk-up's
    // ARRIVAL tick and the returning jelly bomb is REDIRECTED east by the still-
    // facing player (sub_42464B) instead of waiting for the body-block reverse,
    // so the ping-pong starts sooner and completes more legs in 300 ticks.
    CHECK(bounces == 10);                 // the ping-pong really happened
    CHECK(s.state().rng == 0x405862fbu);  // the veer roll really consumed RNG
}
