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

## Provenance discipline

Every extracted constant goes into `facts.md` with its source address or asset
path before it is used in code, so any value in the sim can be traced back to
what it was derived from — or honestly marked as still a guess.
