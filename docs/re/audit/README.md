# How to read the fidelity audits

Each file here is one pass over one system: what the original does, what the
port does, and where the two differ. They are useful — several real bugs were
found this way and nowhere else. But they are **summaries written at a moment
in time**, and both the port and the RE understanding keep moving.

## A claim in these documents is evidence, not a verdict

Treat every statement here — and in `docs/re/facts.md` — as a pointer to the
thing worth re-reading, never as a substitute for reading it:

- **A claim about the PORT** ("the port writes X", "field Y is not hashed",
  "the gate is inverted") must be re-checked against the code as it stands
  today. Someone may have fixed it without updating the note. This has
  happened: facts.md carried a "`Player::prev_action1/2` are not mixed into
  `state_hash()`" follow-up long after `libs/sim/src/hash.cpp:174` started
  mixing them.
- **A claim about the ORIGINAL** ("sub_XXXX does Z") must be re-checked
  against the binary before anything depends on it. This has happened too, in
  both directions: `sub_405654`'s type test was documented as an empty
  campaign list and is actually the warphole rejection (`ai.md` §3.3), and a
  behaviour-4 gate documented as "near-always true" turned out to be a
  distance-travelled check that is false right after every spawn
  (`audit/ai.md` finding 1).
- **A helper's call-site contract is not a reading of the helper.** If the
  contract itself came from the document under audit, the audit inherits the
  document's error. `audit/ai.md`'s own coverage section records exactly that
  costing a real finding.

Over two days in July 2026, four claims in these summarising documents flipped
when someone re-read the source they summarised: two were wrong in the
alarming direction (a bug that was real and worse than described), two in the
reassuring direction (a "gap" that had already been closed). The base rate of
staleness is high enough that "the doc says so" should never end an argument.

## What is trustworthy here

- **Addresses and offsets** (`sub_XXXX`, `dword_XXXXXX`, `+68`) — these are
  facts about the shipped binary and do not rot.
- **Citations** (`pseudo.c` line numbers, disassembly addresses) — they say
  where to look, which is the point.
- **The reasoning**, when you follow it back to the binary yourself.

## `native/...` citations are provenance, not links

Most files here cite paths under `native/` — a local-only 1:1 transliteration of
the binary, plus its disassembly wrapper and oracle harness. **It is gitignored
and is not published**, because it is derived from the decompilation and falls
under the same rule as `pseudo.c` and the original assets. So those paths will
not resolve for anyone reading this repository, and that is on purpose, not rot.
They record where the reading was done; the `sub_XXXX` address next to them is
the part you can check yourself. `docs/re/method.md` has the full explanation of
what `native/` is and why it stays out of the tree.

## What these documents deliberately do NOT contain

No decompiler output and no disassembly listings: the behaviour is described
in prose here, with the address kept so you can go and look at it in your own
tools. That is the same rule the root `CLAUDE.md` states for the whole repo —
exe-derived material stays out of the tree.
