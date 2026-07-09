# Resource-list formats (`DATA/RES/*.RES`)

The `.RES` extension covers **two unrelated text grammars** in the shipped
install (8 files total): a commented `id,value` list (`VALUELST.RES`,
`SOUNDLST.RES`) and a dash-command actor-placement list (`EXTRA<N>.RES`,
5 files: `EXTRA2/3/4/9/10.RES`). Both are plain ASCII, line-oriented, and
tolerant of trailing `;` comments — otherwise they share nothing, so this
doc covers them in two sections. `VALUELST.RES`'s own runtime lookup
contract (`getvalue`, the "DO NOT CHANGE the first number" abort behaviour)
is documented separately in `docs/formats/valuelst.md`; this page is about
the file's on-disk *grammar*, parsed by `bomber::assets::res::load_values`.

## 1. Commented id-list (`VALUELST.RES`, `SOUNDLST.RES`)

One record per line: `<id>,<value>[,<more columns...>][;comment]`.

```
; full-line or trailing comment, stripped before parsing
<id>,<first-column>[,<col2>[,<col3>...]]
```

Parsing rules (`bomber::assets::res::load_values`/`load_sounds`,
`libs/assets/src/reslist.cpp`):

- Everything from the first `;` to end-of-line is stripped (comment), then
  the line is trimmed of whitespace, `\r`, and the DOS EOF byte `\x1a`.
- An empty line (after strip) is skipped.
- A line with no comma is a **warning**, not an error: recorded as
  `"<file>:<lineno>: <line>"` in `warnings` and otherwise ignored — VALUELST
  files carry free-standing prose/header lines the parser must tolerate.
- `id` is the integer before the first comma.
- **`VALUELST.RES` (`load_values`) keeps two views of the same rows:**
  - `values[id]` — the FIRST column only, as `std::int64_t`. This is what
    `Tuning::apply` and every sim-facing consumer reads; a non-numeric first
    column is a warning (`"non-numeric value '<x>'"`), not a throw.
  - `columns[id]` — EVERY numeric column of that row, in file order, via a
    second pass over the same file. The original `getvalue(id)` (see
    `valuelst.md`) is backed by a FLAT table the loader fills by splitting
    each `id,a,b,c,...` row into consecutive slots — so `getvalue(700)`,
    `getvalue(701)`, `getvalue(702)` are `columns[700][0/1/2]`, not three
    separate rows. `column_or(id, index, fallback)` mirrors
    `getvalue(id + index)` reads (used by presentation-only rows like the
    main-menu cursor, `id 700-703`). A non-numeric column simply ENDS the
    numeric run for that row (columns after it are dropped, not warned —
    they belong to the trailing comment the `;` strip already removed in
    the common case, or to prose the file's own author left inline).
- **`SOUNDLST.RES` (`load_sounds`) keeps only `names[id]` = the (single,
  trimmed) second field** — a sound event id mapped to an `.RSS` base name
  (no extension), e.g. `172,bmbthrw1` (see `docs/formats/rss.md`).

Both loaders are lenient on purpose: 1997 `.RES` files are treated as
untrusted input, but a resource list with a stray unparseable line should
not make the whole game (or this port) refuse to start — the line is
recorded in `warnings` for diagnostics instead of thrown.

### Comment convention

Both files carry extensive author prose (comment lines and inline
column-2+ commentary) — see `docs/formats/valuelst.md` for the semantic id
map and `docs/re/facts.md` "Wall-slam SFX" for a worked example of
`SOUNDLST.RES`'s own inline warning comments (e.g. *"the code is HARD-CODED
to play one of the three below randomly"* directly above ids 140-146) that
this port's tests/facts cross-reference as corroborating evidence, not as
parsed data — the parser only sees `id,name`.

## 2. `EXTRA<N>.RES` — stage-actor placement

A **different** dash-command grammar, parsed by `bomber::assets::extra`
(`libs/assets/src/extra.cpp`) rather than `reslist.cpp`. Full mechanics
(actor registry layout, coordinate normalization, warphole linking, the
`-T,H,H` random-placement roll) are RE'd in `docs/re/stage-actors.md` §2;
this section is the file-grammar summary.

`EXTRA<board>.RES` (`sub_404E99`, format string `extra%u.res`) is read line
by line, independently per stage/board id; a missing file simply means that
board has no actors (not an error — "There is no extra.res file for this
level!" in the original, an empty `std::vector<Actor>` in the port).

```
-A,<dir>,<x>,<y>                    dirArrow   (N/E/S/W)
-C,<dir>,<x>,<y>                    conveyor   (N/E/S/W)
-T,<x>,<y>                          trampoline, fixed placement
-T,H,H                              trampoline, RANDOM odd-parity placement
-W,<type>,<idno>,<x>,<y>,<linkto>   warphole
```

- Comment lines start with `;`; blank lines are skipped; a line must start
  with `-` to be an actor record. The command letter is matched
  case-insensitively.
- Fields are split on `,`, each individually trimmed (files pad with
  spaces, e.g. `-A,S, 2, 2`).
- Direction letters decode via `n=0(Up), e=1(Right), s=2(Down), w=3(Left)`
  — exactly GODIR order; an unrecognized letter makes the line invalid.
- Coordinates are **normalized** against the board's tile dimensions:
  negative values wrap from the far edge (`while (v < 0) v += extent`),
  over-large values clamp to the last tile (`sub_404E99`'s
  `norm()`/`sub_405654` port).
- `-T,H,H` (arg1/arg2 uppercase `H`) yields an `Actor` with `random = true`
  and unset `x`/`y`; the CALLER resolves the odd-parity placement with a
  setup-only RNG (never `State::rng`) — see `stage-actors.md` §7/§8 for the
  determinism note.
- Arg-count mismatches or an unrecognized command letter are fatal in the
  ORIGINAL ("dirarrow: needs to have 4 args total..."); this port instead
  **skips** the malformed line and keeps parsing (a documented, deliberate
  leniency divergence — same rationale as the id-list loaders above: don't
  let one bad line abort the whole board).

### Parser

`bomber::assets::extra::parse(path, board_w, board_h)` returns
`std::vector<Actor>`; `load_for_board(game_dir, board, w, h)` builds the
`DATA/RES/EXTRA<board>.RES` path and parses it. `Actor { kind, x, y, dir,
idno, linkto, random }` — see `extra.hpp` for the field-by-field mapping
back to the actor-struct offsets `stage-actors.md` §1 pins in the binary.
