# VALUELST format (`DATA/RES/VALUELST.RES`)

The game's gameplay-tuning value table: a single file mapping small
integer **ids** to integer values (speeds, frame counts, probabilities,
layout coordinates). On-disk it is just one instance of the commented
`id,value[,...]` grammar documented in `docs/formats/res.md` §1 — this page
is about VALUELST.RES's *semantic* contract: how the original's runtime
looks values up, what happens when one is missing, and where the id -> value
map lives in this repo. The condensed id -> meaning table itself is kept at
`docs/valuelst-map.md` (not duplicated here) to avoid two copies drifting
apart.

## Runtime lookup mechanism — CONFIRMED (`getvalue`, `sub_412135`)

The original does not re-parse the text file on every lookup. At startup
the loader (`sub_4121FF`) fills a flat table indexed directly by id
(`table` at `[0x460250]`, count at `[0x46024c]`); `getvalue(id)`
(`sub_412135`) then:

```
if (id < 0 || id >= count) -> not-found path
v = table[id * 4]            // flat dword array, one slot per id
if (v == 0) -> not-found path
return v
```

(Note: `docs/re/facts.md`'s "VALUELST lookup mechanism" section attributes
this same behaviour to `sub_4124a4` — that address is `getstring`, the
MESSAGES.TXT string lookup, per the consistent identification used
elsewhere in this repo's own docs: `sub_412135` = `getvalue`
[`docs/re/player-colour.md`, `docs/re/frontend-flow.md` line 652,
`docs/re/goldman-roulette.md`, `libs/assets/include/bomber/assets/
reslist.hpp`'s own code comment] vs. `sub_4124A4` = `getstring`, returns
`char*` [`docs/re/setup-screens.md` §"String source", `libs/assets/include/
bomber/assets/messages.hpp`, `docs/re/ai.md` §9.5]. This page follows the
majority/self-consistent identification; `facts.md`'s single entry looks
like a stale mislabel from an early pass and was left as-is here since this
task's scope is `docs/formats/`, not editing `facts.md`.)

**Multi-column rows are flattened into consecutive slots**, not kept as one
row: a row written `700,332,140,38,0` in the file becomes
`table[700]=332, table[701]=140, table[702]=38, table[703]=0`, so
`getvalue(701)` reads the row's *second* column, not a separate id 701 row.
This is why `bomber::assets::res::ValueList` keeps two views
(`docs/formats/res.md` §1): `values[id]` (first column only, what
`Tuning::apply` consumes) and `columns[id]` (every column of that row, for
presentation code that reads `getvalue(id+n)` — e.g. the main-menu cursor
reads ids 700/701/702/703 out of the single `700,...` row).

### Missing-value abort — CONFIRMED

The not-found path increments a counter at `[0x460555]`; once 5 lookups in
a row/session miss, the original **exits to DOS**. This is the exact
behaviour VALUELST.RES's own header warns about: *"DO NOT CHANGE the first
number! if you do the program will exit to DOS because it cannot find a
particular value."* — i.e. the id column is a load-bearing index, not
cosmetic labelling; a scheme/tool that renumbers ids without updating every
reference can crash the original game. This port's loader is deliberately
more forgiving (`res.md` §1: a bad line becomes a `warnings` entry, not a
process-ending error) since 1997 files are treated as untrusted input here,
not as a contract the port itself must also enforce by aborting.

### `getvalue` call sites vs. batch-loaded gameplay values

An exhaustive scan of literal `getvalue`/`sub_412135` call sites in the
binary found only **28**, and every one requests a **non-gameplay** id: 95,
97, 700-768 (menu layout coordinates), 900/905 (AI), 1200-1250 (campaign).
The core gameplay ids (fuse length, speeds, powerup caps, etc.) are all
marked `; PGT` in the file's own comments and are **not** fetched through
`getvalue` at all — strong evidence they're loaded as a batch straight into
a settings struct through a separate init path, with `getvalue` reserved
for the "look this up occasionally" ids (menu/UI/AI/campaign layout).
Either way, this port's model — `Tuning::apply(id, value)` called once per
row at match-config build time, id-indexed exactly like the original's flat
table — is faithful to both paths: PGT rows apply once at build time (like
the batch load), and non-PGT rows are simply available for any
presentation code that wants a direct `at_or(id, fallback)` read (like
`getvalue`).

## Units (from the file's own authored comments)

- Speeds: hundredths of a pixel per frame.
- Probabilities: expressed as 1-in-N (roll `rand() % N == 0`, or similar,
  depending on the site — see the specific mechanic in `docs/re/facts.md`).
- Frame counts: at the nominal tick rate (VALUELST ids 25/30, = 20 — this
  is also this port's `libs/sim` tick rate).
- A handful of ids hold coordinate pairs (`id,x,y`); this port's
  single-value `values` map keeps only the first column for those (see
  `columns`/`column_or` in `docs/formats/res.md` §1 for the rest).

## Where the id map lives

`docs/valuelst-map.md` is the maintained id -> meaning table, split into
"consumed by `sim::Tuning` today" and "mapped, not yet consumed" (ids RE'd
but with no sim/game code reading them yet). Update that file, not this
one, when a new id gets pinned or ported — this page documents the file
*format* and lookup contract, which changes far less often than the id
inventory.

## Parser

`bomber::assets::res::load_values` (`libs/assets/src/reslist.cpp`,
`res.md` §1) is the parser; there is no VALUELST-specific code path beyond
that — the file is just the largest and most heavily-commented instance of
the generic `id,value[,...]` grammar.
