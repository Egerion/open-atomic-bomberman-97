# Fidelity audit — `ai.cpp` (system 13, W4, light re-verify)

**Verdict: 2 NEW findings beyond the two prior pseudo.c-only audits (ai.md
§11 2026-07-10, and the 2026-07-16 movement audit) — both only visible once
diffed against the SOURCE-LEVEL native transliteration and confirmed byte-
for-byte against the shipped binary via `native/tools/disasm.py`. Everything
else the two prior audits fixed is re-verified intact. Highest severity:
Finding 1 (behaviour 4's "Manhattan gate" is a spawn/punch-distance-travelled
check, not the "near-constant TRUE" absolute-coordinate sum ai.md §3.4/§9.4
documented and the port implements) — HIGH.**

Scope: `libs/sim/src/systems/ai.{hpp,cpp}`, `libs/sim/include/bomber/sim/brain.hpp`
only. The danger-map WRITERS (bomb/flame/enclosure stamping in
`sub_42331C`/`sub_426D06`/the hurry updater) belong to `bombs.cpp`/`flames.cpp`/
`enclosure.cpp` (ledger items 2/3/7) and were spot-checked only where the AI
reads their output; a full writer-side re-audit is out of scope here (already
covered once by the 2026-07-12 "AI danger map" pass, `docs/re/facts.md`).
Campaign rovers/ghosts (`sub_401B5C`, `dead-code` `sub_40AF20`/`sub_40B046`,
`sub_40BEE7` stats history) are out of scope — versus AI only.

References read in full: `native/src/game/batch_0x40A140.cpp` (personality
init `sub_40A140`, dispatcher `sub_40A1C6`, all eight behaviours, the
flame-safety veto `sub_40A76E`, the safe-tile predicate `sub_40A59D`, wander
base-dir re-roll `sub_40A4F7`), `native/src/game/batch_0x40902A.cpp` (danger/
obstacle grid primitives `sub_40902A`/`sub_409083`, walkability cache
`sub_4090E7`, BFS scratch-pool helpers, and the three BFS pathfinders
`sub_4092A1`/`sub_40970B`/`sub_409C1F`). Cross-checked against the shipped
binary with `python native/tools/disasm.py 0xADDR` for every finding below
(and for one claim that turned out to be a false alarm, see "Re-verified"
§3). `docs/re/ai.md` (whole document, all sections) and `docs/re/facts.md`
"AI danger map" (2026-07-12) read for prior-audit context.

---

## Finding 1 — Behaviour 4's "Manhattan gate" is a spawn/punch-DISTANCE-TRAVELLED check, not an absolute-coordinate sum; the port's substitute inverts its usual truth value at match start

**Original**: `sub_40ABED` (bomb-near-enemy), `native/src/game/batch_0x40A140.cpp`
lines 297-354, specifically the HEXRAYS-FIX at lines 316-329:

```c
// HEXRAYS-FIX: original is 'sub_42665C(a1[5]); abs_(); sub_4266A3(a1[6]);
// v3 = abs_();' with v3 declared __int64 and tested via '(int)v3 +
// HIDWORD(v3) >= 3' — a chained-register-argument artifact. CORRECTED
// against disasm 0x40AC24..0x40AC69 (fuller than ai.md sec 3.4's summary,
// which omits the subtraction): each operand is a DIFFERENCE, not a bare
// coordinate — "sub edx,eax" runs before each abs_() call. The real gate is
// abs((dword_45ED70+46>>16) - sub_42665C(a1[5]))
//   + abs((dword_45ED70+48>>16) - sub_4266A3(a1[6])) >= 3,
v3 = abs_((*(int *)(dword_45ED70 + 46) >> 16) - sub_42665C(a1[5]))
   + abs_((*(int *)(dword_45ED70 + 48) >> 16) - sub_4266A3(a1[6]));
if ( v3 >= 3 ) { /* proceed to the enemy-scan cross */ }
```

`dword_45ED70+46/+48` is the brain's freshly-refreshed **current** tile
(written every dispatch, `sub_40A1C6`); `a1[5]/a1[6]` are the actor's
**`+20/+24`** — a pixel-space snapshot written ONLY at spawn and at
punch-restart (`docs/re/ai.md` §3.4/§9.4, unchanged by this audit). So the
gate is **Manhattan distance the AI has walked since its last spawn or
punch-restart >= 3 tiles** — not "how far the spawn tile itself is from the
map origin".

**Byte-confirmed** via `python native/tools/disasm.py 0x40ABED`:

```
40AC24  mov eax,[g_45ED70]        ; brain
40AC29  mov edx,[eax+0x2E]        ; brain+46 (pos_x dword)
40AC2C  sar edx,0x10              ; -> current tile X
40AC2F  mov eax,[ebp-0x1c]        ; actor (a1)
40AC32  mov eax,[eax+0x14]        ; actor+0x14 = actor+20 (spawn/punch snapshot X, pixels)
40AC35  call sub_42665C           ; pixel -> tile
40AC3A  sub edx,eax               ; edx = currentTileX - snapshotTileX
40AC3E  call abs_
... (mirror for Y: brain+48/actor+24/sub_4266A3) ...
40AC64  add edx,eax               ; sum of abs diffs
40AC69  cmp dword [ebp-8], 3
40AC6D  jl  0x40ad7a              ; sum < 3 -> behaviour 4 passes (return 0)
```

This is an explicit **`sub`** before each `abs_()` call — a true difference,
never a bare magnitude. `docs/re/ai.md` §3.4/§9.4 ("[RESOLVED]") describes
only `abs(spawnTileX) + abs(spawnTileY) >= 3` (the snapshot's own magnitude,
no subtraction), calls it "a near-constant TRUE per player" and says the
port's `abs(currentTileX) + abs(currentTileY) >= 3` is "the faithful
determinable analog... same near-always-true result" — this is the exact
kind of pseudo.c-only miss the native transliteration exists to catch, and
its own comment says so explicitly ("fuller than ai.md sec 3.4's summary,
which omits the subtraction").

**Port**: `libs/sim/src/systems/ai.cpp` lines 932-937 (`AISystem::behave_bomb_enemy`):

```cpp
// (2) Manhattan gate: abs(tileX) + abs(tileY) >= 3 over the actor's snapped
// position. See the header note — we use the current tile as the faithful
// analog of the stale +20/+24 snapshot (equal at spawn, near-always true).
const int mx = px < 0 ? -px : px;
const int my = py < 0 ? -py : py;
if (mx + my < 3) return false;
```

**Visible effect**: the original's gate is **near-constant FALSE right after
a spawn or a punch** (current tile == snapshot tile at that instant, diff =
0 < 3) and only becomes true once the AI has net-displaced >= 3 tiles from
that reset point — i.e. a freshly-spawned or just-punched AI will NOT
consider bombing a cornered enemy via behaviour 4 until it has wandered/
chased/fled far enough away. The port's substitute (`abs(currentTileX) +
abs(currentTileY) >= 3`, using the absolute map-coordinate magnitude of the
CURRENT tile, not a distance-travelled) is true almost everywhere on every
real board and totally insensitive to spawn/punch timing — so the port's
AIs are willing to drop next to an enemy immediately after spawning or being
punched, in a scenario where the original's AIs are gated off. This changes
WHEN behaviour 4 is even considered (and hence its `rand()%5` draw, an
RNG-order-relevant gate) for a meaningful, common window of play (the first
few tiles after every spawn/respawn and after every punch).

**Severity**: HIGH — behaviour 4 (bomb-near-enemy) is a mid-priority,
frequently-relevant behaviour, and this changes its activation window for
every AI player after every spawn and every punch-restart, in the opposite
direction from what both the doc and the port assumed ("near-always true"
vs "near-always false-then-true").

**Confidence**: HIGH — confirmed at the disassembly-byte level (not just
pseudo.c), matching the transliteration's own self-correcting comment.

**Suggested fix**: add a per-player "AI gate reset tile" snapshot (mirrors
the original's `+20/+24`): set to the player's current tile at spawn/respawn
and at punch-restart (the same two moments the original's `+20/+24` are
written, `docs/re/ai.md` §3.4), then gate `behave_bomb_enemy` on
`abs(current_tile.x - snapshot.x) + abs(current_tile.y - snapshot.y) >= 3`
instead of the current absolute-magnitude test. This needs a new hashed
`Player` field (or reuse of an existing one if `bombs.cpp`'s punch-init path
already snapshots something equivalent — not found in this audit's scope) —
zero golden impact by construction (no AI players in golden, `ai.md` §11),
but grows the hash layout by one field per player, so `tests/test_golden.cpp`
would need its one-time constant recapture noted the same way the `team`
field's did.

---

## Finding 2 — `behave_walk_path`'s "can't improve, stand" branch writes `path_target_cost`, but the original leaves it untouched (stale)

**Original**: `sub_40B20F`, `native/src/game/batch_0x40A140.cpp` lines 607-615
(the flee sub-branch's "no strictly-safer tile" case):

```c
v1 = sub_424D37(*(int *)(dword_45ED70 + 46) >> 16, *(int *)(dword_45ED70 + 48) >> 16);
if ( v1 <= sub_424D37(v10, v11) )
{
  *(_WORD *)(dword_45ED70 + 4) = *(_WORD *)(dword_45ED70 + 48);   // +4 (target X) = own tile X
  *(_WORD *)(dword_45ED70 + 6) = *(_WORD *)(dword_45ED70 + 50);   // +6 (target Y) = own tile Y
  *(_WORD *)(v4 + 46) = -1;                                       // godir = -1 (stand)
  return 1;                                                       // ACT, chain stops
}
```

Note there is **no write to `+8`** (`path_target_cost`) here — only `+4`/`+6`
(the target tile) and the actor's godir. `docs/re/ai.md`'s own §3.2
pseudocode box already transcribes it this way (`brain.target(+4/+6)=pos;
player.godir(+46) = -1; return 1` — no `brain[+8]=...` line), so this is not
a case the pseudo.c-only audits could have missed from the doc alone — it's
that the port's code silently added a write the doc never asked for.

**Byte-confirmed** via `python native/tools/disasm.py 0x40B20F` (0x40B3EC-0x40B422):
the branch writes actor+46 ("brain +4"/+6" via `mov word ptr [eax+4],dx` /
`[edx+6],ax`) and the caller's godir (`mov word ptr [eax+0x2e], 0xffff`),
then jumps **directly** to the function epilogue (`jmp 0x40b588`) — no
`sub_40A76E` veto call, and no instruction anywhere in the branch touches
byte offset 8.

**Port**: `libs/sim/src/systems/ai.cpp` lines 667-675 (`AISystem::behave_walk_path`):

```cpp
const std::int32_t here = danger_at(px, py);
if (here <= danger_at(bx, by)) {
    br.has_path_target = true;
    br.path_target_x = static_cast<std::int16_t>(px);
    br.path_target_y = static_cast<std::int16_t>(py);
    br.path_target_cost = here;  // nonzero: the stale-target drop above ignores it
    write_move(out, -1);
    return true;
}
```

**Visible effect**: the top of the danger branch invalidates a held target
when `has_path_target && path_target_cost == 0 && danger_at(target) != 0`
(mirroring `dword_45ED70+8`'s only read site). The original leaves `+8`
**stale** here — whatever it last held, from either a fresh zero-initialised
brain (0) or the most recent *successful* flee-improvement write elsewhere in
this same function (`*(_WORD*)(dword_45ED70+8) = sub_424D37(v10,v11)`, which
can itself be 0 if that improvement reached full safety). A brain that has
never yet taken a "found something less-dangerous-but-still->0" step, or
whose last such step reached danger 0, has stale `+8 == 0`: on the VERY NEXT
decide, the invalidation check at the top of the danger branch fires
(`!+8` true) and drops the target — sending the original back through the
FLEE sub-branch again (a **second, fresh `sub_40970B` call and its own
tie-break RNG draw**) rather than the DIRECTED sub-branch. The port's
`path_target_cost = here` is always nonzero on this path (we're inside the
`danger_at(px,py)!=0` branch), so the invalidation check can **never** fire
for a target latched here — the port instead falls through to the
DIRECTED branch next tick, calls `directed_bfs(px,py,px,py,...)` (start ==
goal), draws ITS tie-break, and returns -1 (`has_path_target=false; return
false;`, passing down to 3-7). Both paths eventually "unstick" the AI, but
they diverge in **which BFS runs (flee vs directed-to-self), how many
extra RNG draws happen and in what order, and whether behaviours 3-7 get a
turn one tick sooner or later** — an RNG-order-relevant difference in the
"trapped in persistent, non-improvable danger" edge case (tight corners
during hurry-mode, or fully surrounded by bomb blast radii with no
strictly-safer neighbour).

**Severity**: MEDIUM — a narrower edge case than Finding 1 (requires the AI
to be standing in danger with no strictly-safer neighbour reachable), but it
sits in the single most load-bearing behaviour (§3.2, "the workhorse") and
is RNG-order-relevant, so it can desync an AI-bearing replay/oracle-diff the
moment this state is reached.

**Confidence**: HIGH — confirmed at the disassembly-byte level; the write
that would falsify this (a store to brain+8) simply does not exist in the
branch's instruction range.

**Suggested fix**: drop the `br.path_target_cost = here;` line in this
branch (leave `path_target_cost` at whatever it already held), matching
`docs/re/ai.md` §3.2's own pseudocode transcription and the disasm. Add a
regression test that latches a target via this exact branch, ticks again
with the same danger persisting, and asserts the resulting BFS-call
sequence (flee vs directed-to-self) matches a `path_target_cost` that was 0
going in.

---

## Re-verified faithful (prior fixes intact + one false alarm resolved)

All five §11 (2026-07-10) code deviations, the §12 (2026-07-10 follow-up)
dead-vs-stunned mislabel fix, and the 2026-07-16 movement-audit's two fixes
were re-checked directly against the native transliteration / disasm and
are **present and correct**:

1. **Walk-path veto fall-through** (`return g != -1;`, ai.cpp 645/690):
   confirmed the original's `return *(int*)(v4+44)>>16 != -1` really is a
   read of the packed dword whose HIGH WORD aliases the very godir word
   (`v4+46`) the veto (`sub_40A76E`) may have just set to -1 — byte-exact
   equivalent of the port's `g != -1`. (Same packing trick as the brain's
   `+2/+4` flag/target-X pack, §9.1.)
2. **Grab-glove polarity** (`random_below(s_,2) != 0`, ai.cpp 592): matches
   `sub_40BD44`'s `v3 && *(v3+62)==*(a1+62) && rand_()%2` (truthy grabs)
   exactly, `native/src/game/batch_0x40A140.cpp` lines 803-809.
3. **Grab-glove sliding-bomb exclusion**: confirmed no `!under->moving`
   guard remains in `behave_grab_drop` (ai.cpp 585-596).
4. **Boxed-in `iters` off-by-one** (`if (out_iters==0) out_iters=1;`):
   independently re-derived from `sub_4092A1`'s scratch-pool scan
   (`native/src/game/batch_0x40902A.cpp` lines 234-320) — an all-neighbours-
   blocked start seeds nothing, the do-while's frontier scan finds zero
   active entries, `v29` stays 0, and `++v32` still executes once before the
   `while(v29)` test exits — `iters` becomes 1, never 0, confirming the
   port's fix is exactly right.
5. **AI dispatch stun/freeze gate**: `simulation.cpp` line 398
   (`if (ai_sys && !sub_stunned && !frozen)`) still gates `ai_sys->decide`
   correctly; both halves of the original's `v113 && !dword_4621E0` remain
   modelled.
6. **§12 dead-vs-stunned target liveness**: re-read `behave_bomb_enemy`
   (ai.cpp 949), `pick_live_enemy` (ai.cpp 1005-1030), and
   `behave_seek_enemy` (ai.cpp 1096) — all three test `present && alive`
   only, no `Player::stun` check, matching `sub_421CB5`/`sub_422718`'s
   `+0`/`+8` (active/not-dead) semantics with no `+58` (stun) involvement.
7. **2026-07-16 BFS seed order (fixed godir 0..3)**: confirmed in all three
   pathfinders (`directed_bfs`, `flee_bfs`, `powerup_scan_bfs`, ai.cpp) the
   seed loop is `for (int g = 0; g < 4; ++g)`, matching `sub_4092A1`/
   `sub_40970B`/`sub_409C1F`'s identical `for (i = 0; i < 4; ++i)` seed loops
   (`native/src/game/batch_0x40902A.cpp` lines 214/398/623) — the ±1 tie draw
   only steers the later `(camedir + v35*j)&3` child-spawn order in the
   do-while, never the seed order. Confirmed structurally faithful (still a
   documented simplification vs the original's beam-flood shape, not a
   determinism bug — see below).
8. **2026-07-16 flee boxed-in-vs-no-improvement split**: `behave_walk_path`'s
   `if (first < 0) { br.has_path_target=false; return false; }` (fully boxed
   in, passes down) vs the `here <= danger_at(bx,by)` "stand" branch (chain
   stops) is structurally correct per `sub_40B20F` 601-615 — this is the
   SAME branch Finding 2 lives inside; the split itself (which branch fires)
   is faithful, only the extra `path_target_cost` write inside the "stand"
   arm is new.

**One suspected-then-retracted finding, noted for future auditors.**
`sub_40A81F`'s (wander) new-turn base, `native/src/game/batch_0x40A140.cpp`
line 275, reads `BYTE2(*(_DWORD *)(dword_45ED70 + 62))`. Read literally (a
single byte from brain offset 62+2=64's LOW byte) this looks like a
different, seemingly-dead field from `wander_dir` (+64, a WORD) — and a grep
of the entire `native/src/game/` corpus for `dword_45ED70` shows brain+62 is
**never written** anywhere (only zeroed once by `sub_40A140`'s match-start
memset), which would make the "BYTE2" term a compile-time constant 0.
Disassembling `0x40A81F` directly resolves this: the actual instruction is
`mov edx, [brain+0x3E]; sar edx, 0x10` (0x40A862-0x40A86B) — a 16-bit
**HIWORD** extraction of the dword at brain+62, not an 8-bit BYTE2. Since a
dword's high word starting at +62 covers bytes 64-65, this HIWORD IS
`*(_WORD*)(brain+64)` — i.e. `wander_dir` itself. The transliteration's
`BYTE2(...)` (inherited verbatim from Hex-Rays' pseudo.c rendering) is a
decompiler mislabel; the actual original computes `v2 = (wander_dir +
2*(rand()%2) - 1) & 3`, exactly what `behave_wander` (ai.cpp line 1136)
already does. **No port bug — the port is correct; the transliteration's own
comment at that one line is misleading and worth a future correction there**
(out of scope to fix here — this audit changes no code, and the note is
about `native/`, not `libs/`).

## Coverage

Read in full and diffed: dispatcher (`sub_40A1C6`, incl. draws A/B, the
personality/offscreen fatal guards, the behaviour-chain loop and its
one-arg-`@<eax>`-callee HEXRAYS-FIX), personality init (`sub_40A140`), all
eight behaviours (`sub_40BD44`/`sub_40BE02`/`sub_40B20F`/`sub_40AD8D`/
`sub_40ABED`/`sub_40BAF5`/`sub_40B8C2`/`sub_40A81F`), the flame veto
(`sub_40A76E`), the safe-tile predicate (`sub_40A59D`), the three BFS
pathfinders (`sub_4092A1`/`sub_40970B`/`sub_409C1F`) including their shared
scratch-pool allocator (`sub_4091A0`/`sub_4091C9`/`sub_409231`/`sub_409264`),
the obstacle/walkability grid (`sub_409083`/`sub_40902A`/`sub_4090E7`), and
the RNG draw order/count for every behaviour and every BFS call. Disasm
cross-checked (not just pseudo.c) for: the dispatcher's chain-call, behaviour
4's Manhattan gate, behaviour 2's "can't improve" branch, and behaviour 7's
wander-turn base. Not re-diffed here (covered by other ledger systems or
already exhaustively pinned by facts.md's own dedicated audits, cited
in-line): the danger-map WRITERS (bombs.cpp/flames.cpp/enclosure.cpp), the
enemy-finder/player-at-tile/drop-clearance/bomb-capacity helpers
(`sub_422718`/`sub_421CB5`/`sub_423188`/`sub_4245DA` — their own home
batches were not re-read end-to-end this pass, only their call-site
contracts as exercised from `batch_0x40A140.cpp`), and the input-flag bridge
on the mover side (`sub_41F29B`, already the subject of its own facts.md/
ai.md §7 audits and the "Round-start input freeze"/ice-buffer passes).
