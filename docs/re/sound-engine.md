# The sound SELECTION engine (`sub_427961` and friends)

Read 2026-07-27 from BM95.EXE (Watcom, imagebase 0x400000), corroborated by
four live launches of the original with `debug=3`. This file documents the
mechanism behind a whole class of audio-fidelity gaps: BM95 does **not** play
"the clip at SOUNDLST id N". Every call site names a **group base** and the
engine chooses a member for it, out of a subset that was itself randomised when
the game started.

Prior audits (`docs/re/coverage-audit.md`, the 2026-07-22 oracle audio pass)
suspected all of this but could not confirm it: `sub_427961`, `sub_427BFB`,
`sub_4278F2` and `sub_42741E` sit in a span that is not transliterated in
`native/` and are stubbed in `native/src/game/_m1_stubs.cpp`, so the oracle
harness could not arbitrate. These are the real routines.

(`native/` is the local-only 1:1 transliteration of the binary. It is gitignored
as exe-derived material and is not published, so that path will not resolve for
a reader of this repository; `docs/re/method.md` explains what it is. Nothing
here depends on it — the addresses and the `debug=3` log lines are the evidence.)

## 1. The data structures

Three parallel arrays, all indexed by SOUNDLST id and all (re)allocated by the
loader `sub_42814B` (0x428480 / 0x42849F allocate the two counters):

| global | meaning |
|---|---|
| `dword_463080` | slot count (the log reports a **3551-element array**) |
| `dword_463094` | `char*` clip base name per slot; **NULL = empty slot** |
| `dword_463088` | per-slot **play counter** |
| `dword_463090` | per-slot **last-played frame** (debounce only) |
| `dword_46307C` | currently active SFX voices |

A **group** is therefore purely positional: the run of consecutive non-NULL
`dword_463094` entries starting at the id the caller named. There is no group
field in SOUNDLST.RES and no group table in the binary — `sub_427961` walks the
array until it hits an empty slot.

## 2. `sub_427961` — the pick (0x427961)

The one function 70 call sites use. Paraphrased from the disassembly:

```
play_group(id):
    if id < 0 or id >= count:          return          # 0x427972
    if names[id] == NULL:              return          # 0x427996
    n = 0                                              # 0x42799F-0x4279DA
    for i = id; i < count and names[i]; ++i: ++n       #   the contiguous run
    if n == 0:                         return
    least = counts[id]                                 # 0x4279E6-0x427A42
    for j in 0..n-1: least = min(least, counts[id+j])
    for k in 0..199:                                   # 0x427A44-0x427A87
        pick = id + rand() % n
        if counts[pick] == least: break
    load(names[pick]); start_voice()                   # 0x427A98 / 0x427A9D
    counts[pick] += 1                                  # 0x427AB0
```

Three details that matter and that guesswork would get wrong:

- **It is least-played-first, not uniform.** The draw is *rejection-sampled*
  against the group's minimum play count, so every member is heard exactly once
  before any is heard twice, and the order inside each cycle is re-randomised.
  Audibly this is an equal-use shuffle, not a random pick — the difference is
  obvious over a round.
- **The 200-draw ceiling (`0xC8`) is a fallback, not a policy.** If 200 draws
  never land on a least-played member the last draw is used anyway, so a very
  large group degrades gracefully towards uniform instead of looping forever.
- **The counter is charged even when the voice is dropped.** `counts[pick] += 1`
  runs unconditionally after the name resolves, past the point where the
  concurrency cap inside `sub_427859` may have refused to start anything.

## 3. `sub_427F1B` — the load-time cull (0x427F1B)

`play_group` alone would pick over the whole authored block. It does not,
because `sub_42814B` runs a cull over sixteen ranges right after parsing
SOUNDLST (0x42858E-0x428760). `sub_427F1B(base, end, keep)`:

1. **Compaction** (0x427F30-0x427FF8) — a two-cursor stable squeeze over
   `[base, end]` that moves every occupied slot down so the block starts exactly
   at `base` with no holes. This is what makes bases the author never wrote
   resolvable: SOUNDLST has **no id 700** (the death-taunt block starts at 701),
   yet `sub_427961(700)` is the taunt call — because the compaction has moved
   701 into 700 by then.
2. **Random trim** (0x428033-0x428124) — while the run is longer than `keep`,
   delete a uniformly random member (`rand() % n`), shift the tail down and NULL
   the last slot. It logs each pass with the format string
   `"culling the %u series down from %u to %u (culled out %u)"`.

The table, with the counts the shipped SOUNDLST.RES actually holds:

| block | authored | keep | what it is |
|---|---|---|---|
| 200-299 | 20 | **3** | exploding bomb |
| 400-499 | 84 | **7** | powerup pickup voice |
| 700-999 | 282 | **7** | post-death taunt |
| 1200-1299 | 80 | **2** | "huge string of bombs" taunt |
| 1400-1699 | 144 | **7** | "you are now AWESOME" milestone |
| 2300-2599 | 88 | **8** | generic disease voice |
| 2700-2799 | 39 | **5** | "hurry up!" callout |
| 3000+50k .. 3049+50k, k=0..8 | 3-36 | **4** | per-disease voice |

Every `keep` above is the normal-memory arm. When the low-memory flag
`dword_464824` is set the binary substitutes **1** for all of them, so a LOWMEM
boot has exactly one clip per group.

Blocks NOT in the table keep everything they have — including **2800-2810**, the
eleven "ATOMIC BOMBERMAN!" title takes, and 2600 (quit), 1700 (draw), 2000
(winner), 20 (nav blip), 40 (drop refused), 170 (grab).

The cull is not only a startup event. VALUELST id 7 (authored **1800**) is, in
the file's own words, "how many seconds between voluntary clearings of the sound
cache and **re-choosing**/re-loading of the soundlst.res file" — `sub_428AEF`
calls `sub_42814B` again on that timer, so a half-hour session re-rolls its
subsets mid-play.

### Live confirmation

`debug=3` writes the cull log. From a run of the original:

```
Total of 1051 sounds loaded into a 3551-element array.
culling the 200 series down from 20 to 3 (culled out 17)
culling the 700 series down from 282 to 7 (culled out 275)
...
```

The same log records every name resolution (`NM: '<clip>.snd' -> ...`), which is
exactly the `sub_411D17` call `sub_427961` makes on the chosen slot. Four
consecutive launches, stopped at the title screen, resolved **ZAI08A**,
**GEN8C2**, **GEN8A**, **ZAI08F** for the title sting. The variation the project
owner reported is real and this is where it comes from.

## 4. The three (four) play primitives

| routine | address | group pick? | counts against cap? | used by |
|---|---|---|---|---|
| `sub_427961` | 0x427961 | yes | yes | everything — 70 call sites |
| `sub_4278F2` | 0x4278F2 | **no**, exact slot | yes | 2 sites (below) |
| `sub_427BFB` | 0x427BFB | yes | **no** | the 4 screen stings |
| `sub_427ABB` | 0x427ABB | yes, debounced | yes | 1 site (jelly bounce) |

- **`sub_4278F2`** is the same body with the group walk removed. Its two callers
  are the enclosure wall-slam (`sub_426818`, which draws its own `rand() % 3`
  once per arm and then replays that one id — already ported) and the death
  handler at 0x41DDE4, which follows `sub_427961(300)` with
  `sub_4278F2(actor[+4] + 340)`. `actor[+4]` was pinned 2026-07-28 — it is the
  **death-animation index** — and the overlay is now ported; §10 has it, and
  corrects this entry's old claim that "only id 341 exists in that span".
  - **It does not increment the play counter, it OVERWRITES it.** Where
    `sub_427961` ends `counts[pick] += 1` (0x427AB0), `sub_4278F2` ends
    `counts[id] = dword_464994` (0x427950-0x427956) — the same `dword_463088`
    array, but assigned the game-FRAME counter. So any slot an exact play
    touches is left with a play count in the thousands, and §2's least-played
    rejection sampler will not choose it again for the rest of the session
    (until VALUELST id 7's periodic re-roll zeroes the counters). Live
    consequence: a death that rolls anim 10-13 quietly retires that member of
    the trampoline group. NOT yet ported — `SoundBank::play_exact` has no frame
    to assign — and left as a follow-up rather than guessed at.
- **`sub_427BFB`** picks identically but plays through `sub_427B36`, which builds
  its own sound object outside the counted pool. Its four callers:
  `sub_42B060` → 2800 (title intro), `sub_412987` → 2600 (menu quit),
  `sub_42A721` → 1700 (draw), `sub_42ACBE` → 2000 (winner).
- **`sub_427ABB`** wraps `sub_427961` with `if (last[id] + 3 > now) return;`
  against the game-frame counter `dword_464994`, then stamps `last[id]`. Its one
  caller is `sub_423776`, the **jelly** bomb's wall reversal (SOUNDLST 135) — a
  jelly pinballing between two walls would otherwise re-trigger every frame. Its
  non-jelly sibling one branch down (`sub_427961(130)`, "bombstop") is not
  debounced.

**`sub_427BFB` does NOT block.** `sub_427B36` ends at `sub_419D88`, which is the
DirectSound `IDirectSoundBuffer::Play` thunk — asynchronous. The decisive
evidence is the quit path: `sub_412987` calls `sub_427BFB(2600)` and then
`Sleep(4000)` (`sub_452012` → KERNEL32 `Sleep`, arg `0xFA0`). That sleep only
makes sense if the call returned immediately. So the title sting plays *over* the
title art rather than gating it, and a non-blocking port is correct.

## 5. The concurrent-voice cap

`sub_427859` (the counted voice launcher, 0x427859) opens with

```
if (getvalue(8) < active_voices) return;      # 0x42786F-0x42787A
```

`getvalue` is `sub_412135` (the VALUELST table lookup, sentinel `-12345` for
"unset"). **VALUELST id 8 is authored `5`**, and the file's own comment is *"how
many concurrent sounds do we want to allow?"*.

The eviction rule is: **there is none.** An over-cap sound is silently dropped;
a voice already playing is never cut short. `dword_46307C` is incremented when a
voice starts (0x4278DB) and decremented by the completion callback installed at
0x427374. Music (`sub_42741E`/`sub_4273A4`, its own single handle
`dword_463064`, loop count `0xFFFF`) and the `sub_427BFB` stings sit outside the
count entirely.

## 6. What the port does with all this

`libs/audio`:

- **`SoundBank`** (`sound_bank.hpp/.cpp`) is the SDL-free selection engine: slot
  table, cull table, compaction, least-played-first pick with the 200-draw
  ceiling, and the 3-frame debounce. Unit-tested (`ctest -R sound_bank`).
- **`AudioEngine`** exposes the same four primitives — `play` (`sub_427961`),
  `play_exact` (`sub_4278F2`), `play_sting` (`sub_427BFB`, its own uncapped
  stream) and `play_debounced` (`sub_427ABB`) — and enforces the cap by reading
  VALUELST id 8 at init, dropping rather than stealing.
- The cosmetic generator is **seeded from the wall clock at init**. That single
  line is the fix for the reported bug: the old engine did pick randomly, but
  from a hardcoded LCG seed, so every launch produced the identical sequence and
  the title sting was the same take forever.

**Determinism.** The original draws sound picks from the same libc `rand()` the
gameplay uses. The port deliberately does **not** mirror that: `SoundBank` owns
its own generator and never touches `sim::State::rng` (root `CLAUDE.md` rule 6).
Wiring them together would desync every online match and move every golden hash.
No golden moved with this change.

**Deliberately not ported:** the low-memory `keep = 1` arm (the port has no
LOWMEM mode), the VALUELST-id-7 30-minute re-roll, the selective/total sound
pre-caching arms (VALUELST ids 3 and 4, both authored `0`), and the death
handler's `sub_4278F2(340 + actor[+4])` overlay (field unidentified).

## 7. Corrections to earlier notes

- **`facts.md` "Throw is silent" over-claimed.** Its conclusion (throwing emits
  no sound of its own) stands, but its supporting claim that the `bmbthrw` clips
  are *"loaded but never played"* is **wrong**. SOUNDLST loads 170-175 with no
  gap — `grab1, grab2, bmbthrw1, bmbthrw3, bmbthrw4, bmbthrw5` — and 170 is not
  a culled block, so `sub_427961(170)` (bomb grab) picks across all **six**. No
  call site names 172; the group walk reaches it anyway.
- **`HeadHit` was not one clip short — it should not exist.** SOUNDLST 360-363
  is `bombhit1..4` and the 2026-07-27 pass widened the port's group from 360-362
  to 360-363 on that basis. That answered the wrong question. The range was
  never the issue: **nothing in the binary ever calls 360**, so no member of the
  group is ever heard and the whole cue is invented. See §8, which enumerates
  every call site. The cue was removed 2026-07-28 and the silence is pinned by
  `tests/audio/test_sound_director.cpp`.
- The audit's three suspicions all survive: the six single-slot SFX, the
  two-member nav blip / refused buzz, and the load-time cull to a random subset
  picked least-played-first. All three are now mechanism, not conjecture.

## 8. The complete call-site census (2026-07-28)

§2-§4 explain how a group is chosen. This section answers the other half — WHO
asks, and for what — and it is the evidence behind every "the original is silent
here" claim in `libs/audio` and `libs/game`.

**Method, and why it is exhaustive.** A byte-level sweep of the whole `BEGTEXT`
section for the `E8 rel32` encoding, at every offset rather than every
instruction boundary, cannot miss a direct call. It finds **87** into the play
primitives: 70 x `sub_427961`, 6 x `sub_42741E` (music), 4 x `sub_427BFB`,
2 x `sub_4278F2`, 2 x `sub_427859`, 2 x `sub_427B36`, 1 x `sub_427ABB` — the
last three groups being the engine's own internals. The 70 matches §4's count,
which is the cross-check.

Direct calls are the only kind there are: the **literal address of every one of
these routines appears nowhere in the image**, in code or in data. There is no
function-pointer table, no dispatch through a global, nothing an `E8` scan could
have stepped over. So "id N has no call site" is a complete statement, not a
failure to find one.

**The ids named.** 65 of the 70 `sub_427961` sites load a literal into EAX
(Watcom's first register argument). The other five are:

| site | argument | what it is |
|---|---|---|
| `0x41E03B` | `3000 + 50*idx` | the per-disease voice block |
| `0x41E549` | a local | the powerup voice — its only four writers store **-1, 400, 135, 1400** and nothing else |
| `0x427B13` | its own argument | `sub_427ABB` forwarding, i.e. always 135 |
| `0x41DDE4` (`sub_4278F2`) | `340 + actor[+4]` | the death overlay, field still unidentified |
| `0x426A55` (`sub_4278F2`) | `140 + dword_462244` | the wall slam, `rand() % 3` latched per arm |

No site loads **360-363** as a literal — `mov eax,0x168` occurs nowhere in the
image — and `sub_421F7E`, the head-hit handler itself, makes no audio call of any
kind. **The port's removal of the HeadHit cue therefore stands** and the silence
`tests/audio/test_sound_director.cpp` pins is correct: a bomb landing on a head
is silent.

> **CORRECTION, 2026-07-28.** This paragraph used to read "no site, *literal or
> computed*, can produce 360-363", and the computed half of that was wrong.
> `0x41DDE4` computes `340 + actor[+4]` and `actor[+4]` reaches 24, so the death
> overlay **does** address 360-363 — and 350-353 — whenever the death-animation
> roll lands there. `bombhit1..4` are not unreachable; they are reachable from a
> cue that has nothing to do with head hits. See §10.

**Screens that are SILENT.** Reachability over the same call graph settles a
whole class of questions the port had been guessing at:

| routine | what it is | sound in its closure |
|---|---|---|
| `sub_42DBCC` / `sub_42DB80` / `sub_41485A` | the generic LIST DIALOG | **none** — 344 functions, zero play calls |
| `sub_42E938` | the generic text-entry widget | **none** — 155 functions |
| `sub_42EDE0` | the generic yes/no dialog | **none** — 143 functions |
| `sub_42FEB0` | the list's letter-jump | **none** — a leaf, no calls at all |
| `sub_4028D2` | the map/scheme EDITOR screen | **none in its own body or any direct callee** |
| `sub_402595` / `sub_4023A2` | the powerup sub-editor + its prompt chain | **none** |

So the three pickers built on the list dialog — the help/`.BM` browser
(`sub_41431C` -> `sub_414235`), the `*.SCH` picker (`sub_407582`) and the `*.cam`
campaign picker (`sub_4015C6`) — navigate and accept in complete silence. The
only audible thing any of them can produce is the **error box** on an empty glob,
and that is `sub_414340`'s doing, not the list's.

**The two modals, by contrast, DO blip.** `sub_414340` (two-line acknowledge,
blip @`0x414532`) and `sub_41456C` (one-line yes/no, blip @`0x4147B0`) both open
their key loop with an unconditional `sub_427961(20)` for every real key — only
the `-1`/`-2` no-key codes skip it — and **neither plays an accept sting on any
answer**. Every screen in the port that puts up one of these shapes should blip
on any key and sting on none.

**The unconditional any-key blip is the rule, not the exception.** Every front-end
key loop fires `sub_427961(20)` before its dispatch switch, so even an unmapped
key clicks: the menu `sub_42B9CE` @`0x42BB2E`, the player setup `sub_410F81`
@`0x411724`, the level/rounds screen `sub_406DDE` @`0x407094`, the editor chooser
`sub_403184` @`0x403288`, the DRAW loop @`0x42A755`, the RESULTS tally
@`0x42AE04`. A port-only key handler that returns early therefore has to blip
first or it is dropping a sound the original makes.

**The two round-end wait loops differ from `sub_42A088`.** This is the source of
several fidelity bugs and is worth stating plainly:

| | `sub_42A088`'s own loop (boot logos, TITLE) | `sub_42A3F6`'s DRAW @`0x42A73A` / RESULTS @`0x42ADE9` |
|---|---|---|
| any real key | blip 20 | blip 20 |
| Enter / Space | sting 10 | sting 10 |
| **Escape** | **sting 10** (it is one of the three accept codes, `0x42A136`-`0x42A155`) | **nothing** — `0x42A7EF` / `0x42AE9E` set `dword_464A68 = 2` and leave |
| **idle timeout** | forced key 13 applied at `0x42A117`, BEFORE the no-key test — so **blip AND sting** | forced key 13 applied at `0x42A79D` / `0x42AE4C`, AFTER it — so **sting ALONE** |

The VICTORY/TEAM tail is a third shape again: `sub_42A088(name, 0)` then a
blocking `sub_413CB0(3000)`. No key is read for the whole display, so that
screen makes no sound at all beyond the 2000 winner voice fired under it.

**The spooger run is silent.** `sub_41F29B`'s spooge loop (`0x420AAE`-`0x420B6E`)
calls only the bomb constructor `sub_41EB13`, and all four of its exits — bomb
already there, powerup there, tile blocked, allotment spent — jump to `0x420B73`
and from there to `0x420CEC`, **past the entire sound block** at `0x420C26`-
`0x420CC1` (the 40 buzz, the 1200 taunt, the 550 splat, the 100 drop). Only the
plain drop, which places at the player's OWN tile, reaches any of them.

**SFX 40 is not network-only.** `frontend-flow.md` used to claim it "can never
trigger in the boot/menu/results path" and was "correctly absent in the port".
The front-end sites are indeed `sub_40C06A() == 1` guest guards, but the
in-match warphole refusal at `0x420C2B` has no such gate — it is a purely local
event, and the port plays it. Corrected in that file.

## 9. MUSIC: every track, every switch (2026-07-28)

§1-§8 are about SFX. This section is the same treatment for the music channel,
and it closes a batch of audit findings in one go.

**Method: exhaustive.** The same byte-level `E8 rel32` sweep as §8, run over
every offset in the image. `sub_42741E` has **six** direct call sites and its
literal address appears **zero** times anywhere in the image, in code or data —
so there is no table, no function pointer, and "six" is complete, not
best-effort.

| # | site | id | track | who |
|---|---|---|---|---|
| 1 | `0x42B073` | 1000 | TITLE | `sub_42B060`, the boot/title sequence |
| 2 | `0x42BB0C` | 1010 | MENU | `sub_42B9CE`, the main menu |
| 3 | `0x42A436` | 1020 | WIN | `sub_42A3F6`, the Play handler's head |
| 4 | `0x42A6DD` | 1130 | DRAW | `sub_42A3F6`, the round-end outcome tier |
| 5 | `0x42B11B` | 1040 | NETWORK | `sub_42B0CE`, START NET GAME |
| 6 | `0x42B4C0` | 1040 | NETWORK | `sub_42B47D`, JOIN NET GAME |

There is no seventh. In particular **nothing starts 1030** (`lose`) — SOUNDLST
names it and the game never plays it — and nothing re-starts 1020 after site 3.

### 9.1 The menu owns the switch BACK

Sites 5 and 6 have no counterpart that restores the menu track, and they need
none. `sub_42B9CE` is an OUTER loop (one iteration per menu *visit*) around an
INNER one (the per-frame poll). The outer body raises a local restart flag at
`0x42BA69`; the inner loop's head tests it at `0x42BB01`, starts 1010, and
clears it. **Every** dispatch arm — Play `sub_42A3F6`, START NET `sub_42B0CE`,
JOIN NET `sub_42B47D`, Options `sub_4080DC`, Credits `sub_41302D`, the help
browser `sub_41431C`, a cancelled quit `sub_412987` — ends in `jmp 0x42BE06` →
`jmp 0x42B9EA`, the top of the outer body, which re-raises the flag. So
returning to the menu from anywhere restarts 1010 from sample 0.

That also means `sub_42741E` is **not** idempotent on the current id and must not
be made so in a port: coming back from the (silent) help browser genuinely
restarts the menu track in the original.

### 9.2 Round music: the guard is the OPTION, and only the option

The round init `sub_410B6E` ends with, at `0x410E88`:

```
if (dword_4648C0 == 0) tunes_play_wave_tune();   /* 0x410E98 */
else                   free_music();             /* 0x410E91, sub_427342 */
```

`dword_4648C0` is options.ini's **`disable_game_music=`** — the parser at
`0x40650D` matches that exact key string and the writer at `0x405F6F` formats it
back out. Three things follow, all of which the port had wrong:

1. **No network arm.** The guard is a bare test. The port skipped the option
   entirely for online matches on the theory that music is "presentation-only,
   so netplay just keeps music on" — which inverts the argument: being
   presentation-only is precisely why each peer can honour its own setting
   without agreement, traffic or a hash change.
2. **No stage-art arm.** The tune start is not downstream of anything visual;
   `sub_4293E5` is called from the round init, not from the art loader. The port
   had both branches nested inside `if (load_stage(...))`, so a stage whose PCX
   failed to load ran silent.
3. **The ON arm silences the round** rather than leaving the setup track
   running.

The one place this path *does* consult the network is inside `sub_4293E5`
itself (`sub_40C06A` at `0x4294D2`), and it only chooses GENERIC (1120) over the
per-level track — never whether music plays.

`sub_4293E5` is `tunes_play_wave_tune` by its own log string (`0x45A81F`). It
reads `names[1100 + level]`, falls back to **1120** when that slot is empty, and
resolves through the `.hds` ("hard-disk sound") extension; it also builds a
`.cds` ("CD sound") name at `0x42946E` and then never uses it — a vestige of a
cut CD-audio path.

### 9.3 The outcome track is NOT unconditional

`0x42A6DD` (1130) sits behind two gates the round-loop exit passes first:

- **attract**, `dword_464938` at `0x42A6CB`;
- **campaign**, `dword_46489C` at `0x42A63B`.

The campaign gate is the interesting one. A campaign round end branches away
completely: it shows at most one modal (`sub_414340`, only when the pacing flag
`dword_464894` is 2, set by `sub_4016DA`), then calls the round init
`sub_410B6E` again at `0x42A68B` for the next stage. **It never reaches 1130, and
it never reaches DRAW, the RESULTS tally or VICTORY either.** The port shows all
three under 1130; the music half is fixed, the screens half is a known open
divergence recorded here and in `campaign.md`.

### 9.4 There is NO music/SFX mix — the duck was invented

The port ducked music to 55 % of the SFX bus with an uncited constant. The
original attenuates nothing:

- **Master volume** is set exactly once in the whole program, at `0x4194F0`,
  to `0x7FFF` = maximum, during sound-system bring-up. `sub_41AF93`, the setter,
  has one direct caller and its literal address appears nowhere.
- **Per-object volume** defaults to `0x7FFF` too, stored by the object
  constructor at `0x4197A9`.
- **`sub_41A50D`**, the only wrapper around `IDirectSoundBuffer::SetVolume`
  (the single `call [edx+0x3c]` in the image, at `0x41A5A5`, with
  `E_INVALIDARG`/`DSERR_CONTROLUNAVAIL` handling to confirm it), has five direct
  callers and **all five are inside the sound library**, each re-applying an
  object's already-stored level. No game code ever requests a volume.

So music and effects share one bus at full scale, and their relative loudness is
whatever the authored `.RSS` files carry. The duck is removed.

## 10. The death-anim overlay — PINNED (2026-07-28, task #22 closed)

§4 recorded `sub_4278F2(340 + actor[+4])` at `0x41DDE4` as unportable because
`actor[+4]` could not be identified. It is the **death-animation index**.

**Where it comes from.** `sub_41DCB2` is the death handler holding the overlay
call. It has exactly **two** direct callers and its literal address appears zero
times, so the writer set below is complete:

- `sub_41DE63` — the local roll. `n = getvalue(105)`, clamped to `>= 1`, then
  `actor[+4] = rand() % n + 1`, stored at `0x41DF04` immediately before the call.
  **VALUELST id 105 is authored 24**, so the domain is a uniform **1..24** on
  every fresh death. It bails before rolling for a network-owned player (type
  byte 4 — it logs *"Ignoring a network player's supposed death."*).
- `sub_41DE04` — the network apply, storing the peer's value verbatim at
  `0x41DE4E`, except that the LOWMEM flag `dword_464824` forces it to 1. Its
  packet carries three 16-bit words (player, anim, killer), which is exactly what
  the local path packs and sends.

**Three independent locks on the reading**, beyond the arithmetic:

1. VALUELST 105's own comment — *"how many different death animations do we
   have? (die 1 through die 24) \*\*\* if you modify this and play over the net,
   make sure the other guys have EXACTLY the same number of death anims!!!"* —
   matches both the modulo and the fact that the value is replicated.
2. `sub_41F29B` at `0x41F43A` special-cases `actor[+4] == 9` to move the body
   upwards by `getvalue(106)`; VALUELST 106's comment is *"how many pixels per
   reference frame does death anim #9 move upwards (the angel)?"*.
3. SOUNDLST's own comment above 341: *"death anim sounds BASED on which anim is
   chosen (this is just clunk-type sound effects to sync with the anim, no
   screams or anything)"*.

**Which of the 24 are audible — and the old "1 in 24" estimate was wrong.**
`340 + v` spans 341..364. That range is neither culled nor compacted (§3's table
covers 200-299 and 400-499, not 300-399), so authored ids stand. SOUNDLST
occupies **nine** of the twenty-four:

| v | id | clip | what it was authored as |
|---|---|---|---|
| 1 | 341 | `burnedup` | the death overlay itself |
| 10-13 | 350-353 | `1017`, `1036`, `1045`, `trampo` | *"step on a trampoline"* |
| 20-23 | 360-363 | `bombhit1..4` | *"stunned by a bomb landing on your head"* |

So **37.5 % of deaths make an overlay sound, and only 1 in 24 is the clip the
author meant.** The other eight audible values are an **authoring collision in
the original data**: the trampoline and head-hit blocks were written inside the
index space the `340 + N` overlay reserved, and because `sub_4278F2` addresses a
slot directly rather than walking a group, it reaches them. The remaining 15
values hit empty slots and the routine returns without a sound.

This is reproduced faithfully, collision included. Note the contrast that makes
it clearly a collision rather than a design: the trampoline TILE has its own real
caller (`sub_427961(350)` at `0x41EE4C`, gated on tile type 3) and that one is a
four-member **group** pick, whereas the death overlay always plays the same exact
member — anim 10 is always `1017`, anim 13 always `trampo`.

**Port divergence, stated up front.** The port draws the index on the
presentation RNG (root `CLAUDE.md` rule 6), so two peers can hear different
overlays for the same death. In the original the index is a genuine gameplay
value — it selects the death sprite and is replicated — so if the port ever
renders per-anim death visuals the index must move into `State` and be hashed.
As a sound-only feature it stays cosmetic. The LOWMEM force-to-1 arm is not
ported (the port has no LOWMEM mode), consistent with §6.
