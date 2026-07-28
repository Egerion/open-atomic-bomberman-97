# BM95.EXE — in-round input shell, HUD, and round-end sequence

Facts distilled from static analysis of `BM95.EXE` (Watcom C, imagebase
0x400000) and the shipped install. **Facts only — addresses and observed
behaviour, never copied code.** Companion to `docs/re/frontend-flow.md` (the
menu/boot/results screen flow this document's round loop is nested inside).

Scope: what happens **during** a live round (`GameApp::run_match`'s
territory) — the auxiliary (non-movement) keys the original polls every
frame, the in-round HUD, and the round-end shell between "one side left" and
the results tier. This has never been RE'd before; our port currently
handles only Esc (bail to menu) and gameplay input in `run_match` — this
document says exactly how that compares.

## The round driver — `sub_42A3F6`, nested inside the Play handler

`sub_42A3F6` (@ 0x42A3F6) is the **entire Play flow**: player setup →
round → results tier → repeat-or-exit. It is reached from the main menu's
row 0 (`docs/re/frontend-flow.md` "main-menu items") and from its own tail
(loops back to itself for the next round of a multi-round match). Structure,
top to bottom:

1. `sub_42741E(0x3FC)` — starts the looping "win" music track (1020) at
   handler entry (`docs/re/frontend-flow.md` "Results MUSIC").
2. `sub_410F81()` — player/level setup screens (goldman wheel, player slots,
   LEVEL & ROUNDS); returns early for attract mode.
3. `sub_410B6E()` — level load + round init (chooses the stage, resets the
   round timer via `sub_4104C2`/`sub_410494`).
4. **`sub_43A6FC(sub_42A191)`** — registers `sub_42A191` (@ 0x42A191) as
   a **per-frame tick callback**. This is the actual gameplay driver: it
   advances `dword_464994` (the frame/tick counter), calls the movement/AI/
   physics chain, the per-player render-and-cornerhead pass (`sub_420F07`,
   which is what calls the per-player mover `sub_41F29B` — the function named
   in this task's brief, confirmed at pseudo.c 23659), the clock/HUD draw
   (`sub_4105D2`, below), and the "hurry" flash. When the round ends it is
   first **suspended** (`sub_42A16F(1)` at pseudo.c 29821 sets the guard
   `dword_4646AC`, making the callback a no-op) for the duration of the
   DRAW/RESULTS/VICTORY screens — re-enabled (`sub_42A16F(0)`, 30113) if a
   multi-round match loops back into the next round — and only
   **unregistered** (`sub_43A74C(sub_42A191)`, at the teardown label
   pseudo.c 29815-29818) when the Play flow exits for good. Either way, gameplay
   ticking and its render pass genuinely stop the instant the round-driver
   loop exits, before the outcome screens run their own bespoke wait loops.
5. **An endless loop whose every iteration is: poll ONE key code from
   `sub_43A508`, dispatch on it through the auxiliary-key chain, then do the
   standard per-frame work** (pseudo.c 29705-29819) — the loop this task
   calls "the in-round main loop". `sub_43A508` polls one already-remapped virtual key
   code per iteration (same primitive used by the menu/boot code, chained
   through `sub_43A56C`/`sub_43A624`/`sub_43DF28`; `sub_43DF28` applies the
   player's key-remap table, so the numeric codes below are **post-remap
   virtual key ids**, not raw scancodes — consistent with `docs/re/
   results-and-options.md`'s `sub_407B9D` key-remap UI using the same id
   space, e.g. **328/336/331/333 = Up/Down/Left/Right, 315 = F1**
   everywhere else in the front end).
6. Round-end shell: DRAW or RESULTS-tally wait loop, then VICTORY/loop-back
   (below).

(Provenance: `sub_42A3F6` @ 0x42A3F6, pseudo.c 29610-30152; `sub_42A191`
@ 0x42A191, pseudo.c 29488-29557; `sub_410B6E` @ 0x410B6E, pseudo.c
14689-14848 [level/round init, not the tick loop — an earlier task
assumption that `sub_410B6E` contains a per-frame loop calling `sub_41F29B`
is corrected: `sub_410B6E` runs once at round setup, not per frame]; per-
player mover call site pseudo.c 23659 inside `sub_420F07` @ 0x420F07, itself
called once per tick from `sub_42A191` at pseudo.c 29527.)

**Port status: pipeline audited, one port-added latency source found and
fixed (2026-07-10, prompted by a "Hockey Rink movement feels different"
report after the ice mechanic itself was proven byte-faithful — `facts.md`
"Ice / input-lag").** The original's bound here is `sub_42A191` firing once
per displayed frame off the flip loop, reading live key state
(`sub_41E61E`) inline in the SAME callback that consumes it — so a keypress
waits at most one display frame plus up to one 50 ms tick. `GameApp::
run_match` (`game_app.cpp`) mirrors this shape correctly: `SDL_PollEvent`
drains the queue once per outer iteration, `collect_inputs()` calls
`SDL_GetKeyboardState`/`SDL_GetGamepadButton` fresh and inline as each
tick's argument (no intermediate buffering hop, no stale cache reused across
a catch-up burst), and the fixed-timestep accumulator schedules ticks off
the same timestamp the poll just observed. The one thing that DIDN'T match:
`SDL_Delay(2)` — this loop's pre-vsync throttle, still present after
`SDL_SetRenderVSync(1)` was added (`GameApp::init`'s own comment there) —
ran unconditionally AFTER an already vsync-blocked `SDL_RenderPresent`,
stacking a flat 2 ms of dead time onto every rendered frame for no
presentational benefit, delaying this loop's next `SDL_PollEvent` (the only
point a fresh keypress becomes visible to `collect_inputs()`). Fixed by
skipping it whenever `SDL_GetRenderVSync` reports vsync is actually active,
keeping it only as the fallback throttle on a driver where vsync is a
no-op. Empirically measured (temporary instrumentation, `BOMBER_GAME_DIR`
pointed at the real install, synthetic held keypresses via Win32
`SendInput`, compared physical-keydown timestamp against the sim tick index
that first reflects it pressed): mean keydown-to-tick latency dropped from
~29 ms to ~23 ms across paired runs, with both before and after staying
within the same one-frame-plus-one-tick ballpark the original's own bound
implies (observed worst case ~56 ms either side, i.e. just over one 50 ms
tick, not the 2-3 extra ticks a gross pipeline bug would add) — confirming
the fix trims a real but modest per-frame excess rather than papering over
a larger structural latency bug. No `libs/sim` change; golden untouched.

**Frame-pacing follow-up (2026-07-11, prompted by an "in-game is now
slightly stuttery" report after the above): "vsync-or-nothing" was not
enough — replaced by an explicit refresh-boundary pacer.** The 2026-07-10
fix assumed `SDL_RenderPresent` blocks on vblank whenever
`SDL_GetRenderVSync` reports vsync on. On Windows windowed mode it does
not, reliably: DWM gives the swapchain a multi-frame flip queue, so
presents return instantly in bursts (measured 4-12 ms frame deltas) until
the queue fills, then stall (20-25 ms). The sim's accumulator crossings
land on that jerky CPU-side train, and ticks get assigned to rendered
frames in 2/4-frame beats instead of the steady 3-frames-per-tick that a
20 Hz sim on a 60 Hz display needs. Measured (temporary env-gated
instrumentation, `SDL_GetTicksNS` delta per `SDL_RenderPresent` + sim
ticks per frame, ~900-frame live runs against the real install,
interleaved trials): with the delay skipped, 0.7-41% of tick-to-tick gaps
were a frame off (run-to-run spread tracks DWM/occlusion state); the old
unconditional `SDL_Delay(2)` partially damped queue-stuffing (0-20% off)
— which is why the skip read as a NEW stutter — but neither is correct.
`run_match` now sleeps to the next display-refresh boundary after each
present (`SDL_DelayNS`, target advanced by the display mode's
`refresh_rate_numerator/denominator` period, re-phased whenever present
itself blocks past the target, 60 Hz fallback when the mode is unknown),
and the tick accumulator runs on `SDL_GetTicksNS` (the old millisecond
`SDL_GetTicks` quantization was a second, smaller beat source). Post-fix
measurement: every tick gap exactly 3 frames (299/299), tick cadence
50.00 ms mean / 0.49 ms stdev, frame delta 16.67 ms mean / 0.45 ms stdev.
The 2026-07-10 latency win is preserved: one `SDL_PollEvent` +
`collect_inputs()` sample per displayed frame, no fixed extra delay on
the input path — the pacer's sleep is a no-op whenever present genuinely
blocks, and otherwise supplies exactly the block vsync failed to provide.
Presentation-only; no `libs/sim` change; golden untouched.

## Input acquisition — the GAMEPLAY key reader, CONFIRMED (2026-07-11)

Prompted by the "bomb placement sometimes just vanishes" report (the sim's
placement gate was separately proven faithful by a 30k-trial fuzz —
`tests/test_placement_diag.cpp` G/H/I/J/K): is losing a short tap AUTHENTIC
to the original, or a port infidelity? Traced the whole keyboard path:

- **The gameplay reader `sub_41E61E` @ 0x41E61E reads a LIVE STATE ARRAY,
  not the event queue.** Its keyboard case (`+16 == 2`, pseudo.c
  22326-22356) tests `byte_4A2BA0[<configured scancode>]` directly for all
  six keys (4 directions → the local godir flags, bomb/action →
  `+56`/`+57`). `byte_4A2BA0` (0x4A2BA0) is the engine's per-scancode
  down-state table.
- **The array's only writer is `sub_433E14` @ 0x433E14** (pseudo.c
  36389-36394): make sets `byte_4A2BA0[scan] = 1`, break sets `0`, a make
  while already down sets `2` (auto-repeat marker) — and it separately
  appends a 24-byte event record to the queue at `dword_45C528`. That queue
  feeds ONLY the auxiliary reader (`sub_43A508` →
  `sub_43A56C`/`sub_43A624`/`sub_43DF28`, the aux-key table above);
  gameplay never pops it.
- **The feed is DirectInput BUFFERED data, drained by the per-frame pump.**
  `sub_43B5EC` @ 0x43B5EC (called from `sub_43A508`, i.e. once per
  displayed frame) loops `sub_4449B0` @ 0x4449B0 — vtbl offset +40 =
  `IDirectInputDevice::GetDeviceData`, 32-record batches into 0x4A3EA0 —
  through `sub_43B700` @ 0x43B700 (scancode translate + extended-prefix +
  the software auto-repeat synthesizer at `dword_4A2DB0` timestamps) into
  `sub_433E14`, then runs the `PeekMessageA` pump.
- **Consumption cadence = one edge check per displayed frame.**
  `sub_41F29B` shuffles the key bytes (`+54=+56`, `+55=+57`, then
  `+56=+57=0`; pseudo.c 22976-22979) and re-reads them via `sub_41E61E`
  (23037) once per `sub_42A191` frame callback; the drop block is edge-gated
  on `+56 && !+54` at that same cadence.

**Verdict: tap loss exists in the original but its window is ONE DISPLAYED
FRAME (~14-16 ms at 60-70 fps), not one 50 ms tick.** A press and release
whose DirectInput records are both drained by the same per-frame pump (tap
entirely inside one frame) leaves the array at 0 by read time — lost, same
as any state-poll model. But any tap spanning a frame boundary is seen down
by that frame's read and fires the edge. The port sampled
`SDL_GetKeyboardState` only once per 50 ms tick, widening the original's
loss window ~3x — a normal ~30-40 ms tap could vanish entirely, which is
exactly the reported symptom. **Fixed (presentation-side, golden frozen):**
`GameApp::run_match` now samples `collect_inputs()` once per rendered frame
(this loop is the port's flip-loop callback equivalent) and latches
action1/action2 downs until the next tick consumes them (`tap_latch`),
restoring the original's frame-cadence capture. Directions are deliberately
NOT latched: they are level-driven (the original integrates held time in
ms, so a sub-tick direction tap moved a few px at most — stretching it to a
full 50 ms budget would overshoot the original more than dropping it does),
while the action keys are edge-consumed, capture-or-lose. The sim's
`TickInputs` contract is unchanged — only how the shell fills it.

## The auxiliary key table — EXHAUSTIVE, CONFIRMED

Reading the exact nested-if chain at pseudo.c 29709-29788 (not paraphrased —
every comparison in the chain is accounted for below):

| key code | gate | action | notes |
|---|---|---|---|
| `1` | `sub_413D01()` (debug/skip-logos flag) | `sub_42A325()` — dumps a debug text file | **debug cheat**, see below |
| `4` | none | `sub_413BB0()` — arms the DOS text-mode debug overlay (`dword_45BCD4=1`, `sub_42C098(0)`) | **debug cheat**, see below; no visible in-game effect without the mono/color text-page overlay actually being read by a debugger/second monitor — effectively inert on a normal VGA session |
| `0x11` = **17** | none | `dword_46492C=-1; dword_464A68=2;` — **abort round, forfeit, back to menu** | the SAME target the menu's Escape handler jumps to (`docs/re/frontend-flow.md`'s Escape/Quit dispatch — condition "code ≥ 17 AND (code ≤ 17 OR code == 27)", i.e. exactly {17, 27} — uses the identical 17/27 pairing) — see "Esc negative finding" below for why only 17 fires here |
| `18` | `sub_413D01()` | `sub_4165D2()` — toggles a display mode (`dword_460BA4` ? `sub_416591` : `sub_4162F0`) | **debug cheat**, gated — now fully characterized, see below |
| `0x111` = **273** | none | `dword_4646B4 = 1` | **not an abort** — arms a flag consumed only by the DRAW/RESULTS wait loops' 6 s auto-advance test (`(sub_42247A()\|\|dword_4646B4) && time>t0+6000`, pseudo.c 29838/30062); reset to 0 at round entry (29704). Effectively a "skip the outcome screens quickly" latch, not a live pause/skip of the round itself |
| `288` | none | `sub_413D45()` — opens the **"Internal debugging information window"** (MESSAGES.TXT id 400) | **debug cheat**, NOT gated by `sub_413D01()` — reachable in retail; shows total/audio mem, local net id, BOMBER_ID, critical-retrans rate, audio cache hit % (msg ids 405/410/411/415/420); dismissed by Enter/Esc. Same handler the main menu binds to raw code 288 (`docs/re/frontend-flow.md`'s table) |
| `[0x112, 0x131]` = **274..305** | none | `sub_40C678()` | **network-only stats dump** — gated internally on `sub_413D01()`, writes a per-tick statistics text file (`aStatisticsForA` header, 200 `sec/byte-in/byte` lines) via `fopen(..., "at")`; a no-op with no visible effect in local play beyond the file I/O |
| `0x13B` = **315** (F1) | `!sub_40C06A()` (local only) | `sub_41431C()` — opens the generic `.BM` help browser **without leaving the round** | same routine the main menu's row 5 opens (`docs/re/frontend-flow.md` "main-menu items", corrected entry) |
| `324` | mixed | network ready/chat message send (`sub_40FB90`/`sub_40CE27`) + (if `sub_413D01()` and a pending gold player) commits the gold pick | **network-only**, N/A locally |
| `27` (Esc) alone | — | **NOTHING** — falls through both `< 0x1B` and `> 0x1B` branches untouched | see "Esc negative finding" below |
| anything, while `dword_464938` (attract mode) | — | `dword_46492C=-1; dword_464A68=2;` (same as key 17) | any key aborts an attract-mode demo round back to the menu — already documented in `docs/re/frontend-flow.md` "Attract mode" |

All codes not listed above (including ordinary movement/gameplay keys) fall
through to the loop's shared tail label and are consumed by the normal per-tick input
collection elsewhere in the tick callback (`sub_42A191`'s chain) — outside
this loop's switch, which only handles the auxiliary set above.

(Provenance: nested-if chain pseudo.c 29709-29788, transcribed range by
range above; `sub_42A325` @ 0x42A325 pseudo.c 29577-29603; `sub_413BB0`
@ 0x413BB0 pseudo.c 16683-16688; `sub_42C098` @ 0x42C098 pseudo.c
31004-31020 [installs/clears a debug render callback; `0x42C184`/`0x42C1E8`
are literal `0xB0000`/`0xB8000` — the MDA/CGA text-mode video segments,
confirming this is a DOS-text-page debug overlay]; `sub_4165D2` @ 0x4165D2
pseudo.c 18408-18415; `sub_413D45` @ 0x413D45 pseudo.c 16752-16832
[`sub_43C734`/`sub_41696C` modal, msg ids 400/401/405/410/411/415/420,
dismiss on Enter(13)/Esc(27) — pseudo.c 16822-16829]; `sub_40C678`
@ 0x40C678 pseudo.c 11415-11439; MESSAGES.TXT ids `400,"Internal debugging
information window"` / `405,"Total mem: %u, Audio mem: %u"` /
`410,"Our local net id: %u"` / `411,"BOMBER_ID: %u"` /
`415,"Critical retrans rate:%6.3f"` / `420,"Audio cache hits: %u%%"`.)

### Pause negative finding — NO PAUSE KEY EXISTS (CONFIRMED)

**The original has no player-facing pause.** Every comparison in the round
loop's key chain is enumerated in the table above; none of them is a pause.
'P' (80) and 'p' (112) both land in the `> 0x1B` branch below `0x111`,
where the chain does nothing; no other code freezes the game either. Two
things LOOK like a pause and are not, for the record:

1. **The automatic round-timer freeze** — `sub_421969() <= 1` (one side
   left) → `sub_410522()` clears the timer-running flag `dword_4601B4`
   (pseudo.c 29791-29792). Game state keeps ticking (death animations play,
   the tick callback stays registered); only the clock stops. Not
   key-triggered. (Details under "Timer auto-pause" below.)
2. **The modal tick suspension** — `sub_42A16F(x)` @ 0x42A16F (pseudo.c
   29479-29484) is a bare setter of `dword_4646AC`, and `sub_42A191` (the
   tick callback) is a complete no-op while that guard is set (pseudo.c
   29504). Opening the F1 help browser mid-round is bracketed
   `sub_42A16F(1)` … `sub_41431C()` … `sub_42A16F(0)` (pseudo.c
   29769-29771), so **the whole game freezes under the help overlay** —
   sim, rendering, HUD, everything. This is the closest thing to a pause
   the shipped game has, and it is a side effect of the modal, not a pause
   feature. Note the **round clock still burns wall-clock time under it**:
   `sub_4105D2` accumulates `dword_4601B8 += now - dword_4601A0` per call
   while the timer-running flag is set, and nothing clears that flag around
   the F1 bracket — so the first tick after the modal closes adds the
   entire modal duration to the round clock in one lump. Reading the manual
   mid-round costs you round time.

3. **The engine's own "Paused" dialog exists — and is explicitly disabled at
   boot (CONFIRMED 2026-07-09, closes a coverage question raised against this
   doc).** The literal string `aPaused` (pseudo.c 1640, `"Paused"`) IS built
   into a real modal window with a "Done" button (hotkey 27/Esc) — but the
   function that builds it is **`sub_43A7C0`** (pseudo.c 42103-42134,
   `sub_43C734`/`sub_432298` dialog chrome — the SAME primitives
   `docs/re/frontend-flow.md`'s "sub_43C734 dialog-chrome primitive" section
   already pins), not "`sub_43A7D4`" — that address appears only inside
   `sub_43A7C0`'s own body, as a decompiler diagnostic about an undefined
   local at that instruction, not as a separate function; no `sub_43A7D4`
   function exists in the binary. Tracing every caller, root to leaf:
   - `sub_43A7C0` is reachable **only** as the default value of a function
     pointer, `dword_4A37BC = sub_43A7C0` (pseudo.c 41809, set once at
     windowing-engine init, `sub_43A400`).
   - `dword_4A37BC` is called from exactly one site: `sub_43A784()` (pseudo.c
     42082-42096), whose body is three steps — call through the function
     pointer, then spin polling `sub_43A508` until it yields 27, then tear
     down —
     i.e. open the dialog, then loop the SAME low-level key-poll primitive
     `sub_43A508` (`docs/re/in-match-shell.md`'s own round-driver section
     already cites this as the primitive every context — round loop, menu
     loop — reads keys through) until it returns 27 (Esc), then tear the
     window down. This genuinely IS a generic, reusable "Paused" modal in the
     engine layer, callable from anywhere `sub_43A508` runs.
   - `sub_43A784` itself is called from exactly one site: `sub_43A594`
     (pseudo.c 41897-41927), the raw-key dispatcher every `sub_43A508` call
     runs through BEFORE the caller-specific key remap — when the incoming
     key id equals `dword_4A37B4` it calls `sub_43A784()`, where
     **`dword_4A37B4` is the "pause key"
     id, defaulted to `281`** at the same `sub_43A400` init (pseudo.c 41808).
     Had this default survived, key 281 would have opened "Paused" from
     EVERY `sub_43A508` call site in the whole binary (menu, round loop,
     everywhere) — a genuine engine-level pause feature, likely shared with
     other titles built on this same GNW-style windowing layer.
   - **It doesn't survive.** BM95's own boot sequence,
     `sub_414DF4` (the `WIN_INIT` log line owner, pseudo.c 17522-17561),
     calls **`sub_43A8BC(-1, 0)`** (pseudo.c 17542) — the very first
     statement after confirming the window system came up — which sets
     `dword_4A37B4 = -1`. Since `sub_43A594` first checks that the incoming
     key id is **not** -1 and only then compares it against `dword_4A37B4`,
     and `dword_4A37B4` is now itself -1,
     the pause branch can **never** fire again: a real key's raw id is only
     ever compared against -1 while already known not to equal -1. This
     makes `sub_43A7C0`/`sub_43A784` provably unreachable for the rest of
     the process's life — a deliberate, one-line opt-out, not a coincidence
     or an unremapped key.
   - (Aside, not this lead's scope: the sibling pause-key global,
     `dword_4A37B0` = **302** — a DIFFERENT `sub_43A594` branch calling
     `sub_43A8D4`, a video-mode-toggle-looking routine — is never touched by
     `sub_43A8BC`, so it is NOT proven dead by this trace; out of scope
     here.)

   **Conclusion: this doesn't contradict "NO PAUSE KEY EXISTS" — it explains
   the mechanism.** The engine BM95 is built on ships a real, working, generic
   pause dialog; BM95 turns it off with one call before any menu or gameplay
   code runs. There is no reachable trigger for it anywhere in the retail
   EXE. Nothing to port; the finding is recorded here because it pins the
   *reason* no pause exists (an explicit opt-out of a real feature) rather
   than leaving it as "no code path happens to call it".

So: our port having no pause handling in `run_match` is **faithful** — a
pause key would be an invention (doubly so now: the original's own engine
offers one and BM95 explicitly declines it). If we ever port the mid-round F1
help, the faithful behaviour is "freeze the sim, let the clock keep running".

### Esc negative finding — CONFIRMED, and it changes what our port's Esc should do

**Raw Esc (27) alone does NOT abort an in-round match in the original.**
Tracing the exact comparison chain: the outer dispatch splits the key code
into a **"less than 0x1B"** arm (pseudo.c 29709) and a **"greater than
0x1B"** arm (29743) with no equal case —
**27 (`0x1B`) satisfies neither branch** and falls straight through to the
attract-mode test at pseudo.c 29788, which jumps to the abort branch only
when `dword_464938` is set. In a real, player-controlled round, pressing Esc
reaches the shared fall-through label at the loop's tail having done
**nothing** — no abort, no pause, no visible effect.

**The key that actually aborts/forfeits mid-round is `0x11` = 17 (CONFIRMED
Ctrl+Q).** This is the same numeric-control-code pattern already established
elsewhere in the binary: `docs/re/frontend-flow.md`'s main-menu Ctrl+E-×6
map-editor trigger uses raw code 5 = the ASCII control code for Ctrl+E; by
the identical convention, ASCII control code 17 = **DC1 = Ctrl+Q**. The menu
loop's own Escape/Quit dispatch — whose condition is "code ≥ 17 AND (code ≤ 17
OR code == 27)", i.e. exactly {17, 27} (`docs/re/frontend-flow.md` line
~30861) — treats 17 and 27 as synonyms **only
there**. In the round loop the two codes are NOT synonyms: only 17 is
wired to the abort branch, 27 is not reachable by that branch at
all. So in-round, **Ctrl+Q forfeits/aborts to the menu; Esc is inert**.

This directly changes the answer to the task's framing question ("what
should our Esc-to-menu really do — instant quit, confirm prompt, or
forfeit?"): the original's answer is **none of those for Esc** — Esc is not
bound to anything mid-round; the real abort key is Ctrl+Q, and when it
fires it is an **instant, unconfirmed forfeit** (`dword_464A68=2` with no
dialog, immediately followed by the standard teardown at the two shared tail
labels (pseudo.c 30139 / 29815): `sub_40FB44` [network-notify, N/A locally] → a 10×100 ms drain
loop pumping `sub_40EA1E`/`sub_413CB0` → `sub_43A74C` unregisters the tick
callback → return to the menu). There is no confirm prompt anywhere in this
path (contrast the main menu's Quit row, which does pop one).

**Port status (documented gap, not fixed by this doc — DOCS-ONLY task):**
`GameApp::run_match` currently maps SDL's Esc key to `AppInput::MatchOver`
(`libs/game/src/game_app.cpp:1555-1556`), i.e. our port's Esc behaves like
the original's **Ctrl+Q** (instant, unconfirmed forfeit) rather than like
the original's literal Esc (inert). Functionally the port's Esc gives
players a working "quit to menu" affordance the original also has — just
bound to a different key. Of the auxiliary keys, **F1 is now wired**
(2026-07-08): `run_match` opens the generic `.BM` help browser
(`GameApp::present_help_browser_modal`) with the sim tick loop suspended for
the modal's duration, mirroring this doc's `sub_42A16F(1)/(0)` bracket — with
ONE deliberate deviation: on close the frame accumulator is reset, so the
round clock does NOT absorb the modal's wall-clock duration (the original's
documented lump-sum clock burn under "Pause negative finding" point 2 is a
bug we chose not to reproduce). The 288 debug window and the other
debug-gated keys (1/4/18/274-305) were audited 2026-07-09 (see "Debug/cheat
keys — summary" below, `docs/re/coverage-audit.md` row #35) and confirmed
**N/A, not a port gap**: each is either OS/hardware-specific dead weight,
unresolvable at the decompiler level, netplay-only (ADR-0003), or would
require inventing diagnostic content our engine has no equivalent
instrumentation for. Faithfully matching the original key-for-key would mean
binding forfeit to Ctrl+Q and leaving Esc inert, which would be a worse
player experience than what we have; this is flagged as a fact, not a
recommendation to regress the binding.

## The in-round HUD — CONFIRMED: a live countdown clock IS drawn (refutes "bare field")

The original draws a **numeric round-timer HUD every tick** — the task's
framing question ("does the original draw a clock/score line, or is the
field bare?") is answered: **there is a clock; there is no score/kill
line.**

`sub_4105D2` (@ 0x4105D2) is called once per tick from `sub_42A191`
(pseudo.c 29518), unconditionally (no debug gate, no options toggle found).
Its body:

1. Advances the round clock (`dword_4601A4`/`dword_4601BC` = seconds
   elapsed/remaining), driven off `sub_43ACF8()` (the same millisecond clock
   used throughout) — this is the SAME timer `sub_41087D()` reads to decide
   "time up" and `sub_410522()`/pseudo.c 29792 pauses while ≤1 side is alive
   (see "timer auto-pause" below).
2. **Untimed rounds (`dword_4601A8 == 1001`)** draw the literal glyph string
   `aInfinity` ("∞") at `getvalue(110)`, `getvalue(111)` instead of digits —
   so the HUD still appears, just showing infinity, when a round has no time
   limit.
3. **Timed rounds** format `MM:SS` via message **`281 = "%u:%02u"`** — the
   two arguments are the seconds-remaining count divided by 60 and the same
   count modulo 60, in that order — then draw it **digit by digit** using the
   **`aNumericFont`** glyph set (a dedicated font resource distinct from the
   FONT6 UI font used by `.BM` screens), one `sub_415920` blit per character
   at `x = getvalue(110)` (**525**), `y = getvalue(111)` (**36**), advancing
   `x` by each glyph's width plus **`getvalue(112)`** (**4** px — the
   VALUELST file's own comment: "extra pixels of space between the timer
   font digits") per digit.
4. **Colour changes at ≤30 s remaining** — `byte_49D38F` (the normal ink
   colour) is swapped for `byte_49A390` (a distinct, presumably warning-red,
   palette index) once that same seconds-remaining count is **≤ 30**. This is a separate, permanent colour
   change (not the blinking "hurry" flash below, which is a separate
   late-round text warning).

**Timer auto-pause while the round is already decided (CONFIRMED, distinct
from any key).** `sub_42A3F6`'s main loop calls `sub_421969()` (alive-player
or alive-side count) every iteration and, once it drops to `≤1`, calls
`sub_410522()` — which clears `dword_4601B4` (the "timer running" flag), so
`sub_4105D2` stops advancing `dword_4601AC`-derived elapsed time. This is
NOT a player-triggered pause; it is the game freezing its own clock once a
side has already won, presumably so the linger-and-death-animation window
doesn't visibly burn round time. No key toggles this.

**CORRECTED 2026-07-09 — a per-player score/kill HUD element DOES exist.**
This document's earlier claim ("no score/kill/lives HUD element exists in
`sub_4105D2` or anywhere else in the tick callback chain... only `sub_4105D2`
[clock] and the 'hurry' block draw any text/HUD element") was wrong: it
listed `sub_420F07` among the functions "read function-by-function" but
missed a real draw block inside it. See "The player row" below.

### The "hurry" late-round flash — CONFIRMED, separate from the clock colour change

Still inside `sub_42A191` (pseudo.c 29531-29549), after the clock draw:
reads **`getvalue(101)` = 60** and compares it against the round's total
length; when the **remaining time is within 5 seconds of `getvalue(101)`**
(the test is "elapsed > total − 5", i.e. within the last ~5 s of crossing the 60 s-remaining
threshold — practically: fires once, near the 55-60 s-remaining mark, not
continuously for the rest of the round) it plays **SFX 2700** once
(`dword_464984` latch guards the one-shot) and, **on alternating frames**
(`dword_464994 & 4`, i.e. flashing at roughly a quarter of the tick rate),
blits the string `aHurry` ("hurry") centred at `dword_464A70/2,
dword_464A6C/2` (screen centre). This is a one-time warning flash near
60 s remaining, independent of the clock-colour change at ≤30 s.

**Port status: DONE — no longer a gap.** ROADMAP "In-round shell — RE'd +
ported 2026-07-08" reconciled this: `game_app.cpp` now draws the MM:SS clock
HUD (`getvalue(110)/(111)/(112)` for position/spacing, message 281 for the
format, showing infinity when untimed, warning ink at ≤30 s) and the "hurry"
flash + SFX 2700 at the `getvalue(101)` = 60 s threshold, driven off
`sim::State::ticks_left`. This document pinned the facts the port pass used;
they are now implemented, not just recorded.

(Provenance: `sub_4105D2` @ 0x4105D2, pseudo.c 14456-14552; `sub_41087D`
@ 0x41087D pseudo.c 14565-14572; `sub_410522` @ 0x410522 pseudo.c
14419-14425; `sub_421969` @ 0x421969 pseudo.c 24048-24056; hurry block
pseudo.c 29531-29549; VALUELST `101,60` / `110,525` / `111,36` / `112,4`
[file's own comment: "extra pixels of space between the timer font
digits"]; MESSAGES.TXT `281,"%u:%02u"`.)

### The player row — CONFIRMED (`sub_420F07`, corrects the point above)

`sub_420F07` (@ 0x420F07, the "per-player render-and-cornerhead pass" this
document's round-driver section already names) has its own per-player draw
block, separate from the clock/hurry HUD, gated `if (byte_461BD4[152*i])`
(the "Player struct" facts.md entry's `+0x10` field — CONFIRMED here to mean
"this slot has ever had a player in this MATCH", not "alive this round",
since a round-eliminated slot still draws — see the marker overlay below):

Per slot `i`, in this order:

1. **Row x** ← `getvalue(115 + i/2)` (integer divide — the column), via the
   getvalue primitive `sub_412135`.
2. **Row y** ← `getvalue(113 + (i & 1))` (the parity row).
3. **Win count** ← the **upper 16 bits** of the dword at
   `dword_461C2C[38*i]` — this expression is `sub_421AC8(i)` inlined; see the
   paragraph below for why the second number is the kill count.
4. **Format string** ← `getstring(37)` = `"S:%d K:%d"`, via `sub_4124A4`.
5. **sprintf** (`sub_4518D0`) the win count into that format into a scratch
   buffer. *Annotation from the original transcription: the SECOND `%d`
   argument is lost to the decompiler's variadic-call argument undercount —
   it is resolved below via `sub_421AC8`'s OWN call sites elsewhere.*
6. **Ink and shadow colours** ← `sub_41672F(i)` and `sub_416867(i)` — the
   per-player colour pair (docs/re/player-colour.md).
7. **Draw** the formatted line with `sub_41696C` onto surface
   `dword_464AE4`, at (row x, row y), box width **90**, foreground = the
   `sub_41672F` ink, background = the `sub_416867` shadow.
8. **Eliminated overlay** — if the dword at `dword_461BC4[38*i]` (player
   struct **+0x00**, "active/moving") is **zero**, i.e. this slot is dead
   THIS round: look up the sequence named by the literal `aXxx` = `"xxx"`
   (lowercase) with `sub_41D957`, take its **frame 0** via `sub_41DAA7`, and
   blit it with `sub_415920` at the SAME (row x, row y) as the text.

**Layout — pixel-exact from the VALUELST file's OWN comments** (not
inferred): `; two vertical (Y) coordinates of each player row across the
top` → `113,6` / `114,26`; `; left (X) coordinates of each player column
across the top` → `115,10` `116,110` `117,210` `118,310` `119,410`. So this
is a **5-column x 2-row grid of up to 10 player slots** spanning the top of
the screen (y=6 or y=26; x=10/110/210/310/410), column = `i/2`, row = `i&1`
(pairs (0,1),(2,3),(4,5),(6,7),(8,9) stacked in each column). The box width
passed to `sub_41696C` (90 px) is a literal, not a `getvalue()` id.

**The two numbers ARE win-count and kill-count, not a decompiler artifact.**
`sub_421AC8(slot)` returns the **upper 16 bits** of the dword at
`dword_461C2C[38*slot]` — the same accessor `sub_420F07` inlines for its win
count in step 3 above — and it is independently used at the
RESULTS tier's match-clinch check (`sub_421AC8(k) >= dword_464A7C`, pseudo.c
29943, "wins needed to clinch") and its own per-player score print (pseudo.c
29959-29960, `sub_421B0F(j)` / `sub_421AC8(j)`, immediately followed by
`sub_4124A4(31)` = MESSAGES.TXT `31,"Player %u score: %u (kills: %d)"`) —
`docs/re/results-and-options.md` §1 already pins this exact pair as **"Score
= `sub_421AC8(i)` (cumulative match win count)... 'kills' figure...
`sub_421B0F(i)`"**. Our port already tracks both of these under
`GameApp::win_count_`/`kill_count_` (results.hpp's `tally_kills`, wired for
the RESULTS screen) — `sub_420F07`'s row is the SAME two counters, drawn
LIVE during the round instead of only at RESULTS.

**The "xxx" overlay is a real sprite, not synthesized text.** `aXxx` is the
literal C string `"xxx"` (`char aXxx[4] = "xxx"`, pseudo.c 1556), passed to
`sub_41D957` — the same named-sequence lookup the tile/flame/powerup art all
go through (a binary search over a loaded ANI's sequence table, confirmed by
reading `sub_41D957`'s body: `stricmp` binary search, "Unable to find
sequence" error on a miss). The shipped `MISC.ANI` (also home to `goldman`,
`cursor1`, `teamring0/1`) carries a `'xxx'` sequence verbatim: 1 step, frame
`XXXX.TGA`, 77x20 px (confirmed via `abtool ani MISC.ANI` against this
install). `sub_415920` is the same generic single-frame blit `sub_41696C`'s
clock digits use elsewhere in this document — so this is a genuine icon
overlay, not a text glyph.

**No panel/background art backs this row.** The only call immediately before
the per-player loop, `sub_429790`, is joystick-input polling (unrelated,
confirmed by reading its body) — there is no fill/border/backdrop draw call
site anywhere in `sub_420F07`. The row is a bare text+icon overlay directly
on the live game field, matching the clock HUD's own styling.

**A separate, NOT ported finding, now FULLY TRACED: the "cornerhead" face
bubble — CONFIRMED N/A for a same-screen port (2026-07-09).** Still inside
`sub_41F29B` (the per-player animation-advance this document's round-driver
section already cites for the idle-fidget "cornerhead" poses — CONFIRMED
unrelated to this section, already ported as `Renderer`'s
`panic_ticks_`/`panic_variant_`), a SEPARATE block (pseudo.c 23269-23276)
draws a `KFACE.ANI` face. It sprintf's the sequence name from the template
`aKfaceS = "kface %s"` plus a direction word; `KFACE.ANI` ships `kface
north/east/south/west`, 4 single-frame ~40x40 images, confirmed via `abtool
ani KFACE.ANI`. The blit is `sub_415A9F` at **player x (struct dword +7)
minus 4** and **player y (dword +8) minus 34**, frame index **0** of the
direction sequence, drawn every tick the gate holds — no health/disease/
event condition, no cadence beyond "gate is true this frame". The **gate**
is: this player's slot index — the struct's byte distance from the array
base `dword_461BC4` divided by the 152-byte stride — equals the global
`dword_45BE3C`. The direction word comes from `sub_413AED`, fed the
**third byte (bits 16-23) of the struct dword at +21** — the facing byte;
`sub_413AED` indexes the 4-entry direction-name table `off_45BCC4` with
that byte masked to its low 2 bits.

`dword_45BE3C`'s semantics are now **exhaustively traced** — every read and
write in the whole decompile (5 sites total, confirmed by a full-file grep,
no others exist):

- **Declaration/init** (pseudo.c 2235): a global int statically initialised
  to **-1** — starts "nobody designated."
- **Round-start reset** (pseudo.c 23986, `sub_421793`, the per-round init
  function that also resets the +53/+54 fields on all 10 slots and shuffles
  the spawn-point tables): `dword_45BE3C = -1;` — cleared at the top of
  every round, so it never silently survives from a previous round.
- **The only local WRITE that isn't a reset** (pseudo.c 22358-22372, inside
  `sub_41E61E` — the per-player *human input decoder*, `switch` on the same
  input-type-category byte `docs/re/setup-screens.md`'s "State model" section
  pins at player-struct `+16` (0=OFF, 1=COMPUTER, 2=keyboard, **3=joystick**,
  4=other controller)): **only `case 3` (joystick) touches it.**
  `sub_429520(stick_index, &out_x, &out_buttons, &out_y)` reads the live
  joystick sample cached by the polling loop `sub_429790` (confirmed: the
  cached sample's slots 8 and 9 are the X/Y axes rescaled to 0-100, and
  slot 10 is `pji.dwButtons`, the raw `joyGetPosEx` button bitmask — so the
  second out-parameter here is that raw button bitmask, not a percentage).
  If that bitmask is **exactly 74**, `dword_45BE3C` is set to *this player's
  own slot index* — computed as the player struct's byte distance from the
  array base `dword_461BC4` divided by the 152-byte stride — and
  `sub_4101F1(index)` fires; if it is **exactly 138**, `dword_45BE3C` is
  cleared to `-1` and `sub_4101F1(-1)` fires. Both 74 (`0b01001010`) and 138 (`0b10001010`)
  are multi-bit chords, not single-button presses — an unlikely-to-hit-by-
  accident gesture, and one that requires a **joystick specifically**
  (neither keyboard `case 2` nor the "other controller" `case 4` ever touch
  `dword_45BE3C`).
- **`sub_4101F1`** (pseudo.c 14249) is a **network-send** wrapper:
  `sub_40C326(57)` + `sub_40CE27((int16*)0x39, &value, ..., 2)` — it puts the
  new value on the wire as message id **57 (0x39)**, a 2-byte payload.
- **The matching receive-side handler, `sub_40E2D8`** (pseudo.c 12786-12795,
  one of the message-dispatch wrappers gated on `sub_40C497` of the incoming
  message pointer, alongside every other `sub_40Exxx` handler in that
  block): on a valid inbound message it reads the **signed 16-bit field at
  offset +12 of the message record** and passes it to **`sub_4226F6`**.
- **`sub_4226F6`** (pseudo.c 24697-24702) is a trivial setter: it stores its
  argument into `dword_45BE3C` and returns that same value — the mirror-side
  write, applying whatever slot index a network peer broadcast, with no
  validation of any kind.
- **The only READ anywhere** is the KFACE gate above (pseudo.c 23269).

**Conclusion: this is a netplay-only, joystick-gated "designate one slot to
show a face-bubble over" broadcast, with no local-multiplayer analogue.**
Three independent facts rule out a same-screen port rather than merely
leaving it "lower confidence":
1. It is **joystick-chord-exclusive** — the write path lives solely in
   `case 3` of the input decoder and triggers on the RAW `dwButtons` bitmask
   being exactly 74 (set) / 138 (clear), multi-button chords tied to a 1997
   stick's physical button layout. Our port DOES drive type-3 joystick slots
   (SDL3 `GamepadMapper`, ROADMAP 2026-07-08), but the mapper deliberately
   exposes only d-pad/left-stick + south/east buttons — there is no raw
   button-bitmask surface on which "exactly 74" could be reproduced
   faithfully; any substitute chord would be invented.
2. It is **network-replicated, not locally computed** — the write is
   immediately mirrored to every peer over message 57, and the receive
   handler (`sub_4226F6`) applies a peer's value with no local identity
   check at all. In a same-screen game there is no "peer" to broadcast to
   or receive from; the entire round-trip this global exists to serve
   (telling OTHER machines which slot to draw a marker over) is meaningless
   on one machine.
3. It has **no stable "which slot" identity to reuse** — it is not "the
   first human," "keyboard-set-0's slot," or any other fixed role; it is
   whichever joystick user happens to hold an obscure button chord at that
   instant, reset to "nobody" every round. There is no keyboard-set-0-style
   analogue in the trace to translate; inventing one (e.g. "always show it
   over local slot 0") would be exactly the invented-visual risk row #42
   already flagged, now confirmed rather than merely suspected.

Per this document's own decision rule (trace fully, then port only if the
semantics translate cleanly to same-screen local multiplayer): **not
ported.** Closed as a documented N/A, not a residual gap — see
`docs/re/coverage-audit.md` row #42.

**Port status: DONE for the player row; N/A (confirmed, not a port gap) for
the cornerhead face bubble.** `GameApp::draw_player_row` (`game_app.cpp`,
called from `run_match` after `Renderer::draw_frame`) now draws the S:/K:
grid in each slot's `AssetStore::slot_color` (the same helper
`present_scoreboard`'s non-team row already uses for the identical
`sub_41672F` ink), with the `MISC.ANI` "xxx" sequence
(`SequenceSet::eliminated_marker`) overlaid on a round-dead slot.
`sim::Player::present`/`sim::Player::alive` stand in for
`byte_461BD4`(+0x10)/`dword_461BC4`(+0x00) respectively — both fields
facts.md's "Player struct" entry already names, now correctly mapped to
their actual meanings (match-membership vs. round-alive) via this block's
own two independent gates.

(Provenance: `sub_420F07` @ 0x420F07 pseudo.c 23628-23701 [already partially
cited by this document's round-driver section]; `sub_421AC8`/`sub_421B0F`
@ 0x421AC8/0x421B0F pseudo.c 24112-24130; `sub_41D957` @ 0x41D957 pseudo.c
21760-21806; `aXxx` pseudo.c 1556; VALUELST `113,6` `114,26` `115,10`
`116,110` `117,210` `118,310` `119,410` with the file's own two comment
lines quoted above; MESSAGES.TXT `37,"S:%d K:%d"` [only call site in the
whole decompile]; `docs/re/results-and-options.md` §1 [`sub_421AC8`/
`sub_421B0F` cross-reference, RESULTS screen's own use of the identical
pair]; `sub_41F29B` cornerhead-face block pseudo.c 23269-23276, `aKfaceS`
pseudo.c 1554; `MISC.ANI`/`KFACE.ANI` sequence tables confirmed via `abtool
ani` against this install's `DATA/ANI/`; `dword_45BE3C` full trace —
declaration pseudo.c 2235, `sub_421793` round-reset pseudo.c 23977-23986,
`sub_41E61E` joystick-case write pseudo.c 22279-22391 [`case 3` block
22358-22391], `sub_429520` joystick-sample accessor pseudo.c 28957-28980,
`sub_429790` joystick-poll loop confirming that the cached sample's dword
slots 8/9/10 hold X% / Y% / `dwButtons` respectively, pseudo.c 29090-29152,
`sub_4101F1` network-send wrapper pseudo.c
14249-14257 [message id 0x39=57], `sub_40E2D8` receive-side dispatch
pseudo.c 12786-12795, `sub_4226F6` mirror-side setter pseudo.c 24697-24702;
input-type category table `docs/re/setup-screens.md` "State model —
CONFIRMED (`sub_421DD2`)".)

## The round-end shell — from "one side left" to the results tier

The task's ported "~3 s linger" and the original's actual shell, precisely
matched up:

1. **Round loop exit.** Every iteration of the endless auxiliary-key loop
   ends with a three-part test (pseudo.c 29810): `dword_464AEC` clear
   **and** `sub_421947() > 1` **and** `sub_41087D()` false → jump to the
   shared teardown label at pseudo.c 30139. That label doubles
   as the **bottom of the loop**: with `dword_464A68 == 0` it does nothing
   and the loop iterates again — so that guard reads "more than one
   alive (`sub_421947`) and clock not expired (`sub_41087D`) → keep
   looping". (`dword_464AEC` is only ever set by the network key 324 —
   always 0 locally.) When the survivor count drops to ≤1 **or** the clock
   runs out, the guard fails and execution falls INTO the round-end shell
   (29812+). There is **no fixed "3 s linger" inside the loop** — no
   dedicated post-game-over grace period exists the way our port's
   `over_ticks = 3 * kTicksPerSecond` window does; whatever visual
   "settling" happens (death animations finishing, the auto-frozen clock —
   see the timer auto-pause note) happens because the tick callback keeps
   running normally right up to the instant the shell unregisters it.
2. **Music switch — the 1130 track underlies the WHOLE outcome tier
   (CORRECTS `docs/re/frontend-flow.md`).** `sub_42741E(0x46A)` (track 1130,
   SOUNDLST label "draw") starts on loop exit at pseudo.c 29820 —
   **unconditionally, BEFORE the survivor test at 29823**. So DRAW, the
   RESULTS tally, AND the VICTORY/TEAM screen all play under **1130**;
   nothing ever re-starts 1020 in the outcome tier. The full Play-flow
   music model, from the now-exhaustive call-site list of both by-id music
   wrappers (`sub_42741E`: pseudo.c 29696/29820/30200/30277/30502/30783;
   `sub_4274AB`: 28922/28942/28947 — no other callers exist):
   - **Play entry** (pseudo.c 29696): track **1020** ("win") — this is
     actually the **setup-screens music** (player select / LEVEL & ROUNDS),
     not victory music.
   - **Round init** (`sub_410B6E`'s music block, pseudo.c 14847-14850): if the
     Options toggle **"Disable music during gameplay"** (`dword_4648C0`,
     options.ini `disable_game_music=` — `docs/re/results-and-options.md`
     §3 row 13) is set, `sub_427342()` frees the music → the round is
     silent; otherwise **`sub_4293E5()` @ 0x4293E5 starts the PER-LEVEL
     stage track: SOUNDLST id `1100 + level`** (it reads the id table at
     `dword_463094 + 4*(1100+level)`), falling back to **id 1120 (0x460)**
     when the level has no entry (or network / level==-1). These are
     SOUNDLST ids, unrelated to the VALUELST 1100-1110 net-timing rows.
     Immediately after, the round clock is armed: `sub_410494(dword_464948)`
     (total seconds) → `sub_4104C2()` (reset elapsed) → `sub_4104F0()`
     (set the timer-running flag).
   - **Round end** (29820, `0x42A6DD`): **1130** replaces the stage track and
     carries through DRAW/RESULTS/VICTORY; on a multi-round loop-back the next
     `sub_410B6E` replaces it with the next stage track.
     **"Unconditionally" above is too strong — CORRECTED 2026-07-28**
     (`docs/re/sound-engine.md` §9.3). It is unconditional *with respect to the
     survivor test*, which is what that sentence was about, but the round-loop
     exit passes two earlier gates to get there: **attract** (`dword_464938`
     @`0x42A6CB`) and **campaign** (`dword_46489C` @`0x42A63B`). A campaign
     round end reaches neither 1130 nor any outcome screen — see
     `docs/re/campaign.md` "Round end".
     Two further notes on the round-init bullet above, both settled by the same
     pass: the guard on `dword_4648C0` has **no network arm and no stage-art
     arm** (the `sub_40C06A` check lives inside `sub_4293E5` and only picks
     GENERIC over the per-level track), and `sub_4274AB`'s extension is `.hds`
     — `sub_4293E5` also builds a `.cds` name and discards it, a vestige of a
     cut CD-audio path.
   `docs/re/frontend-flow.md`'s "Results MUSIC — 1020 (win) played under
   the VICTORY screen" is therefore **wrong on the VICTORY half** (its DRAW
   half — 1130 under DRAW.PCX — is right but incomplete): the port's
   `start_music(1020)` on VICTORY does not match the binary; faithful is
   1130 under all three outcome screens, with 1020 as the setup-screens
   track. Flagged as a port follow-up when this section was written
   (DOCS-ONLY task) — **fixed since**: `game_app.cpp`'s `kDrawMusicId`
   (1130) now starts for DRAW, RESULTS, and VICTORY/TEAM alike, `kWinMusicId`
   (1020) is scoped to the Play/setup path only (coverage-audit table row
   #21, "RESOLVED").
3. **DRAW branch (`sub_4219B0(...) == -1`, no survivor):** draws DRAW.PCX
   (cut, no wipe — `docs/re/frontend-flow.md`'s `sub_42A088` cut semantics),
   plays sting group **1700** once, then runs a **bespoke wait loop**
   (pseudo.c 29830-29881, NOT the generic `sub_42A088` primitive) with:
   - nav-blip (SFX 20) on any real key;
   - `sub_40C06A()==1` (network client) forces Esc — N/A locally;
   - **auto-advance once the millisecond clock `sub_43ACF8()` passes the
     loop's entry timestamp + 6000 ms — a literal
     **hardcoded 6000, not a `getvalue()` id** — gated on
     `sub_42247A() || dword_4646B4` (all-AI roster OR the in-round 273-key
     latch, see the aux-key table above);
   - accept keys 13/32/904 (904 = a network-specific code, N/A locally) play
     SFX 10 and exit; anything ≥32 and ≠904 loops; codes <27 other than -1/
     -2/13 also just loop (i.e. most keys are ignored — only Enter/Space/the
     net code accept);
   - **SFX 40 ("enrt1", "can't do that here")** fires if `sub_40C06A()==1`
     and a key was pressed — network-only, matches `docs/re/frontend-flow.md`.
4. **RESULTS branch (a survivor exists):** loads RESULTS.PCX, computes the
   **winner-so-far index** (call it *clinched*; it is **-1** if nobody has
   clinched yet) and prints
   tallies (`docs/re/results-and-options.md` §1 territory) — then runs an
   **almost identical bespoke wait loop** (pseudo.c 30051-30105) with the
   same 6000 ms hardcoded auto-advance gate, the same accept-key set (13/32/
   903 this time — a different net code, still N/A locally), and the same
   SFX-40 network buzz.
5. **Loop-back or VICTORY.** After the RESULTS wait:
   - **No clinch yet (*clinched* is -1):** if the seated-slot count is **≥ 2**
     (at least 2 players have a score entry — i.e. the match isn't
     degenerate), calls
     `sub_410B6E()` again — **starts the next round of the same match**,
     looping the whole `while(1)` structure from step... this is the
     multi-round match structure `docs/re/frontend-flow.md`'s "RESULTS
     tally tier" section flagged as still needing a port when this section
     was written — **shipped since**, ROADMAP "Multi-round best-of-N loop +
     RESULTS tally 1:1 — DONE 2026-07-08". If that count is **< 2**
     (fewer than 2 players ever scored — a degenerate/aborted setup), shows
     message 47 ("Too many players have left the game!", header 95 "NOTE!")
     and sets `dword_464A68 = 2` (quit to menu) — **no forfeit-vs-quit
     distinction here; this is a hard bailout**, not a normal round loss.
   - **Clinched (*clinched* is not -1):** formats `aTeamU`/`aVictoryU` (already
     documented in `docs/re/frontend-flow.md`), draws it via the generic
     `sub_42A088(name, 0)` (cut, no wipe, no wait — `wait=0`), then **a
     literal `sub_413CB0(3000)`** — a **hardcoded, blocking 3000 ms
     `Sleep`-loop** (100 ms chunks, pumping the message loop each chunk;
     `sub_413CB0` is a real OS sleep, not a frame-count linger — see its
     body, pseudo.c 16728-16743), no `getvalue()` id backs this 3 s. Only
     after the sleep does it set `dword_464A68 = 1` (clean match-win flag,
     the ONLY site in the whole binary that assigns 1 to this variable —
     every other assignment is 0/2/10) and fall through to teardown.
6. **Teardown (the two shared tail labels at pseudo.c 30139 / 29815, reached
   by every exit path):** if
   `dword_464A68` is nonzero, calls `sub_40FB44` (network-notify;
   `sub_40FCE6()` locally — a local-only stub call, not further RE'd here as
   out of scope), then a **10-iteration × 100 ms drain loop**
   (`sub_40EA1E`+`sub_413CB0(100)` — 1 s total, pumping input/message loop
   so any queued network/UI events flush), then `sub_43A74C` unregisters
   the `sub_42A191` tick callback and the function returns — control goes
   back to `sub_42B9CE` (the menu, `__noreturn` loop) or to `sub_42A3F6`'s
   own caller if it was itself re-entered for a repeat round.

**Corrected constant for our ported "~3 s linger":** the task asked us to
find the "real constant/getvalue id" backing the ~3 s post-round linger our
port has (`GameApp::run_match`'s `over_ticks = 3 * sim::kTicksPerSecond`,
`libs/game/src/game_app.cpp:1588`). **There is no `getvalue()` id — it is a
hardcoded `sub_413CB0(3000)`, i.e. a literal 3000 ms**, and critically it
happens **AFTER** the RESULTS screen's own wait-for-input step, immediately
before showing VICTORY/TEAM — not as a grace period tacked onto the
moment-of-death the way our port's `over_ticks` currently works (our port's
3 s starts counting the instant `sides_remaining() <= 1`, runs concurrently
with normal simulation ticking, and then hands off to the Results screen;
the original has no such grace window inside the round loop at all — see
point 1 above — and instead sleeps 3 s later, after RESULTS, right before
VICTORY). The numeric value (3000 ms / 3 s) our port already uses happens to
match the original's `sub_413CB0(3000)` exactly, which is a correct constant
even though it is wired into a different point in the sequence — a
structural gap noted when this section was written (multi-round loop-back,
RESULTS tally, and the two 6 s bespoke wait loops were then the
still-missing pieces per `docs/re/frontend-flow.md`'s "RESULTS tally tier"
section) rather than a constant-value bug. All three have since shipped:
the multi-round loop-back and RESULTS tally via ROADMAP "Multi-round
best-of-N loop + RESULTS tally 1:1 — DONE 2026-07-08" (`docs/re/frontend-
flow.md` now reads "RESULTS tally tier — RE'd, port DONE"), and the 6 s
auto-advance wait loop as `kResultsDwellMs` in `game_app.cpp`'s
DRAW/RESULTS screen handlers.

**CORRECTION (2026-07-27) — the last sentence above was wrong, and point 1
already contained the refutation.** Calling our 60-tick window "a correct
constant wired into a different point in the sequence" conceded too much:
the original's round-over moment is not a constant at all, so there was
nothing for a 3 s window to be a correct value *of*. The loop-continue
guard at `0x42A6B2` keeps looping while `sub_421947` (the per-frame player
count latched by `sub_420F07`) is greater than 1 — and that count is NOT
the alive count. `sub_41DCB2` (the kill) only raises the slot's dying flag;
the per-player pass `sub_41F29B` clears the slot's in-play flag only after
the death sequence has played its final step, the step count coming from
`sub_41DA5C`. A corpse mid-animation therefore still counts, and the round
is over one frame after the LAST death animation completes. This section
previously read `sub_421947` as an alive count and did not notice that
`sub_421969` — the OTHER counter, which stops counting a dying player once
its animation frame index passes `getvalue(25)` (= 20, i.e. 1 s), and which
the clock-pause at `0x42A636`, the bomb code and the enclosure stepper all
use — is a different function with different treatment of corpses. Two
distinct edges, neither of them a fixed 3 s:

| edge | predicate | what it does |
| --- | --- | --- |
| freeze / clock pause | `sub_421969` ≤ 1 (partway into the death anim) | stops the clock; our `round_frozen` (`simulation.cpp`) |
| round over | `sub_421947` ≤ 1 (death anim fully finished) | falls into the outcome tier |

The clock-expired exit (`sub_41087D` at `0x42A6BE`) has no linger at all —
it drops straight into the outcome tier. There is no `getvalue()` id and no
literal constant anywhere in the round-over path; the hardcoded 3000 ms
sleep this paragraph found is real but lives much later, after the RESULTS
wait and just before VICTORY. Ported 2026-07-27: `MatchRunner`'s
`advance_round_end` now waits on `Renderer::death_fx_active()` (each effect
measured against its own sequence length — the same rule `sub_41DA5C`
applies) and returns immediately on a spent clock. The shipped "die green"
sequences run 12-93 steps, so at 20 Hz half of them were being truncated by
the old flat 60 ticks. NETPLAY keeps the fixed window: sequence lengths come
from the local install's `DATA/ANI`, which `build_hash` does not cover, so
an animation-driven handoff tick is not guaranteed to agree between peers.

**End-of-round jingle/sting ids** (stings were pinned in
`docs/re/frontend-flow.md`; the music placement is corrected by step 2
above): draw sting = SOUNDLST **1700** (`sub_427BFB(1700)`, one-shot group,
GUMP1.RSS), "we have a winner" voice = SOUNDLST **2000** (`sub_427BFB(2000)`,
PROUD.RSS group, played right after the RESULTS wait exits — pseudo.c
30027, i.e. BEFORE the 1500 ms/3000 ms sleeps, not after), outcome-tier
music = SOUNDLST **1130** (DRAW.RSS — under DRAW **and** RESULTS **and**
VICTORY, per step 2), setup-screens music = SOUNDLST **1020** (WIN.RSS,
started at `sub_42A3F6` entry, replaced by the per-level stage track —
SOUNDLST `1100+level`, fallback 1120 — at each round init).

(Provenance: full round-end shell pseudo.c 29788-30152; `sub_413CB0`
@ 0x413CB0 pseudo.c 16728-16743 [confirmed a real Sleep(100) loop, not
frame-driven]; `dword_464A68` write sites — `grep`'d exhaustively across the
whole decompile, 22 sites, only pseudo.c 30132 assigns 1; VALUELST has no
id for the 1500/3000/6000 ms constants used in this shell — all three are
literal immediates in the disassembly, confirmed absent from
`docs/valuelst-map.md`'s existing tables and re-checked against the raw
VALUELST.RES text.)

## Round rotation — what carries, what resets, and who drives it online (CONFIRMED 2026-07-25)

Prompted by a live report: *"a match set to 2 rounds cuts straight back to the
main menu when the first round ends"*, observed ONLINE. This section pins the
whole round-to-round path end to end. Everything below was read directly out of
`sub_42A3F6`'s tail and `sub_410B6E`; nothing is inferred from the summaries
above.

### DRAW is a PREFIX to the RESULTS tally, not an alternative to it

The single most load-bearing correction in this pass. The outcome tier splits on
`sub_4219B0(...)` (the round-survivor query, pseudo.c 29823):

- **survivor exists → jumps directly to the RESULTS tier's own head**
  (pseudo.c 29824 → the `aResultsPlt` load at 29888). DRAW.PCX is skipped
  entirely.
- **no survivor → DRAW.PCX** (`sub_42A088(aDraw, 0)` 29825, sting `sub_427BFB(1700)`
  29826) and its bespoke wait loop — and when that loop exits it does three
  teardown calls (`sub_415CE3`/`sub_415C1F`/`sub_4150BD`, 29882-29884) and then
  **falls straight through into that same RESULTS head** (29885). There is no
  jump over it.

So a drawn round shows **DRAW and then the cumulative tally**, dismissing two
wait loops, before the next round starts. This document's earlier "DRAW branch"
/ "RESULTS branch" wording implied they were alternatives; they are not.

**Port status: FIXED 2026-07-25.** The local Results handler
(`game_app.cpp` `run_app`) showed DRAW alone and went straight back to Match; it
now runs `present_scoreboard()` after the DRAW screen. The netplay path does the
same (`run_netplay_match_seats`).

### Who dismisses the outcome screens — the HOST, never a client

Reading the DRAW wait loop's key chain in full (pseudo.c 29832-29881); the
RESULTS loop (30051-30105) is the same shape with 903 in place of 904:

| condition | effect |
|---|---|
| any real key | nav blip SFX 20 (29835-29836) |
| `sub_40C06A() == 1 && !sub_40F386()` | the key is **forced to 27** (29837-29838) → the `<= 27` arm sets `dword_464A68 = 2` and leaves: **a client whose net session is dead is kicked to the menu** |
| `(sub_42247A() \|\| dword_4646B4) && now > t0 + 6000` | the key is forced to 13 — the all-AI/attract auto-advance only |
| `27` | `dword_464A68 = 2`, abort to the menu |
| `32` (Space) or **`904`** | jumps straight to the loop's accept arm: accept sting SFX 10, leave the loop — **no dwell gate** |
| `13` (Enter) | falls PAST the accept arm to the tail, where a client is intercepted (next row) and everyone else is dwell-gated (row after) |
| `sub_40C06A() == 1` (tail, 29869-29873) | **SFX 40 and `continue`** — a client pressing Enter simply cannot dismiss the screen |
| tail dwell (29877-29878) | Enter accepts only once `t0 + 1000 * sub_4148AC()` has passed |
| on accept, `sub_40C06A() == 2` | the HOST broadcasts `sub_40F064(904, ...)` (29875) — kind 32, the code that appears in a guest's own key stream and dismisses its screen |

- **`sub_4148AC` @ 0x4148AC (pseudo.c 17319-17324) is two lines**: `sub_40C06A()`
  truthy → `getvalue(13)`, else `1`. `DATA/RES/VALUELST.RES` line 66 is `13,3`, so
  the minimum dwell before Enter works is **1000 ms locally and 3000 ms in a net
  game** — the "let the other machines catch up" gate.
- **`sub_40F386` @ 0x40F386 (pseudo.c 13489-13492) returns `dword_4600D4`**, the
  live net SESSION ID (`dword_46013C >> 16` at 11130, matched against inbound
  message ids at 11348/11886, zeroed on teardown at 11501/11671). So the
  "network client forces Esc" note earlier in this document is CONDITIONAL: it
  fires only once the session is gone, not on every client frame.

### The next round's LEVEL is pushed by the host, every round

`sub_410B6E` @ 0x410B6E is re-entered for every round, and its first act after
`sub_42A16F(1)` is to resolve the stage into `dword_46499C` — and it splits on
role (pseudo.c 14727-14769):

- **GUEST (`sub_40C06A() == 1`)**: sets `dword_46499C = -1` and then **BLOCKS in a
  pump loop until it stops being -1** (14730-14743), with Esc the only way out
  (`dword_46492C = -1; dword_464A68 = 2`). The value arrives from the wire — the
  receive-side store is pseudo.c 12780 — so **a guest does not compute the next
  round's map at all; it waits to be told, on every round.**
- **HOST or LOCAL**: `dword_464998 < 0` (the LEVEL screen's RANDOM row) → up to
  **200 tries** of `rand() % max(getvalue(35), 1)`, accepting the first index
  whose **`getvalue(1150 + index)`** is nonzero (the enabled-rotation allow-list);
  otherwise `dword_46499C = dword_464998` verbatim (14745-14759). So **a fixed
  level replays the SAME map every round and RANDOM re-rolls per round.**
- **HOST additionally** drains 10×100 ms and calls `sub_4101B5` (14760-14769),
  whose body puts `dword_46499C` on the wire as the **low 16 bits** of the
  outgoing message's payload word (pseudo.c 14240) — the send that unblocks
  every guest's loop above.
- `dword_46499C` IS the stage index everything else keys off:
  `"field%u.plt"` (14778) and `"extra%u.res"` (6903).

### Carried vs reset

| | across rounds of one match | source |
|---|---|---|
| win count (`sub_421AC8`) / kill count (`sub_421B0F`) | **CARRIED** — the RESULTS tally is cumulative | §1 of `docs/re/results-and-options.md` |
| player powerup inventory | **RESET** every round to the `getvalue(50+j)` baselines | `sub_4214BC`, per-round init |
| stage / level | fixed → same map; RANDOM → re-rolled | `sub_410B6E`, above |
| the Goldman roulette SPIN | **once per match** — it lives at the head of `sub_410F81`, which the loop-back does NOT re-enter (only `sub_410B6E` is called again) | `docs/re/goldman-roulette.md` §2 |
| the Goldman +1 inventory AWARD | **re-applied every round** (it rides the per-round inventory reset) | `sub_4214BC` |
| the pending gold player `dword_46492C` | written in the RESULTS tier — **and skipped in a net game** | `sub_4034BC`'s `!sub_40C06A()` gate |
| the tick counter `dword_464994` | **monotonic across the whole match**, not reset per round | `sub_42A191` |
| music | the outcome tier's 1130 is replaced by the next round's `1100 + level` track at each `sub_410B6E` | §2 above |

Two smaller confirmations: a round that ends on the CLOCK awards nobody a win
(both `sub_4219B0` and the tally writer early-out on the timeout, which is why
a time-up round is a DRAW even with someone still standing — our
`round_winner()`'s `if (s.ticks_left == 0) return -1;` already matches); and the
"at least 2" guard on the loop-back counts **SEATED SLOTS**, not players with a
score, so its message-47 bailout is unreachable in local play.

**[NEEDS BINARY]** — not settled this pass, and not needed by the port as built:
the exact kind-32 payload the RESULTS loop injects (903) is inferred from the
DRAW loop's 904 by symmetry rather than read at its own injection site; and the
host's own teardown broadcast when it Escapes mid-match (kind 37 family) was not
traced, so our port's guest currently learns of a host forfeit only through the
peer-drop timeout.

### Port mapping (2026-07-25)

Our wire is stronger than the original's here, so the shape is reproduced rather
than the messages:

- The original pushes only the **level index** (`sub_4101B5`) per round and lets
  each machine rebuild the board from it. We push the **whole resolved
  `sim::MatchConfig`** through the SAME `net::SetupSession` confirm/ack the
  pre-match setup stage uses — for the reason `setup_session.hpp` already
  documents (a guest whose `.SCH`/`EXTRA<n>.RES`/VALUELST differ would build a
  different board from the same index and desync on tick 0). No new MsgType and
  no protocol-version bump: round N+1's config is just another confirmation.
- The guest's blocking wait at `sub_410B6E`'s head becomes the **outcome screen's
  gate** (`libs/game/include/bomber/game/screens/net_round_gate.hpp`): the host's
  accept confirms the next round, the guest's screen ends when that confirmation
  arrives. Same host-drives-the-advance shape, one screen earlier.
- Each round's seed and RollbackSession tick base come from
  `libs/net/include/bomber/net/round_rotation.hpp` — derived on both peers from
  the match seed + the round index, never exchanged. The seed doubles as the
  round's IDENTITY, so a replayed blob from an earlier round cannot be adopted as
  the next one.
- The original's 3000 ms networked dwell (`sub_4148AC` above) is deliberately NOT
  reproduced: it exists because the 1997 broadcast is fire-and-forget, and our
  confirm/ack barrier is strictly stronger — the host cannot leave the screen
  until the guest holds the exact bytes.
- The 1000 ms LOCAL dwell before Enter accepts an outcome screen is a real,
  cited behaviour our screens do not have. Recorded here as an unported fidelity
  item, not fixed by this pass.

## Debug/cheat keys — summary (all CONFIRMED, all facts worth keeping)

| trigger | gate | effect |
|---|---|---|
| in-round key `1` | `sub_413D01()` (the `-nologo`/debug global) | writes a debug text dump (`sub_42A325`) — a loop of one `"%s\n"` line per entry, the entry count taken from the first slot of a small local array, file opened in `"wt"` (text-write) mode |
| in-round key `4` | none (always live) | arms a DOS text-mode debug overlay pointer (mono/color text page, `0xB0000`/`0xB8000`) — invisible under normal VGA graphics mode, a leftover text-mode debug hook |
| in-round key `18` | `sub_413D01()` | toggles a display mode via `sub_4165D2` (`dword_460BA4` ? `sub_416591` : `sub_4162F0`) — FULLY CHARACTERIZED below, a colour-remap QA overlay, not a keyboard-remap screen |
| in-round key `288` | none (always live, also live in the main menu per `docs/re/frontend-flow.md`) | opens the "Internal debugging information window" — mem/audio-mem/net-id/BOMBER_ID/retrans-rate/cache-hit stats |
| in-round keys `274..305` | `sub_413D01()` (internal to `sub_40C678`) | dumps a 200-line per-tick statistics file, `"at"` append mode |
| main-menu Ctrl+E ×6 | none | the map editor (`docs/re/results-and-options.md` §5, already fully RE'd — not re-covered here) |

None of these are gated behind a documented "debug build" flag distinct from
`dword_460260` (`sub_413D01`, the same global the boot logo-skip and title
sting/timeout logic already uses) — the retail EXE ships every one of these
live, and two (`4`, `288`) have no gate at all. They are genuine,
shippable-binary developer cheats, not decompiler artifacts.

### Row #35 closure — CONFIRMED N/A for every key, individually justified (2026-07-09)

`docs/re/coverage-audit.md` row #35 tracked these five key groups (`1`, `4`,
`18`, `274..305`, `288`) as "absent (F1 only)". Each was re-read function-body
by function-body against `pseudo.c` this pass (not just the dispatch-table
one-liners above) to decide port-vs-N/A per the task's classification rule
(gameplay state → port only if sim-safe; presentation/diagnostic → port
directly; netplay → N/A per ADR-0003). Verdict: **all five are N/A**, none of
them a residual port gap — each for a distinct, evidenced reason, not a blanket
"low priority, skip":

- **Key `1` (debug text dump, `sub_42A325`, pseudo.c 29577-29603).** Reading
  the full body: it opens a file in text-write mode (`aWt_3` = `"wt"`) — but
  **the register holding the filename argument is flagged undefined by the
  decompiler itself** (a diagnostic at pseudo.c 29604), so the target
  filename is not recoverable; and the dump's own content buffer (the second
  slot of a small local array, fed to `fprintf` with the `"%s\n"` format
  string `aS_7` in a loop) is built through
  `sub_41D826`/`sub_41485A`, neither of which resolves what data is being
  serialized without further RE this pass didn't do. This is the SAME class
  of decompiler gap `docs/re/coverage-audit.md`'s existing TODO(RE) crumb
  (`sub_43D398`'s "possibly undefined" X register, table row after §6) is
  explicitly allowed to stay unresolved rather than guessed. **N/A — cannot
  be faithfully reproduced; the original's own filename and payload are not
  recoverable from this decompile, and guessing either would be an invented
  substitute, not a port.**
- **Key `4` (DOS text-mode debug overlay arm, `sub_413BB0`+`sub_42C098`,
  pseudo.c 16683-16688 / 31004-31020).** Full trace: `sub_413BB0` sets
  `dword_45BCD4=1` then calls `sub_42C098(0)`, which — reading its body —
  only ever clears/reinstalls a raw function-pointer callback
  (`dword_45BFD0`) and closes a file handle (`dword_45BFC4`) if one is open;
  the paired "read" side (not reached by this key at all, per the exhaustive
  key-table trace above) writes text glyphs directly into the `0xB0000`/
  `0xB8000` MDA/CGA video segments — physical memory-mapped text-mode video
  RAM that does not exist as an addressable concept under a modern protected
  Win32/SDL3 process, let alone a cross-platform one. **N/A — the underlying
  hardware primitive (raw VGA text-page memory) has no substitute to port
  to; even the original doc already notes this is "effectively inert on a
  normal VGA session" in the shipped game itself.**
- **Key `18` (live HSL colour-remap tuning dialog, `sub_4162F0`/`sub_416591`/
  `sub_415ED1`/`sub_41604C..sub_41627C`, pseudo.c 18190-18415).** Full trace
  (not done by the earlier "not further characterized" pass): this opens a
  `sub_43C734`-family dialog — the SAME dialog-chrome primitive this repo's
  own boot/quit-confirm dialogs pin pixel-exact (`sub_43C734`/`sub_432298`/
  `sub_412E33`, per the "Pin sub_43C734 dialog chrome" commits) — with 9
  buttons (R/G/B ±5-step sliders, prev/next slot, "Reload Default Color") that
  live-edit `byte_460BD0/BDA/BE4[slot]` (10 slots) and call `sub_41604C` →
  **`sub_414A65`** to regenerate that slot's player-colour remap table
  in-memory from the edited R/G/B percentages and redraw a live swatch
  preview (`sub_43D1C0`, RGB555 → palette LUT). `sub_414A65` itself IS
  already RE'd and ported — it's the exact `.RMP` fallback builder
  `docs/re/player-colour.md` documents (table row #26, `recolor_image`/
  `recolor_image_rmp`) — so the underlying colour math is not the blocker.
  What IS missing: (a) our port has **no equivalent of `dword_460260`/
  `sub_413D01()`** — no "debug mode" concept exists anywhere in `GameApp` to
  gate a developer-only dialog behind (confirmed: `grep -rn
  "debug_mode\|nologo" libs/game/src` finds nothing); and (b) our port's
  colour pipeline recolours whole PRE-BUILT texture sets once at
  `AssetStore::build_player_sets` load time (`docs/re/player-colour.md` "Our
  port"), not a live per-pixel remap-and-reblit the original's DirectDraw
  path does — wiring a "live edit, see the swatch AND every on-screen sprite
  update instantly" tool would mean adding texture-regeneration-on-the-fly to
  `AssetStore`, a real new capability, not a key binding. **N/A — the colour
  math has a home (already ported), but the tool built around it needs two
  things the port doesn't have (a debug-mode flag, live texture
  regeneration) for a feature with zero player-facing value; out of
  proportion to a "low priority, developer/QA-only" row.**
- **Key `288` (Internal debugging information window, `sub_413D45`, pseudo.c
  16752-16832).** Full trace of its 5 stat lines: total/audio mem
  (`sub_418713`, msg 405), local net id (`sub_40FBFD`, msg 410), BOMBER_ID
  (`sub_40FC22`, msg 411), critical retrans rate (`dword_464950/dword_46495C`,
  msg 415), audio cache hit % (`sub_42717D`, msg 420). **3 of 5 lines are
  pure netplay** (net id / BOMBER_ID / retrans rate — N/A per ADR-0003, same
  bucket as `sub_40C678` below); **the remaining 2** (mem stats, audio cache
  hit %) are Watcom-CRT/DirectSound-specific engine instrumentation our SDL3
  port's `AssetStore`/`AudioEngine` has no equivalent counters for (no
  "audio cache" concept exists — samples are loaded directly). Porting the
  window would mean either 3 blank rows or fabricated numbers standing in for
  metrics the port doesn't track — inventing displayed content, which this
  repo's "no invented visuals/content" rule rules out just as much for a
  debug readout as for game art. **N/A — ungated and reachable, but every
  field is either explicitly out of scope (netplay) or has no honest local
  value to show.**
- **Keys `274..305` (network per-tick statistics dump, `sub_40C678`).**
  Already established elsewhere in this document and in
  `docs/re/coverage-audit.md` §4 as the network-stats-dump function —
  **N/A per ADR-0003**, no new finding this pass, listed here only for the
  row's completeness.

**Net effect on `docs/re/coverage-audit.md` row #35:** RE status stays
**pinned** (it already was); Port status changes from **absent** ("needs a
decision") to **N/A** ("decision made, individually justified per key") — a
genuine closure, not a deferral. No `libs/sim` or `libs/game` code changed by
this pass; nothing in the determinism contract is touched.

### Key `18`'s dialog — a colour-remap QA overlay, NOT a keyboard-remap screen (CONFIRMED 2026-07-09)

A separate string-literal sweep of the decompile flagged `aChangeWhichRem`
("change which remap...", pseudo.c 1492, drawn at (70,200) via `sub_43CD44`
inside `sub_4162F0`) as a possible match for `docs/re/results-and-options.md`
§2's key-remap UI (`sub_407B9D`, "Keyboard definitions"). Tracing
`sub_4162F0` in full (pseudo.c 18349-18387) rules that out — it is a
completely different, developer-only screen:

- **Reachability.** `sub_4162F0` is called from exactly one place:
  `sub_4165D2` (pseudo.c 18408-18415, `dword_460BA4 ? sub_416591 :
  sub_4162F0` — open if closed, close if open), which itself is called from
  exactly one place: the in-round debug key `18` handler (pseudo.c 29734,
  gated `sub_413D01()`). It is NOT reachable from `sub_4080DC` (the Options
  screen), `sub_407B9D` (the real key-remap UI), or anywhere else — a full
  grep of `sub_4162F0`/`sub_4165D2`/`sub_416591` confirms these three call
  sites are the only ones in the binary.
- **It is about player COLOUR, not player keys.** The window's own button
  callback is `sub_41627C` (pseudo.c 18339-18345): `byte_460BD0[dword_460BA0]
  = getvalue(5*dword_460BA0+200)`, `byte_460BDA[...] = getvalue(+201)`,
  `byte_460BE4[...] = getvalue(+202)` — these are the SAME three arrays
  `docs/re/results-and-options.md` §1 already cites as *"the `.RMP` tail
  bytes"* (the per-player-colour RGB override bytes read by the recolour
  pipeline `docs/re/player-colour.md` documents), and `dword_460BA0` is a
  0-9 cursor cycled by one of the dialog's own buttons (`sub_41624E`,
  pseudo.c 18330-18336, `++dword_460BA0 >= 10 → 0`) — i.e. **"which [of the
  ten player .RMP colour] remap[s to reload/inspect]"**, not "which keyboard
  set". The button's own label (also a raw literal, not a `getstring()` id)
  is `aReloadDefaultC` = `"Reload Default Color"` (pseudo.c 1493) — spelling
  out exactly what it does.
- **It's a hardcoded, non-localized debug string — proof this never shipped
  as player-facing UI.** `aChangeWhichRem`/`aReloadDefaultC` are drawn by
  passing the raw `char*` literal straight to `sub_43CD44`/`sub_432298`,
  never through `sub_4124A4`(`getstring`)/MESSAGES.TXT the way every other
  UI string in the front end is (contrast §2's real remap UI, whose every
  label — `getstring(1100)`, `(1110)`, `(1120-1125)`, `(1130)`, `(1140)` — is
  a message-table lookup). A real, localized, ship-quality screen would not
  hardcode its captions.
- **Layout, for completeness:** 3 rows of paired arrow-icon buttons
  (`unk_4591FA`/`unk_4591FC`, raw icon bitmaps — not `getstring()` text —
  at y=40/80/120, x=10/90) around an unread `.RMP`-adjacent index (the
  buttons' own callbacks, `sub_4160D6`/`sub_416094`/`sub_41615A`/
  `sub_416118`/`sub_4161DE`/`sub_41619C`, were not further traced — out of
  scope, since the screen as a whole is confirmed non-shippable), the
  "Reload Default Color" button at (10,186), the "change which remap..."
  label at (70,200), and two more buttons at (10,226)/(40,226) —
  `sub_41624E` (cycle `dword_460BA0` 0-9) and `sub_416220` (untraced).

**Conclusion: no relationship to `KeyRemapScreen`/`sub_407B9D` and no port
gap.** `docs/re/results-and-options.md` §2's key-remap UI facts (header
"Keyboard definitions", the 2×6 action-button grid, `sub_407AD9`'s capture
modal) remain the complete and only player-facing remap flow; our
`KeyRemapScreen` does not need a "which keyboard set" prompt because the
original's real remap screen never has one (both keyboard sets' six actions
are shown together in one 2×6 grid, per §2). `aChangeWhichRem` belongs to
this dead-in-retail-for-normal-players, `-nologo`-gated colour-remap QA tool
instead — same class of finding as this document's `288`/`274..305`
debug-cheat rows above, recorded here for completeness, not for porting.

(Provenance: `sub_4162F0` @ 0x4162F0 pseudo.c 18348-18387; `sub_416591`
@ 0x416591 pseudo.c 18393-18403; `sub_4165D2` @ 0x4165D2 pseudo.c 18407-18414
[already cited above]; `sub_41624E` @ 0x41624E pseudo.c 18330-18335;
`sub_41627C` @ 0x41627C pseudo.c 18338-18345; `aChangeWhichRem` pseudo.c
1492; `aReloadDefaultC` pseudo.c 1493; call-site grep across the full
decompile confirming exactly one caller per function in this chain;
cross-reference `docs/re/results-and-options.md` §2 [`sub_407B9D`, the real
key-remap UI] and §1 [`byte_460BD0`/`BDA`/`BE4` ".RMP tail bytes"
cross-reference already in that doc].)

## Cross-reference

- `docs/re/frontend-flow.md` — the menu/boot/results screen flow this
  round loop is nested inside; its "RESULTS tally tier" section documents
  the multi-round match structure and per-round scoreboard, which shipped
  via ROADMAP "Multi-round best-of-N loop + RESULTS tally 1:1 — DONE
  2026-07-08" — this document's round-end shell section is the connective
  tissue between that tier and `sub_42A3F6`'s full control flow.
  Its "Results MUSIC" paragraph and the 1020/1130 tunables rows were
  CORRECTED by this pass (1130 under all outcome screens; 1020 = setup
  music; per-level stage music `1100+level` discovered) and are now ported.
- `docs/valuelst-map.md` — new ids `101`, `110`, `111`, `112` added by this
  pass (clock HUD); id `101` was already loosely noted as "hurry timing", now
  pinned to an exact value and formula.
- `libs/game/src/game_app.cpp` — `run_match` (`~line 1545`) is the port's
  in-round loop; this document's Esc finding, clock HUD, and hurry-flash
  facts are all implemented there (ROADMAP "In-round shell — RE'd + ported
  2026-07-08"). `draw_player_row` (called from `run_match`, 2026-07-09) adds
  "The player row" section's S:/K: grid + "xxx" dead-slot marker; the
  cornerhead face bubble in that same section is CONFIRMED N/A (2026-07-09
  `dword_45BE3C` trace) — a netplay-only, joystick-gated broadcast with no
  same-screen-multiplayer analogue, not a port gap.
