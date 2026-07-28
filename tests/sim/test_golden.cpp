// Golden-hash regression tests. These scenarios were captured from the
// pre-refactor simulation (2026-07-03) and pin the EXACT behaviour: state
// hash, RNG stream position, everything.
//
// If one of these fails you changed gameplay behaviour. That is either a bug
// (fix it) or a deliberate faithfulness improvement from new RE facts â€” in
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
    // Disarm the round-start input freeze (facts.md "Round-start input
    // freeze", VALUELST id 30 â‰ˆ 1 s of dead input): these scenarios were
    // captured acting from tick 0 and golden E's scripted choreography
    // depends on it; tests/test_freeze.cpp pins the freeze itself.
    cfg.tuning.input_freeze_ticks = 0;
    return cfg;
}

}  // namespace

// NOTE 2026-07-08: every constant below was recaptured. Root-cause: commit
// 21f6187 ("Phase 1 mechanic-fidelity sweep") shipped this file's constants
// ALREADY WRONG â€” even checked out at that exact commit, none of the five
// scenarios reproduce their own pinned hashes (verified byte-for-byte; the
// "tests green" claim in that commit's message was never true). Two genuine
// sim bugs rode along on top of that broken baseline and are fixed here:
//   1. Conveyor "no input" push (StageActorSystem::move_on_actor case (a))
//      added the player's own speed on top of conveyor_speed, contradicting
//      sub_41F29B's case (a) pseudocode (docs/re/stage-actors.md Â§3), which
//      never reads the player's speed when there is no input. Fixed by a new
//      MovementSystem::move(..., use_player_speed) parameter, false only for
//      the belt-forced/no-input case.
//   2. EnclosureSystem::update() gated `warn`/`closing` on `ticks_left > 0`,
//      so the wall spiral froze solid the instant the match clock hit zero.
//      The original's remaining-seconds predicate (sub_410578, clamped >= 0)
//      never re-freezes once armed â€” the walls keep closing through sudden
//      death (docs/re/enclosure.md Â§2, which already documents golden A/D/E
//      as relying on never reaching the clock at all). Fixed by dropping the
//      guard; our ticks_left is likewise clamped at 0 so the predicate stays
//      monotonic.
// Also, commit b0dc533 (AI port) added a `brains` hash block (hash.cpp) that
// mixes kMaxPlayers Brain-sized zero words into EVERY scenario's digest â€”
// including golden A, which has no AI and no players at all â€” without
// recapturing this file, growing the hash layout out from under the pinned
// constants a second time.
//
// UPDATE 2026-07-08 (TEAM wiring, docs/re/ai.md TEAM follow-up /
// docs/re/setup-screens.md +84 byte): hash.cpp now mixes a `Player::team` word
// per present player (one new mix() call per player, right after
// trigger_placed). Every golden scenario's players default team=0 (no config
// sets MatchConfig::team[]), so gameplay is byte-identical â€” this is a
// one-time HASH-LAYOUT recapture only (CLAUDE.md determinism contract rule 5),
// same shape as the b0dc533 brains-block growth above. Golden A is unaffected
// (0 players -> 0 new mix words). Golden D's pinned RNG-stream values
// (kExpectedRng) are UNCHANGED by this commit â€” verified byte-for-byte before
// recapturing the hashes below â€” confirming the team wiring adds no new RNG
// draws on the untamed (all-zero-team) path.
// Two test-fixture-only fixes ride along (no sim behaviour change, just
// removing an accidental dependency on the ticks_left==0 "no clock" edge
// case that the enclosure fix above turned into "sudden death from tick 0"):
// golden A and tests/test_ai.cpp's open_arena() now set an explicit, large
// ticks_left so these clock-free scratch scenarios stay clock-free, matching
// this file's own "empty state"/"no clock" framing.
//
// UPDATE 2026-07-09 (Goldman wheel clogs, docs/re/goldman-roulette.md Â§9):
// hash.cpp now mixes a `Player::clogs` word per present player (right after
// `Player::team`) â€” a speed-penalty count fed ONLY by the new
// MatchConfig::born_with_clogs, which every golden config leaves at its
// default 0 (none of these scenarios use the Goldman wheel). One-time HASH-
// LAYOUT recapture only (CLAUDE.md determinism contract rule 5), same shape
// as the team-wiring update above. Golden A is unaffected (0 players -> 0
// new mix words; its hash constant below is UNCHANGED, verified). Golden D's
// kExpectedRng and golden E's final rng/bounces are UNCHANGED (verified
// byte-for-byte before recapturing the hashes below) â€” confirming clogs adds
// no new RNG draws and no gameplay change on the zero-clogs path.
//
// UPDATE 2026-07-09 (campaign rover/ghost hazards, docs/re/campaign.md
// "Rover/ghost/AI roster", "Per-tick mover"): hash.cpp now mixes
// State::rovers (a count word + per-entry words) and
// State::campaign_hazards_active/hazard_clear_timer (one packed word) â€”
// three new hash TERMS, all after the bombs block. Every golden scenario's
// MatchConfig leaves campaign_rovers/campaign_ghosts at their default 0, so
// `rovers` stays empty (mix(0) for the count word, zero per-entry words) and
// campaign_hazards_active/hazard_clear_timer stay false/0 (mix(0)) for the
// ENTIRE run â€” RoverSystem::tick's first action is an early return on
// `!s.campaign_hazards_active`, so it draws no RNG and touches no other
// state. One-time HASH-LAYOUT recapture only (CLAUDE.md determinism contract
// rule 5), same shape as the two updates above. Golden D's kExpectedRng and
// golden E's final rng/bounces are UNCHANGED (verified byte-for-byte before
// recapturing the hashes below) â€” confirming rovers/ghosts add no new RNG
// draws and no gameplay change on every existing (zero-hazard) scenario.
//
// UPDATE 2026-07-09 (per-level tile regeneration + ice/input-lag, docs/re/
// facts.md "Per-level tile regeneration" / "Ice / input-lag"): hash.cpp now
// mixes State::regen_timer (one word, right after dud_gate) and, per present
// player, Player::ice_history (a 30-entry ring buffer, 4 packed words, right
// after the disease word) â€” TWO new hash TERMS. Every golden MatchConfig
// leaves Tuning::level_index at its default 0 ("new traditionalist"), whose
// regen_seconds/ice_delay_ms are both 0 â€” TileRegenSystem::update() and
// MovementSystem::ice_delay() both take their very first early-return branch
// every tick for every scenario, so regen_timer never moves off 0 and
// ice_history is never written. One-time HASH-LAYOUT recapture only (CLAUDE.md
// determinism contract rule 5), same shape as the updates above. Verified by
// running the full suite before/after this change: every non-hash assertion
// in this file â€” golden A's final rng, golden D's kExpectedRng at all four
// checkpoints, golden E's bounce count (7) and final rng â€” is BYTE-IDENTICAL
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
//   2. Kick fidelity (sub_41EC84's in-loop kick probe, the branch taken when
//      the along-axis offset to the tile centre is zero): the kick now fires on
//      the ARRIVAL tick (was one tick later), and a bomb sliding in another
//      direction is snapped + REDIRECTED (sub_42464B). Kick+action2 stops own
//      sliding bombs (sub_4247C5). Reaches B (kick players) and E (the
//      choreography kicks earlier and redirects the returning jelly bomb:
//      bounce count 7 -> 10, veer-roll RNG stream shifts).
//   3. Eviction scatter (sub_41E16A) + trigger downgrade (sub_424C47), spooge
//      owner-gate/player-stop/fuse-stagger, throw fuse restart, grab-while-
//      sliding: reach B (all gloves born_with) and C (born_with trigger).
//   4. Reversed disease applied to the RESOLVED dir, humans only (sub_41F29B
//      ~23049): reaches D (disease gauntlet) â€” movement-only, RNG-neutral.
// Hash-layout growth rides along: per-bomb fuse_init + stop_pending words
// (zero-bomb scenarios unaffected). PROOFS run before recapture: golden A
// passes BYTE-IDENTICAL (old constant kept â€” no players, no bombs, bare-ctor
// state has dud_gate 0). Golden D's kExpectedRng is byte-identical at all
// four checkpoints, and with the x20 temporarily reverted D's ticks 200-600
// hashes reproduce the OLD constants exactly â€” isolating D's delta to the
// dud_gate value plus one RNG-neutral reversal divergence in ticks 600-800.
//
// UPDATE 2026-07-10 (disease-system fidelity audit, docs/re/facts.md
// "Disease system fidelity audit 2026-07-10"): a DELIBERATE behaviour
// recapture, reaching D ONLY (the sole scenario with active diseases).
//   1. DiseaseSystem::spread_and_age() now ages/expires each player BEFORE
//      scanning them as a contagion source, not after (sub_41F29B: per
//      player, freshness--, then age+=frameDelta/cure, THEN that SAME
//      player's own contagion scan â€” all before the next player's slot). A
//      disease that expires this tick no longer spreads on its last tick,
//      and a surviving disease transmits its post-age value, not last
//      tick's.
//   2. Disease aging and contagion (both source and target eligibility) are
//      now frozen for stunned players (a gate on +8 being clear wraps the
//      whole block in
//      the original), matching the "present && alive && stun==0" valid-
//      other-player test used everywhere else in this codebase (ai.cpp
//      etc.).
//   3. DiseaseSystem::give()'s Swap no longer swaps move_budget â€”
//      sub_41DFB6's XOR trick only ever swaps the two integer-pixel
//      position fields (+0x1c/+0x20, our x/y).
// None of the three add or remove an RNG draw: kExpectedRng below is
// BYTE-IDENTICAL at all four checkpoints (verified before recapturing the
// hashes) â€” pure state/ordering fixes, no new randomness. Golden A/B/C/E are
// unaffected (full suite run before/after: every one of their assertions is
// byte-identical). Golden D's OWN tick 200/400 checkpoints are ALSO
// byte-identical â€” the fix only bites once a disease is actually contagious,
// aging past expiry, or adjacent to a stun in the 400-600 tick window â€” so
// only kExpectedHash[2]/[3] (ticks 600/800) move below.
//
// CORRECTION 2026-07-10 (offset +8/+58 mislabel, docs/re/facts.md "Stun does
// NOT gate flame-death or pickup" + "Disease system fidelity audit" point 3
// CORRECTED): the disease audit above got point 2 WRONG. The +8-is-clear block
// that wraps disease aging/contagion is the ALIVE gate (+8 = died-this-round
// flag), NOT a "not stunned" gate â€” the +58 head-hit stun is a separate WORD,
// decremented INSIDE that same block (~22982). The spurious `stun == 0` /
// `stun > 0` guards point 2 added to DiseaseSystem (and the matching ones in
// ai.cpp) have been REMOVED: a merely-stunned-but-alive player now ages,
// spreads/catches, and is a valid swap target, exactly as the original. This
// revert is INERT in golden â€” scenario D never produces a stunned player (no
// punch/grab gloves, action keys forced off below â†’ no flying bombs â†’ no
// head-hits â†’ Player::stun stays 0 for the whole run), so the removed guards
// were never exercised. D's hashes below (already recaptured for the flame
// audit) and its kExpectedRng are UNCHANGED by this revert (verified
// byte-identical before/after). Points 1 (age-then-spread) and 3 (no
// move_budget swap) stand.
//
// UPDATE 2026-07-10 (stunned-but-alive movement, docs/re/facts.md "Head hit"
// / "Stun does NOT gate flame-death or pickup" RESOLVED box): player_turn's
// full early-return on `stun > 0` is replaced by decrement-and-fall-through â€”
// stun now skips ONLY the input decode (want_godir forced -1, the skipped
// sub_41E61E) and the bomb-action block (the +56/+57 key bytes' per-tick 0
// reset), while the stage-actor mover still runs (a conveyor keeps carrying a
// stunned player; belt-driven kick probes and warp/trampoline step-ons still
// fire). PROVEN INERT here, zero recapture: no golden board lays any stage
// actor, so the newly-executing mover path moves nothing for a stunned
// player; the action-block skip is behaviourally identical to the old early-
// return (same prev_action1/2 updates); no RNG draw is added, removed, or
// reordered. Full suite run on the change: every constant in this file â€” all
// hashes, D's kExpectedRng at all four checkpoints, E's bounce count (10) and
// final rng â€” passes UNCHANGED. Pinned by tests/test_conveyor.cpp's two
// stun cases (belt carry during stun; no new input / no coasting).
//
// UPDATE 2026-07-10 (enclosure/HURRY arithmetic audit, docs/re/enclosure.md):
// a DELIBERATE behaviour recapture in EnclosureSystem, all RNG-neutral (the
// enclosure draws zero State::rng â€” every kExpectedRng/final-rng assertion in
// this file is UNCHANGED, verified byte-for-byte before recapturing hashes):
//   1. Trigger-boundary arithmetic: `warn`/`closing` used to compare raw
//      ticks_left against threshold*kTicksPerSecond directly, which is NOT
//      the same predicate as the original's whole-seconds comparison (it
//      silently rounds the wrong way â€” see the audit report). Now floors
//      ticks_left/kTicksPerSecond once and compares that against the
//      threshold with the original's exact strictness (`<` for the banner,
//      `<=` for the wall-arm). Net effect: the banner fires 1 tick later, the
//      walls ARM 19 ticks EARLIER, than before this fix.
//   2. Spiral cadence: EnclosureSystem::total/position now replay
//      sub_426818's own advance/accept-or-turn state machine tile-for-tile
//      (including its "phantom" same-tile repeats at 3 of a ring's 4 corners
//      and the ordinary â€” non-phantom â€” second visit to each ring's own
//      start tile) instead of a hand-derived ring-perimeter formula that
//      emitted exactly one event per unique tile. Every ring now takes 4
//      EXTRA 5-tick cadence slots (52 events for ring 0's 48 unique tiles,
//      not 48) to close, so every wall from the first ring corner onward
//      lands several ticks later than before this fix, compounding per ring.
//   3. Wall-triggered bomb detonation now fires the tick AFTER the wall
//      drops (forces the bomb's fuse to fire on the sim's own next
//      tick_fuses() pass), not synchronously on the drop tick â€” mirroring
//      sub_423209's queue-and-drain-next-frame behaviour (sub_42331C).
//   4. A player mid-trampoline-bounce or mid-warp when a wall drops on their
//      tile is no longer crushed (sub_41DE63's states-5/6/7 early-out) â€”
//      unreached by every existing scenario (none combine bounce/warp state
//      with the wall-drop phase), so this is a no-op here.
// Reaches B (game_seconds defaults to 150 -> hurry/wall phase within the
// 3000-tick run: checkpoints 500/1000/1500 stay BYTE-IDENTICAL â€” proved by
// running both revisions â€” since the banner/arm/first-ring tiles all land
// well after tick 1500; checkpoints 2000/2500/3000 move) and C (game_seconds
// = 70, deliberately picked to reach the wall phase inside a short 1500-tick
// run â€” the whole point of the "fast hurry phase" scenario, so its single
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
//      target instead of exploding it â€” it detonates at the START of the
//      NEXT tick, one LINK of a chain per tick). Ownership transfers to the
//      triggering bomb at queue time (flame/kill attribution follows the
//      player who actually set it off), and an arm-chained bomb skips
//      re-blasting back toward the flame that triggered it.
//   2. A brick stays Brick (blocking) for its whole crumble â€” the tile only
//      opens up once `burning` reaches 0 â€” but its hidden powerup reveals
//      immediately at ignition, well before that. Previously the cell went
//      Blank (and the powerup revealed) both at the wrong end.
//   3. A flying (punched/thrown) bomb cannot land on a WARPHOLE tile either
//      (hops onward like it does over a wall) â€” the `exp_` term in that
//      landing check is confirmed dead code (disassembly-verified: a
//      hardcoded non-null pointer tested for truthiness, never zero), so
//      the real condition is the same "type 1 blocks" rule already ported
//      for the sliding-bomb probe.
// Hash-layout growth rides along: a per-bomb `id` word, a `next_bomb_id`
// counter, and the (usually-empty) pending-chain queue.
//
// These changes reach every scenario with any bomb activity, so B, C, D, E
// all recapture. Golden A (0 players, 0 bombs, 10000 ticks of nothing) is
// the control: proved BYTE-IDENTICAL in behaviour â€” its hash still moves,
// but only from the layout growth (next_bomb_id/pending_chain are always
// mix(1)/mix(0) there), and its pinned `rng` value is UNCHANGED. For B-E,
// every non-hash assertion in this file â€” golden D's kExpectedRng at all
// four checkpoints, golden E's bounce count (10) and final rng â€” is
// BYTE-IDENTICAL before and after this change (verified: only the 17 hash
// checks moved, all 7 other assertions passed unchanged), proving the fix
// adds no RNG draws anywhere: it only changes WHEN a chain-queued bomb
// actually detonates and WHEN a brick tile opens up, never the random
// stream. Focused, hand-verifiable coverage for the new mechanics lives in
// tests/test_sim.cpp ("flame arm stops at a bomb it chain-detonates...",
// "bomb explodes at its fuse and burns the brick") and
// tests/test_kick_nuances.cpp / tests/test_trigger_allowance.cpp (updated
// for the one-tick chain defer â€” their assertions already had enough slack
// to pass either way, but their comments now say so honestly).

// UPDATE 2026-07-10 (explosion-draw fidelity audit, docs/re/facts.md "Flame
// arm-shape selection"): a HASH-LAYOUT-ONLY recapture â€” the ONLY change this
// update covers is the new `State::flame_kind` field (which flame-arm PIECE
// a lit cell draws: tip/mid/center, per direction). It is pure DERIVED data,
// computed at ignition from the SAME `from_dir`/reach inputs
// `FlameSystem::spread_to`/`ignite_epicentre` already consume â€” no new
// branch that changes what ignites or when, no new RNG draw. It replaces the
// presentation layer's previous live neighbour-scan (which had its own,
// separate, undocumented bugs â€” see the facts.md entry) with a faithful,
// cast-time-decided value the renderer now just looks up.
//
// Packed into the two previously-unused spare bytes of the per-tile hash
// word `cells`/`hidden`/`floor`/`flame`/`burning`/`flame_owner` already
// share (hash.cpp), NOT a new mix() call â€” so a scenario that never ignites
// a single flame cell hashes BYTE-IDENTICAL, not just behaviourally
// equivalent. Golden A (0 bombs, ever) is exactly that case: its hash below
// is UNCHANGED (still 0xb9f782f923ce72c5) â€” the first golden-hash update in
// this file's history that does NOT need to touch A. B/C/D/E all place and
// explode bombs, so their per-checkpoint hashes downstream of the first
// ignition move: B (bomb activity from tick 0) recaptures all 6 checkpoints;
// C (single checkpoint, after its trigger bombs have fired) recaptures its
// one hash; D and E's EARLY checkpoints (200/400 for D, 75/150 for E) are
// BYTE-IDENTICAL â€” no bomb has exploded yet at those points â€” only their
// LATER checkpoints (600/800 for D, 225/300 for E, after the first
// explosion) move.
//
// RNG-neutrality proof (this run, before recapturing the hashes below):
// golden A's pinned `rng` is unchanged; golden D's `kExpectedRng` passes at
// all four checkpoints (untouched by this update); golden E's bounce count
// (10) and final `rng` (0x405862fbu) both pass unchanged. Only hash checks
// moved â€” 11 of this file's 24 assertions â€” confirming flame_kind changes
// no gameplay, only what gets recorded about a tile that was already going
// to ignite. This recapture is self-contained to flame_kind alone; if
// another concurrent change also touches these constants, reconcile by
// re-running both fixes together rather than merging hex values by hand.
//
// UPDATE 2026-07-11 (death powerup scatter, docs/re/facts.md "Death powerup
// scatter"): a DELIBERATE behaviour recapture. A dying player now scatters
// EVERY powerup it accumulated above its VALUELST start-with baseline back
// onto random floor tiles (sub_41DBFE, reached from the shared death funnel
// sub_41DE63 -> the death-animation-end scatter) â€” a genuinely unported
// mechanic. The scatter draws State::rng ONLY for sub_4255B2's per-token tile
// selection, in kind order (no kind roll, no count roll â€” that is what
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
// (39/10/487/231 â€” the deaths themselves are unchanged, only the scatter is
// added), AND the full state hash + rng captured at the END of the tick BEFORE
// each first death are byte-identical across the two builds (B end-of-38 hash
// 0x89459d..., C end-of-9, D end-of-486, E end-of-230). Every divergence is
// therefore confined to the death tick's new scatter draws. Golden A is
// unaffected (no players, no deaths â€” its hash and rng are UNCHANGED). This
// recapture is self-contained to the death scatter; if a concurrent change
// also touches these constants, reconcile by re-running both together rather
// than merging hex by hand.
//
// UPDATE 2026-07-11 (chain slot transfer, docs/re/facts.md "Bomb capacity is
// a derived live-bomb count"): the chain ownership transfer in
// FlameSystem::spread_to now moves the PLACEMENT SLOT with the owner word
// (sub_4245DA derives capacity from the same +62 word the transfer rewrites)
// â€” fixing the permanent bombs_placed leak behind the live-play "5 max bombs,
// suddenly one placeable" collapse. PROVEN INERT here, zero recapture: the
// fix's only delta is inside `if (hit->owner != owner)`, bombs_placed is
// hashed, and a cross-owner transfer permanently changes the victim's counter
// â€” so any golden that ever chained across owners would have moved its
// downstream checkpoints. All five scenarios pass BYTE-IDENTICAL with the fix
// in place (all 24 assertions): no golden ever chains across owners. Pinned
// by tests/test_chain_slot.cpp.
//
// UPDATE 2026-07-11 (end-to-end tick-order audit, docs/re/facts.md "Per-tick
// call order â€” END-TO-END"): a DELIBERATE behaviour recapture. Four ordering
// fixes: (1) flame-death + pickup now ALSO run per pixel step inside the
// mover (the original's sub_41EC84 post-commit tail, pseudo.c 22699-22717) â€”
// a mid-walk kill aborts the walk and skips that turn's bomb actions, a
// mid-walk pickup applies before them; (2) clock/regen/enclosure moved
// BEFORE the head checks (field_vs_players), matching sub_42A191's
// clockâ†’bombsâ†’ageâ†’enclosureâ†’players frame order under the rotation; (3)
// rovers moved to directly after the player pass (sub_4016DA at 29528);
// (4) flame death and the rover landing kill now honour the sub_41DE63
// bounce/warp immunity (states 5/6/7), like drop_wall already did.
// Reach, proven by running BOTH revisions through a per-tick hash/rng/event
// dump of every scenario's exact script: A, B, C, E pass BYTE-IDENTICAL (all
// their hashes, E's bounce count and final rng, unchanged â€” none of them
// ever hits a same-tick move-vs-field coincidence). D diverges at exactly
// tick 405, where player 1 catches SWAP from a Disease token picked up
// MID-WALK: both revisions emit the same Infected event and draw the same
// single RNG value there (the whole kExpectedRng series below is
// byte-identical, as are the 200/400 hashes), but the new order applies the
// swap inside the pixel loop, so the walk's remaining budget continues from
// the swapped position (the original's in-loop sub_41E21E) instead of the
// swap landing after a completed walk â€” a pure, RNG-neutral position delta
// that moves only the 600/800 hashes. Pinned by tests/test_tick_order.cpp's
// six same-tick coincidence cases.
// UPDATE 2026-07-11 (prev_action1/2 hashing, determinism-contract rule-4
// gap closed): Player::prev_action1/2 are gameplay state (the LABEL_246
// drop/punch edges read them) but were never mixed into state_hash() â€” a
// pre-existing gap the LABEL_246 all-states restructure made load-bearing.
// hash.cpp now mixes one packed word per present player (after
// pickup_pause). ONE-TIME hash-layout growth (rule 5): golden A (0 players)
// is byte-identical; every player-bearing scenario's hashes recaptured (16
// constants: B setup+6, C 1, D 4, E 4+rng-adjacent). RNG-stream safety net
// passed UNCHANGED before the recapture: D's kExpectedRng at all four
// checkpoints, E's bounce count and final rng, A's final rng â€” proving the
// hashing change alters no behaviour, only the digest layout.
//
// UPDATE 2026-07-16 (movement-fidelity audit + colour split, facts.md
// "Bomb/flame colour is not the owner", "Round-start input freeze", and the
// resolved disease-scaling [VERIFY] under "Canonical frame cadence"):
//   1. HASH-LAYOUT: Bomb::colour (bits 44-47 of the bomb flags word),
//      State::flame_colour (bits 56-63 of the per-cell grid word),
//      Player::carried_colour (bits 58-61 of the carried word) and
//      State::input_freeze (bits 40-55 of the campaign word) are new hashed
//      fields â€” all PACKED into existing mix words (zero new mix calls), so
//      zero-valued states digest identically: golden A and golden B's setup
//      hash are byte-identical through this update, while every checkpoint
//      with live bombs/flames moves.
//   2. BEHAVIOUR: disease speed factors now scale the SPEED before the
//      per-frame delta division (sub_41F29B 23432-23440; Â±1 budget unit per
//      sub-frame, diseased players only) â€” nudges golden D's trajectories.
//      The AI-only fixes in the same commit (BFS seed order, boxed-in flee
//      pass-down) cannot touch these scenarios (no AI players).
//   3. The new round-start input freeze (VALUELST id 30, ~1 s) is DISARMED
//      in every config here (see pillars_config) to preserve the scenarios'
//      act-from-tick-0 semantics; tests/test_freeze.cpp pins the freeze.
// Safety net held: golden D's kExpectedRng at all four checkpoints, golden
// E's bounce count (10) and final veer rng, and golden A's final rng all
// passed UNCHANGED before this recapture â€” the RNG stream is untouched.
//
// UPDATE 2026-07-19 (spawn-pocket clear widened, docs/re/facts.md "Spawn-
// pocket clear"): `build_state`'s per-spawn brick clear grew from a radius-1
// "plus" (spawn tile + 4 orthogonal neighbours) to a radius-2 "plus" (spawn
// tile + 2 tiles in each of the 4 orthogonal directions, 9 cells total) â€”
// fixing the first-round AI mass-suicide bug (a corner AI's own opening bomb
// had its entire escape pocket inside its own blast radius, so the faithful
// flee logic could never find a strictly safer tile). The exact original
// mechanism could NOT be pinned to a specific BM95.EXE function despite an
// exhaustive search (every one of the board tile array's ~19 writer call
// sites was inspected â€” see the comment in `libs/sim/src/setup.cpp`); the
// new radius is the smallest shape matching live observation of the running
// original. The clear itself draws NO RNG (a pure cell-array write keyed off
// already-resolved spawn coordinates), so this is a DELIBERATE but RNG-
// neutral behaviour change: it only removes bricks that used to sit within
// 2 tiles of a spawn, shrinking each affected scenario's brick population
// (and hence its powerup-hiding candidate pool) before any RNG is drawn.
// Reach: only scenarios with an actual Brick tile inside a spawn's new
// radius-2 pocket move.
//   - Golden B (all-Brick board, 4 corner spawns): every corner's pocket
//     gains 3 more cleared cells (12 total across 4 spawns), shrinking the
//     brick-hiding candidate pool built in `build_state` before the first
//     powerup-placement RNG draw â€” moves the setup hash and all 6
//     checkpoints.
//   - Golden C (`pillars_config` + one deliberately-placed brick at (2,0),
//     exactly 2 tiles from the (0,0) spawn along the x-axis): that brick is
//     now cleared at setup instead of surviving for the trigger-duel
//     choreography to detonate â€” moves the single pinned hash.
//   - Golden A (no players), D (`pillars_config`, no Brick cells at all â€”
//     every spawn's pocket already reads Blank), and E (`pillars_config`
//     plus one Solid override 5 tiles from its spawn, outside pocket range)
//     are UNTOUCHED: verified byte-identical before recapturing B/C below â€”
//     golden D's kExpectedRng at all four checkpoints, golden E's bounce
//     count (10) and final veer rng, and golden A's final rng all pass
//     UNCHANGED.
// `tests/test_sim.cpp`'s brick/powerup fixtures that used to sit 2 tiles
// from a corner spawn were relocated to an interior tile clear of both
// pockets (same commit) rather than recaptured â€” they test unrelated
// bomb/brick/powerup mechanics that have nothing to do with spawn placement.
//
// UPDATE 2026-07-20 (SIM-side fidelity audit batch, docs/re/audit/*.md +
// docs/re/fidelity-audit.md): a mixed behaviour + hash-layout recapture from
// the confirmed audit fixes. The ones that reach these scenarios:
//   - bombs F1 (round-end freeze, sub_42331C @ 25603): at <= 1 alive side the
//     fuse/explosion/chain tail freezes. Reaches B/C/D/E once a round decides.
//   - bombs F2 (kicked/conveyor speed): the kicked/conveyor slide runs at the
//     BASE speed (~10 px/tick). An earlier audit folded a flat +100*kSubFrames
//     "ground bonus" (LABEL_21) that pushed it to ~19 px/tick, but the native
//     oracle showed the original's +100 is cancelled by a paired one-step
//     position backoff (net ~0.25 tile/tick, cadence-invariant) â€” reverted
//     2026-07-20. E is back at its pre-F2 values (bounce count 10, final rng
//     0xc6a9f3b2). Rovers keep their +100 (a DIFFERENT sub with no backoff).
//   - bombs F3 (conveyor coast) / stage_actors F1 (belt facing): no scenario
//     here lays a conveyor, so inert.
//   - flames F1 (arm iteration order 0,1,2,3): reorders an explosion's RNG
//     draws only when >= 2 arms draw (relocate/scatter) in one blast â€” no
//     golden blast does, so RNG-neutral here (proved: A/D rng unchanged).
//   - diseases F2 (clear() leaves disease_fresh alone): reaches D (the only
//     scenario that cures diseases) as a hashed-field-only, RNG-neutral delta.
//   - bombs F4 (created_tick, same-tick trigger exclusion): a new hashed bomb
//     field + a one-tick placement->trigger gap. Reaches C (trigger duel) and
//     grows every bomb-bearing scenario's hash layout.
//   - AI snapshot seed (ai F1, warp_to = spawn tile at setup): a
//     previously-zero hashed field now = the spawn tile. RNG-neutral; grows
//     every player-bearing setup hash (B setup moved from this alone).
//   - setup/powerups F1 (seed all 13 start_with baselines): INERT here â€” every
//     golden config uses the stock start_with (bomb 1 / flame 2 / rest 0),
//     which the new full seeding reproduces byte-for-byte.
// RNG safety net (verified BEFORE recapturing the hashes): golden A's final
// rng is UNCHANGED (0x2a â€” no players/bombs), golden D's kExpectedRng is
// UNCHANGED at all four checkpoints (no golden blast hits the flames-F1 reorder
// and no D death reaches the freeze within 800 ticks). Golden E's final rng
// DID move at batch 1 (the then-applied bombs F2 sped the kicked slide) â€” but
// that F2 fix was WITHDRAWN and reverted (see the 2026-07-20 REVERT note
// below), returning E to its pre-F2 values, so this line is historical.
// Golden A's hash is UNCHANGED (0 bombs -> no created_tick word,
// 0 players -> no warp_to word); B/C/D/E hashes all recaptured.
// UPDATE 2026-07-20 (SIM-side fidelity audit BATCH 2, docs/re/audit/setup.md
// finding 1 + docs/re/audit/diseases.md finding 1): a DELIBERATE behaviour
// recapture reaching golden B and C ONLY.
//   - setup F1 (hidden-powerup scatter): build_state's powerup-under-brick
//     scatter now uses sub_4258E5's INDEPENDENT REJECTION SAMPLING (draw a
//     random (x,y) â€” rand()%W then rand()%H â€” retry <=200 times, silently drop
//     on exhaustion; plus the interleaved 1-in-10 gate per negative-N unit)
//     instead of the old pre-built-brick-list removal (1 draw/placement). The
//     draw COUNT/ORDER at the tick-0 boundary shifts for every scheme that has
//     bricks AND a nonzero spawn_count, so the setup RNG state (and the
//     dud-gate draw that follows) moves. Reaches B (all-brick board, default
//     positive spawn_counts) â€” setup hash + all 6 checkpoints â€” and C (one
//     brick + default spawn_counts) â€” its single hash. D and E BOTH zero all
//     spawn_counts (`for (auto& c : cfg.tuning.spawn_counts) c = 0;`), so their
//     scatter draws nothing either way and stays BYTE-IDENTICAL; A never runs
//     build_state. VERIFIED before recapture: golden A's rng, golden D's
//     kExpectedRng at all four checkpoints and its 200/400/600/800 hashes, and
//     golden E's bounce count (21) and final rng all pass UNCHANGED â€” the whole
//     move is confined to B and C's setup-seeded stream.
//   - diseases F1 (Swap-with-no-target announce): DiseaseSystem::give() now
//     emits the pickup Infected event BEFORE the Swap target scan (sub_41DFB6
//     announces on the roll, not on a successful swap). RNG-neutral AND
//     hash-neutral here â€” the announce is a derived event (never hashed, rule
//     4) and adds no State::rng draw, and no golden scenario ever rolls a
//     no-target Swap regardless. Pinned by tests/test_disease.cpp.
// (flames F2 from the audit was investigated and NOT applied â€” see
// docs/re/audit/flames.md's resolution: the arm's visible-powerup interceptor
// destroys a re-hit revealed token before it can reach sub_425107, so the port
// is already faithful and no golden moves.)
// REVERT 2026-07-20 (bombs F2 withdrawn as a false positive, docs/re/audit/
// bombs.md Finding 2): the batch-1 "kicked/conveyor flat +100*kSubFrames ground
// bonus" was WRONG â€” LABEL_21's +100 is cancelled by a paired one-step position
// backoff the finding elided, so the faithful slide is the BASE speed (native
// oracle: ~0.25 tile/tick, cadence-invariant; the +100 fold ran ~1.9x too fast
// and diverged). Reverting restores E to its pre-F2 values: bounce count
// 21 -> 10, final rng 0x405862fb -> 0xc6a9f3b2, and all four E checkpoint
// hashes recaptured (below). B/C/D unaffected (no kicked/belt bomb in their
// paths); goldens headless 49/49 green + visual goldens 5/5 re-recaptured. The
// rover +100 (a DIFFERENT sub with no backoff) is NOT affected and stays.
//
// UPDATE 2026-07-26 (enclosure F2 â€” ROUND END STOPS THE SPIRAL,
// docs/re/enclosure.md Â§8, `sub_426818`'s `sub_421969() > 1` top-level gate;
// facts.md "Enclosure/HURRY arithmetic audit" corrected in the same commit).
// A DELIBERATE behaviour recapture reaching golden B and C ONLY.
//   The whole body of the original's enclosure stepper â€” the level-7 tile
//   regen call, the arm/disarm edge and the 250 ms drop loop â€” sits inside the
//   SAME alive-side-count gate that already froze the bomb fuses here
//   (`bombs_frozen`, audit/bombs.md F1), so `simulation.cpp` now gates
//   `tile_regen.update()` and `enclosure.update()` with it too (renamed
//   `round_frozen`). Flames deliberately stay ungated â€” `sub_426D06` has no
//   such check.
//   Why exactly B and C move, and nothing else. Both are pattern-input
//   free-for-alls that decide almost immediately, and both run long enough to
//   reach the hurry phase; every other scenario fails one of those two halves.
//   Measured by instrumenting each scenario with and without the new gate
//   (tick counts are 1-based; "armed" is the pre-change arm tick):
//     B  decided after 45 ticks of 3000, armed 1881 -> never  =>  only the
//        2000/2500/3000 checkpoints move; 500/1000/1500 are BYTE-IDENTICAL
//        because nothing had armed by then even before.
//     C  decided after 11 ticks of 1500, armed  281 -> never  =>  its single
//        tick-1500 checkpoint moves.
//     D  decided after 45 ticks too, but runs only 600 ticks on a 150 s clock
//        and so armed NEITHER before nor after  =>  unmoved, and it stays a
//        live control for "the gate did not touch anything else".
//     A  has no players at all (already frozen from tick 0) and a 9999 s clock
//        that never reaches the hurry window  =>  unmoved.
//     E  is scripted choreography that never reaches the phase  =>  unmoved,
//        all four checkpoints plus the bounce count and the final rng.
//   COVERAGE NOTE: this is why golden F below was added. B and C were the only
//   scenarios whose runs ever reached the wall spiral, and after this change
//   neither does â€” recapturing them alone would have quietly retired the
//   goldens' enclosure coverage. F is a fresh capture (nothing re-baselined)
//   of a round that stays undecided through a complete two-ring spiral.
//
// UPDATE 2026-07-28 (scheme "born with" is a starting-inventory BASELINE,
// docs/re/facts.md "The .SCH -P row's 2nd field is a COUNT that REPLACES the
// starting inventory"). A DELIBERATE behaviour recapture reaching B, C and E
// - exactly the three scenarios that used to set MatchConfig::born_with.
//   The change: that field is gone. sub_403EEE's tail loop writes the
//   scheme's count into VALUELST id 50+kind with the value-table SETTER
//   sub_4121BF, so "born with" IS the starting-inventory baseline
//   (Tuning::start_with) rather than a second, additive channel. setup.cpp no
//   longer replays it through PowerupSystem::apply; the three scenarios below
//   now say `cfg.tuning.start_with[kind] = 1` instead.
//   Two distinct mechanisms move the hashes, both measured:
//     (1) SETUP. apply() runs the pickup dispatcher's mutual-exclusion
//         evictions, and evict() only spares a kind the player holds "at
//         baseline" (`start_with[kind] > 0`). With born_with on a separate
//         channel that test was FALSE for every born-with kind, so B's
//         Spooger grant evicted the Grab granted one iteration earlier and
//         SCATTERED it - 4 players, 4 scatter draws, and every player ended
//         the round WITHOUT the grab glove the scenario asked for.
//         sub_4214BC's baseline write is a raw byte store with no eviction and
//         no RNG, so that is now gone: measured, all four players keep
//         kick/punch/grab/spooger/jelly and setup places zero floor tokens.
//         B's setup hash therefore moves, and everything after it.
//     (2) DEATH SCATTER / HEAD HIT. Their surplus test is
//         `held_count(p, kind) > start_with[kind]`, which reads the very id
//         the scheme overwrites (the original asks getvalue(kind+50) at the
//         same place). A born-with kind used to read 1 > 0 and be dropped;
//         it now reads 1 > 1 and is kept.
//   Which scenario moves, and from where (measured by tracing each run's
//   events under the new code):
//     B  both mechanisms - the setup hash and all six checkpoints move.
//     C  mechanism (2) only. Setup is unchanged; player 1 dies at tick 10
//        holding the born-with Trigger, which is no longer scattered, so the
//        single tick-1500 checkpoint moves.
//     E  mechanism (2) only, and LATE: player 0 dies at tick 231 holding
//        kick/punch/jelly. Checkpoints 75/150/225 are BYTE-IDENTICAL (they
//        passed unchanged through the recapture); only tick 300 and the final
//        rng move, and the bounce count stays 10 - the choreography itself is
//        untouched.
//     A  has no players; D and F set no born-with kind  =>  all unmoved.
//   FOOTNOTE on E's new final rng, 0x405862fb: that is the value this file's
//   own bombs-F2 note records as E's PRE-F2 reading, which looks like a
//   suspicious revert and is not one. State::rng is a plain xorshift32
//   (rng.hpp), so its value is a function of the seed and the DRAW COUNT
//   alone - two runs that make the same number of draws by different routes
//   land on the same word. The tick-300 hash, which does encode the board, is
//   a value this file has never carried.
TEST_CASE("golden A: empty state, 10000 ticks") {
    Simulation a;
    a.state().rng = 42u;
    // The bare ctor also zero-inits ticks_left, which the enclosure system reads
    // as "time's up" (sudden death: EnclosureSystem now correctly keeps closing
    // walls past TimeUp instead of freezing â€” see docs/re/enclosure.md Â§2/Â§6,
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
    cfg.tuning.input_freeze_ticks = 0;  // see pillars_config's disarm note
    // "Born with" is a starting-inventory BASELINE, not a grant replayed
    // through PowerupSystem::apply (facts.md "The .SCH -P row's 2nd field is a
    // COUNT that REPLACES the starting inventory"): the scheme field writes
    // VALUELST id 50+kind, which is Tuning::start_with[]. Was
    // cfg.born_with[...] = true before 2026-07-28.
    cfg.tuning.start_with[static_cast<int>(PowerupType::Kick)] = 1;
    cfg.tuning.start_with[static_cast<int>(PowerupType::Punch)] = 1;
    cfg.tuning.start_with[static_cast<int>(PowerupType::Grab)] = 1;
    cfg.tuning.start_with[static_cast<int>(PowerupType::Spooger)] = 1;
    cfg.tuning.start_with[static_cast<int>(PowerupType::Jelly)] = 1;
    Simulation s(cfg);
    CHECK(s.hash() == 0x7458970437db5433ull);  // setup itself is pinned (setup F1 recapture 2026-07-20)

    // Recaptured 2026-07-12 (canonical frame cadence, facts.md "Canonical
    // frame cadence"): the walk budget accrues per 60 fps frame with the
    // original's truncation (923 -> 921/100 px per tick) and the head-stun
    // burns per frame, so every input-driven trajectory shifts. Golden A
    // (same commit) stayed BYTE-IDENTICAL â€” the no-input path is untouched â€”
    // and E's bounce-count + veer-RNG assertions passed unchanged, pinning
    // that the choreography itself still plays out.
    static constexpr std::uint64_t kExpected[6] = {
        0x7079944fe44c029eull,  // tick 500  (setup F1 recapture 2026-07-20)
        0x44bcba78eca4d2a9ull,  // tick 1000
        0xce180c666117b293ull,  // tick 1500
        // Recaptured 2026-07-26 (enclosure F2 â€” see the file-level UPDATE).
        // Measured with the gate temporarily disabled, on the same run that
        // produced these three: the round is down to one side after 45 ticks,
        // and the stepper USED to arm at tick 1881 and close all 96 tiles by
        // tick 3000. It now never arms â€” which is exactly why the three
        // checkpoints ABOVE (all before 1881) are byte-identical and only
        // these three, all after it, move.
        0x4d08762018244b61ull,  // tick 2000
        0x38ed664fd96e55dfull,  // tick 2500
        0x72a574965ee7e37eull,  // tick 3000
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
    cfg.tuning.start_with[static_cast<int>(PowerupType::Trigger)] = 1;  // was born_with (see B)
    Simulation s(cfg);
    for (std::uint64_t t = 0; t < 1500; ++t) s.tick(pattern(t * 7 + 3));
    // Recaptured 2026-07-12 (canonical frame cadence â€” see golden B's note).
    // Recaptured again 2026-07-19 (spawn-pocket clear â€” see the file-level
    // UPDATE note above): the (2,0) test brick is cleared at setup now.
    // Recaptured again 2026-07-20 (setup F1 rejection-sampling scatter â€” see the
    // file-level BATCH 2 UPDATE note): C's default positive spawn_counts now
    // draw the rejection-sampling stream at setup, shifting the tick-0 RNG.
    // Recaptured again 2026-07-26 (enclosure F2 â€” see the file-level UPDATE):
    // measured, with the gate temporarily disabled, on the very same run that
    // produced the constant below â€” this scenario is down to one side after 11
    // ticks and USED to arm the walls at tick 281 and close all 96 tiles of the
    // spiral by tick 1500; it now never arms, so this single checkpoint moves.
    CHECK(s.hash() == 0x6fab474f1fd6f43aull);
    // Legible companions to the digest, so a stepper regression names itself.
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

    // Ticks 600/800 recaptured (docs/re/facts.md "Flame-arm stops"): the
    // scenario's flame reaches a floor powerup between tick 400 and 600. The
    // arm now stops there instead of burning through, so the state â€” and
    // hence the hash â€” diverges from that point on. Ticks 200/400 are BYTE-
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
    // Ticks 600/800 recaptured a FOURTH time 2026-07-11 (tick-order audit,
    // see the file-level UPDATE note above): the mid-walk Swap at tick 405
    // now relocates the player inside the pixel loop. Ticks 200/400 and the
    // whole kExpectedRng series are byte-identical to before the fix.
        // COMBINED-RECAPTURE NOTE (merge of the state-machine and tick-order
    // audits, 2026-07-11): D's four hashes below were re-recaptured from the
    // MERGED state â€” the state-machine branch's pickup_pause hash-layout
    // growth and the tick-order branch's per-pixel field resolution compose,
    // so neither branch's own constants were valid alone. Safety net held:
    // kExpectedRng at all four checkpoints and every other scenario's
    // assertions passed UNCHANGED on the combined build before this patch.
    // FULL recapture 2026-07-12 (canonical frame cadence â€” see golden B's
    // note): unlike the earlier audits this one legitimately moves the RNG
    // stream too â€” the gauntlet's walkers reach the disease/skate tiles on
    // different ticks (921/100 px walk accrual, molasses/hyper factors now
    // applied to per-frame accruals), so the pickup-driven draws shift in
    // time. Telling detail: the new tick-200 rng equals the OLD tick-400
    // value â€” the same draw sequence, reached sooner â€” and the stream then
    // goes quiet (the gauntlet resolves earlier), which is the expected shape
    // of a cadence change, not draw-order corruption.
    static constexpr std::uint64_t kExpectedHash[4] = {
        0x0ef1674743ad8057ull,  // tick 200
        0x919801537ace8a72ull,  // tick 400
        0x629cf878692a55d0ull,  // tick 600
        0xede44001d13a4df4ull,  // tick 800
    };
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
    cfg.tuning.start_with[static_cast<int>(PowerupType::Kick)] = 1;   // was born_with (see B)
    cfg.tuning.start_with[static_cast<int>(PowerupType::Punch)] = 1;  // was born_with (see B)
    cfg.tuning.start_with[static_cast<int>(PowerupType::Jelly)] = 1;  // was born_with (see B)
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

    // Recaptured 2026-07-12 (canonical frame cadence â€” see golden B's note).
    // The bounce-count and veer-RNG assertions below passed UNCHANGED through
    // the recapture: the script's held-key choreography still lands every
    // kick/punch, only the pixel timeline shifted.
    static constexpr std::uint64_t kExpected[4] = {
        0x14e4774347cc2b22ull,  // tick 75
        0xb26f0905590b54edull,  // tick 150
        0x3de8e4ca81b8f8d7ull,  // tick 225
        0xf959093cebf4b5b6ull,  // tick 300
    };
    int bounces = 0;
    for (std::uint64_t t = 0; t < 300; ++t) {
        s.tick(script(t));
        for (const auto& e : s.state().events)
            if (e.type == Event::Type::JellyBounced) ++bounces;
        if ((t + 1) % 75 == 0) CHECK(s.hash() == kExpected[(t + 1) / 75 - 1]);
    }
    // 10 legs: the jelly ping-pong bomb travels at the base kicked speed
    // (~10 px/tick). An earlier audit folded a flat +100*kSubFrames "ground
    // bonus" here that pushed it to ~19 px/tick (21 legs), but that +100 is
    // cancelled by a paired position backoff in the original (bombs F2, reverted
    // 2026-07-20 after the native oracle showed the kicked slide is ~0.25
    // tile/tick, cadence-invariant). Restoring the base speed restores the
    // pre-F2 leg count and detonation rng. A jelly bounce itself draws no RNG.
    CHECK(bounces == 10);                 // the ping-pong really happened
    CHECK(s.state().rng == 0x405862fbu);  // base kicked speed: detonation back on its pre-F2 tile
}

// NEW 2026-07-26 (enclosure F2/F3, docs/re/enclosure.md Â§5.1/Â§8). B and C used
// to be the only goldens whose runs reached the hurry phase at all â€” and after
// the round-end freeze landed they no longer do (both decide within 45 ticks,
// so the stepper never arms; see the file-level UPDATE note). This scenario
// replaces that lost coverage deliberately: nobody ever presses a key, so no
// bomb is ever dropped, nobody dies, `sides_remaining` stays 2 for the whole
// run and the stepper's round-end gate never trips. It therefore pins, in one
// hash chain, the arm moment, the 250 ms drop cadence over a complete two-ring
// spiral, the fact that the spiral keeps going PAST TimeUp (Â§2 â€” ticks_left
// hits 0 at tick 600, well before the last drop), and the arm-time
// warphole/trampoline sweep (Â§5.1) that leaves the belt and the arrow alone.
TEST_CASE("golden F: a full hurry phase with the round still undecided") {
    MatchConfig cfg = pillars_config();
    // Both spawns are ring-4 tiles (min(x, y, 14-x, 10-y) == 4); with
    // enclosement_depth = 1 the walls close rings 0-1 only, so neither player
    // is ever crushed and the round stays undecided to the last tick.
    cfg.spawns = {{6, 4}, {8, 6}};
    cfg.player_count = 2;
    cfg.seed = 0xEC105u;
    cfg.tuning.game_seconds = 30;
    cfg.tuning.hurry_seconds = 25;
    cfg.tuning.enclosement_depth = 1;
    // One of each actor type, all on ring-4 tiles the spiral never reaches and
    // none under a player, so the ONLY thing that can change them is the arm
    // sweep: the warphole pair and the trampoline must go, the belt and the
    // arrow must stay (sub_405D0C, docs/re/enclosure.md Â§5.1).
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
    // Captured 2026-07-26 (new scenario, nothing re-baselined). The walls arm
    // at tick 180 (30 s clock, hurry 25 â‡’ the arm predicate `remaining <=
    // hurry - 5` first holds with 419 ticks left) and the two-ring spiral's
    // 96th and last tile lands around tick 660.
    static constexpr std::uint64_t kExpected[4] = {
        0xc207d41f909d6e98ull,  // tick 250  (armed at 180; drop index 13)
        0x19b41fdce589f38aull,  // tick 500  (index 63)
        0xcec674472d4d107aull,  // tick 750  (index 96 â€” past TimeUp at tick 600)
        0x97fa3868be8bf297ull,  // tick 1000 (spiral exhausted, board static)
    };
    for (std::uint64_t t = 0; t < 1000; ++t) {
        s.tick(TickInputs{});
        if ((t + 1) % 250 == 0) CHECK(s.hash() == kExpected[(t + 1) / 250 - 1]);
    }
    // Legible assertions alongside the opaque digests, so a regression in the
    // stepper says WHAT broke and not just "some hash moved".
    CHECK(sides_remaining(s.state()) == 2);  // never decided -> never frozen
    CHECK(s.state().ticks_left == 0);        // the clock ran out at tick 600...
    CHECK(s.state().enclose_index == 96);    // ...and the spiral finished anyway (Â§2):
                                             // rings 0-1 = 96 drop events, all landed
    CHECK(s.state().actor_type[4][4] == ActorType::None);       // warphole swept (Â§5.1)
    CHECK(s.state().actor_type[6][10] == ActorType::None);      // warphole swept
    CHECK(s.state().actor_type[6][4] == ActorType::None);       // trampoline swept
    CHECK(s.state().actor_type[4][10] == ActorType::Conveyor);  // belt survives
    CHECK(s.state().actor_type[6][6] == ActorType::DirArrow);   // arrow survives
    CHECK(s.state().warp_dest_x[4][4] == 10);                   // only the ACTIVE flag is cleared
}
