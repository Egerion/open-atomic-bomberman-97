# Reverse-engineering method

How we extract facts from `BM95.EXE`, and the autonomous build/test loop the
sim iterations run in. Working scripts live outside the repo (they read the
copyrighted binary); this documents the approach so it is repeatable.

## Toolchain

- `capstone` (pip) for i386 disassembly from Python — the reliable option in a
  sandbox with short compute slices. A tiny PE loader (`pe.py`) maps sections
  and RVA↔file offsets; the code section is `BEGTEXT`.
- `objdump` / `nm` / `strings` for quick section, symbol, and string surveys.
- Ghidra headless is the right tool for full decompilation but needs a
  persistent machine (analysis runs for minutes) — run it locally, not in the
  sandbox, and treat its output as a facts source.

## Watcom register calling convention

Arguments arrive in **EAX, EDX, EBX, ECX** (in that order), not on the stack.
So a call like `getvalue(41)` compiles to `mov eax, 41 ; call getvalue`. This
is why naive "immediate pushed before call" scanning misses most call sites —
scan for `mov eax/edx/ebx, imm` before the `call` instead.

## Finding a function cold (the getvalue example)

1. Rank `call rel32` targets by how many distinct small-integer immediates they
   receive in EAX across the whole code section. The value getter floats to the
   top (many different ids flow into it).
2. Disassemble its prologue to confirm: a bounds check against a size global, a
   `shl eax, 2` + base-pointer index (a dword table), and a fatal not-found
   path pin it down.
3. Anchor other functions off strings (`strings -t x`), imports (DirectSound
   play for the explosion/death code), or known globals discovered in step 2.

## Autonomous build → test loop

Each sim change is validated without a human in the loop:

```
cmake --preset headless        # once
cmake --build build/headless    # or: make build
ctest --test-dir build/headless # determinism + sim rule tests
./abtool survey <game_dir>      # asset integrity
./bomber_game --demo N out.bmp  # headless screenshot for visual checks
```

The determinism test and the growing `test_sim.cpp` scenario suite are the
guard rails: any behavioral change that breaks a documented rule fails
immediately. Screenshots from `--demo` are diffed/inspected for anything
visual. New behavior lands as: extract fact → encode as `Tuning` field or sim
rule → add a scenario test asserting it → build → test → screenshot.

## `native/` — the citations you cannot open, and why

Many notes under `docs/re/` cite paths like `native/src/game/batch_0x41DAA7.cpp`,
`native/src/globals.cpp`, `native/tools/disasm.py` or `native/docs/M3_NOTES.md`.
**None of those files are in this repository, and a reader who clones it cannot
open a single one.** That is deliberate, and worth stating plainly rather than
leaving as a wall of broken links.

`native/` is a separate, local-only **1:1 transliteration** of BM95.EXE: the
decompiled original carried across into compilable C++ with its arithmetic,
branch structure and globals left exactly as they are, function by function,
plus the small tooling around it (a disassembler wrapper for byte-level
re-checks, and an oracle harness that runs it headless and prints a per-tick
digest). It is not a second port and nothing in `libs/` depends on it. It exists
for two jobs:

1. **A readable second opinion on the binary.** Reading a transliteration is
   faster and less error-prone than re-reading a decompiler window, which is why
   the audits under `docs/re/audit/` cite it so heavily.
2. **A behavioural oracle.** Running it against `tools/oracle_mirror` on the same
   seed and script and diffing the two digest streams points at the first tick
   where the clean-room sim gets a mechanic wrong. That is how several findings
   in `facts.md` were settled empirically instead of by argument.

**It is not published because it is derived from the decompilation**, and the
whole-repo rule (`.gitignore`, root `CLAUDE.md`) is that exe-derived material
never lands in the tree — the same rule that keeps `pseudo.c`, the IDA database,
disassembly listings and the original assets out. A transliteration is
decompiler output that has been retyped; publishing it would undo the policy
rather than satisfy it, so `native/` is gitignored alongside them.

**What that means for a citation.** A `native/...` path is a pointer to *where
the reasoning was done*, on par with a `pseudo.c` line number — not to evidence
this repository ships. The evidence a public reader can independently check is:

- **the `sub_XXXX` address**, which is a fact about the shipped binary and is
  stable in anyone's disassembler;
- **the prose description** of the behaviour, which is what these documents
  contain instead of the code;
- **the port** in `libs/`, which is here in full, and the tests that pin it.

So treat `native/...` as provenance, not as a link. If you have your own copy of
the binary you can go to the address and check the claim; if you do not, the
claim rests on the prose and the address, and should be read with the same
suspicion `docs/re/audit/README.md` asks for everywhere else.

## Provenance discipline

Every extracted constant goes into `facts.md` with its source address or asset
path before it is used in code, so any value in the sim can be traced back to
what it was derived from — or honestly marked as still a guess.
