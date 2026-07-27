# Goldman Roulette Wheel — `sub_4034BC` — RE

Reverse-engineered from `BM95.EXE` (pseudo.c) plus the shipped
`DATA/RES/VALUELST.RES`, `DATA/RES/SOUNDLST.RES` and `MESSAGES.TXT`
(structure and ids only — text/values stay in the install, never committed).
Extends `docs/re/results-and-options.md` §4, which located the wheel; this
doc pins it fully for a faithful port. All addresses imagebase 0x400000.

## 1. What it is

With the Options-screen **"Gold Bomberman"** toggle on (`goldman=1` in
options.ini, `dword_4648BC`), each round's winner becomes the **gold
player** (`dword_46492C`). On the next entry into the Play flow, an animated
roulette wheel spins and awards the gold player **one bonus powerup** (or a
booby prize) that is added to their starting inventory at every round start
while they remain the gold player. The gold player also sparkles ("gold
twinkle") for the first seconds of each round.

## 2. Trigger site — top of `sub_410F81`, once per Play entry (CORRECTED)

`docs/re/results-and-options.md` §4 placed the call "in `sub_410B6E`'s init
path". The line range was right, the function attribution wrong: pseudo.c
15043-15057 is the **head of `sub_410F81` @ 0x410F81** (the PLAYER INPUT
TYPE screen, def at pseudo.c 14924; `sub_410B6E` is the previous function,
14689-14888, and never calls the wheel). Corrected chain:

- `sub_42B9CE` menu row 0 (Play) → `sub_42A3F6` @ 0x42A3F6 entry: starts
  music 1020 (`sub_42741E(0x3FC)`) then calls `sub_410F81()` (pseudo.c
  29696-29697 — the ONLY call site of `sub_410F81` in the binary).
- `sub_410F81` head, exact gate order (pseudo.c 15043-15057):
  1. `!dword_464938` — **not in attract mode** (frontend-flow.md "Attract
     mode"; the demo match never shows the wheel);
  2. `dword_4648BC` — goldman option on;
  3. `!sub_40C06A()` — local game only;
  4. then `sub_4034BC()`; on return, `if (dword_464A68) return` — an Esc
     inside the wheel aborts the whole Play flow (the caller checks
     `dword_464A68` after `sub_410F81` and skips the match).
- Inside `sub_4034BC` itself the guard is re-checked as
  `dword_4648BC && dword_46492C != -1` (pseudo.c 5960) — with **no pending
  gold player the function is a silent no-op**.

So the wheel runs **once per Play entry, before the player-setup screen is
drawn**, whenever a gold player is pending from a previous match's rounds.
Rounds within one match re-init through `sub_410B6E` only (call sites
29700/29805/30112) and never respin the wheel.

**Who becomes the gold player (PINNED, pseudo.c 30004-30022):**
`sub_42A3F6`'s RESULTS tier writes `dword_46492C` unconditionally on every
RESULTS pass — i.e. whenever `sub_4219B0(...) != -1` (a round SURVIVOR
exists, pseudo.c 29823) and the tier is entered at its own head label; a
no-survivor round instead falls straight into the separate DRAW.PCX branch
and never reaches this write at all, so `dword_46492C` is left untouched on a
draw. The value it writes is the tier's **clinch-winner index** — **NOT the
round winner**, but the **match-clinch winner**. That index is
reset to -1 at the top of every RESULTS pass (pseudo.c 29890) and only set
inside the per-player/per-team tally loop when that slot's cumulative match
tally (`sub_421AC8`, the win count) reaches `dword_464A7C`
(`num_to_win_match`) — or, in team mode with `win_by_kills`
(`dword_46497C`) on, when `sub_421B0F`'s round-kill count reaches the target
AND that slot is the **unique** leader (the tally loop's own "exactly one at
the target" counter equals 1). This is the exact same index already ported
as `GameApp::match_clinch()` (`docs/re/results-and-options.md` §1) — the
"we have a winner" match-over check, not the per-round survivor. So the gold
player only changes **when a match is actually clinched** (VICTORY screen),
never on an ordinary mid-match round win:

The write is a three-way decision, tested in this order:

| condition | `dword_46492C` ← |
| --- | --- |
| `sub_40C06A()` truthy — a networked, non-host machine | **-1** (no gold player; this path is not ported) |
| else, `dword_4648BC` clear — goldman option OFF | **-1** |
| else (goldman ON), `dword_464964` set — **team mode** | **2** if the clinch winner's team byte, fetched by `sub_4223E7`, is non-zero; **0** otherwise |
| else (goldman ON, not team mode) | the clinch winner's **player index** — which is **-1** itself when nobody has clinched yet |

Note the team branch encodes the team as **0 or 2**, not 0/1.

`sub_4223E7` (pseudo.c, already cited in `results-and-options.md` §1)
maps a **player index** to its raw 0/1 team-slot byte; the encoding `? 2 : 0`
is the original's own internal representation for `dword_46492C` (a single
global reused as both a player index and a doubled team id elsewhere in the
engine) — the port stores the equivalent **raw 0/1 team id** instead (see
`GameApp::gold_player_`'s doc comment and the award consumer in
`build_match_config`, which already compares it against `setup_team_[i]`
directly), since our team-id space is 0/1 throughout, not 0/2.

Cleared to -1 by: Esc on the wheel (6091), Esc on the player-setup screen
(`sub_410F81`, pseudo.c 15291), Esc on the LEVEL & ROUNDS screen
(`sub_406DDE`, pseudo.c 8188 — CORRECTED 2026-07-09, see §2.1: previously
undocumented, and previously mis-ported as "back one screen" rather than an
abort), the Options-screen Gold Bomberman toggle AND Team Play toggle (both
row 0 and row 6, pseudo.c 9310-9311/9334-9335/9410-9412/9436-9437 — the Team
Play half was previously undocumented, see §2.1), Ctrl+Q mid-round abort
(the round loop's abort branch, pseudo.c 29737), the net-game screens (30280/30504, N/A —
no netplay in this port), and boot init (14661). The three net-only clear
sites (`sub_4046F5` pseudo.c 6563, `sub_410BBA` pseudo.c 14738/14817/14843)
are `sub_40C06A()`-gated and never reachable in a local-only port; not
pinned further.

### 2.1 Two gaps found in the 2026-07-09 full gold sweep — FIXED

Re-auditing every `dword_46492C`/`dword_4648BC` read/write site (not just the
ones §2's original pass already knew about) turned up two real mismatches
against the port, both presentation-only (no `libs/sim`/golden-hash impact —
`gold_player_` is a `GameApp` int, never read by the sim):

1. **The LEVEL & ROUNDS screen's Esc was ported as "back to the player
   screen"; the original aborts the WHOLE Play flow to the menu.**
   `sub_406DDE` (§2's own citation, "the following level/rounds screen") is
   called from `sub_410F81`'s own TAIL (pseudo.c 15504-15517 — guarded on
   `dword_464A68` being clear, the tail runs `sub_4100B9()`, then a net pump
   `sub_40EA1E`, then `sub_406DDE()`), with
   nothing after that call but `sub_401312()` and return — there is no loop
   anywhere that re-shows the player screen on `sub_406DDE`'s Esc. Its own Esc
   handler (pseudo.c 8186-8191, inside `sub_406DDE`'s frame loop) sets
   `dword_46492C = -1; dword_464A68 = 2;` and returns — the SAME
   abort-to-menu shape as the wheel's own Esc (§5) and `sub_410F81`'s own Esc
   (pseudo.c 15289-15299). `GameApp::present_map_select()`'s Escape handler
   now mirrors this: clears `gold_player_ = -1` and returns `AppInput::Back`,
   and the `StartMatch` caller (`game_app.cpp`, the `present_setup`/
   `present_map_select` loop) now treats that `Back` as a full abort
   (`ev = AppInput::Advance; break;`, matching the wheel-abort branch just
   above it) instead of `continue`-ing back into `present_setup()`.
2. **Toggling Team Play on the Options screen never forfeited a pending gold
   player.** Only the Gold Bomberman row itself (case 6) was ported as a
   clear trigger; case 0 (Team Play, pseudo.c 9310-9311/9410-9412) ALSO does
   `dword_46492C = -1` unconditionally on every press, in both the
   Left/Right-driven switches. `OptionsScreen` now tracks
   `team_play_touched_`/`goldman_touched_` (set on ANY Left/Right press of
   either row, not gated on the net before/after value — see the port note
   below) and `GameApp::present_options_screen`'s exit path checks
   `gold_forfeiting_row_touched()` instead of a goldman-only snapshot diff.

**A third, more subtle nuance fixed at the same time:** the original clears
`dword_46492C` INLINE, at the moment of the keypress, unconditionally — not
by comparing the screen's entry and exit snapshots. A player who presses the
Gold Bomberman (or Team Play) row an EVEN number of times before leaving the
Options screen — ending back at the exact value they started with — still
forfeits a pending gold player in the original (the clear already happened on
the first press and nothing un-clears it). A naive "does the final value
differ from the value on entry" check misses this. `OptionsScreen`'s new
`goldman_touched_`/`team_play_touched_` flags are set on every press of
either row regardless of the resulting value, closing this gap too.

(Files touched: `libs/game/src/game_app.cpp` (`present_map_select`'s Esc
handler + its `StartMatch` caller + `present_options_screen`'s exit check),
`libs/game/include/bomber/game/options_screen.hpp` /
`libs/game/src/options_screen.cpp` (the two `_touched_` flags +
`gold_forfeiting_row_touched()`). Provenance: `sub_406DDE` Esc pseudo.c
8186-8191; `sub_410F81` tail pseudo.c 15494-15519; Options screen switches
pseudo.c 9280-9460.)

## 3. Wheel mechanics — mirror-the-arithmetic

State globals: `dword_45E024` wheel rotation, `dword_45E038` ring (pointer)
position, `dword_45E028` direction (±1), `dword_45E030` wheel step-count,
`dword_45E03C` ring step-count, `dword_45E034` phase (0 = free spin,
1 = winding down, 2 = settled), `dword_45E02C` result (prize id, -1 while
spinning).

**Geometry.** The angular circle is `T = 6 * getvalue(1004)` steps
(VALUELST `1004,70` → T = 420), split into **6 segments of T/6 = 70
steps**. Positions on screen come from the Lissajous helpers
`sub_403382`/`sub_40341F` @ 0x403382/0x40341F (pseudo.c 5876-5919):

```
x(a) = getvalue(1000) + getvalue(1002) * cos(2π * getvalue(1006) * a / T)
y(a) = getvalue(1001) - getvalue(1003) * sin(2π * getvalue(1007) * a / T)
```

with VALUELST `1000,320,240` (centre), `1002,200,150` (radii), `1006,1,1`
(Lissajous X/Y frequency — 1:1 = a plain ellipse). The ids 1001/1003/1007
are the second columns of those rows (flat-array getvalue, same model as the
menu-cursor rows in frontend-flow.md). Float math — presentation only.

**Setup — exactly 5 `rand()` draws, in this order** (pseudo.c 5962-5970,
C `rand_()`, presentation-side):

1. `dword_45E028 = 2*(rand%2) - 1` — spin direction ±1;
2. `dword_45E024 = rand % T` — initial wheel rotation;
3. `dword_45E038 = rand % T` — initial ring position;
4. `dword_45E030 = rand%20 + 20` — wheel step budget (20..39);
5. `dword_45E03C = dword_45E030 + rand%20` — ring step budget (≥ wheel's).

Then palette `"roulette.plt"` is applied (`aRoulettePlt`, the standard
`sub_4151AD`/`sub_411D17`/`sub_4151CC` sequence) and the frame loop starts.

**Per frame** (pseudo.c 5978-6108):

- clear + backdrop draw pass (`sub_415CA4`, `sub_429790` → ROULETTE.PCX);
- **ring mover**: if its budget `dword_45E03C > 0`, step the ring
  `max(dword_45E03C, 6)` times by `+direction` (wrapping mod T). Every time
  the ring lands on a segment boundary (`pos % 70 == 0`) it plays the tick
  **SFX 1300**, and — only in phase 1 — decrements the budget, stopping
  exactly on a boundary when it hits 0;
- **wheel mover**: same shape with budget `dword_45E030`, stepping
  `dword_45E024` by `-direction` (opposite direction to the ring), no tick
  sound, budget likewise only consumed in phase 1;
- draw the **6 prize icons** at angles `wheel + k*70` (k = 0..5) via
  `sub_425C7F(x, y, kind)` — the shared powerup-icon sprite drawer (builds a
  `"power %s"` sequence name from the 18-entry kind-name table
  `off_45BE50`, pseudo.c 26718-26735), so the wheel reuses the normal
  in-game powerup ANI icons, no roulette-specific art;
- draw the **pointer**: ANI sequence `"ring"` (`aRing`) resolved with the
  standard `sub_41D957`/`sub_41DAA7` pair (same as the menu cursor) at the
  ring position. RESOLVED (2026-07-08): `"ring"` lives in
  **`DATA/ANI/MISC.ANI`** (sequence table `cursor1`/`goldman`/`ring`/
  `safe`/`scan`/`teamring0`/`teamring1`, checked against the install) —
  AssetStore's ring probe now tries MISC.ANI first;
- phase 2 only: three result lines centred on the wheel (getvalue 1000/1001)
  — `getstring(790)`, `getstring(800 + prize)`, `getstring(791)`;
- input poll (`sub_4102B7`), see §5.

**Key insight — the spin phases.** In phase 0 the budgets are NOT consumed:
both movers rotate at a constant `max(budget, 6)` steps/frame forever, so
the wheel spins indefinitely until the player acts. Enter/Space switch to
phase 1; from then on every boundary crossing eats one budget point, so the
steps-per-frame (== remaining budget, min 6) shrink — a natural
deceleration that always halts each mover exactly ON a segment boundary.
The outcome is therefore fixed by the 5 setup draws **plus the frame on
which the player presses Enter** (the constant-speed phase advances the
relative phase every frame); after the keypress the wind-down is fully
deterministic.

**Result resolution** (pseudo.c 6060-6074), once both budgets are 0, in this
order:

1. **offset** ← ring position **−** wheel rotation.
2. If the ring position was **below** the wheel rotation, **add T** to the
   offset — the unsigned-wrap fix-up, so offset always lands in 0..T-1.
3. **prize** ← entry `offset / 70` (integer divide by the segment size) of
   the wheel-slot table `dword_45B7BC`.

That table (pseudo.c 1934) holds six entries, in slot order: **0, 1, 3, 8, 4,
13** — the six wheel
slots as **inventory powerup ids**: extra bomb (0), flame (1), kick (3),
goldflame (8), skate/speed (4), and **13 = the CLOGS**, a speed-DOWN booby
prize (VALUELST's own legend for id 91 calls it the "special roulette
power-down"; MESSAGES id 813 names it a speed brake). On settling:
**SFX 1320** (buzzer) if the prize is 13, else **SFX 1310** (clapping);
phase → 2.

## 4. The award — +1 starting inventory at every round init

`sub_4034BC` only stores the prize in `dword_45E02C`; the one-line accessor
`sub_403A9C` @ 0x403A9C (pseudo.c 6133-6136) is its **sole consumer's**
entry point. That consumer is the per-round player reset `sub_4214BC`
@ 0x4214BC (pseudo.c 23864-23962, the routine that also places players on
their spawn cells and seeds speed from getvalue(41)/(42)):

- baseline inventory: `player_byte[86 + j] = getvalue(50 + j)` for
  j = 0..14 — the original's inventory is a **15-byte counter array** at
  player-struct offset +86 (VALUELST ids 50-64; our map documents 50-62,
  the 13 scheme powerups — slots 13/14 are the clogs and one more
  engine-internal slot);
- then, goldman only (pseudo.c 23933-23951): `prize = sub_403A9C()`; if
  `0 <= prize < 15`, the player whose index equals `dword_46492C` gets
  `++player_byte[86 + prize]` — in team mode the comparison is against the
  team id (byte +84 mapped to 0/2), so **every member of the gold team**
  gets the increment.

This is the same +86 inventory array the normal pickup/consume paths
increment/decrement in-round (e.g. pseudo.c 22176/24369), i.e. the prize is
granted as **born-with inventory**, not via the pickup routine. Because
`sub_4214BC` runs at every round init and `dword_45E02C` persists until the
next spin, the current gold player receives the prize **again at each round
start** within the following match; the gold player themselves can change
round to round (RESULTS re-assigns `dword_46492C`). `dword_45E02C` is never
reset on consumption — only a new spin (sets it -1, then the outcome)
changes it; if the wheel was Esc-aborted it stays -1 and nothing is granted.

## 5. Input on the wheel screen

Standard `sub_4102B7` poll (pseudo.c 6075-6106); any real key first fires
the nav blip SFX 20.

| key | effect |
|---|---|
| `13` Enter / `32` Space | phase 0 → phase 1 (start the wind-down); in phase 2, dismiss the screen and continue into the setup screens |
| `27` Esc | abort: `dword_46492C = -1` (forfeits the gold player), `dword_464A68 = 2` → `sub_410F81` returns → `sub_42A3F6` skips the match, back to the menu |
| `315` (F1) | help browser `sub_41431C` (net non-host: SFX 40 buzz instead, the usual `sub_40C06A()==1` gate) |
| others | blip only, spin continues |

Not skippable outright: Enter/Space accelerate the landing but the wheel
still visibly decelerates through its remaining boundary ticks.

## 6. The gold twinkle — where VALUELST 1010 actually lives

`sub_4034BC` never reads getvalue(1010). The "twinkling of goldman" happens
**during the following rounds**: the per-frame HUD pass `sub_420F07`
@ 0x420F07 (pseudo.c 23627-23716) — gated `!dword_464938 && dword_4648BC` —
calls `sub_420D4E` @ 0x420D4E (pseudo.c 23549-23585) for the player (or
each member of the team) matching `dword_46492C`. `sub_420D4E` seeds sparkle
particles into a 100-record pool: while round-elapsed < getvalue(1010)
seconds (VALUELST `1010,5`; the file's legend says 0 = indefinitely), each
call rolls `rand()%6` (spawn 5-in-6) and places a particle at
`(player_x + rand()%40 - 20, player_y + rand()%50 - 48)` — three
presentation-side rand draws per spawn attempt. So the gold player sparkles
for the first ~5 s of every round while goldman is pending.

**One correction to the above:** `sub_420D4E`'s 100-record pool loop finds
only the FIRST currently-inactive slot (the first record whose active flag is
zero) and returns immediately after
its spawn-roll for that one slot — it does NOT scan/fill every free slot on
each call. So each call (once per applicable player per `sub_420F07` HUD-pass
invocation) attempts to spawn **at most one** new particle, not a batch.

### 6.1 The render/expire side — `sub_420E39` @0x420E39, NEVER PINNED — for cross-check with the parallel sparkle-render port

`sub_420D4E` only SEEDS the pool (position + a live/dead flag + a per-particle
frame counter reset to 0); it draws nothing. The actual sparkle draw+animate
pass is a SEPARATE function, `sub_420E39` @ 0x420E39 (pseudo.c 23590-23620),
called UNCONDITIONALLY at the very tail of every `sub_420F07` HUD pass
(`return sub_420E39();`, pseudo.c 23701) — i.e. it runs every frame
regardless of `dword_4648BC`/`dword_46492C`/round-elapsed; only the SEEDING
in §6 is gated on those. This matters for the parallel port: an
already-spawned particle keeps animating (and can still be visible) even
after goldman is toggled off or the gold player changes mid-round, since
nothing here checks those flags — only `sub_420D4E` stops making NEW ones.

**Asset — the ANI sequence is named `"goldman"` (`aGoldman_0`, pseudo.c
1555), resolved ONCE and cached (pseudo.c 23598-23602):** guarded on
`dword_4621EC` still being zero — i.e. it happens on the first pass only,
ever, and never again for the life of the process — the function resolves the
name `"goldman"` through `sub_41D957` into the handle `dword_45BE40`, then
stores `sub_41DA5C` of that handle into `dword_4621EC`, its **frame count**.
The guard is on the frame-count cache itself, not on a separate "resolved"
flag.
`sub_41DA5C(seq)` (pseudo.c 21815-21821) reads the resolved sequence record's
`+52` field, which `sub_41DAA7` (the standard frame-fetch pair used
throughout the engine — same pair the wheel's `"ring"`/prize icons use, §3)
takes `frame % (that same +52 field)` from — i.e. `dword_4621EC` is
unambiguously the **"goldman" sequence's total frame count**, not a
duration. Per the established MISC.ANI sequence table (`cursor1` / `goldman`
/ `ring` / `safe` / `scan` / `teamring0` / `teamring1`,
`docs/re/results-and-options.md` §5d), **`"goldman"` lives in
`DATA/ANI/MISC.ANI`** alongside the wheel's own `"ring"` pointer — not a
new/separate asset to locate.

**Per-frame draw + expire (pseudo.c 23603-23616).** The pool is at
`dword_4621CC` — the SAME pool `sub_420D4E` seeds into — and each record is
**4 dwords wide**:

| slot dword | meaning |
| --- | --- |
| +0 | the **active** flag (0 = free) |
| +1 | particle **x** |
| +2 | particle **y** |
| +3 | this particle's own **frame counter** (reset to 0 at seed time) |

It walks all **100** records in index order and, for each record whose active
flag is non-zero, does exactly this, in order:

1. Fetch frame **+3** of the cached `"goldman"` sequence (`sub_41DAA7` on the
   handle `dword_45BE40`).
2. Draw it with `sub_415A9F` at (**+1**, **+2**), with a third argument of 0.
3. **Only if** the remembered tick `dword_4621F0` differs from the current
   real-frame counter `dword_464994`: increment **+3**, and if the
   incremented value **exceeds** the cached frame count `dword_4621EC`, clear
   the active flag — one full playthrough and the slot is freed. Note the
   short-circuit: on a repeat call within the same real frame the counter is
   not incremented at all.

After the loop it stores the current `dword_464994` into `dword_4621F0`
(remembering this frame's tick) and returns that same value.
Three facts this pins that §6 alone did not:

- **The draw call is `sub_415A9F`, the ordinary player-sprite draw routine**
  (the SAME function the per-tick player mover uses to draw the player's own
  body sprite, pseudo.c 23193/23255/23259/23267/23275/23460 — cited already
  by `movement.cpp`'s disease-scaling doc) — a plain opaque sprite blit, NOT
  an additive/glow blend. No colour/palette argument is passed, so the
  sparkle's colour comes entirely from the `"goldman"` ANI's own baked
  pixels, same as every other ANI-driven sprite in the engine.
- **Each particle's own on-screen lifetime is bounded by the `"goldman"`
  ANI's frame count (`dword_4621EC`), NOT by getvalue(1010).** getvalue(1010)
  only gates whether §6's `sub_420D4E` keeps rolling NEW spawns; once a
  particle exists it plays through the `"goldman"` sequence exactly once (no
  looping — the per-particle frame counter only ever counts up, and
  `sub_41DAA7`'s own internal modulo is never allowed to wrap it back,
  because the "incremented counter exceeds the cached frame count" guard
  deactivates the slot the frame AFTER the last real frame) and then
  disappears, INDEPENDENT of whether goldman/round-elapsed/the gold player
  changed in the meantime. A practical consequence: a particle spawned right
  at the getvalue(1010) cutoff can keep visibly playing for a few more real
  frames past that cutoff while it finishes its own animation.
- **The frame advance is gated on a GLOBAL per-real-frame tick counter**
  (`dword_4621F0 != dword_464994`, `dword_464994` incremented once per real
  engine loop iteration at pseudo.c 29517), not on the 20 Hz sim tick and not
  once per `sub_420F07` call — so if `sub_420F07` were ever invoked more than
  once within the same real frame (it normally is not), the particles would
  still only advance one "goldman" ANI frame that real frame, matching every
  other `dword_464994`-gated per-frame effect in the engine (e.g. the same
  guard shape at pseudo.c 25331/26036).

**Port status: PORTED (2026-07-09).** Landed the same day as this section,
in the parallel worktree the scope note above anticipated
(`33669ec`, "Port id-audit.md presentation-side gaps... gold twinkle"):
`Renderer::update_gold_sparkles`/`draw_world` (`libs/game/src/renderer.cpp`),
fed by `GameApp::set_gold_player`. Matches every fact this section pins —
`goldman_anim_.steps.size()` bounds each particle's lifetime (not
getvalue(1010)), the draw is a plain opaque `draw_anim` call (no
additive/glow blend), and getvalue(1010) only gates whether new sparkles
keep spawning, exactly as `sub_420E39`/`sub_420D4E` do.

(Provenance: `sub_420E39` @0x420E39 pseudo.c 23590-23620; `aGoldman_0`
pseudo.c 1555; frame-count accessor `sub_41DA5C` pseudo.c 21815-21821;
frame-fetch pair `sub_41DAA7` pseudo.c 21825-21836; draw call `sub_415A9F`
first defined ~pseudo.c 18057, its other player-sprite call sites pseudo.c
23193-23460; tick counter `dword_464994` increment pseudo.c 29517; caller
`sub_420F07`'s tail pseudo.c 23701.)

## 7. Assets, sounds, strings, values — summary

| kind | id / name | purpose |
|---|---|---|
| palette | `roulette.plt` (`aRoulettePlt`, pseudo.c 1330) | wheel screen palette |
| image | ROULETTE.PCX (ships in DATA/RES) | backdrop, drawn by the shared pass `sub_429790` |
| ANI seq | `"ring"` (`aRing`, pseudo.c 1331) | the pointer riding the wheel rim |
| ANI seqs | `"power %s"` via `off_45BE50[kind]` | the 6 prize icons — the normal powerup icons |
| SOUNDLST | 1300 | wheel tick (file legend: periodic roulette tick) |
| SOUNDLST | 1310 | good-prize settle (clapping) |
| SOUNDLST | 1320 | clogs settle (buzzer; legend names the molasses/clogs power-down) |
| SOUNDLST | 20 / 40 | nav blip / net "can't do that" buzz (usual gates) |
| music | none started | inherits 1020 (WIN.RSS) from `sub_42A3F6` entry |
| MESSAGES | 790 / 791 | result header / footer lines |
| MESSAGES | 800-813 | prize names by inventory id (800+id); note the .SCH writer `sub_403C16` reuses 800+i as the `-P` row comments |
| VALUELST | 1000,1002,1004,1006 (+cols 1001/1003/1007) | centre, radii, circle resolution, Lissajous params |
| VALUELST | 1010 | twinkle duration, seconds (consumed by `sub_420D4E`, not the wheel) |
| VALUELST | 91 | clogs speed penalty (sim-side, `docs/valuelst-map.md`) |
| VALUELST | 805 | "title at top" legend row — **no `getvalue(805)` call exists in `sub_4034BC`**; the title is presumably baked into ROULETTE.PCX (unreferenced id) |

## 8. Port notes / determinism

Everything on the wheel screen (5 spin draws, twinkle spawns) is
presentation randomness → the port uses a presentation LCG, never
`State::rng`. The gameplay-relevant surface is exactly one input:
**+1 born-with inventory of one powerup kind for the gold player at round
setup** — a `MatchConfig`-level per-player bornwith bump applied before the
sim starts, so no tick-order or `State::rng` impact (same shape as the
scheme's `-P bornwith` column). Two mapping notes for the port:

- prize ids 0/1/3/4/8 map 1:1 onto our `PowerupType` (Bomb, Flame, Kick,
  Skate, Goldflame);
- prize id **13 (clogs)** is outside our 13-kind scheme space
  (`kPowerupKinds`): in the original it is inventory slot 13 whose effect
  subtracts getvalue(91) speed (the skate's mirror image). RESOLVED
  2026-07-08 (§9 below): ported as a dedicated `Player::clogs` count +
  `MatchConfig::born_with_clogs` overlay, NOT a 14th `PowerupType` — clogs
  is never a scheme-configurable/spawnable/forbiddable kind (§9.2), so it
  does not belong in the `kPowerupKinds`-wide tables (`start_with`,
  `limits`, `spawn_counts`, `forbidden`, `born_with`/`born_with_extra`).
  `wheel_prize_to_powerup` keeps returning `PowerupType::None` for id 13 —
  that mapping is correct and permanent (clogs never was a `PowerupType`);
  the wheel's award path now branches on `prize_id == kClogsPrizeId`
  separately instead of routing through `wheel_prize_to_powerup`.

## 9. Clogs effect — pinned (RESOLVED 2026-07-08)

### 9.1 The arithmetic — mover function `sub_41F29B` @ 0x41F29B

The per-tick player mover (pseudo.c 22740-23498, cited by `movement.cpp` for
its disease-scaling order) computes the walk budget added each tick in its
non-trigger-carrying branch (pseudo.c 23430-23440 — the `else` arm of the
test "the upper half of the player-struct dword at **+44** equals -1", the
carried-trigger-bomb marker).

The walk budget is built in exactly this order:

| step | operation |
|---|---|
| 1 | read `getvalue(90)` — "speed added per skate" |
| 2 | budget ← that value **×** the **skate count**, **plus** the player's base speed, the dword at player-struct **+112** (seeded from `getvalue(42)`) |
| 3 | read `getvalue(91)` — "clogs speed penalty" |
| 4 | budget ← budget **−** (**clogs count** × that penalty). The clogs count is the inventory byte at `+86 + 13` |
| 5 | if the **Slow (molasses) disease flag** at player-struct **+132** is set: budget ← budget **/ 3** (integer divide) |
| 6 | if either the **Fast** flag at **+133** **or** the **Super** flag at **+137** is set: budget ← **3 × budget / 2** (multiply first, then divide) |
| 7 | budget ← `dword_464958` × budget / `dword_46494C`, the frame-ratio scale (≈1 at 20 Hz) — the divide is **unsigned** |

Steps 5 and 6 are two independent `if`s, not an if/else, and they apply in
that order — so a player who is both slowed and hyped gets the `/3` first
and the `×3/2` on the already-divided value.

(The skate count and the clogs count are both registers the decompiler flags
"possibly undefined" at this address — it lost their producer across an
earlier branch in this large state-machine function — but the positional
pairing with getvalue(90)="speed added per skate" / getvalue(91)="clogs speed
penalty" (`docs/valuelst-map.md` ids 90/91) and the identical shape to the
port's own `skates * skate_speed_bonus` term pins them unambiguously as the
skate and clogs counts respectively.)

Pinned rule: **base speed (getvalue 42) + skates·getvalue(90) −
clogs·getvalue(91)**, THEN disease scaling (molasses /3 first, then
hyper/super ×3/2) — clogs and skates are mirror-image LINEAR terms folded
into the SAME pre-disease base, added/subtracted in that order, before any
disease multiplier touches the total. No separate duration or decay: like
skates, the count is a per-round-reset inventory value (born-with only,
§9.2), not a timed effect. No floor: the arithmetic does not clamp the
budget to a minimum before disease scaling (matches the port's existing unclamped
`p.speed` for skates); in practice clogs is capped at exactly 0 or 1 per
round (§9.3 — no cross-round stacking), so `base + 0 - 1*150 = 923-150 =
773` is the only non-zero case, well above zero.

### 9.2 Reachability — wheel-only, confirmed by the pickup dispatcher gap

The per-kind pickup dispatcher `sub_41E21E` @ 0x41E21E (pseudo.c 22148-22265)
switches on `kind` for cases 0 through 0xC (0..12) only; kind 13 (clogs) and
14 fall to `default: break` — picking up a floor-scattered clogs/kind-14
token has **no effect through the normal pickup path** (no increment, no
per-kind side effect). The ONLY site that increments `player_byte[86+13]` is
the wheel's born-with grant in `sub_4214BC` (§4). So despite `sub_421F7E`'s
head-hit drop roll (`rand()%15`, pseudo.c 24363) and `sub_4255B2`'s general
scatter machinery nominally covering kind 13 in their index range, clogs can
only ever be **granted** by the Goldman wheel; it is not a spawnable/
scheme/-P-forbiddable/pickup-through-play kind. (This also means our
`PowerupSystem::head_hit`/`scatter` need no clogs case: our port's `surplus()`
switch already `default: return false`s for kinds outside its explicit list,
which is the correct behaviour for clogs — a player can never legitimately
have clogs > 0 via anything but the wheel overlay, and the head-hit roll
should not be able to strip/drop it either, matching the original's dead
code path for kind 13 there.)

### 9.3 No cross-round stacking — reset-then-+1 every round, skate interaction

`sub_4214BC`'s FULL per-round sequence (§4, pseudo.c 23932-23951) is: first
`player_byte[86+j] = getvalue(50+j)` for j=0..14 (the whole 15-byte inventory,
RESET to the VALUELST baseline — id 63 = "not used yet" = 0 is slot 13's
baseline, confirmed against the shipped VALUELST.RES), THEN, goldman only,
`++player_byte[86+prize]` on top of that fresh reset. So the gold player's
clogs count is baseline(0) + 1 = **exactly 1 every round they hold gold**,
never 2, 3, ... — an unbroken gold streak does NOT accumulate a growing
clogs count; each round-init independently re-derives "1 clogs, or 0" from
scratch (mirrors §4's "receives the prize AGAIN at each round start", not a
persistent running total). The SAME reset-then-+1 shape applies to every
other wheel prize (skate/bomb/flame/kick/goldflame) — `born_with_extra`
already models this correctly (a plain overlay re-applied fresh each
`build_match_config` call, §8/`match_config.hpp`); `born_with_clogs` must
follow the identical "set, not accumulate across calls" contract.

Skates and clogs are independent counters (`+90` vs `+99` in the original)
that both fold linearly into the same walk-budget expression — no interaction
beyond both terms being present in the sum (a player who is simultaneously
the wheel's gold player AND has picked up real skates in-round nets `base +
skates*90 - clogs*91`, exactly the port's formula, §9.4).

### 9.4 Port

- `Player::clogs` (new `std::int32_t`, mirrors `Player::skates`): the
  born-with-only clogs count. No `PowerupSystem::apply/remove` case (there
  is no normal-play pickup/drop path, §9.2) — it is set ONLY at `setup.cpp`
  from `MatchConfig::born_with_clogs[i]` (a per-player count, not a
  `kPowerupKinds`-wide bool array like `born_with_extra`, since clogs is
  outside that space per §8).
- Speed formula (`setup.cpp` and `PowerupSystem::apply/remove` for Skate)
  becomes `start_speed + skates * skate_speed_bonus - clogs *
  clogs_speed_penalty`, mirroring §9.1's "(base + skates·getvalue(90)) −
  clogs·getvalue(91)" exactly (skate term
  added, clogs term subtracted, both before the per-tick disease scaling
  already ported in `movement.cpp`).
- `Tuning::clogs_speed_penalty` (VALUELST id 91) added alongside the
  existing `skate_speed_bonus` (id 90).
- Hashed: `Player::clogs` is new gameplay state (a born-with-only counter
  that changes `p.speed`, itself already hashed) — added to `hash.cpp` as
  its own mixed word, a one-time hash-layout growth (CLAUDE.md determinism
  contract rule 5, same pattern as `Player::team`'s addition). Defaults to 0
  on every existing scenario/golden config (no config sets
  `born_with_clogs`), so `mix(0)` at that new word for every player in every
  golden scenario — the digest LAYOUT shifts (recaptured in the same
  commit) but no scenario's GAMEPLAY (positions/timings/outcomes) changes,
  since clogs was unreachable before this port existed.
- Wheel wiring (`goldman_wheel.hpp`/`game_app.cpp`): `wheel_prize_to_powerup`
  is left returning `None` for id 13 (correct — clogs never was a
  `PowerupType`, §8); the award site in `game_app.cpp`'s `build_match_config`
  gains a parallel branch: `prize_id == kClogsPrizeId` sets
  `cfg.born_with_clogs[i]` (or `+= 1` — see `MatchConfig::born_with_clogs`'s
  doc comment for the exact accumulation semantics) for the gold
  player/team, alongside the existing `wheel_prize_to_powerup` branch, not
  instead of it.
- Wheel icon (`sub_4034BC`'s prize-icon drawer, pseudo.c ~6043, calls
  `sub_425C7F(x, y, kind)` for k=0..5 uniformly over ALL SIX slots including
  clogs (§3) — the icon name table `off_45BE50` (pseudo.c 26718-26735) is
  indexed by kind up to at least 13 (`"power %s"` sequence name), so the
  original DOES draw a real clogs icon on the wheel, not a blank/missing
  slot; nothing in `sub_4034BC` special-cases slot 13's drawing (only its
  RESULT-line message id (800+13) and settle SFX (1320 vs 1310, §3) differ).
  RESOLVED 2026-07-09, see §9.5 — the port now draws a real "power clog"
  icon for slot 13 instead of skipping it.

### 9.5 Wheel prize-icon render for slot 13 (clogs) — pinned (RESOLVED 2026-07-09)

`sub_425C7F` @ 0x425C7F (pseudo.c 26718-26735) is the shared floor-powerup
icon drawer the wheel calls for every one of its 6 slots (§3's per-frame
icon-draw loop, pseudo.c 6022-6031, which passes the slot's screen x, its
screen y, and its **raw inventory kind id** read straight out of the wheel-slot
table `dword_45B7BC` — `13` for the clogs slot, no special-casing).

`sub_425C7F` takes three register arguments (x in EAX, y in EDX, kind in EBX)
and does four things, in order:

1. **Build the sequence name** into a 100-byte local buffer: sprintf
   (`sub_4518D0`) of the format string `aPowerS` with the kind's entry from
   the name table `off_45BE50`, i.e. `"power " + <kind name>`.
2. **Resolve** that name to an ANI sequence with `sub_41D957`.
3. **Fetch frame 0** of it with `sub_41DAA7`.
4. **Draw** it with `sub_415920` at the (x, y) it was given, and return that
   call's result.

No palette, tint or colour argument appears anywhere in the chain.

`aPowerS` (pseudo.c 1566) is the literal format string `"power %s"`.
`off_45BE50` (pseudo.c 2261-2280) is an 18-entry table of name strings,
"weak"-typed in the decompile. Its contents, by index:

| 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|---|
| bomb | flame | disease | kicker | skate | punch | grab | spooge | goldflame |

| 9 | 10 | 11 | 12 | **13** | 14 | 15 | 16 | 17 |
|---|---|---|---|---|---|---|---|---|
| trigger | jelly | disease3 | random | **clog** | (unused) | (unused) | (unused) | (unused) |

Index **13 = `"clog"`** — so for the clogs wheel slot `sub_425C7F` builds
the sequence name `"power clog"` and resolves it the identical way as the
other five slots (`sub_41D957`/`sub_41DAA7`, the same pair the pointer's
`"ring"` sequence and the in-round floor powerups use). CONFIRMED against
the shipped `DATA/ANI/POWERS.ANI`: a byte scan of the file shows a `SEQ`
chunk named `power clog` immediately after `power random` (the 13th of 13
named sequences: bomb/flame/kicker/disease/punch/skate/jelly/grab/spooge/
goldflame/disease3/trigger/random/clog) — the icon is a real shipped asset,
not a placeholder or reused sprite; no recolour is applied (the wheel's
icon loop carries no palette/tint argument, same as the other 5 slots, §3).

Port (landed in `f374e22`): `SequenceSet::clogs_anim` (`sequences.hpp`/
`sequences.cpp`) resolves `a.powers()` sequence `"power clog"` outside the
`kPowerupKinds`-indexed `powerup_anim[]` loop (clogs is not a
`sim::PowerupType`, §8/§9.2, so it cannot share that table).
`GoldmanScreen::draw()` (`goldman_screen.hpp`) special-cases
`prize_id == kClogsPrizeId` in its 6-icon loop to draw `seqs_->clogs_anim`
instead of routing through `wheel_prize_to_powerup` (which correctly stays
`PowerupType::None` for id 13, §8) — mirroring `sub_4034BC`'s own
uniform-loop-with-uniform-lookup shape (one drawer, one name table, no
branch on slot index) even though the port's data model necessarily splits
clogs out of `PowerupType` space.

(Provenance: `sub_425C7F` @ 0x425C7F pseudo.c 26718-26735; `off_45BE50`
pseudo.c 2261-2280; `aPowerS` pseudo.c 1566; call site `sub_4034BC`
pseudo.c 6022-6031; asset confirmation `DATA/ANI/POWERS.ANI` byte scan,
"power clog" SEQ chunk present.)

(Provenance: `sub_4034BC` @ 0x4034BC pseudo.c 5921-6110; helpers
`sub_403382`/`sub_40341F` pseudo.c 5876-5919; wheel slots `dword_45B7BC`
pseudo.c 1934; accessor `sub_403A9C` pseudo.c 6133-6136; award site
`sub_4214BC` pseudo.c 23864-23962; twinkle `sub_420F07`/`sub_420D4E`
pseudo.c 23627-23716/23549-23585; trigger `sub_410F81` head pseudo.c
15043-15057 (def 14924), caller `sub_42A3F6` pseudo.c 29696-29697; gold
player assignment pseudo.c 30004-30022; mover speed arithmetic `sub_41F29B`
pseudo.c 23430-23440 (function def 22740; the skate-count and clogs-count
registers are the two the decompiler flags "possibly undefined", at
addresses 0x41FD13 and 0x41FD30);
pickup dispatcher `sub_41E21E` pseudo.c 22148-22265 (no case 13/14); head-hit
drop roll `sub_421F7E` pseudo.c 24332-24378; VALUELST rows 90/91/805/
1000-1010; SOUNDLST 1300/1310/1320; MESSAGES ids 790/791/800-813.)
