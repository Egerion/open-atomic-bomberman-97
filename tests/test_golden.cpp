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
// CORRECTED): the disease audit above got point 2 WRONG. The `if (!+8)` block
// that wraps disease aging/contagion is the ALIVE gate (+8 = died-this-round
// flag), NOT a "not stunned" gate — the +58 head-hit stun is a separate WORD,
// decremented INSIDE that same block (~22982). The spurious `stun == 0` /
// `stun > 0` guards point 2 added to DiseaseSystem (and the matching ones in
// ai.cpp) have been REMOVED: a merely-stunned-but-alive player now ages,
// spreads/catches, and is a valid swap target, exactly as the original. This
// revert is INERT in golden — scenario D never produces a stunned player (no
// punch/grab gloves, action keys forced off below → no flying bombs → no
// head-hits → Player::stun stays 0 for the whole run), so the removed guards
// were never exercised. D's hashes below (already recaptured for the flame
// audit) and its kExpectedRng are UNCHANGED by this revert (verified
// byte-identical before/after). Points 1 (age-then-spread) and 3 (no
// move_budget swap) stand.
//
// UPDATE 2026-07-10 (stunned-but-alive movement, docs/re/facts.md "Head hit"
// / "Stun does NOT gate flame-death or pickup" RESOLVED box): player_turn's
// full early-return on `stun > 0` is replaced by decrement-and-fall-through —
// stun now skips ONLY the input decode (want_godir forced -1, the skipped
// sub_41E61E) and the bomb-action block (the +56/+57 key bytes' per-tick 0
// reset), while the stage-actor mover still runs (a conveyor keeps carrying a
// stunned player; belt-driven kick probes and warp/trampoline step-ons still
// fire). PROVEN INERT here, zero recapture: no golden board lays any stage
// actor, so the newly-executing mover path moves nothing for a stunned
// player; the action-block skip is behaviourally identical to the old early-
// return (same prev_action1/2 updates); no RNG draw is added, removed, or
// reordered. Full suite run on the change: every constant in this file — all
// hashes, D's kExpectedRng at all four checkpoints, E's bounce count (10) and
// final rng — passes UNCHANGED. Pinned by tests/test_conveyor.cpp's two
// stun cases (belt carry during stun; no new input / no coasting).
//
// UPDATE 2026-07-10 (enclosure/HURRY arithmetic audit, docs/re/enclosure.md):
// a DELIBERATE behaviour recapture in EnclosureSystem, all RNG-neutral (the
// enclosure draws zero State::rng — every kExpectedRng/final-rng assertion in
// this file is UNCHANGED, verified byte-for-byte before recapturing hashes):
//   1. Trigger-boundary arithmetic: `warn`/`closing` used to compare raw
//      ticks_left against threshold*kTicksPerSecond directly, which is NOT
//      the same predicate as the original's whole-seconds comparison (it
//      silently rounds the wrong way — see the audit report). Now floors
//      ticks_left/kTicksPerSecond once and compares that against the
//      threshold with the original's exact strictness (`<` for the banner,
//      `<=` for the wall-arm). Net effect: the banner fires 1 tick later, the
//      walls ARM 19 ticks EARLIER, than before this fix.
//   2. Spiral cadence: EnclosureSystem::total/position now replay
//      sub_426818's own advance/accept-or-turn state machine tile-for-tile
//      (including its "phantom" same-tile repeats at 3 of a ring's 4 corners
//      and the ordinary — non-phantom — second visit to each ring's own
//      start tile) instead of a hand-derived ring-perimeter formula that
//      emitted exactly one event per unique tile. Every ring now takes 4
//      EXTRA 5-tick cadence slots (52 events for ring 0's 48 unique tiles,
//      not 48) to close, so every wall from the first ring corner onward
//      lands several ticks later than before this fix, compounding per ring.
//   3. Wall-triggered bomb detonation now fires the tick AFTER the wall
//      drops (forces the bomb's fuse to fire on the sim's own next
//      tick_fuses() pass), not synchronously on the drop tick — mirroring
//      sub_423209's queue-and-drain-next-frame behaviour (sub_42331C).
//   4. A player mid-trampoline-bounce or mid-warp when a wall drops on their
//      tile is no longer crushed (sub_41DE63's states-5/6/7 early-out) —
//      unreached by every existing scenario (none combine bounce/warp state
//      with the wall-drop phase), so this is a no-op here.
// Reaches B (game_seconds defaults to 150 -> hurry/wall phase within the
// 3000-tick run: checkpoints 500/1000/1500 stay BYTE-IDENTICAL — proved by
// running both revisions — since the banner/arm/first-ring tiles all land
// well after tick 1500; checkpoints 2000/2500/3000 move) and C (game_seconds
// = 70, deliberately picked to reach the wall phase inside a short 1500-tick
// run — the whole point of the "fast hurry phase" scenario, so its single
// tick-1500 hash moves). Goldens A/D/E are unaffected: A has no clock
// (dormant, ticks_left seeded huge); D/E run 800/300 ticks and never reach
// even the banner (~tick 1800 for their game_seconds=150 default).

// UPDATE 2026-07-10 (flame-system fidelity audit, docs/re/facts.md "Chain-
// reaction timing" / "Brick crumble timing" / exp_ resolution in "Bomb/
// warphole reconciliation"): a DELIBERATE behaviour recapture. Three
// findings, all reaching virtually every scenario that ever places a bomb:
//   1. Chain reactions are NOT instantaneous (sub_423209's queue: a flame
//      arm that reaches another bomb, a trigger-button press, a flying bomb
//      landing on flame, and a sliding bomb entering flame all QUEUE their
//      target instead of exploding it — it detonates at the START of the
//      NEXT tick, one LINK of a chain per tick). Ownership transfers to the
//      triggering bomb at queue time (flame/kill attribution follows the
//      player who actually set it off), and an arm-chained bomb skips
//      re-blasting back toward the flame that triggered it.
//   2. A brick stays Brick (blocking) for its whole crumble — the tile only
//      opens up once `burning` reaches 0 — but its hidden powerup reveals
//      immediately at ignition, well before that. Previously the cell went
//      Blank (and the powerup revealed) both at the wrong end.
//   3. A flying (punched/thrown) bomb cannot land on a WARPHOLE tile either
//      (hops onward like it does over a wall) — the `exp_` term in that
//      landing check is confirmed dead code (disassembly-verified: a
//      hardcoded non-null pointer tested for truthiness, never zero), so
//      the real condition is the same "type 1 blocks" rule already ported
//      for the sliding-bomb probe.
// Hash-layout growth rides along: a per-bomb `id` word, a `next_bomb_id`
// counter, and the (usually-empty) pending-chain queue.
//
// These changes reach every scenario with any bomb activity, so B, C, D, E
// all recapture. Golden A (0 players, 0 bombs, 10000 ticks of nothing) is
// the control: proved BYTE-IDENTICAL in behaviour — its hash still moves,
// but only from the layout growth (next_bomb_id/pending_chain are always
// mix(1)/mix(0) there), and its pinned `rng` value is UNCHANGED. For B-E,
// every non-hash assertion in this file — golden D's kExpectedRng at all
// four checkpoints, golden E's bounce count (10) and final rng — is
// BYTE-IDENTICAL before and after this change (verified: only the 17 hash
// checks moved, all 7 other assertions passed unchanged), proving the fix
// adds no RNG draws anywhere: it only changes WHEN a chain-queued bomb
// actually detonates and WHEN a brick tile opens up, never the random
// stream. Focused, hand-verifiable coverage for the new mechanics lives in
// tests/test_sim.cpp ("flame arm stops at a bomb it chain-detonates...",
// "bomb explodes at its fuse and burns the brick") and
// tests/test_kick_nuances.cpp / tests/test_trigger_allowance.cpp (updated
// for the one-tick chain defer — their assertions already had enough slack
// to pass either way, but their comments now say so honestly).

// UPDATE 2026-07-10 (explosion-draw fidelity audit, docs/re/facts.md "Flame
// arm-shape selection"): a HASH-LAYOUT-ONLY recapture — the ONLY change this
// update covers is the new `State::flame_kind` field (which flame-arm PIECE
// a lit cell draws: tip/mid/center, per direction). It is pure DERIVED data,
// computed at ignition from the SAME `from_dir`/reach inputs
// `FlameSystem::spread_to`/`ignite_epicentre` already consume — no new
// branch that changes what ignites or when, no new RNG draw. It replaces the
// presentation layer's previous live neighbour-scan (which had its own,
// separate, undocumented bugs — see the facts.md entry) with a faithful,
// cast-time-decided value the renderer now just looks up.
//
// Packed into the two previously-unused spare bytes of the per-tile hash
// word `cells`/`hidden`/`floor`/`flame`/`burning`/`flame_owner` already
// share (hash.cpp), NOT a new mix() call — so a scenario that never ignites
// a single flame cell hashes BYTE-IDENTICAL, not just behaviourally
// equivalent. Golden A (0 bombs, ever) is exactly that case: its hash below
// is UNCHANGED (still 0xb9f782f923ce72c5) — the first golden-hash update in
// this file's history that does NOT need to touch A. B/C/D/E all place and
// explode bombs, so their per-checkpoint hashes downstream of the first
// ignition move: B (bomb activity from tick 0) recaptures all 6 checkpoints;
// C (single checkpoint, after its trigger bombs have fired) recaptures its
// one hash; D and E's EARLY checkpoints (200/400 for D, 75/150 for E) are
// BYTE-IDENTICAL — no bomb has exploded yet at those points — only their
// LATER checkpoints (600/800 for D, 225/300 for E, after the first
// explosion) move.
//
// RNG-neutrality proof (this run, before recapturing the hashes below):
// golden A's pinned `rng` is unchanged; golden D's `kExpectedRng` passes at
// all four checkpoints (untouched by this update); golden E's bounce count
// (10) and final `rng` (0x405862fbu) both pass unchanged. Only hash checks
// moved — 11 of this file's 24 assertions — confirming flame_kind changes
// no gameplay, only what gets recorded about a tile that was already going
// to ignite. This recapture is self-contained to flame_kind alone; if
// another concurrent change also touches these constants, reconcile by
// re-running both fixes together rather than merging hex values by hand.
//
// UPDATE 2026-07-11 (death powerup scatter, docs/re/facts.md "Death powerup
// scatter"): a DELIBERATE behaviour recapture. A dying player now scatters
// EVERY powerup it accumulated above its VALUELST start-with baseline back
// onto random floor tiles (sub_41DBFE, reached from the shared death funnel
// sub_41DE63 -> the death-animation-end scatter) — a genuinely unported
// mechanic. The scatter draws State::rng ONLY for sub_4255B2's per-token tile
// selection, in kind order (no kind roll, no count roll — that is what
// distinguishes it from the head hit). New draws land ONLY on a death tick, so
// the reach is every scenario that produces a death:
//   - B (4-player brick brawl): first death at tick 39, so all six 500-tick
//     checkpoints move.
//   - C (trigger duel): first death at tick 10, its single tick-1500 hash
//     moves.
//   - D (disease gauntlet): the auto-drop diseases (diarrhea/ebola) make the
//     otherwise-key-silent players lay bombs; the first flame death is tick
//     487, so checkpoints 200/400 stay BYTE-IDENTICAL (hash AND kExpectedRng)
//     and only 600/800 move.
//   - E (jelly choreography): first death at tick 231, so 75/150/225 stay
//     BYTE-IDENTICAL and only tick 300 + the final rng move.
// ISOLATION PROOF (run this revision against the sim reverted to main, both
// with the same instrumentation): the first-death TICK is identical on both
// (39/10/487/231 — the deaths themselves are unchanged, only the scatter is
// added), AND the full state hash + rng captured at the END of the tick BEFORE
// each first death are byte-identical across the two builds (B end-of-38 hash
// 0x89459d..., C end-of-9, D end-of-486, E end-of-230). Every divergence is
// therefore confined to the death tick's new scatter draws. Golden A is
// unaffected (no players, no deaths — its hash and rng are UNCHANGED). This
// recapture is self-contained to the death scatter; if a concurrent change
// also touches these constants, reconcile by re-running both together rather
// than merging hex by hand.
//
// UPDATE 2026-07-11 (chain slot transfer, docs/re/facts.md "Bomb capacity is
// a derived live-bomb count"): the chain ownership transfer in
// FlameSystem::spread_to now moves the PLACEMENT SLOT with the owner word
// (sub_4245DA derives capacity from the same +62 word the transfer rewrites)
// — fixing the permanent bombs_placed leak behind the live-play "5 max bombs,
// suddenly one placeable" collapse. PROVEN INERT here, zero recapture: the
// fix's only delta is inside `if (hit->owner != owner)`, bombs_placed is
// hashed, and a cross-owner transfer permanently changes the victim's counter
// — so any golden that ever chained across owners would have moved its
// downstream checkpoints. All five scenarios pass BYTE-IDENTICAL with the fix
// in place (all 24 assertions): no golden ever chains across owners. Pinned
// by tests/test_chain_slot.cpp.
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
    CHECK(a.hash() == 0xb9f782f923ce72c5ull);
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
    CHECK(s.hash() == 0x66be0a37b86e9b94ull);  // setup itself is pinned

    static constexpr std::uint64_t kExpected[6] = {
        0x105f4d7a3bf6ed26ull,  // tick 500
        0x03732ecd4c85b2feull,  // tick 1000
        0x919458232e2014b6ull,  // tick 1500
        0xd48820b65fdca5fbull,  // tick 2000
        0xc0ccfa2533a4ea76ull,  // tick 2500
        0x388c59156818b30bull,  // tick 3000
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
    CHECK(s.hash() == 0x06581d909d647db0ull);
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
    //
    // Ticks 600/800 recaptured a THIRD time 2026-07-10 (explosion-draw
    // fidelity audit / `flame_kind`, see the file-level UPDATE note above):
    // ticks 200/400 stay byte-identical (this scenario's first bomb hasn't
    // exploded yet at either checkpoint); kExpectedRng is unchanged.
    static constexpr std::uint64_t kExpectedHash[4] = {
        0xb0284a38351747a2ull,  // tick 200 (unchanged: before the first death)
        0x3ce7c5c7298f1eb8ull,  // tick 400 (unchanged: before the first death)
        0x5c307422fa29e961ull,  // tick 600
        0x461fa6be7d9078ceull,  // tick 800
    };
    static constexpr std::uint32_t kExpectedRng[4] = {0xca47489cu, 0x49cffff6u, 0xdd6d0230u,
                                                      0xa9af166bu};
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
        0x9d00c5fc62311dbdull,  // tick 75  (unchanged: before the first death)
        0xd48a974feb70ee26ull,  // tick 150 (unchanged: before the first death)
        0x46e776f3df49b380ull,  // tick 225 (unchanged: first death is tick 231)
        0xd9aff8e9a93ca5e7ull,  // tick 300
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
    CHECK(s.state().rng == 0xc6a9f3b2u);  // the veer roll really consumed RNG
}
