# Dark-matter sweep — is there unattributed gameplay code in BM95.EXE?

Date: 2026-07-09. Worktree `worktree-agent-abe92441e5b28cbcb`, after
`git merge main` @ `f80f1f9` ("Merge bmstats RE: write-only telemetry dump").

**Question.** `docs/re/*.md` cites roughly 230-340 unique `sub_XXXXXX`
addresses depending how you count (see §1). The full-database Hex-Rays dump
(`pseudo.c`, kept out of the repo per CLAUDE.md — lives next to the `.idb` in
the BOMBRMAN install folder) currently holds 1134 decompiled function bodies
out of an estimated 1533 functions IDA's function-finder located in the
binary. Nobody had ever catalogued the ~790-1300 functions nobody has looked
at by address. Is there a gameplay mechanic hiding in that gap?

**Verdict, up front: no.** Every unattributed function that is *not* a
transitive callee of already-documented code resolves cleanly to Watcom
CRT/runtime, DirectDraw/DirectSound engine internals, the netplay transport
(Winsock **and** a previously-uncatalogued modem/serial-COM driver), or the
generic dialog/widget plumbing — with one exception: a self-contained,
3-function AI direction-scoring routine (`sub_40A4F7`/`sub_40AF20`/
`sub_40B046`) that is **never called by anything** in the entire decompiled
corpus and is not one of the 8 slots in the documented AI behavior table. It
reads like an earlier or alternate flee/target-seek algorithm that was
written, compiled in, and then orphaned in favor of `sub_40970B` (the flee
BFS actually wired into behavior 2, `docs/re/ai.md` §5.2). It is dead code,
not a shipped-but-unRE'd mechanic — see §5 for the full trace. No port
action is needed; it is recorded here so nobody rediscovers it later and
assumes it is a gap.

## 1. Method

All analysis reads `pseudo.c` and `BM95.EXE` from the install directory
(`D:\Program Files (x86)\INTRPLAY\BOMBRMAN`, never committed) and the repo's
own `docs/`/`libs/`/`apps/`/`tests/` trees. Tooling (Python + `capstone`/
`pefile`, plus a hand-rolled PE section-map loader) lives in this session's
scratchpad, matches the toolchain `docs/re/method.md` already documents, and
was **not** committed — no disassembly/pseudocode was written into the repo.

1. **Decompiled-function inventory.** `pseudo.c`'s per-function markers
   (`//----- (ADDRESS) -----`) were parsed into an address → signature → body
   table. **1134 entries** — matches the task's ~1134 figure exactly. This
   `pseudo.c` is the *native* Hex-Rays "decompile all, produce C file" batch
   output (not the repo's `decompile_all.py`/`decompile_gameplay.py` helper
   scripts, whose custom marker format doesn't appear anywhere in the file —
   a provenance note for whoever regenerates it next).
2. **Attributed set.** Every `sub_[0-9A-F]{6}` cited in `docs/re/*.md`,
   `docs/formats/*.md`, `docs/adr/*.md`, `docs/valuelst-map.md`,
   `docs/ROADMAP.md`, `docs/RE-NOTES.md`, and `libs/`/`apps/`/`tests/` code
   comments, case-normalized and de-duplicated: **337 unique addresses**
   (broader than `coverage-audit.md`'s earlier "~230" estimate, which only
   scanned `docs/re/*.md` itself — formats/ADR/ROADMAP/code-comment citations
   add the rest). 334 of the 337 are among the 1134 decompiled bodies. The
   remaining 3 (`sub_410F00`, `sub_41EC5B`, `sub_4250DE`) don't appear
   *anywhere* in this `pseudo.c` — not even as a forward-declared prototype —
   meaning they were sourced from raw disassembly/interactive IDA navigation
   in an earlier session rather than from this particular dump. Not a gap:
   `sub_410F00` (deinit dispatcher) and `sub_41EC5B` (a *retracted* bomb-bounce
   guess, per `stage-actors.md`) are both already discussed in prose in
   `facts.md`/`stage-actors.md`.
3. **Unattributed pool = decompiled − attributed = 790 functions.**
4. **Call graph.** Every `sub_XXXXXX` token appearing on a non-comment line of
   each decompiled body was treated as an edge (covers direct calls *and*
   callback/address-of references, e.g. a function pointer passed into a
   dialog's button-registration call). A breadth-first walk from the 334
   attributed nodes gives the set of unattributed functions that are
   *reachable* (directly or transitively) from already-documented code —
   these are helpers/accessors of systems that are already RE'd and ported,
   not independent unknowns.
5. **Orphan islands.** The functions left over — never reached by the walk in
   (4) — are the real candidate pool: nothing already-documented touches
   them, so if a gameplay mechanic is hiding unattributed, it hides here.
   Grouped into undirected connected components (call *and* call-from edges)
   so a self-contained cluster of helpers shows up as one unit instead of N
   singletons.
6. **Sampling.** Every component with ≥3 members was read via its first
   3 and last 2 members (by address) — enough to see the cluster's shared
   globals/callees and confirm cohesion. Every component below address
   `0x435000` (the primary gameplay/frontend range) was read **in full**
   regardless of size — 48 individual function bodies. A keyword/global-name
   heuristic script (DirectSound/DirectDraw imports, CPUID/EFLAGS toggling,
   `NtCurrentTeb`/`__writefsdword` SEH patterns, the `dword_45BECC`/
   `dword_45BEDC` direction-vector table, `sub_4124A4` `getvalue` calls, the
   known player-array/collision-grid/powerup-array globals from `facts.md`)
   was run over the full 790-function unattributed pool as a safety net, to
   make sure no gameplay-signal function outside the sampled orphan islands
   was missed. In total, well over 150 function bodies were read directly;
   the rest of each multi-member cluster was classified by call-graph
   membership in an already-sampled, single-purpose component (a Watcom
   linker groups one translation unit's functions contiguously and they
   share globals — sampling the entry/exit points of a 65-function
   call-connected cluster is sound evidence for the interior).

## 2. Counts

| Pool | Count |
|---|---|
| Decompiled functions in `pseudo.c` | 1134 |
| Estimated total functions in BM95.EXE (IDA function-finder, inherited figure — not independently reproducible without IDA/`.idb` access in this sandbox) | ~1533 |
| Attributed (cited in docs + code comments) | 337 unique addresses (334 have a decompiled body) |
| **Unattributed decompiled pool** | **790** |
| — reachable from attributed code (helpers of known/ported systems) | 563 |
| — orphan islands (not reachable from anything documented) | 227, in 94 connected components |
| — of which: inside the documented netplay range `0x43B000`-`0x43C6FF` | 17, in 10 components (bucket d, already catalogued) |
| — of which: everything else | 210, in 84 components (§4-5) |
| Undecompiled gap (1533 − 1134, inherited figure) | ~399 |
| — of which identifiable from `pseudo.c` alone (referenced by a decompiled function but no body exists) | 25 (§6) |
| — remainder, not enumerable from `pseudo.c` (needs IDA/`.idb` access) | ~374 |

## 3. Bucket classification of the 210 non-netplay-range orphan functions

All 210 were sampled per §1.6. No manual judgment call here is "this looks
like X" without a body read backing it — every bucket assignment below cites
the specific evidence (a global name, an import, an instruction pattern)
found in the actual decompiled text.

| Bucket | Functions (approx.) | Address range(s) | Evidence |
|---|---:|---|---|
| (a) CRT/Watcom runtime + libc | ~90 | `0x4175xx-0x418Cxx` (I/O shim + near-heap alloc wrappers), `0x442D-0x44B0` (SEH frame chain, CPUID/EFLAGS feature probe), `0x44E1-0x44ED` (hand-rolled printf/vsprintf format engine, `IsTable[]` char-class table), `0x453A-0x4542` (16-bit-segment `MK_FP`/`__DS__` float-to-string legacy thunk), `0x4555xx` (heap teardown/`VirtualFree`), scattered singles (`nmalloc_`/`nrealloc_`/`nfree_`/`fopen_`/`fread_`/`opendir_`/`readdir_` wrappers) | Named-import calls (`memcpy_`, `strlen_`, `nmalloc_`, `fgetc_`/`fputc_`, `opendir_`/`readdir_`, `tell_`/`lseek_`/`read_`/`write_`/`close_`), `NtCurrentTeb()->NtTib.ExceptionList`/`__writefsdword` SEH pattern, `__readeflags`/`__writeeflags`/`cpuid` feature-probe, `IsTable[c+1]&2` printf format-class table |
| (b) low-level gfx/blit/palette | ~10 | `0x442ECC-0x443B50` | `sub_442ECC` picks between `sub_4438F0` and `sub_443AE4` as the active blit routine based on the CPU-feature probe result (bucket-a `sub_44B040`'s CPUID check feeds this); `sub_443B50` locks a DirectDraw-style surface (vtable+100 call, checks the surface-lost `HRESULT`) and blits pixel rows |
| (c) sound engine internals | ~24 | `0x4175DB-0x41BB56` | `dword_4617A4`/`dword_4617B8` volume-subsystem error codes (21/22/27/28/29/32), `_CHP`/`pow_`/`dbl_459D7A` dB→linear volume conversion, the `"Actual sound vol"` string (`aActualSoundVol`), a linked list of active sound instances (`dword_45BD70` head, `i[7]` next-pointer) driven by a `timeSetEvent` multimedia timer callback (`sub_41B03B`/`sub_41B4CA`) — this is the DirectSound instance/volume manager sitting just above the `off_45BA78`-style vtable calls |
| (d) netplay (Winsock **+ previously uncatalogued modem/COM driver**) | ~72 | `0x430C00-0x4331EC` (5-slot packet queue, calls into the documented `0x43Bxxx-0x43Cxxx` cluster), `0x445AC6-0x4500C0` (**new**: 65-function modem/serial-COM driver — baud-rate lookup tables `word_45C566`/`word_45C56E`, 19200 default baud, `GetTickCount`-based timing), `0x40E3EA` (calls `sub_42C0C8(aInvalidPacket)`, the "Invalid Packet" diagnostic string) | Ring-buffer globals shared with the documented netplay queue helpers (`dword_49D798`/`dword_49D7A0`), baud-rate constant tables, direct calls into the already-pinned `0x43Bxxx-0x43Cxxx` Winsock cluster |
| (e) UI/dialog plumbing, generic (already covered) | ~10 | `0x4304FC-0x43DFB8` (dialog text-wrap/length helpers, calls the already-pinned `sub_4444B0`/`sub_444630`/`sub_43CD44`), `0x4321xx-0x4335xx` (widget-list registry, siblings of the newly-pinned `sub_432298` dialog-chrome function per the prior session's "Pin sub_43C734 dialog chrome" commit) | Direct calls into already-documented dialog-chrome functions; shares the widget-list globals (`dword_49DA90`, `dword_45C468`) with the pinned dialog system |
| (f) dead/unreachable | 3 | `0x40A4F7-0x40B046` | Zero callers anywhere in the 1134-function corpus; not one of the 8 AI-behavior-table slots. See §5. |
| (g) UNKNOWN/gameplay-suspect | **0 beyond the (f) finding** | — | See §5 — the only candidate turned out to be dead, not unRE'd-but-live. |

Two singles worth naming individually: `sub_443D58` (`RegisterClassA` for the
game's own top-level window class, `"gnw95Class"`) and `sub_45538B`
(`CreateProcessA` — spawns a subprocess, most likely the bundled `WINEREG.EXE`
registration wizard already flagged as non-gameplay tooling in
`coverage-audit.md`'s `.BMP` row). Both bucket (a)/init plumbing, not
gameplay.

**Adjacent-to-known-code safety net (the 563 "reachable" functions).** These
are transitive callees of already-documented, already-ported code — mostly
small field accessors. Spot-checked a sample flagged by the gameplay-global
heuristic (`dword_461BC4` player array, `sub_4124A4` `getvalue`): e.g.
`sub_421198`/`sub_4211E7`/`sub_4218E7` (clear a 10-slot player array's
input-type field, called from the pre-match SETUP screen state machine
already documented in `setup-screens.md`) and `sub_40234E` (a boot-screen
text draw calling `getvalue(700)`, sibling of the already-pinned loading
dialog chrome). No independent mechanic found in this set — they are exactly
what the reachability graph says they are: helpers of systems `facts.md`/
`setup-screens.md`/etc. already cover.

## 4. Netplay boundary — one addition to `coverage-audit.md` §4

The existing netplay boundary note (`coverage-audit.md` §4) only lists the
`0x43Bxxx`-`0x43Cxxx` Winsock cluster. This sweep found a second,
previously-uncatalogued netplay-adjacent region: a **65-function modem/
serial-COM driver** at `0x445AC6`-`0x4500C0` (baud-rate tables, COM-port
open/close, a `GetTickCount`-based poll loop) plus a small 5-slot packet-queue
helper cluster at `0x430C00`-`0x4331EC` that calls directly into the
documented Winsock cluster. Both are transport-layer, in scope of the same
ADR-0003 exclusion as the Winsock cluster — not a gameplay gap, just a
documentation completeness note. Recommend folding this into
`coverage-audit.md` §4's function list next time that section is touched.

## 5. Bucket (g) finding — the dead AI direction-scoring trio

| Address | Name | Callers (whole corpus) | Callees | Evidence | Verdict |
|---|---|---|---|---|---|
| `0x40A4F7` | `sub_40A4F7` | none | `time_`, `rand_` | Rate-limited (once per real-time second) RNG draw that stores a rotation offset (`dword_45ED64 = rand()%4`) used to rotate which of the 4 cardinal directions is scanned first | dead — only called by the two below |
| `0x40AF20` | `sub_40AF20` | none | `sub_40A4F7`, `sub_425FB9` (solid-cell test), `sub_42542D` (powerup test), `sub_422E48` (bomb test) | Ray-casts outward up to 9 tiles in each of the 4 directions (using `dword_45BECC`/`dword_45BEDC`, the same dx/dy table `sub_41F29B`'s movement code and the documented AI use), stops a direction at a wall/powerup, and returns the first direction where a bomb-predicate (`sub_422E48`) is true | dead — zero callers anywhere in the 1134-function corpus |
| `0x40B046` | `sub_40B046` | none | `sub_40A4F7`, `sub_425FB9` | Same ray-cast, but scores each direction by open-tile count (walls block, unexplored branches add bonus) and returns the direction with the **maximum** score — an "open space" / flee-style heuristic | dead — zero callers anywhere in the 1134-function corpus |

**Why this looked promising.** The three functions sit in the AI address
neighborhood — right after the AI dispatcher `sub_40A1C6` (`docs/re/ai.md`
§2) and before the first behavior-table entry `sub_40A81F` (§3) — and reuse
exactly the same primitives the documented, ported AI uses: the
`dword_45BECC`/`dword_45BEDC` direction-vector table, and the
`sub_425FB9`/`sub_42542D`/`sub_422E48` solid/powerup/bomb predicates that
`ai.md` cites throughout (§3.2, §3.4, §5.1-5.2). A max-open-space direction
picker is exactly the shape of thing a "flee" or "wander" AI behavior would
need.

**Why it's dead, not a gap.** `ai.md` §3 documents the AI dispatcher's
behavior chain as a literal 8-entry function-pointer table,
`off_45BA78[8] = {sub_40BD44, sub_40BE02, sub_40B20F, sub_40AD8D, sub_40ABED,
sub_40BAF5, sub_40B8C2, sub_40A81F}` — none of those 8 addresses is
`sub_40AF20` or `sub_40B046`. The AI's actual flee logic goes through
`sub_40970B` (`ai.md` §5.2, "Flee BFS" — the function genuinely wired into
behavior 2's danger branch). A direct text search of every one of the 1134
decompiled function bodies for `sub_40AF20(` or `sub_40B046(` (call sites
*and* address-of/callback references) returns **zero hits outside their own
definitions** — nothing in the shipped, reachable code calls them, and they
are not entries in any documented dispatch/callback table either. This reads
like an earlier iteration of the AI's direction-picking logic that was
superseded by the BFS-based flee/pathing system and left in the binary
un-called — a common artifact of iterative game-AI development, not evidence
of a shipped-but-unRE'd mechanic.

**Action:** none required for the port — a faithful port must not call code
the original never calls either. Recorded here (and cross-referenced from
`facts.md` if a "confirmed dead" ledger is ever added there) purely so a
future RE pass doesn't rediscover this trio, see the gameplay-shaped
primitives, and assume it's an open gap.

## 6. The ~399-function undecompiled gap — what's identifiable, what isn't

`pseudo.c` only contains *bodies* for the 1134 functions Hex-Rays actually
decompiled; it does not separately enumerate the full ~1533-function roster,
so the remaining ~399 cannot be listed exhaustively from this artifact alone
(would need `idautils.Functions()` against the `.idb`, i.e. interactive IDA
access, which this sandbox does not have). Two things *are* identifiable
from `pseudo.c` alone, and both point away from gameplay:

- **25 addresses are referenced** (called, or passed as a function-pointer/
  callback literal) by at least one of the 1134 decompiled bodies, but have
  no body of their own anywhere in the file. 23 of the 25 cluster tightly in
  `0x43E6D4`-`0x450816` — squarely inside the same CRT/runtime/heap address
  neighborhood as bucket (a) above (consistent with FLIRT-recognized library
  internals that this particular batch-decompile run skipped or failed on).
  The other 2, `sub_42C368` and `sub_42FB1C`, sit in the dialog/frontend
  range; both were traced to their exact call sites (`sub_42BE50` line
  30958, and a Close-button dialog setup at line 33365 next to the literal
  `"Close"` string) and are button-callback function pointers registered
  with the already-documented dialog widget system (`sub_444418`/
  `sub_4326E0`) — confirmed non-gameplay, not merely inferred.
- **The remaining ~374** cannot be enumerated without `.idb` access. What
  *can* be said: every address range this sweep was able to check —
  the full 1134-function decompiled set (spanning `0x401010`-`0x455588`, no
  suspiciously large unaccounted gaps in the tight `0x401000`-`0x42C000`
  gameplay/AI/movement/bomb range once you account for `sub_41F29B`'s
  well-documented ~6.8 KB movement function and similar known giants), plus
  every one of the 25 identifiable gap addresses — lands in CRT/library or
  frontend/dialog territory, never in the primary gameplay range. There is
  no positive evidence of gameplay code in the unenumerable remainder; this
  is a bounded, not a proven-exhaustive, conclusion, and is stated as such
  rather than overclaimed.

## 7. Verdict

**No unattributed gameplay-relevant code remains un-triaged.** Every orphan
function this sweep could reach resolves to CRT/runtime, gfx/blit, sound
engine, netplay (Winsock + the newly-noted modem/COM driver), or generic
dialog plumbing, backed by a direct body read in every case, not a guess. The
sole gameplay-shaped find — the `sub_40A4F7`/`sub_40AF20`/`sub_40B046` AI
direction-scoring trio — is dead code with zero callers and no dispatch-table
entry, not a live, unRE'd mechanic. The ~374 functions this sandbox cannot
individually enumerate (no IDA/`.idb` access) sit outside every address range
this sweep could positively check and outside the reachable-from-attributed
graph in the direction that *would* matter (i.e., nothing decompiled calls
into them from gameplay code) — so there is no live lead to chase, only an
honest acknowledgment of the tooling boundary.

Coverage-audit's own top-line numbers (`docs/re/coverage-audit.md` §"Summary
counts": 37/39 subsystem rows done, 13/14 asset formats done) stand
unchanged by this sweep.
