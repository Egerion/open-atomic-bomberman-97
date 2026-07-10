# Computer-player AI — reverse-engineering (BM95.EXE, `ai.c`/`search.c`)

Exhaustive RE of the Atomic Bomberman computer opponent, distilled from the
`BM95.EXE` decompile (Watcom C, imagebase 0x400000). Facts only — addresses,
offsets, moduli, and the confirmed control flow; no exe code is committed. Every
claim cites its `sub_XXXX` / line. Items that could not be fully pinned from the
pseudocode are flagged **[VERIFY]** and repeated in the closing checklist.

Companion design doc: `docs/adr/0005-ai-architecture.md`.

**IMPLEMENTATION STATUS: COMPLETE (2026-07-05); TEAM WIRING LANDED (2026-07-08);
DIFFERENTIAL FIDELITY AUDIT 2026-07-10 (see §11).** All 8 behaviours are ported
and live in `libs/sim/src/systems/ai.{hpp,cpp}` (Stages 2-5). The `sub_40ABED`
OOB X-table (§3.4/§9.4) and the enemy-finder's two `rand()%10` passes (§5.3)
were pinned from the shipped binary. TEAM mode originally reduced to `slot !=
self` because there was no `Player::team` field; that follow-up has since
landed — `Player::team` (hashed, copied verbatim from `MatchConfig::team[]` at
setup, itself fed by the setup screen's 'T' toggle, docs/re/setup-screens.md)
now gates the enemy scans (`AISystem::same_team`, §3.4/§5.3) and round-end
("one team left", our semantics — see the round-end note below). This grew
the hash layout by one word per player, so `tests/test_golden.cpp` needed a
one-time constant recapture (see that file's own note); the untamed
(all-zero-team) RNG stream and gameplay are unchanged. A 2026-07-10 pass
independently re-verified every behaviour's arithmetic and RNG draw order
against pseudo.c line-by-line and found (and fixed) five real deviations —
none touching golden (no AI players there) — see §11 for the full verdict
table and evidence.

## 0. One-paragraph shape

The AI is a **reactive priority-list**, not a planner. Every tick, for each
computer player, the engine runs an ordered list of eight small "behavior"
functions (`off_45BA78[8]`); the first one that decides to act writes the
player's **input-flag bytes** (direction `+46`, bomb-key `+54/+56`, action-key
`+55/+57`) and returns 1, short-circuiting the rest. The very same movement code
that consumes a human's DirectInput then consumes those bytes — **the AI is an
input provider, byte-for-byte indistinguishable downstream from a keyboard**
(confirmed at `sub_41F29B` line 23028-23036; see §7). Priorities, top to bottom:
grab-glove panic-drop → punch a bomb in the face → walk a path (to a chosen
target, else flee danger) → blast bricks → drop-a-bomb-next-to-an-enemy → chase
a powerup → chase an enemy → wander randomly. "Smartness" is shallow: it has a
real BFS pathfinder and a real danger map, but target selection is random-ish,
bomb-drop is gated on coin-flips, and there is no look-ahead beyond the current
danger grid. It is deliberately beatable.

## 1. Brain storage — the 10-slot array

- **Allocation** `sub_40A0FE` (0x40A0FE): `dword_45ED6C = sub_418511(68, 10)` —
  a block of **10 brains × 68 bytes**, one per player slot. Freed by `sub_40A0C2`.
  Called once at match setup (`sub_40A0FE` invoked at line 14626).
- **Personality init** `sub_40A140` (0x40A140): zeroes the block, then
  `for i in 0..9: brain[68*i].word = rand() % max(1, getvalue(900))`. Only the
  leading **u16 (offset +0) = the personality id** is seeded; getvalue(900) = 1,
  so `rand() % 1` = **always 0** — every brain gets personality 0. (Invoked at
  line 15120.) The "unknown AI personality" fatal check (`aUnknownAiPerso`,
  `sub_40A1C6` line 10387) fires if that id is non-zero — dead code while 900==1.
- **Active-brain pointer** `dword_45ED70`: during one AI update this holds
  `68*slot + dword_45ED6C`, i.e. the current brain. All eight behaviors read the
  brain through this global, not through a parameter — so the update is
  effectively single-threaded over the shared pointer (fine for our port: we
  pass the brain by reference).

### 1.1 Brain struct field map (68 bytes)

Offsets confirmed from the writes/reads in `sub_40A1C6`, the eight behaviors, and
the mover `sub_401B5C`. Types are the widths the code dereferences.

| Off | Type | Name (ours) | Meaning / evidence |
|----:|------|-------------|--------------------|
| +0  | u16  | `personality` | seeded `rand()%getvalue(900)` in `sub_40A140`; validated in `sub_40A1C6` |
| +2  | u16  | `has_path_target` | **the path-target FLAG** (RESOLVED §9.1): tested `if (*(_WORD*)(+2))`, set `= 1` on acquire, cleared `= 0`. The 16.16 coord reads `*(int*)(+2)>>16` alias word +4 (X) |
| +4  | u16  | `path_target_x` | target **tile X** (high half of the goal X / flee bestX). `*(_WORD*)(+4) = v10` in the flee store |
| +6  | u16  | `path_target_y` | target **tile Y** (high half of the goal Y / flee bestY). `*(_WORD*)(+6) = v11` in the flee store |
| +8  | u16  | `path_target_cost` | danger score of the goal tile at capture (`sub_40B20F`: `*(_WORD*)(+8) = sub_424D37(v10,v11)`); re-checked (`!*(_WORD*)(+8)`) to invalidate a stale target |
| +10 | u16  | `has_pow_target`? | "seeking a powerup-target actor" latch (`sub_40B8C2`) — set on acquire, cleared on timeout/failure |
| +12 | i32  | `target_timer` | ms accumulator for the +10/+24 pursuits; `+= dword_464958` each tick, times out at `10*dword_46494C` (~10 frames) |
| +16 | i32ptr | `target_actor` | pointer to the pursued **powerup grid cell** (`sub_40B8C2`: `= sub_422718(...)`, then walked via `sub_4092A1`) |
| +20 | i32  | `pow_step_dir` | godir of the next step toward the +16 target (`sub_40B8C2`: `= v7-1`) |
| +24 | u16  | `has_enemy_target`? | "seeking a powerup within range" latch (`sub_40BAF5`) |
| +28 | i32  | `enemy_timer` | ms accumulator for the +24 pursuit (same timeout rule) |
| +32 | i32ptr | `enemy_actor` | pointer to the pursued powerup cell for the ranged-powerup behavior (`sub_40BAF5`: `= v6` from `sub_409C1F`) |
| +36 | i32  | `enemy_step_dir` | godir of the next step toward the +32 target |
| +40 | u16  | `subtile_x` | player sub-tile offset X = `sub_426599(actor.x)` (set in `sub_40A1C6`) |
| +42 | u16  | `subtile_y` | player sub-tile offset Y = `sub_4265EB(actor.y)` |
| +44 | u16  | `vel_perp` | rotated velocity component (perp): built from ±dir tables × subtile in `sub_40A1C6` |
| +46 | i32(16.16) | `pos_x` | AI's tile-X in the **high word** (`>> 16`); the value every behavior reads as "where I am". Set from `actor.x` in `sub_40A1C6` |
| +48 | i32(16.16) | `pos_y` | AI's tile-Y in the high word |
| +50 | u16  | `pos_y_sub` | low sub-tile Y (bounds-checked ≥0 in `sub_40A1C6`) |
| +52 | u16  | `state_flag` | AI action state: **9 = "committed to blast bricks / drop"** (set by `sub_40AD8D`, cleared to 0 when the situation clears; also cleared in `sub_40A76E`). The task's "+24 state flag" is actually here at +52 |
| +58 | u16  | `bump_stop`? | set to -1 in `sub_40A76E` when the intended step hits flame (abort the step) |
| +62 | dword | `team/self ref` | `BYTE2(...)` used as a base godir in wander (`sub_40A81F` line 10496); also compared team-wise in `sub_40BD44` |
| +64 | u16  | `wander_dir` | the persistent wander godir (`sub_40A81F`): re-rolled `rand()%4` when blocked |

Notes:
- `+46`/`+48` (position) and `+52` (state) are the load-bearing fields; the
  timers `+12`/`+28` and target pointers `+16`/`+32` implement the two
  "commit to a moving target for N frames" pursuits.
- The task brief's guessed offsets (state +24, timers +12/+28, target +32, pos
  +46/+48) are **mostly right**: pos +46/+48 ✓, timers +12/+28 ✓, one target ptr
  +32 ✓; the *state flag* is **+52 not +24** (+24 is the second pursuit's latch).

## 2. Per-brain update dispatcher — `sub_40A1C6` (0x40A1C6)

Signature `sub_40A1C6(slot@eax, actor_ptr@edx)` — Watcom regcall. This is the
function facts.md previously tagged "player %u offscreen" (it *contains* that
error string, but its job is the AI update). Called once per computer player per
tick (§7). Flow:

```
1.  scratch = alloc(rand()%1000 + 100); free(scratch)   // RNG draw #A (throwaway)
2.  if slot not in 0..9: bail
3.  brain = dword_45ED70 = 68*slot + dword_45ED6C
4.  brain.pos_x(+46)  = actor.x(+28)              // 16.16, tile in hi word
    brain.pos_y(+48)  = actor.y(+32)
    brain.subtile_x/y(+40/+42) = sub_426599/sub_4265EB(actor.x/y)
    brain.vel_perp(+44), brain.[+46-lo] = rotate(subtile, actor.godir(+44))
    (offscreen guard: fatal if actor pos < 0)
5.  personality guard: fatal if brain.personality(+0) != 0
6.  BEHAVIOR CHAIN:  v15 = off_45BA78;  i = 0
    do { if (v15[i] && v15[i]()) break; i++; } while (v15[i-1])
        // walk the 8-entry table; STOP at the first behavior that returns non-0
7.  brain.pos_x/y(+46/+48) re-read from actor (post-move refresh); offscreen guard
8.  scratch = alloc(rand()%1000 + 100); free(scratch)   // RNG draw #B (throwaway)
```

**The two `rand()` draws #A and #B (lines 10359, 10412)** feed only a
malloc-then-free of random size — a scratch allocation whose *bytes* are never
used. They still advance the global PRNG, so they are part of the RNG
order/count contract even though their effect is nil. (This looks like a
debugging/heap-stress artifact left in the shipped build.)

Each behavior reads the brain via `dword_45ED70` and the actor via its `a1`
parameter; a behavior "acts" by writing the actor's input bytes and returning 1.

## 3. The behavior table — `off_45BA78[8]` (data @ 0x45BA78, line 2018)

Priority order (index 0 = highest). "acts ⇒ returns 1 and the chain stops."

| # | Func | Role | Fires when (summary) |
|--:|------|------|----------------------|
| 0 | `sub_40BD44` | **Grab-glove drop/hold** | player has grab (+92) |
| 1 | `sub_40BE02` | **Punch a bomb ahead** | player has punch (+91) |
| 2 | `sub_40B20F` | **Walk the path** (to target, else flee) | almost always (the workhorse) |
| 3 | `sub_40AD8D` | **Blast bricks** | bricks adjacent & coin-flip |
| 4 | `sub_40ABED` | **Bomb near an enemy** | enemy within a few tiles & clear |
| 5 | `sub_40BAF5` | **Seek a nearby powerup** | powerup within getvalue(920) |
| 6 | `sub_40B8C2` | **Seek an enemy** | on a 1/50 whim, chase a live foe |
| 7 | `sub_40A81F` | **Wander** | fallback: drift, re-roll dir on block |

### 3.0 — `sub_40BD44`: grab-glove drop / hold (priority 0)
Byte-exact (0x40BD44, verified 2026-07-05):
```
if !player.grab(+92): return 0
if player[+148] (carrying a grabbed bomb):        // already holding one
    player.bombkey(+56)=0; player.bombkey_last(+54)=0; return 1    // just HOLD it, act
else:
    v3 = bomb_at(pos)                             // sub_422E48 at the brain's tile
    if v3 and *(v3+62)==*(player+62) and rand()%2:  // RNG: 50%  (own bomb underfoot; +62 is owner, not team)
        player.bombkey(+56)=1; player.bombkey_last(+54)=0; return 1  // grab it (edge)
    return 0
```
So a grab-AI standing on its own bomb grabs it ~half the time (a fresh bomb-key
edge, which the mover routes to `try_grab` — the AI double-taps: it dropped the
bomb on an earlier tick, now presses again while standing on it to snatch it into
its hands).

**CORRECTION — "hold" is a lob, not an indefinite hold.** When carrying, behaviour
0 writes `+56=0` (bomb key up) every tick. The mover's carried-bomb throw block
(`sub_41F29B` LABEL_246, line 23286: `if (+148 /*carried ptr, dword idx 37*/) if
(v112 || !+56) { throw it; +148=0; }`) fires the throw on `!+56` — i.e. as soon as
the key is released. Since behaviour 0 forces `+56=0` while carrying (and it
short-circuits the chain so nothing re-presses the key), the throw block fires the
VERY NEXT tick: the grab-AI **grabs its own bomb, then lobs it forward** the
following tick. It does NOT hold the bomb indefinitely. (Field note: `+148` = the
carried-bomb pointer, aliased as dword index `+37`; `sub_40BD44` tests `+148`, the
throw tests `+37` — the same field.) Our port reproduces this exactly WITHOUT
special-casing: `behave_grab_drop` leaves `action1=false` while carrying, and our
`player_turn` throw block already throws a carried bomb on `!action1` (release) —
the identical `!+56` gate — so the carried bomb is lobbed on the next tick just as
in the original. (`try_grab` also imposes a `pickup_pause` stun, so there is a
one-tick settle between grab and lob, matching the original's pickup pause.)

**CORRECTED 2026-07-10 (owner, not team):** `+62` on BOTH operands
(`*(bomb+62) == *(actor+62)`, line 11025) is the **owner/self index** — the
same HIWORD(+60) id `sub_4245DA` counts (§9.3) — not a team byte; it is not
gated on the team-mode global `dword_464964` the way behaviour 4's genuine
`+84` team compare is (§3.4/§5.3). So this is unconditionally "the bomb is
mine" (`bomb.owner == self`) in every match, team or not — the same reduction
the mover's own grab block uses (`under->owner == i`, simulation.cpp) and our
port's `own` check reproduces exactly, with no `same_team()` call. (An earlier
draft of this note mislabeled +62 as a team word and, worse, the port's
`random_below(s_, 2)` polarity was inverted from the actual `&& rand()%2`
truthy-to-grab condition below — both wording and code are now RESOLVED; see
the boxed pseudocode's `rand()%2` line, unchanged and correct.) **Draw:** the
`rand()%2` (row b0, §8) fires ONLY when grab is held, not carrying, and a
matching bomb is underfoot (resting OR sliding — `sub_422E48` excludes only
flying/carried, so a sliding own bomb is grabbable too, corrected 2026-07-10)
— otherwise behaviour 0 draws nothing.

### 3.1 — `sub_40BE02`: punch a bomb ahead (priority 1) — CONFIRMED (Stage 5)
```
if !player.punch(+91): return 0
if rand()%4: return 0                             // RNG: only 1-in-4 presses even considered
for i in 0..3:
    if bomb_at(pos + godir[i]): break             // find a bomb on an orthogonally-adjacent tile (dword_45BECC/45BEDC)
else: return 0
player.godir(+46)=i                               // face it
player.actionkey(+57)=1; player.actionkey_last(+55)=0; return 1   // punch (edge)
```
Byte-exact against 0x40BE02. Uses the CORRECT 4-element godir tables
`dword_45BECC[4]={0,1,0,-1}` / `dword_45BEDC[4]={-1,0,1,0}` (not the OOB tables
behaviour 4 uses). Port note: `try_punch` (sub_424A50) uses `p.facing`, and the
mover sets `p.facing = d` unconditionally (even when the bomb blocks the step), so
writing the direction toward the bomb faces the AI at it before the action2 block
runs; the mover also gates `try_punch` on `!bombkey`, satisfied here (the AI sets
only action2). The glove always swings; only the launch + SFX are gated on a bomb
being present (facts.md punch).

### 3.2 — `sub_40B20F`: walk the path (priority 2, the workhorse)
This is where nearly every AI decision resolves. Two modes: **danger-present**
(current tile is dangerous ⇒ flee) and **danger-clear** (idle/roam).

```
if danger_here := sub_424D37(pos) != 0:           // I'm standing in/near a threat
    # (a) validate any existing path target; drop it if it's now safe/reached
    if brain.target(+2) and !brain[+8] and sub_424D37(target) [reached]: brain.target(+2)=0
    if brain.target(+2):                           # have a directed goal -> path to it
        sub_4092A1(pos, target, maxdist=20, &firstdir, &iters, 0)   // DIRECTED BFS
        if !firstdir: brain.target(+2)=0; return 0
        player.godir(+46) = firstdir - 1
    else:                                          # no goal -> flee to the safest reachable tile
        sub_40970B(pos, &firstdir, maxdist=20, &iters, 0, &safeX, &safeY)  // FLEE BFS (min danger)
        if !firstdir: brain.target(+2)=0; return 0
        brain.target(+2)=1
        if danger_here <= sub_424D37(safeX,safeY):     # can't improve -> stand and re-plan
            brain.target(+4/+6)=pos; player.godir(+46) = -1; return 1
        else: brain.target(+4/+6)=safe; brain[+8]=danger(safe); player.godir(+46)=firstdir-1
    sub_40A76E(player)                             # abort the step if it walks into fresh flame
    return player.vel_perp(+44)>>16 != -1
else:                                              # danger_here == 0  (safe)
    if player.trigger(+95) and !player.punch(+91) and !(rand()%10):   // RNG: 1/10, detonate remote bombs
        player.actionkey(+57)=1
    for i in 0..3: if sub_40A59D(pos+godir[i]): break   // is ANY neighbor tile walkable+safe?
    brain.target(+2)=0
    return (found a walkable neighbor) ? 0 : 1     // if boxed in with nowhere safe, "act" (stall) else pass down
```
Key facts:
- **`sub_4092A1`** = directed BFS to `target`, depth ≤ 20, returns the godir of
  the first step (§5.1). **`sub_40970B`** = the flee BFS: same wavefront but
  scored by the danger map `sub_424D37`, returning the first step toward the
  minimum-danger reachable tile (§5.2).
- `sub_40A76E` (§5.4) is the flame-safety veto: if the chosen step lands on
  flame, it cancels the step (godir ← -1) and clears state.
- The remote-detonation whim (trigger held, `rand()%10==0`) lives here, only when
  the AI feels safe.
- **CORRECTED 2026-07-10 (differential audit).** The pseudocode's final line of
  the danger branch — `return player.vel_perp(+44)>>16 != -1` — was always
  correctly transcribed here, but our C++ port had drifted from its own doc:
  `behave_walk_path` hard-coded `return true` after `flame_veto()` in BOTH the
  directed and flee sub-branches, instead of returning whether the veto left a
  real step (`g != -1`). Byte-exact re-read of pseudo.c 10841-10842
  (`sub_40A76E(v4); return *(int*)(v4+44)>>16 != -1;`) confirms the doc: when
  the veto cancels the chosen step, behaviour 2 must **return 0 (pass down)**,
  giving behaviours 3-7 a turn — and their draws — on that tick, not stall
  silently. This is RNG-order-critical (the realized draw list depends on
  which behaviour fires, §8) and was fixed in `ai.cpp` (`return g != -1;` in
  both branches). The "can't improve, stand still" branch a few lines above
  (`brain.target(+4/+6)=pos; ...; return 1`) is unaffected — the original
  returns 1 unconditionally there and never calls the veto (there is no godir
  to veto: the step is already -1), matching the port's plain `return true`.
  No golden impact (golden has no AI players); a new `tests/test_ai.cpp` case
  covers the fall-through.

### 3.3 — `sub_40AD8D`: blast bricks (priority 3)
Byte-exact (0x40AD8D, verified 2026-07-05):
```
if sub_4245DA(actor.playerIndex) < actor.maxBombs(+86): // spare-capacity gate (CONFIRMED from disasm, §9.3)
    if player.constipation(+134): return 0             // can't drop
    count = #{ i in 0..3 : sub_425FB9(pos+godir[i]) == 2 }  // # of adjacent BRICK tiles (type 2)
    if count:
        if sub_423188(pos.x, pos.y):                   // drop-tile CLEARANCE check (see below)
            r = max(1, getvalue(915))                  // 915 = 5
            if rand()%r: return 0                      // RNG: 1-in-5 -> DROP, else pass
            player.bombkey_last(+54)=0; player.bombkey(+56)=1
            brain.state(+52)=9; return 1               // drop a bomb; commit to the escape flee
        return 0                                       // tile not clear: no drop
    else:                                              // no adjacent bricks
        if brain.state(+52)==9: brain.state=0
        return 0
else:                                                  // all my bomb slots are already on the field
    if brain.state(+52)==9: brain.state=0
    return 0
```
So an AI standing next to a destroyable brick, with at least one spare bomb slot
(live own bombs < max bombs +86, §9.3), drops a bomb ~1/5 of the eligible ticks.
It does NOT flee inside this behaviour — it sets `state_flag(+52)=9` ("committed
to a brick-blast drop") and returns; the very next tick(s) the danger grid lights
up under the new bomb, and **behaviour 2 (walk/flee, §3.2) runs first in
priority** and paths the AI out of the blast. State 9 is cleared back to 0 the
moment there are no adjacent bricks (or capacity fills) — i.e. once the AI has
moved off / the situation resolves. (State 9 is otherwise inert in the versus AI: no behaviour keys off it
except this self-clear; it exists for the original's anim/telemetry.)

**`sub_423188(x,y)` is NOT an escape-route search** (correcting the earlier "can
I escape?" gloss). Byte-exact (0x423188):
```
if sub_422E48(x,y): return 0                 // a bomb already on this tile -> can't drop here
v5 = sub_405654(x,y)                         // an ENTITY (rover/ghost, dword_45E0A8 stride-38) here?
return (!v5 || v5[1] != 1) && sub_425FB9(x,y) == 0   // no solid(kind-1) entity AND cell type == 0 (blank floor)
```
It is a **drop-tile clearance** predicate: "may a bomb be placed on THIS tile" —
no bomb here (`sub_422E48`), no solid campaign entity here (`sub_405654`, the
rover/ghost list — empty in versus), and the cell is blank floor (`sub_425FB9==0`,
NOT a wall/brick). Called with the AI's own standing tile, so in the versus AI it
reduces to `!bomb_at(pos) && cells[pos]==Blank`. There is **no look-ahead**: the
original trusts behaviour 2 to flee afterward, it does not verify an escape exists
before dropping. Our port reproduces exactly the two conditions we model (the
`sub_405654` entity array has no versus-mode equivalent; it is empty here — see
§5.7 campaign rovers). This is the same clearance predicate behaviour 4 uses.

### 3.4 — `sub_40ABED`: drop a bomb next to an enemy (priority 4) — RESOLVED (Stage 5)
```
if sub_4245DA(actor.playerIndex) >= actor.maxBombs(+86): return 0  // no spare bomb slot -> pass (CONFIRMED, §9.3)
if abs(tileX(+20)) + abs(tileY(+24)) >= 3:             // Manhattan gate over the STALE +20/+24 snapshot
    for i in 0..4:                                      // scan the 5-tile cross (X=dword_45BAB0[i], Y=dword_45BA9C[i])
        who = player_at(pos + off[i])   (sub_421CB5)    // sub_421CB5 = live (active +0, NOT-DEAD +8) PLAYER at tile (self zeroed out)
        if who:
            if dword_464964 and me.team(+84)==cell.team(+84): return 0   // team mode only
            if !sub_423188(pos): return 0               // drop-tile clearance (NOT an escape search, §3.3)
            if rand()%5: return 0                       // RNG: 1-in-5 -> drop
            player.bombkey_last(+54)=0; player.bombkey(+56)=1; return 1   // drop; DO NOT continue the loop
return 0
```
`sub_421CB5(x,y)` is **confirmed** to return the live **player** (`dword_461BC4`
stride-152 scan; pseudo.c 24207 `*i && !i[2]` = `+0` active and `+8` NOT-DEAD —
`i[2]` is offset +8, the died-this-round flag, NOT the +58 head-hit stun;
CORRECTED 2026-07-10, an earlier pass mislabelled `+8` "not-stunned"), matching
tile via `sub_42665C/sub_4266A3`. The caller zeroes its own actor `+0` across the
probe
(`v5=*a1; *a1=0; sub_421CB5(...); *a1=v5`) so **self is excluded** — our port
skips the self slot instead. The loop does NOT continue after the first hit: the
first enemy found (in cross order) either drops or `return 0`s the whole behaviour.

**[RESOLVED — the OOB X-table read (§9.4).** `dword_45BA9C[5] = {0,-1,0,1,0}`
(the Y offsets, a clean vertical cross) and `dword_45BAB0[] = {-1}` (ONE element).
Reading `dword_45BAB0[0..4]` runs PAST the 1-element array into the adjacent
`.data`. **Reading the shipped BM95.EXE bytes at VA 0x45BAB0..0x45BAC0 gives the
five dwords `{-1, 0, 0, 0, 1}`** (the bytes after `-1` are literally `0,0,0,1`,
then `dword_45BAC4 = 21356` follows). Paired index-for-index with the Y table,
the 5 scan offsets `(X,Y)` are:
`(-1,0) (0,-1) (0,0) (0,1) (1,0)` = **LEFT, UP, SELF, DOWN, RIGHT — a perfect
5-tile plus/cross** centred on the AI. So the "garbage" OOB read is coherent (the
trailing zeros + the `1` happen to complete the cross); it is a documented OOB
read, never a fault in practice. Our port reproduces the exact constants
(`kEnemyScanX[5]={-1,0,0,0,1}`, `kEnemyScanY[5]={0,-1,0,1,0}`). Index 2 is the
self tile, excluded by the self-skip.

**[RESOLVED] the Manhattan gate operands.** `(int)v3 + HIDWORD(v3) >= 3` is
`abs(sub_42665C(a1[5])) + abs(sub_4266A3(a1[6])) >= 3`, where `a1[5]/a1[6]` are
the actor's `+20/+24` — a 16.16 **snapshot** of the actor position. `+20/+24` are
written to the current position only at spawn and at punch-init
(`*(+20)=*(+28); *(+24)=*(+32)`, 0x425?), and to the warp destination on warp-out
— NOT during ordinary walking. `sub_42665C/sub_4266A3` are pixel→tile, so the
gate is `abs(spawnTileX) + abs(spawnTileY) >= 3`, a **near-constant TRUE** per
player (every real Bomberman spawn tile has abs-sum ≥ 3 except a hypothetical
(1,1) corner). We do not track that snapshot field; the faithful determinable
analog is the AI's **current tile** (equal to the spawn snapshot at match start,
same near-always-true result). Reproduced as `abs(tileX)+abs(tileY) >= 3` over the
current tile.

### 3.5 — `sub_40BAF5`: seek a nearby powerup (priority 5)
```
if !brain.has_target(+24) and !(rand()%50):            // RNG: 1/50 chance to ACQUIRE a powerup target
    range = getvalue(920)                              // 920 = 4  ("how close a powerup must be")
    sub_409C1F(pos, range, &nsteps, 0, &cell)          // BFS scan for a powerup within `range`
    if cell and getvalue(920)+1 >= nsteps:             // within range+1 steps
        brain.has_target(+24)=1; brain.timer(+28)=0; brain.actor(+32)=cell
if !brain.has_target(+24): return 0
brain.timer(+28) += frameDelta
if brain.timer(+28) >= 10*msPerFrame: brain.has_target=0; return 0   // give up after ~10 frames
cell = brain.actor(+32)
if !cell or cell.kind != 2 (not a live powerup): brain.has_target=0; return 0
sub_4092A1(pos, cell.tile, maxdist=getvalue(920)+1, &firstdir, &nsteps, 0)   // DIRECTED BFS to it
if !nsteps and rand()%2: brain.has_target=0            // RNG: 50% give up if unreachable
if !firstdir: brain.has_target=0; return 0
brain.step_dir(+36) = firstdir-1
player.godir(+46) = sub_40A59D(next) ? brain.step_dir : -1
return 1
```
getvalue(920) = 4 is confirmed by its VALUELST label: *"how close does a powerup
have to be for an AI to go for it?"* It is the **pathfind depth AND the pickup
range** for this behavior.

### 3.6 — `sub_40B8C2`: seek an enemy (priority 6)
Structurally identical to §3.5 but targets a **player** (`sub_422718`) instead of
a powerup, using the +10/+16/+20 field trio, `maxdist = 20`:
```
if !brain.has_target(+10) and !(rand()%50):            // RNG: 1/50 acquire an enemy target
    p = sub_422718(actor)                              // pick a random LIVE opponent (see §5.3)
    if p: brain.has_target(+10)=1; brain.timer(+12)=0; brain.actor(+16)=p
if !brain.has_target(+10): return 0
brain.timer(+12) += frameDelta
if brain.timer(+12) >= 10*msPerFrame and !(rand()%50): brain.has_target=0; return 0   // RNG: 1/50 give up on timeout
p = brain.actor(+16)
if !p or p.kind!=1 or p.dead(v4[2]=+8): brain.has_target=0; return 0   // v4[2] is +8 (died-this-round), NOT +58 stun
sub_4092A1(pos, p.tile, maxdist=20, &firstdir, &nsteps, 0)   // DIRECTED BFS toward the foe
if !nsteps and rand()%2: brain.has_target=0            // RNG: 50% give up if unreachable
if !firstdir: brain.has_target=0; return 0
brain.step_dir(+20)=firstdir-1
player.godir(+46) = sub_40A59D(next) ? brain.step_dir : -1
return 1
```
Note it does **not** drop bombs — it only walks toward the enemy; the actual
bombing of a cornered foe is behavior §3.4, which sits *above* it in priority.

**RESOLVED (Stage 5) — the acquire, timeout, and give-up draws.** Byte-exact
(0x40B8C2): the acquire `rand()%50` (line 10881) fires only when `!+10`; on a hit
`sub_422718` **itself draws `rand()%10` (pass-1 start), plus a SECOND `rand()%10`
if pass 1 is empty** (§5.3) — these inner draws are part of the RNG contract even
though §8's table lists only the outer draws. The timeout give-up (line 10894) is
`(10*msPerFrame <= +12) && !(rand()%50)` — the `%50` is drawn ONLY once the timer
reaches 10 (short-circuit `&&`), and on a FAILED roll the target is KEPT (unlike
powerup-seek §3.5, whose 10-tick timeout is unconditional). The unreachable
give-up `if (!nsteps && rand()%2)` (line 10914) draws `%2` only when the BFS found
no path. Target liveness reload: `!v4 || *v4 != 1 || v4[2]` = target gone / not
the alive value (+0 != 1) / **DEAD** (`v4[2]` = +8, the died-this-round flag —
NOT the +58 stun; CORRECTED 2026-07-10, was mislabelled "stunned"). Our port
stores the target's **slot** (not a pointer) and reloads liveness as
`present && alive` each tick (a stunned-but-alive foe stays a valid target).

### 3.7 — `sub_40A81F`: wander (priority 7, fallback)
```
if !(rand()%25):                                       // RNG: 1/25 chance to pick a NEW random turn
    v2 = (brain[+62].byte2 + 2*(rand()%2) - 1) & 3      // RNG: turn ±90° off the base dir
    if sub_40A59D(pos + godir[brain.wander(+64)]):      // if the current wander dir is safe...
        brain.wander(+64) = v2                          //   ...adopt the new turn
if sub_40A59D(pos + godir[brain.wander(+64)]):          // wander dir walkable+safe?
    player.godir(+46) = brain.wander(+64); return 1     // step that way
else:
    brain.wander(+64) = rand()%4; return 0              // RNG: blocked -> re-roll dir, pass
```
`sub_40A59D(x,y)` = the "tile is safe to step onto" predicate: **no bomb**
(`sub_422E48`) **and not a solid/brick cell** (`sub_425FB9`, the cell-type reader
— see §5.6 [CORRECTED]) **and no flame** (`sub_42708D`) **and passable**
(`sub_424D37 == 0`, i.e. danger-free) — line 10474-10482. It does **not** test
for a floor powerup, so a wandering AI CAN drift onto a loose powerup and pick it
up; deliberate seeks go through §3.5, but powerup tiles are not walls to the AI.

## 4. The danger map — `sub_424D37` / the threat grid `dword_4621F4`

The AI's entire sense of safety is one integer grid.

- **Reader** `sub_424D37(x,y)` (0x424D37): bounds-check, then
  `return dword_4621F4[y*cols + x]` — a flat `rows*cols` dword grid (size
  `dword_462204`). 0 = safe; larger = more dangerous.
- **Writer** `sub_424DFE(x,y,v)` (0x424DFE): `grid[...] = max(grid[...], v)`
  (keeps the strongest threat).
- **Cleared** each frame and repopulated from live hazards. The three sources:
  1. **Active flame** — the flame-grid updater (`sub_426D06`, "flame %s green")
     writes **1000** at a lit cell (line 27418) → flame tiles are maximally
     dangerous. **Precision, 2026-07-10:** there are two write sites — the
     per-tick `sub_426D06` (27418) writes 1000 only in its `else` branch (flame
     type `+4 != 9`), and the spawn-time `sub_426FCC` (27492) writes 1000 for
     every newly-created flame including on the brick-dissolve tick. A type-9
     flame (a brick actively dissolving) skips the per-tick 1000-write and
     instead runs the dissolve countdown — but that tile is still a solid brick
     (`sub_425FB9 == 2`) until it clears, i.e. still an obstacle the AI's BFS
     never enters or scores, so the distinction is invisible to every AI
     decision. Our port's blanket `flame[y][x] > 0 → 1000` is behaviourally
     equivalent; no code change.
  2. **Live bombs (predicted blast)** — the bomb updater (`sub_42331C`, starts
     0x42331C, the "regular/trigger/jelly" bomb machine; danger write at line
     25685) writes `v = (bomb[+66]>>16) + 100` at the bomb tile, then propagates
     that same `v` outward along all 4 rays up to the bomb's flame length,
     **stopping AT a wall or brick (`sub_425FB9` nonzero — nothing is written on
     that tile) or AT another bomb (`sub_422E48`), and extending one tile PAST a
     floor powerup (`sub_42542D`: mark it, then stop)** (line 25689-25704).
     **CORRECTED 2026-07-10** (differential audit against pseudo.c, byte-exact
     re-read of the loop): an earlier draft of this bullet had the "one tile
     past" exception on the wrong tile kind (it said "past a brick", which
     directly contradicted §9.2 below and was never true — `sub_425FB9` returns
     nonzero for BOTH a solid wall (1) and a brick (2), and the loop's `if
     (sub_425FB9(...)) break;` fires before any write, so a brick tile never
     gets a danger value). §9.2's account was always the correct one; our port
     (`ensure_grids()`, ai.cpp) already implements it exactly and needed no
     change. So the AI "sees" where a bomb is about to reach and how soon.
     **RESOLVED (§9.2):** bomb field `+66` is the **elapsed** fuse phase (16.16)
     — it counts UP, and the detonation test fires once `(+66>>16) >
     getvalue*msPerFrame` (end-of-life), so a bomb nearer detonation has a
     LARGER `+66` and thus a LARGER danger value. Our port uses `100 +
     elapsed_ticks` (`= fuse_total - remaining`; a waiting trigger bomb gets the
     max). It only orders live-bomb tiles relative to each other — never vs
     flame's 1000 — and the danger grid is unhashed scratch, so it never touches
     the golden.
  3. **The closing walls ("fire-god")** — the enclosure/hurry updater (line
     27200-27222) walks the spiral of imminent bricks and writes a decaying
     `v = 10*getvalue(910) + 100`, then `-= 10` per step, along
     `getvalue(910)` tiles of look-ahead. getvalue(910) = 15, label: *"how far
     ahead the fire-god tells the AIs that it is a threat."* So AIs start fleeing
     the shrinking arena ~15 tiles before the wall arrives.

The danger map is a **shared, sim-global grid rebuilt every tick** from bombs +
flames + the closing walls — not per-AI. Our port reconstructs the same grid
each tick from `State` (§ADR).

## 5. Pathfinding & helpers

### 5.1 Directed BFS — `sub_4092A1` (0x4092A1)
`sub_4092A1(sx, sy, tx, ty, maxdepth, &firstdir, &iters, &maxfront)`.
- A wavefront over a fixed **100-node open list** `dword_45ED68` (6 dwords/node:
  `[alive, x, y, firstdir, camedir, agebits]`), seeded with the ≤4 open neighbors
  of the start (each tagged with the godir `i+1` it came from — the return value).
- **Tie-break RNG**: `v35 = 2*(rand()%2) - 1` at entry — a per-call ±1 that flips
  the order the two side-branches `(camedir + v35*±1) & 3` are expanded, so paths
  of equal length pick a randomized-but-deterministic turn.
- Blocked test = `sub_409083(x,y)` → `dword_45E0E4[20*x + y]` (a **precomputed
  obstacle grid**, stride 20/col; 1 = blocked, out-of-bounds = blocked). This
  grid bakes solids + bricks + bombs (built elsewhere each tick).
- Visited/cost via `sub_40902A(x,y,cost)` + `sub_4091C9` (node alloc) +
  `sub_4091A0` (reset). Cost increments by 10 per ring.
- Returns the **godir of the first step** of a shortest path to `(tx,ty)`
  (`firstdir`, 1..4 → godir 0..3 after `-1`), `iters` = rings expanded,
  `maxfront` = peak frontier size. `firstdir==0` ⇒ no path within `maxdepth`.

**CORRECTED 2026-07-10 — boxed-in start reported the wrong `iters`.** The
original is a `do…while`: it always completes at least one pass and
increments its ring counter `v32` before testing the loop condition
(9705-9821), so a start tile with **zero** open neighbours (the seed loop
allocates no nodes) still exits with **`iters = 1`**, never 0. Our port's
`directed_bfs`/`powerup_scan_bfs` never entered their `while` loop in that
case, leaving `out_iters` at its initial 0. This is RNG-order-critical:
behaviours 5/6 gate their unreachable-target `rand()%2` give-up on `iters ==
0` (§3.5/§3.6, §8 rows b5d/b6d), so the port drew a spurious extra `%2` in the
boxed-in state that the original never draws. Fixed in `ai.cpp` (both
pathfinders): `if (out_iters == 0) out_iters = 1;` immediately before the
final `return -1;` — a no-op whenever any neighbour was ever seeded, since the
ring-drain already bumps `iters` to ≥ 1 before that point. Reachable in real
play (rare): a boxed-and-*dangerous* start makes behaviour 2 pass down through
3/4 to 5/6, which then invoke the directed BFS while boxed in. No golden
impact (golden has no AI players). For `powerup_scan_bfs` specifically the
same correction is currently **unobservable** — behaviour 5 only reads its
scan's `iters` via `range+1 >= iters`, gated on a FOUND cell, and a boxed-in
scan finds none — but it is applied for structural parity with `sub_4092A1`'s
identical do-while shape.

**Structural note (not a determinism bug):** the original is not a plain FIFO
BFS — it is a beam-flood where each node first walks straight along its own
`camedir` (reusing its array slot in place) and spawns two perpendicular
children per step in a `v35`-flipped order, deferring same-pass children by
one "age" bit (9761-9812). Our port is a clean 4-neighbour ring BFS with the
same one-tie-break-per-call contract. When a **unique** shortest path exists,
both return the identical first step; when several equal-length paths
exist, the two algorithms' tie-breaking can steer them to *different* (but
equally shortest) first steps. Draw count/order is unaffected (still exactly
one tie-break per call) — this is a walking-choice fidelity gap in rare tie
positions, not an RNG-order/determinism break, and is left as a known,
accepted simplification (porting the exact beam-flood would be a much larger,
higher-risk rewrite for a cosmetic difference).

**Overflow note:** past the fixed 100-node frontier the original calls a
`__noreturn` fatal handler (`sub_4091C9` failing → `sub_4128C9`, 15987); the
port silently caps further pushes (`if (open_n < 100)`). Unreachable on any of
our boards (15×11, depth ≤ 20 keeps the frontier well under 100), so this is
inert in practice — noted for completeness, not fixed (a silent cap is
strictly safer than a crash and the game never ships a board that hits it).

### 5.2 Flee BFS — `sub_40970B` (0x40970B)
`sub_40970B(sx, sy, &firstdir, maxdepth, &iters, &maxfront, &bestx, &besty)`.
Same wavefront machinery and same ±1 tie-break RNG (`v40 = 2*(rand()%2)-1`), but
there is **no goal tile**: each expanded cell is scored by `sub_424D37` (danger),
tracking the minimum. The moment a **danger-0** tile is reached it returns that
path's first step immediately; otherwise it exhausts the frontier and returns the
first step toward the lowest-danger tile found (`bestx/besty`). This is the
core "run away from bombs/flame" routine driving §3.2's flee branch.

### 5.3 Enemy finder — `sub_422718` (0x422718) — RESOLVED (Stage 5)
Picks a **random** live opponent from the 10-player array `dword_461BC4` (stride
152). It is **not** nearest-enemy — targeting is random among live foes. Two
nested passes, byte-exact:
- **Pass 1** starts at `i = rand()%10` and scans 10 slots forward (wrapping).
  It SKIPS a slot when: it is self (`a1 == v7`), absent (`!+16`), **another
  computer player (`+16 == 1`)**, inactive (`!+0`), or **DEAD** (`v7[2]` = +8,
  the died-this-round flag — NOT the +58 stun; CORRECTED 2026-07-10, pseudo.c
  24741). The first surviving slot is a live **human** opponent; in a no-team
  match it is returned
  immediately, in team mode only if its team `+84` differs.
- **Pass 2** runs only if pass 1 exhausts all 10 without a hit. It starts at a
  **SECOND `rand()%10`** and scans again, but drops the `+16 == 1` test — so it
  RELAXES to **any** live opponent (including other AI). Same self/absent/active/
  stun/team filters otherwise. Returns the first hit, else 0 (no live opponent).

So `sub_422718` draws **one `rand()%10` always** (pass-1 start) and a **second
`rand()%10` only when pass 1 finds nothing**. These inner draws are part of the
RNG contract (they sit inside behaviour 6's acquire, §8). Our port
(`pick_live_enemy`) mirrors both passes and returns the target **slot**; the
no-team team filter reduces to `slot != self`. The +16 player-type byte:
1 = computer, 2 = human (§7), so "skip +16==1" in pass 1 = "prefer a human, fall
back to AI in pass 2".

### 5.4 Step-into-flame veto — `sub_40A76E` (0x40A76E)
Given the just-chosen godir at actor `+46`, if the tile one step ahead is on fire
(`sub_42708D`), it zeroes the AI state (+52), sets +58 = -1, and **sets the godir
to -1** — cancelling the step so the AI never voluntarily walks into flame even
when its path/flee said to. Called at the end of §3.2's danger branch; its
return value is discarded by the caller (`sub_40A76E(v4);`, statement, no use of
the result) — only the +52/+58/godir side effects matter, and it is §3.2's
*next* statement (the `vel_perp>>16 != -1` return, corrected above) that reads
the godir it may have just cancelled.

**CONFIRMED 2026-07-10: `+58` is write-only, our port's omission is correct.**
A full-file scan for reads of brain offset +58 across the dispatcher and all
eight behaviours finds exactly one hit — the write in `sub_40A76E` itself
(line 10478). Nothing downstream (no behaviour, no the dispatcher, no mover)
ever reads it, so it is dead state in the original; our `Brain` struct
correctly has no field for it (ADR-0005 §3), a harmless simplification, not a
gap.

### 5.5 Powerup scan — `sub_409C1F` (0x409C1F)
`sub_409C1F(sx, sy, ?, maxdepth, &iters, ?, &firstdir/&cell)`. Same BFS as
`sub_4092A1` but the goal test is `sub_42542D(x,y)` (**powerup at tile**): it
returns as soon as it reaches a tile carrying a floor powerup, yielding the first
step toward it and the powerup cell. Used by §3.5 with depth = getvalue(920).

### 5.6 "Safe tile" predicate — `sub_40A59D` (0x40A59D)
`!bomb(sub_422E48) && !solid_or_brick(sub_425FB9) && !flame(sub_42708D) &&
danger==0 (sub_424D37==0)`. The universal "may I stand here" gate used by wander
and the final step of every pursuit. **[CORRECTED Stage 3]** the middle test is
`sub_425FB9` — the **CELL-TYPE** reader (0 = blank floor, 2 = brick; any non-zero
= wall/brick), NOT the powerup test `sub_42542D`. Read fresh from the decompile:
```
if (sub_422E48(x,y)) return 0;   // a bomb here
if (sub_425FB9(x,y)) return 0;   // a solid or brick cell here
if (sub_42708D(x,y)) return 0;   // flame here
return sub_424D37(x,y) == 0;     // danger-free
```
So `sub_40A59D` does **NOT** reject a floor powerup — it never calls
`sub_42542D`. The AI MAY step onto a powerup tile; that is how behaviour 5 takes
its final step onto its target (and how a wandering AI can drift onto a loose
powerup). Only the `sub_42542D`-based goal tests in the powerup scan (§5.5) treat
a powerup tile specially. (Earlier drafts of §3.7/§5.6 read `sub_425FB9` as a
powerup test — that was wrong; the versus-AI never refuses a powerup tile.)

### 5.7 The AI-entity mover — `sub_401B5C` (0x401B5C) — rovers/ghosts only
`sub_401B5C` is the **campaign rover/ghost** mover (advances a non-player AI
entity along its `+46` godir, bumping the anim counter once per pixel-step, per
facts.md ANI §). The **human-shaped computer players do NOT use it** — they reuse
the player mover `sub_41EC84`/`sub_41F29B` after the AI writes their input bytes
(§7). `sub_401B5C` matters only if/when we port campaign monsters (out of scope
for the multiplayer AI). Its intersection direction-change uses getvalue(1200)
(=3, "chance a ghost/rover changes dir at an intersection") and getvalue(1205)
(=3, "chance the change is NOT toward a human") — campaign tunables, listed here
for completeness but not part of the versus AI.

## 6. Tunables (VALUELST) — confirmed meanings

Read straight from the file's own comments (`DATA/RES/VALUELST.RES`).

| id | value | VALUELST label | Role in the AI |
|---:|------:|----------------|----------------|
| 900 | 1 | "how many different AI personalities are predefined?" | brain-init spread: `personality = rand()%900`. =1 ⇒ every brain is personality 0; the multi-personality branch is dormant |
| 910 | 15 | "how far ahead the fire-god tells the AIs that it is a threat" | closing-wall danger look-ahead: writes a decaying threat along 15 tiles of the incoming spiral (§4.3) |
| 915 | 5  | "chance that an AI will execute the blast-bricks routine" | `sub_40AD8D`: drop-on-brick fires when `rand() % max(1,915) == 0` ⇒ ~1/5 of eligible ticks |
| 920 | 4  | "how close does a powerup have to be for an AI to go for it?" | `sub_40BAF5`: BFS depth **and** acceptance range for the powerup-seek (`getvalue(920)+1 >= steps`) |
| 1200 | 3 | "chance a ghost/rover changes dir at an intersection" | **campaign** rover mover only (`sub_401B5C`) — not versus AI |
| 1205 | 3 | "chance the dir change will NOT be toward a human" | **campaign** rover mover only |

No other VALUELST ids are read by the versus-AI code paths. (900/905 appear in
the literal-getvalue survey in facts.md; 905 is not referenced by any AI function
in the decompile — **[VERIFY]** 905 is likely unused/campaign.)

## 7. How the brain drives the player — the input-flag bridge (the key fact)

At `sub_41F29B` (the per-player updater), line **23028-23036**:

```c
if ( v113 && !dword_4621E0 ) {                 // player is eligible to act this frame
    if ( *((_BYTE*)v111 + 16) == 1 )           // +16 == 1  => COMPUTER player
        { sub_40179F(); sub_40A1C6(slot, v111); }   //   run the AI brain
    else                                        // +16 == 2 => local human
        sub_41E61E(v111);                       //   read DirectInput
}
// ...then the SAME code below consumes v111's input bytes (godir +46, +54/+56 bomb,
//    +55/+57 action) and runs the mover sub_41EC84 — identically for AI or human.
```

So:
- **`+16` is the player-type tag** (slot-fill loop, line 23788-23801):
  **1 = computer/AI**, **2 = local human**, 3 = entering, 4 = dying, 0 = absent.
  With one local human, slot **index 1 is forced to AI** and the first free slot
  is the human.
- The AI's *only* outputs are the player's **input-flag bytes**, the exact same
  bytes a keyboard would set:
  - **godir `+46`** (16.16; hi word 0..3 = Up/Right/Down/Left, or -1 = no move)
  - **bomb key `+56`** ("down this frame") and **`+54`** ("down last frame") —
    the drop is edge-gated on `+56 && !+54` in the mover (see facts.md "Spooger"
    / LABEL_246). The AI writes `+56=1; +54=0` to force a fresh press.
  - **action key `+57`** ("down this frame") and **`+55`** ("last frame") — for
    punch/trigger, same edge rule.
- `sub_40179F` just clears a scratch global (`dword_4646C0 = 0`) before the run.

**RESOLVED 2026-07-10 — `v113` is richer than "present && alive", and our
caller was missing one piece of it.** `v113` (set true at 22981, the top of
this per-player pass) is cleared to false for: a stun/pickup-pause countdown
(`actor+58 > 0`, i.e. `Player::stun`), and player-type modes 4/5/6/7
(entering/dying/dead, 23015-23026); `dword_4621E0 > 0` is a separate global
freeze. So the original skips the **entire** dispatch — draws A/B included —
for a stunned, entering, dying, or globally-frozen player, not just an absent
or dead one. Our `simulation.cpp` tick loop already excludes dying/dead via
its own `present && alive` guard, and has no wall-clock/menu concept to freeze
against (`dword_4621E0` has no equivalent in a headless, externally-ticked
sim — pausing is simply "the caller stops calling `tick()`"), but it was
missing the **stun** exclusion: a stunned-but-still-`alive` AI player (e.g.
mid `pickup_pause` after a grab) would still get `AISystem::decide()` called,
drawing draws A/B (and possibly a behaviour's draws) on a tick the original
draws nothing. `player_turn` already no-ops a stunned player's turn (ticks the
countdown and returns before reading any input), so this was invisible to
gameplay but not to the RNG stream — fixed by gating the call on `p.stun == 0`
(`simulation.cpp`, the step-1 player loop). No golden impact (golden has no AI
players); a new `tests/test_ai.cpp` case pins a stunned AI drawing zero RNG.

**Consequence for the port (the linchpin):** the computer player is a
`PlayerInput` producer. If our `AISystem` writes the same `up/down/left/right/
action1/action2` a human would, and we feed it through the identical `TickInputs`
→ `player_turn` path, the AI is *provably* consistent with the movement/bomb code
we already ship — no separate "AI mover" to keep in sync. This is exactly the
design in `docs/adr/0005-ai-architecture.md`.

## 8. The full `rand()` order/count contract (per AI-controlled player, per tick)

The sim's determinism hinges on reproducing **the order and count of PRNG draws**
(CLAUDE.md rule 2). Within one computer player's update, draws happen in this
exact sequence. Draws inside the BFS helpers happen *when that helper is called*
by the active behavior; only ONE behavior runs to completion per tick (the chain
short-circuits), so the realized draw list depends on which behavior fires — but
the sequence *within* each path is fixed. Reference table (address → modulus →
decision):

| Order | Site (addr / line) | Draw | Gates / used for |
|------:|--------------------|------|------------------|
| A | `sub_40A1C6` 10359 | `rand()` (÷—) | scratch alloc size `%1000+100` (result unused; **still a draw**) |
| — | *(behavior chain runs; the fired behavior's draws below, in order)* | | |
| b0 | `sub_40BD44` 11025 | `rand()%2` | grab own bomb underfoot? |
| b1 | `sub_40BE02` 11046 | `rand()%4` | consider a punch this tick? |
| b2 | `sub_4092A1` 9705 **or** `sub_40970B` 9900 | `2*(rand()%2)-1` | path/flee BFS turn tie-break (one draw per BFS call) |
| b2'| `sub_40B20F` 10846 | `rand()%10` | (safe branch) detonate remote bombs? |
| b3 | `sub_40AD8D` 10607 | `rand()%max(1,getvalue(915))` | blast-bricks drop? (~1/5) |
| b4 | `sub_40ABED` 10556 | `rand()%5` | bomb-near-enemy drop? |
| b5a| `sub_40BAF5` 10949 | `rand()%50` | acquire a powerup target? |
| b5b| `sub_409C1F` 10128 (entry) | `2*(rand()%2)-1` | powerup-scan BFS tie-break (when acquiring) |
| b5c| `sub_4092A1` 9705 | `2*(rand()%2)-1` | path-to-powerup BFS tie-break |
| b5d| `sub_40BAF5` 10986 | `rand()%2` | give up unreachable powerup? |
| b6a| `sub_40B8C2` 10881 | `rand()%50` | acquire an enemy target? |
| b6a1| `sub_422718` (pass 1) | `rand()%10` | enemy-finder pass-1 start index (drawn when acquiring) |
| b6a2| `sub_422718` (pass 2) | `rand()%10` | enemy-finder pass-2 start index (ONLY if pass 1 is empty) |
| b6b| `sub_40B8C2` 10894 | `rand()%50` | give up enemy on timeout? (drawn only once timer≥10) |
| b6c| `sub_4092A1` 9705 | `2*(rand()%2)-1` | path-to-enemy BFS tie-break |
| b6d| `sub_40B8C2` 10913 | `rand()%2` | give up unreachable enemy? (only if no path) |
| b7a| `sub_40A81F` 10494 | `rand()%25` | wander: pick a new turn? |
| b7b| `sub_40A81F` 10496 | `rand()%2` | wander: which ±90° turn |
| b7c| `sub_40A81F` 10513 | `rand()%4` | wander: re-roll dir when blocked |
| B | `sub_40A1C6` 10412 | `rand()` (÷—) | scratch alloc size `%1000+100` (result unused) |

Init-time (once at match setup, not per tick):
- `sub_40A140` 10330: `rand()%max(1,getvalue(900))` per brain (10 draws) — all 0.

**Critical ordering notes for the port:**
1. Draws **A** and **B** bracket *every* AI update, unconditionally — 2 fixed
   draws per AI player per tick, regardless of behavior. Our port must reproduce
   both (or, if we drop the vestigial scratch-alloc, do so *uniformly* and
   re-baseline any AI golden — but since golden has no AI players, see ADR, it is
   simplest to keep the two draws for exactness).
2. Only ONE behavior body's draws occur per tick (short-circuit). The realized
   list is A, then the fired behavior's draws (which include its BFS tie-break
   draw if it calls a pathfinder), then B.
3. The BFS tie-break draw (`2*(rand()%2)-1`) is drawn **once per pathfinder
   invocation**, at the top, before any expansion — independent of path length.
4. `getvalue(915)`/`getvalue(920)` etc. are **not** RNG; they are config reads.

**Cross-player ordering:** the outer player loop in `sub_41F29B`'s caller runs
players in slot order 0..9 (facts.md "Player struct + update loop",
`sub_420F07`), so the *global* per-tick AI draw stream is the concatenation of
each AI player's list in ascending slot index. Our tick must iterate AI players
in the same order (§ADR tick placement).

## 9. Follow-up checklist — RESOLVED (Stage 2 Hex-Rays/pseudo pass, 2026-07-05)

The five items below were pinned from the decompile before hashing the brain.
All are numeric tie-breakers / drop-suppression heuristics; none changes the
AI's structure. Evidence is quoted inline (no exe code committed — these are
offsets and control flow only).

1. **[RESOLVED] Brain field packing +2/+4/+6/+8.** From `sub_40B20F`
   (0x40B20F) and the BFS `sub_4092A1` start-equality test `if ( a1 != a4 ||
   a2 != a3 )` (line 9711): the directed call passes `(posX, posY, +4>>16,
   +2>>16)` as `(a1=sx, a2=sy, a3, a4)`, and since a1(sx=posX) is compared to
   a4 and a2(sy=posY) to a3, the target's X arg is a4 (`+2`) and its Y arg is a3
   (`+4`). Combined with the flee store (`+4 = bestX (v10)`, `+6 = bestY
   (v11)`, `+8 = danger(bestX,bestY)`) and the reads `*(int*)(+2)>>16`,
   `*(int*)(+4)>>16` (which alias the packed words), the layout is a 16.16-style
   pack:
     - **word +2** = the "has path target" flag (tested `if (*(_WORD*)(+2))`,
       set `*(_WORD*)(+2) = 1`, cleared `= 0`);
     - **word +4** = target **tile X** (high half of the X coord / bestX);
     - **word +6** = target **tile Y** (high half of the Y coord / bestY);
     - **word +8** = captured **danger cost** of the goal tile
       (`*(_WORD*)(+8) = sub_424D37(bestX,bestY)`), used to invalidate a stale
       target (`!*(_WORD*)(+8)` re-check at line 10784).
   Our `Brain` mirrors this as `has_path_target` / `path_target_x` /
   `path_target_y` / `path_target_cost` (plain ints, tile granularity).
2. **[RESOLVED] Bomb field +66 = ELAPSED fuse phase (16.16).** `sub_42331C`
   line 25685: `v44 = (*(int*)(bomb+66) >> 16) + 100`, propagated along the 4
   rays out to `bomb+76` (flame length) via `dword_45BECC/45BEDC` (the DX/DY
   tables), stopping at a brick/wall (`sub_425FB9`) or a bomb (`sub_422E48`) and
   one tile PAST a floor powerup (`sub_42542D`). `+66` counts UP: the fuse/flame
   detonation test (line 27409/27422) fires (`*state = 0`) once
   `(+66 >> 16) > getvalue(10|20) * msPerFrame` — i.e. at end-of-life — so a
   bomb nearer detonation has a LARGER `+66` and thus a LARGER danger value.
   Our integer port uses `danger = 100 + elapsed_ticks`, where `elapsed_ticks =
   fuse_total - remaining_fuse` (a waiting trigger bomb, `fuse <= 0`, gets the
   max = a standing threat). This scalar only ORDERS live-bomb tiles against
   each other (never vs flame's flat 1000), and the danger grid is unhashed
   scratch, so its exact magnitude never touches the golden.
3. **[RESOLVED — byte-confirmed from BM95.EXE, 2026-07-09] The "column guard"
   is neither a column count nor an undefined comparand: it is the standard
   spare-bomb-capacity gate.** Decoded from the raw instruction bytes (PE
   BEGTEXT, raw 0x400 = VA 0x401000; capstone x86-32 over the function
   regions), because Hex-Rays lifted the comparand as an undefined `v2`/edx:
   - **The comparand.** In both callers the edx setup the decompiler dropped
     is right at function entry. `sub_40ABED` at 0x40AC01: `xor edx, edx` then
     `mov dl, byte ptr [eax+0x56]` (eax = the actor), i.e. edx = zero-extended
     **player byte +86 = max bombs (bomb capacity)**; then `mov eax,
     [eax+0x3C]; sar eax, 0x10; call sub_4245DA; cmp eax, edx; jl <proceed>`.
     `sub_40AD8D` is instruction-for-instruction identical at 0x40ADA1/0x40ADAF
     (`mov dl, [eax+0x56]` … `cmp eax, edx; jl 0x40ADDB`). So the gates are
     `sub_4245DA(...) < maxBombs(+86)` ⇒ proceed, else pass down.
   - **What sub_4245DA counts.** The earlier "bombs in column x" reading was
     wrong. The argument is the actor dword `+60 >> 16` (`[eax+0x3C]`,
     `sar 0x10`), and +60 is a PACKED field, not a position: `sub_41EB13`
     passes its **low byte** and its **high word** as separate bomb-create
     arguments (line 22513-22517: `25 * (player+60 >> 16) + counter` is the
     per-player slot allocator; `HIWORD(player+60)` is create-arg a5), and
     `sub_422EDE` stores that high word into the **bomb word at +62** (word
     index 31) — the owner player index — while the bomb's real position goes
     to +28/+32. `sub_4245DA` (disasm: `mov eax, [bomb+0x3C]; sar eax, 0x10;
     cmp eax, [ebp-0x14]`) reads the bomb dword at +60, whose high word IS
     that owner word at +62. So `sub_4245DA(idx)` = **count of live bomb
     slots owned by player idx** — exactly the count the mover compares
     against +86 at the normal drop (line 23346 `+86 > sub_4245DA(...)`), the
     spooge-loop stop (line 23336 `+86 <= sub_4245DA(...)`), and the
     "last bomb" grunt (line 23363 `+86 - 1 == count`).
   - **Consequence for behaviours 3/4:** the gate is simply "do I have a spare
     bomb slot?" — a bomb elsewhere in the AI's tile column is irrelevant, and
     an AI with capacity ≥ 2 may bomb again while its first bomb is still
     live. Our port previously used the "safest interpretation" placeholder
     ("no bomb already in my column", comparand 1); that was **corrected** to
     `bombs_placed >= max_bombs ⇒ pass down` (`ai.cpp` behaviours 3/4), where
     `Player::bombs_placed` is the sim's maintained equivalent of the
     original's owner scan (same counter `BombSystem::drop` gates on).
4. **[RESOLVED — byte-confirmed from BM95.EXE] `sub_40ABED` scan geometry
   (behavior 4, Stage 5 IMPLEMENTED).** i in 0..4: `v7 = posX +
   dword_45BAB0[i]`, `v8 = posY + dword_45BA9C[i]`, then `who = sub_421CB5(v7,v8)`
   (a live **player** at the tile — the `dword_461BC4` stride-152 scan,
   `+0` active and `+8` NOT-DEAD (pseudo.c 24207 `*i && !i[2]`; +8 = died-this-
   round, NOT the +58 stun — CORRECTED 2026-07-10); self excluded by the `*a1=0`
   probe trick).
   Tables: `dword_45BA9C[5] = {0,-1,0,1,0}` (Y offsets) and `dword_45BAB0[] =
   {-1}` (ONE element). Reading `dword_45BAB0[0..4]` runs PAST it into the
   adjacent `.data` — an **out-of-bounds read**. **Reading the shipped BM95.EXE
   at VA 0x45BAB0..0x45BAC0 gives `{-1, 0, 0, 0, 1}`** for those five dwords (the
   bytes after `-1` are `0,0,0,1`; `dword_45BAC4 = 21356` follows). Paired with
   the Y table the 5 offsets are `(-1,0)(0,-1)(0,0)(0,1)(1,0)` = **a clean
   LEFT/UP/SELF/DOWN/RIGHT plus-cross** — the OOB "garbage" is coherent, never a
   fault. Ported verbatim (`kEnemyScanX[5]={-1,0,0,0,1}`,
   `kEnemyScanY[5]={0,-1,0,1,0}`). The Manhattan gate `(int)v3 + HIDWORD(v3) >= 3`
   is `abs(sub_42665C(a1[5])) + abs(sub_4266A3(a1[6])) >= 3` where `a1[5]/a1[6]`
   = the actor's `+20/+24` — a **stale 16.16 spawn/punch position snapshot** (set
   to `+28/+32` only at spawn/punch, and to the warp exit on warp-out; NOT during
   walking). So the gate is a near-constant TRUE per player (spawn-tile abs-sum
   ≥ 3 for all real starts). We do not track that snapshot; ported as
   `abs(tileX)+abs(tileY) >= 3` over the current tile (the determinable analog,
   equal at match start). See §3.4.
5. **[RESOLVED] getvalue(905) = unused reserved id.** No `sub_412135(905)`
   (getvalue) call exists anywhere in the decompile; `VALUELST.RES` has **no
   `905,<n>` line** at all (900,1 then jumps to 910,15). The only reference is
   the editor's VALUELST *writer* (`sub_4124A4(905)` at line 5056), which uses
   the label-STRING accessor `sub_4124A4` (returns `char*`) to round-trip the
   file's comments — not a gameplay read. So getvalue(905) is a reserved slot
   between 900 (personalities) and the 910-block; it feeds nothing.
6. **[NOTE, not a blocker]** the two scratch-alloc `rand()` draws (A/B) are
   almost certainly heap-debug residue; kept for exact RNG parity (§8).

Also updated inline from this pass: §1.1 (+2 = has-target flag, +4/+6 =
target tile X/Y, +8 = captured cost), §4.2 (+66 = elapsed phase), §3.4 (the
offset-table geometry + Manhattan gate), and §6 (905 confirmed unused).

## 11. Differential fidelity audit — 2026-07-10 (RNG-order verification against pseudo.c)

A line-by-line re-derivation of every behaviour's arithmetic and RNG draw
order/count, independent of this document's own prior claims, against a fresh
read of `pseudo.c` (the same standard as the movement core-feel audit,
facts.md "Core-feel audit 2026-07-10"). Five research passes each traced a
cluster of `sub_XXXX` functions end to end and cross-checked them against
`ai.cpp`/`ai.hpp` and this file. Verdict table:

| # | Area | Functions | Verdict |
|--:|------|-----------|---------|
| 1 | Dispatcher & personality | `sub_40A1C6`, `sub_40A140`, `off_45BA78`, `sub_41F29B`/`sub_420F07` | IDENTICAL (dispatcher, chain loop, table order, personality init, outer loop) + **DEVIATION-fixed** (caller-side stun gate, §7) |
| 2 | Danger grid, flee BFS, walk-the-path | `sub_424D37/DFE`, `sub_426D06`, `sub_42331C` (danger write), the closing-wall writer, `sub_40970B`, `sub_40A76E`, `sub_40B20F` | IDENTICAL, **doc-only fix** (§4 brick/powerup contradiction) + **DEVIATION-fixed** (walk-path veto return value, §3.2) |
| 3 | Directed BFS, powerup scan, seek-powerup, readers | `sub_4092A1`, `sub_409C1F`, `sub_40BAF5`, `sub_40A59D`, `sub_425FB9`, `sub_422E48`, `sub_42708D`, `sub_42542D`, `sub_409083` | IDENTICAL + **DEVIATION-fixed** (boxed-in `iters`, §5.1) + documented structural caveat (BFS is a faithful shortest-path rewrite, not the original's beam-flood — §5.1) |
| 4 | Bomb-drop behaviours | `sub_40AD8D`, `sub_40ABED`, `sub_40BD44`, `sub_4245DA`, `sub_423188`, `sub_405654` | IDENTICAL (blast-bricks, bomb-near-enemy, capacity gate, clearance predicate) + **DEVIATION-fixed** (grab-glove polarity + sliding-bomb exclusion, §3.0) + **doc-only fix** (+62 mislabelled "team", §1.1/§3.0) |
| 5 | Enemy targeting | `sub_422718`, `sub_421CB5`, `sub_40B8C2`, `sub_40BE02` | ~~IDENTICAL — no deviations found~~ → **DEVIATION-fixed** in a 2026-07-10 follow-up (§12): the target-liveness `q.stun` reads in `behave_bomb_enemy`/`pick_live_enemy`/`behave_seek_enemy` mirrored `+8` (`v7[2]`/`!i[2]`) but this pass mislabelled +8 "stunned" — it is the DEAD flag. The extra `stun` check wrongly skipped stunned-but-alive foes; removed |
| 6 | Team filter | `AISystem::same_team`, the enemy-scan/finder team gates | IDENTICAL — confirmed `slot != self` + nonzero-team-equality matches the `dword_464964`-gated `+84` compares; unaffected by this pass |

**No unlisted `rand()` call site was found anywhere in the AI's reachable
functions** — every draw in `sub_40A1C6` and all eight behaviours maps 1:1 to
a row in §8's table (one precision fix: b5b is `sub_409C1F` line 10128, not
"10??").

### The five code deviations (all fixed in `ai.cpp`/`simulation.cpp`, none touching golden)

1. **Walk-path veto fall-through (`behave_walk_path`, §3.2).** The original
   returns `vel_perp(+44)>>16 != -1` after calling the flame veto — i.e. it
   PASSES DOWN to behaviours 3-7 (with their draws) when the veto cancels the
   chosen step. The port hard-coded `return true` in both the directed and
   flee sub-branches, always stalling instead. This is the highest-value find
   of the pass: a real RNG-order/count divergence for any AI-bearing replay
   where a flee/directed first step lands on fresh flame (constructible: the
   directed BFS's obstacle grid does not include flame, §5.1/§9, so a chase
   can route straight through a burning tile). Fixed: `return g != -1;`.
2. **Grab-glove inverted polarity (`behave_grab_drop`, §3.0).** `sub_40BD44`
   grabs when `rand()%2` is **truthy** (`&& rand()%2`, pseudo.c 11025); the
   port checked `random_below(s_,2) == 0` — the opposite branch of the same
   coin flip. Draw count was unaffected (still exactly one draw when eligible)
   but the DECISION was inverted on every seed, diverging state, hash, and
   every downstream draw the moment an eligible AI actually stands on its own
   bomb. ai.md's own pseudocode block (§3.0) already had the polarity right;
   only the port's code and this file's prose commentary had drifted. Fixed:
   `random_below(s_, 2) != 0`.
3. **Grab-glove sliding-bomb exclusion (`behave_grab_drop`, §3.0) — a real
   draw-COUNT desync.** `sub_422E48` matches a RESTING **or SLIDING** bomb
   (motion `!= flying(2) && != carried(3)`); the port additionally required
   `!under->moving`, silently narrowing the underfoot test and skipping the
   whole `rand()%2` draw whenever the AI's own bomb happened to be sliding —
   a case the shared mover's `try_grab` already supports mid-slide
   (`bombs.cpp`: "motion states 0 AND 1 both qualify"). Fixed: dropped the
   `!under->moving` condition (folded into the same edit as #2).
4. **Boxed-in `iters` off-by-one (`directed_bfs`/`powerup_scan_bfs`, §5.1).**
   The original's `do…while` always completes one ring before testing its
   exit condition, so a start tile with zero open neighbours still reports
   `iters = 1`; the port's `while`-style loop never entered the ring-count
   path in that case and left `iters = 0`. Behaviours 5/6 gate their
   unreachable-target `rand()%2` give-up on `iters == 0`, so the port drew a
   spurious extra roll in the boxed-in state. Fixed:
   `if (out_iters == 0) out_iters = 1;` before each function's final
   `return -1;`. Rare in real play (needs a boxed-AND-dangerous start) and
   currently unobservable for `powerup_scan_bfs` specifically (applied for
   structural parity regardless).
5. **AI dispatch missing the stun gate (`simulation.cpp`, §7).** The
   original's per-player eligibility flag `v113` excludes a stunned actor
   (`+58 > 0`) from the ENTIRE AI dispatch, draws A/B included. The port
   called `AISystem::decide()` for any `present && alive && ai` player
   regardless of `Player::stun`, drawing spurious RNG on a tick the original
   skips outright (gameplay-invisible, since `player_turn` already no-ops a
   stunned player's turn, but not RNG-invisible). Fixed:
   `if (p.ai && p.stun == 0) ai.decide(i, in);`. The other two components of
   `v113` (entering/dying player-type modes, and the global freeze
   `dword_4621E0`) have no equivalent gap: dying/dead is already excluded by
   the loop's own `present && alive` guard, and a headless, externally-ticked
   sim has no menu/pause state to freeze against (pausing is simply "the
   caller stops calling `tick()`").

### Why none of this touches golden

Every fix above is reachable only through `AISystem`, which runs only for
`players[i].ai == true` (ADR-0005 §7). `tests/test_golden.cpp` sets no `ai`
player in any scenario, so `AISystem::decide()` is never called there — the
five fixes are proven inert on golden by construction, not just by
observation. Verified anyway: the full suite (`ctest --test-dir build/headless
-C Debug`) passed 37/37 both before and after these changes, `golden` and `ai`
included, with **zero constant recaptures needed** in `tests/test_golden.cpp`.
New regression coverage for all five fixes lives in `tests/test_ai.cpp`
("2026-07-10" test cases).

## 12. Follow-up correction — target-liveness `+8` is DEAD, not stunned (2026-07-10)

A sixth deviation, missed by §11's pass (which signed off "Enemy targeting —
IDENTICAL"): every enemy/target-liveness scan in `ai.cpp` read the target's
`Player::stun` **in addition to** `Player::alive`, on the belief that the
original's `+8` exclusion (`sub_421CB5` 24207 `!i[2]`; `sub_422718` 24741
`v7[2]` / 24728 `!v8[2]`; behaviour 6 liveness `v4[2]`) meant "skip stunned".
It does not — **`+8` is the player's "died this round" flag** (set by the
death-applier `sub_41DCB2`, never by the head-hit handler `sub_421F7E`, which
writes the stun countdown to a separate WORD at **`+58`**; full evidence in
facts.md "Head hit / Stun does NOT gate flame-death or pickup"). So `+8` is
exactly our `!alive`, and the extra `stun` test was a NEW deviation: the port
wrongly skipped a merely-**stunned-but-alive** enemy that the original would
still bomb / chase / pick as a target.

Fixed by removing the `stun` conditions (leaving the `present && alive` that
already mirrors `!+8`) in: `behave_bomb_enemy` (§3.4 cross scan),
`pick_live_enemy` (§5.3, both passes), and `behave_seek_enemy` (§3.6 liveness
reload). The §3.4/§3.6/§5.3/§9.4 prose above is corrected inline.

**NOT touched — the OWN-turn stun gate is correct.** `simulation.cpp`'s AI
dispatch gate (`if (p.ai && p.stun == 0) ai.decide(...)`, §7 / §11 fix #5) and
`player_turn`'s stun handling model **`+58`** (the head-hit stun that blocks a
player's own *new-input acquisition*), which IS real — left exactly as-is. Only
the OTHER-player target-liveness reads, which mirror `+8`, were the mislabel.

GOLDEN: none. `AISystem` runs only for `ai==true` players and no golden
scenario has one, so all 37 tests stay green with zero recaptures. Regression
coverage: `tests/test_ai.cpp` "Mislabel fix: an AI still bombs a stunned-but-
alive enemy (+8 not +58)" (the AI drops a bomb on an enemy kept stunned every
tick — impossible under the old `stun == 0` gate, which had no other drop path
in that room).
