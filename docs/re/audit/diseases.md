# Disease system fidelity audit (2026-07-20)

**Verdict: faithful overall — one real audible-feedback bug (Swap-with-no-target
stays silent) and one inert hash-only field divergence (`clear()` also zeroes
`disease_fresh`), plus one low-confidence edge-case note on the ShortFuse
floor. Everything else independently re-verified against the native
transliteration matches exactly, including the extensive 2026-07-10 audit's
already-fixed items.**

Scope: `libs/sim/src/systems/diseases.cpp`/`.hpp`, the disease-effect gates in
`libs/sim/src/systems/movement.cpp`, `bombs.cpp`, `libs/sim/src/simulation.cpp`
(LABEL_246 tail, reversed-controls decode), `libs/sim/src/systems/ai.cpp`
(Constipation gate), and the presentation-side disease flash/voice
(`libs/game/src/renderer.cpp`, `libs/game/src/sound_director.cpp`).

Method: line-by-line read of `native/src/game/batch_0x41DAA7.cpp` (`sub_41DF4C`
cure @269-285, `sub_41DFB6` assign @288-345, `sub_41E16A`/`sub_41E21E` pickup
dispatch @348-514, `sub_41EB13` bomb-param fill @729-768) and
`native/src/game/batch_0x41F29B.cpp` (the per-player disease/contagion block
@236-330, LABEL_246 @642-759, the reversed-controls flip @394-410), cross-read
against `docs/re/facts.md`'s existing "Disease system" (§326-420) and "Disease
system fidelity audit 2026-07-10" (§2411-2584) entries, which already cover
most of the ground here in detail.

## Findings

### 1. Skull pickup can be silent when the first internal roll is Swap-with-no-target

> **FIXED 2026-07-20 (batch 2).** `DiseaseSystem::give()` now pushes the
> `Infected` announce event at its TOP — before the Swap branch's target scan —
> matching `sub_41DFB6`'s announce-before-`if (v7 == 7)` order, and
> `assign_random()` calls `give()` unconditionally (the old `continue` that
> skipped it on a no-target Swap is gone). RNG- and hash-neutral (the announce
> is a derived, unhashed event and adds no `State::rng` draw). Pinned by
> `tests/test_disease.cpp` "a swap roll with no valid target still emits the
> pickup announce". No golden moved.

**Severity:** Medium (player-audible feedback bug, narrow trigger condition).
**Confidence:** High (direct arithmetic read, confirmed absent from any
existing test).

**Original** (`sub_41DFB6`, `native/src/game/batch_0x41DAA7.cpp:298-333`;
pseudo.c ~22041-22098): the disease-kind roll and its announce sound are
resolved and played *before* the code ever looks at whether the roll was
Swap:

```c
for (i = 0; i < 200; ++i) {
    v7 = rand_() % 9;
    result = sub_40C06A();               // net-role predicate; 0 locally
    if (!result || v7 != 7) break;        // locally: always breaks after ONE draw
}
if (a2) {                                 // a2 = "announce" (first roll of a batch)
    if (rand_() % 3) sub_427961(2300);            // 2/3: generic "oh no"
    else sub_427961(50 * v7 + 3000);              // 1/3: per-disease voice, incl. Swap's own range
}
if (v7 == 7) {
    for (j = 0; j < 200; ++j) { /* random-player target scan; on success: swap + return */ }
    // falls through here with NO state change if no valid target is ever found
} else {
    /* set disease flag / duration / freshness */
}
```

The sound plays unconditionally whenever `a2` (announce) is set, purely as a
function of the roll `v7` — independent of whether the subsequent Swap target
search (only entered `if (v7 == 7)`) actually finds anyone. A Skull pickup
therefore **always** makes a sound on its first (announcing) roll, even when
that roll is Swap and there's nobody left alive to swap with.

**Port** (`libs/sim/src/systems/diseases.cpp:60-73`):

```cpp
void DiseaseSystem::assign_random(int idx, int count) {
    for (int c = 0; c < count; ++c) {
        auto d = static_cast<Disease>(random_below(s_, kDiseaseKinds));
        if (d == Disease::Swap && !has_swap_target(idx)) continue;
        give(idx, d, c == 0);
    }
}
```

When `d == Disease::Swap` and `has_swap_target(idx)` is false, the loop
`continue`s *before* calling `give()` — which is the only place that pushes
the `Event::Type::Infected` event `give()` (`diseases.cpp:23-58`) uses to
drive the voice line in `SoundDirector` (`libs/game/src/sound_director.cpp:
147-156`). The announce sound is therefore skipped entirely for that roll.

**Visible effect:** for a plain Skull (`assign_random(idx, 1)`), if the
single roll is Swap-with-no-target, the pickup produces **zero** audio
feedback where the original always plays one of the two voice lines. For a
purple/SuperDisease skull (`assign_random(idx, 3)`, only the first of the
three rolls ever announces — `c == 0`), the same first-roll failure silences
the *entire* three-disease pickup, even though the other two rolls (if not
also Swap-with-no-target) still take effect and change the player's state.
The RNG draw count is unaffected (the `random_below` roll for the disease
kind still happens before the `continue`), so this is purely a missing-cue
bug, not a determinism/hash divergence.

The condition requires no other player to currently be `present && alive`
(`DiseaseSystem::has_swap_target`, `diseases.cpp:16-21`) — realistically a
last-player-standing picking up a leftover skull after everyone else has
died, or a 1-human-vs-0-others debug/edge configuration. `tests/
test_disease.cpp`'s two Swap tests (`"the skull can roll swap..."`,
`"swap exchanges position only..."`) both run in a 2-player match where a
target always exists, so this path is untested; its own comment ("A skull
token always emits an Infected event") states the exact assumption this
finding disproves for the no-target case.

**Suggested fix:** always announce (push the `Infected` event with the
resolved `Disease` kind, gated only on `announce`) even when a rolled Swap
finds no target, e.g. split `give()` so the sound/event dispatch runs before
the target-availability check, mirroring the original's ordering — the event
carries `data = Disease::Swap` either way since `sound_director.cpp` only
reads `ev.data` for the voice-range base, never any state that would differ
between "swapped" and "swap fizzled."

### 2. `DiseaseSystem::clear()` also zeroes `disease_fresh`, unlike either original cure site

**Severity:** Low (no observable gameplay effect; hashed-field-only
divergence from a byte-accurate oracle mirror).
**Confidence:** High.

**Original:** neither cure site touches the freshness field (`+128`,
`Player::disease_fresh`'s counterpart):

- `sub_41DF4C` (`native/src/game/batch_0x41DAA7.cpp:269-285`, the direct cure
  — expiry and the cure-roll pickup path both call this) zeroes only `+120`
  (age), `+124` (duration), and the 14-byte `+132..+145` flag block:
  ```c
  *(_DWORD *)(result + 120) = 0;
  *(_DWORD *)(result + 124) = 0;
  for (i = 0; i < 14; ++i)
      if (*(_BYTE *)(i + v1 + 132)) *(_BYTE *)(i + v1 + 132) = 0;
  ```
- The contagion source-clear inlined in `sub_41F29B` when `diseases_multiply`
  is off (`native/src/game/batch_0x41F29B.cpp:308-319`) likewise only zeroes
  `+132..+145` (inside the copy loop) then `+120`/`+124`:
  ```c
  for (i = 0; i < 14; ++i) {
      v103[i + 132] = v111[i + 132];
      if (!dword_464A78) v111[i + 132] = 0;
  }
  if (!dword_464A78) { v111[30] = 0; v111[31] = 0; break; }
  ```

`+128` (freshness) is written in exactly one other place — unconditionally,
on every fresh infection (`sub_41DFB6`, `batch_0x41DAA7.cpp:339-341`:
`v5[32] = sub_412135(129)`) — and is otherwise read only by the contagion
source gate (`!+128`, requires freshness to have counted down to 0 to be
eligible to spread) and never by the target-validity check (`!v103[30]`,
which is the age field, not freshness). A stale non-zero freshness value left
behind on a *healthy* player (`+120 == 0`) can never be observed: the source
gate already requires `+120 != 0` first (short-circuits before the freshness
term matters), and any later infection overwrites `+128` outright.

**Port** (`libs/sim/src/systems/diseases.cpp:10-14`):

```cpp
void DiseaseSystem::clear(Player& p) {
    p.disease.fill(false);
    p.disease_timer = 0;
    p.disease_fresh = 0;      // <- no counterpart in sub_41DF4C or the inlined source-clear
}
```

**Visible effect:** none in isolation, for the same reason the original's
stale value is unobservable — `disease_fresh` is only ever consulted through
`disease_timer > 0` (contagion source) or unconditionally overwritten on
reinfection (`give()`, `diseases.cpp:53`). It **is** hashed
(`libs/sim/src/hash.cpp:181`, `p.disease_fresh << 40` folded into the same
`mix()` call as `disease`/`disease_timer`), so a byte-accurate oracle mirror
that leaves a stale nonzero freshness after a cure would diff against this
port's zeroed value on that field alone — indistinguishable from a real bug
in an oracle-diff session despite being gameplay-inert. Flagging for
transliteration accuracy / to preempt exactly that false-positive read.

**Suggested fix:** drop the `p.disease_fresh = 0;` line from `clear()` (both
call sites — direct cure and contagion source-clear — already leave the
field alone in the original); harmless to leave as-is if the oracle-diff
harness is not close to exercising this field, but trivial to make
byte-exact.

### 3. ShortFuse's `max(1, fuse/3)` floor has no original counterpart (low confidence, likely inert)

**Severity:** Low (inert under current/default tuning).
**Confidence:** Low — the original's own zero-fuse handling downstream
(`sub_422EDE`/bomb countdown) was not traced as part of this disease-focused
pass; flagging the arithmetic delta only.

**Original** (`sub_41EB13`, `native/src/game/batch_0x41DAA7.cpp:757-758`):

```c
if (*(_BYTE *)(a1 + 138))     // ShortFuse
    v10 /= 3;                 // unclamped integer division
```

**Port** (`libs/sim/src/systems/bombs.cpp:34-35`):

```cpp
b.fuse_init = s.tuning.fuse_frames;
if (p.sick(Disease::ShortFuse)) b.fuse_init = std::max(1, b.fuse_init / 3);
```

Under the shipped default (`Tuning::fuse_frames = 40`, VALUELST id 41),
`40 / 3 = 13`, nowhere near the clamp — this is currently unreachable in
practice. It would only diverge under a custom scheme setting the base fuse
below 3 ticks via id 41, an untested configuration; whether a literal 0-tick
fuse behaves identically to a 1-tick fuse in this port's own countdown
(`bombs.cpp`/`simulation.cpp` fuse tick-down, ledger item #2) is outside this
audit's scope. Noting for the record rather than as a confirmed bug.

## Verified faithful (no change)

Independently re-derived from the native source in this pass (in addition to
the extensive existing coverage in `docs/re/facts.md`'s "Disease system" and
"Disease system fidelity audit 2026-07-10" sections, cited inline):

- **Roster: exactly 9 diseases, `rand() % 9`**, no 10th disease and
  specifically **no "no-kick" disease** — `types.hpp:51-52` vs `sub_41DFB6`'s
  `rand_() % 9` (`batch_0x41DAA7.cpp:302`); matches facts.md's own
  2026-07-10 reconfirmation ("no 10th disease... exists in the binary").
- **Kind → effect table** (Slow ÷3, Fast/Super ×3/2, Constipation blocks only
  the drop block, Diarrhea/Super force the drop edge every tick,
  ShortFlame→flame=1 (overridden by Goldflame→max(cols,rows)), ShortFuse ÷3,
  Swap = 2-field x/y-only teleport, Reversed = `(g+2)&3` humans-only) —
  matches `facts.md:334-344` exactly; independently re-checked against
  `sub_41EB13` (`batch_0x41DAA7.cpp:737-758`) and the LABEL_246 tail
  (`batch_0x41F29B.cpp:642-747`).
- **Molasses-then-hyper speed order, factors-before-delta-scale** —
  `movement.cpp:63-69`; the task's "already audited, verify" item, reconfirmed
  against the same disease-scaling site.
- **Reversed-controls flip point**: applied to the *resolved* godir, after
  the opposite-key filter, humans-only (`+16 != 1`) — `simulation.cpp:
  444-453` matches `batch_0x41F29B.cpp:402-404` (`v111[23] &= 3; if (+140 &&
  +16 != 1) v111[23] = (v111[23]+2)&3;`) exactly, including the gate order
  (clamp first, then flip).
- **Constipation gates only the drop block** (grab/spooge/plain-drop), never
  the carried-bomb throw-release or the action2 (kick-stop/punch/trigger)
  block — `simulation.cpp:242-281` vs `batch_0x41F29B.cpp:645-747`
  (`!+134` only guards the `if (+56 && !+54 && !+134)` block at line 677);
  also mirrored in the AI's own drop gate (`ai.cpp:742-745`).
- **Diarrhea/Super auto-drop force**: `+56=1, +54=0` unconditionally at the
  top of LABEL_246 (every alive tick, including bounce/warp flights), plus
  the `v112` "poop" sound flag — `bombs.cpp:64-70`, `simulation.cpp:242-245`
  vs `batch_0x41F29B.cpp:644-650`.
- **Contagion**: overlap `|dx| ≤ tileW-10, |dy| ≤ tileH-10` (40/36 confirmed
  via `dword_4648A4`/`dword_4648A0`, `facts.md:261`), source requires
  `disease_timer > 0 && disease_fresh == 0`, target requires healthy
  (`disease_timer == 0`) with no freshness check on the target side, `multiply`
  clears the source when off — `diseases.cpp:125-143` vs
  `batch_0x41F29B.cpp:295-324`; independently re-verified, matches the
  already-fixed 2026-07-10 audit exactly.
- **Age-then-spread per-player two-pass ordering**, and **stun does NOT gate
  aging/contagion** (only `!alive` does; `+8` is the died-this-round flag, not
  the `+58` stun countdown) — both already fixed/documented in the
  2026-07-10 audit; independently re-verified against the `!+8` gate at
  `batch_0x41F29B.cpp:236` and the freshness/age block at lines 279-294.
- **Skull pickup dispatch**: cure-roll before any powerup effect
  (`sub_42BE0B`/cure gated only on `diseases_curable`, not current sickness —
  the one deliberately-not-replicated RNG-draw-count deviation, already
  documented in facts.md's 2026-07-10 audit and re-confirmed present,
  unchanged, at `diseases.cpp:75-79` — not re-flagged here), Skull = 1 roll
  with announce, SuperDisease = 3 rolls (only the first announces) —
  `simulation.cpp:82-104` vs `batch_0x41DAA7.cpp:404-475` (cases `2` and
  `0xB`).
- **Voice sound selection**: 1-in-3 the per-disease line (`3000 + 50*idx`,
  itself a `sub_427961` contiguous-slot random pick — confirmed by
  `facts.md:2721-2724`'s independent derivation of that helper), else 2300 —
  correctly implemented presentation-side (`sound_director.cpp:147-156`),
  never touching `State::rng` (determinism contract rule 6 compliant).
- **200-try net-only reroll-away-from-Swap in the disease roll itself**
  (`batch_0x41DAA7.cpp:298-307`, distinct from the target-scan loop) is
  correctly NOT replicated: `sub_40C06A() == 0` locally means the loop always
  breaks after exactly one `rand() % 9` draw, matching `assign_random`'s
  single `random_below` per roll; this project's port only ever runs the
  original's local-game code path (already-established convention, e.g.
  `facts.md:2350-2356`).
- **14-byte vs 9-flag contagion copy width** (`+132..+145` vs the 9 real
  disease bytes) — already-documented inert detail (`facts.md:2516-2524`),
  correctly not ported (`Player::disease` is a 9-element array).
- **Disease-timer bit-3 visual flash** (presentation, low priority per this
  audit's brief) — `renderer.cpp:738` (`disease_timer & 8`) matches the
  confirmed `v111[60] & 8` mechanism (`facts.md:373-411`); not re-derived in
  depth here beyond confirming the current code still reads the field this
  way.

## Provenance

`native/src/game/batch_0x41DAA7.cpp`: `sub_41DF4C` (cure) @269-285,
`sub_41DFB6` (assign) @288-345, `sub_41E16A`/`sub_41E21E` (pickup dispatch)
@348-514, `sub_41EB13` (bomb param fill) @729-768.
`native/src/game/batch_0x41F29B.cpp`: per-player alive/freshness/age/contagion
block @195-330, reversed-controls flip @379-410, LABEL_246 tail @642-759.
Cross-checked against `docs/re/facts.md` "Disease system" (§326-420) and
"Disease system fidelity audit 2026-07-10" (§2411-2584).
