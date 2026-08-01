# Scheme (arena map) format (`DATA/SCHEMES/*.SCH`)

Plain-text, line-oriented, **self-documenting** arena definition. 67 files
ship with the game (`BASIC.SCH` etc.). Written by the original scheme editor
(`sub_403C16` @ 0x403C16, `docs/re/results-and-options.md` §5) and read by
the stage loader. No binary header, no magic — every line is either a
comment or a `-X,...` directive.

## Line grammar

- Lines are trimmed of leading/trailing whitespace, `\r`, and the DOS EOF
  byte `\x1a`.
- A line starting with `;` (after trim) is a **comment**, ignored.
- Any other line not starting with `-` is ignored (forward-compatible: an
  unrecognized non-directive line does not abort parsing).
- A directive line is `-<letter>,<fields...>`. The letter selects the
  record kind; `default:` in the `switch` — an unknown `-X` letter is
  silently skipped, since the format is versioned and newer schemes may add
  directives an older parser doesn't know.

```
-V,<version>
-N,<name>
-B,<brick_density>                    0-100 percent
-R,<row_index>,<row_text>             one line per grid row
-S,<player>,<x>,<y>,<team>            one line per spawn point
-P,<id>,<born_with>,<has_override>,<override_value>,<forbidden>,<comment>
```

### `-R` — grid row

`row_text` is the grid row itself, one character per tile:

| char | meaning |
|---|---|
| `#` | solid (indestructible) wall |
| `:` | brick **candidate** (see "Brick fill is randomized" below) |
| `.` | blank floor |

All rows must be the same width; the loader throws `std::runtime_error`
("scheme rows differ in width") otherwise. At least one row is required
("scheme has no rows").

### `-S` — spawn point

`player` (spawn slot index), `x`, `y` (tile coordinates), plus an optional 4th
field: this slot's **TEAM** (`docs/re/facts.md` "The `.SCH` `-S` row's 4th
field is the per-slot TEAM"). At least 3 fields (`player,x,y`) are required.

The team field is a **boolean** — `sub_403EEE` stores `value != 0` — and it is
written into the slot's start record only when the row actually carries four
fields. That distinction matters: a three-field row leaves the slot at
`sub_4049C0`'s default, which is **slot parity** (0,1,0,1,…), not 0. The
reader's tail loop then pushes all ten records into the player records' `+84`
team byte, the same byte the PLAYER INPUT screen's `T` key toggles — so the
scheme supplies the roster the screen opens with, and a `T` press overrides it.
The map editor authors the field as a per-spawn "team ring" marker
(`sub_4028D2`).

19 of the 67 shipped schemes carry a non-parity layout here; fifteen of them
are the same "slots 0-4 vs slots 5-9" split (`E_VS_W`, `N_VS_S`, `TENNIS`,
`VOLLEY`, `PINGPONG`, …). Whether the teams MEAN anything in a given match is
a separate, downstream gate: the Options screen's team-play flag
(`dword_464964`).

### `-P` — powerup rule

One row per powerup kind (13 rows in the shipped files, in the order: extra
bomb, flame, disease, kick, skate, punch, grab, spooger, goldflame, trigger,
jelly, super-disease, random — `docs/valuelst-map.md`). Fields:

| field | meaning |
|---|---|
| `id` | powerup kind index |
| `born_with` | count a player starts the match holding — **replaces** the VALUELST baseline (see below) |
| `has_override` | whether `override_value` overrides the scheme-wide hide count |
| `override_value` | per-kind hidden-powerup count override |
| `forbidden` | 1 = this powerup kind never spawns on this map |
| `comment` | trailing free text — see below |

At least 5 numeric fields are required; the trailing `comment` is optional.
The comment text is not arbitrary: `sub_403C16`'s writer emits `getstring(800
+ id)` here — the SAME MESSAGES.TXT 800-block strings the Goldman Roulette
result screen prints for that prize (`docs/re/goldman-roulette.md` §7, "the
`.SCH` writer `sub_403C16` reuses 800+i as the `-P` row comments"). Our
writer round-trips whatever `PowerupRule::comment` already holds rather than
inventing MESSAGES.TXT text (which is not committed to this repo); an empty
comment writes a bare `-P,...` line the reader tolerates.

**`born_with` is not a separate grant channel** (`docs/re/facts.md` "The
`.SCH` `-P` row's 2nd field is a COUNT that REPLACES the starting inventory").
After the parse, `sub_403EEE` calls the VALUELST **setter** `sub_4121BF(50 +
id, count)` for every row whose count is `> 0`, overwriting the per-kind
starting-inventory value the whole game reads through `getvalue`. So a scheme
asking for 3 bombs starts every player on exactly 3, not on the VALUELST's 1
plus a grant — and every other reader of id 50+kind (notably the death-scatter
and head-hit "above baseline" tests) sees the scheme's number too. A count of
`0` is "no opinion": the gate is `> 0`, so it cannot zero the default.

## Brick fill is randomized — NOT baked into the file

The `:` grid cells are only brick **candidates**; the actual destructible
layout is rolled fresh **every match** by `sub_4260F5` (`docs/re/facts.md`
"Per-match brick fill — CONFIRMED"). Walking the board row-major:

```
v = cell(x, y);                       // 0=blank, 1=solid, 2=brick candidate
if (v == 2 && rand() % 100 >= brick_density) v = 0;   // knock back to blank
```

so each `:` becomes a real brick with probability `brick_density`% (the `-B`
value); `#`/`.` cells copy straight through and draw **no** `rand()` call
(short-circuited by `v==2`). The original seeds this from the wall-clock
(`time()`/`srand()`), so its layout is non-reproducible run-to-run and not
tied to any match seed. Our port (`build_match_config`) performs the exact
same row-major, per-candidate roll, driven by a **setup-only** LCG seeded
from the match seed — never `State::rng` — so the sim's per-tick RNG draw
contract stays untouched while identical match seeds still reproduce
identical boards.

## Validation the parser enforces

- `-R` line needs 2 comma-fields (`row_index,row_text`); a malformed one
  throws `"bad -R line"`.
- `-S` line needs >= 3 fields; fewer throws `"bad -S line"`.
- `-P` line needs >= 5 fields; fewer throws `"bad -P line"`.
- All grid rows must have equal width.
- At least one row must be present.
- Malformed/short `.SCH` files are treated as untrusted 1997 input; the
  loader throws rather than silently truncating (`sch::load`, plain
  `std::ifstream`/`std::getline`, no `BinaryReader` needed since this is a
  text format).

## Parser

`bomber::assets::sch::load`/`to_text`/`write` (`libs/assets/src/sch.cpp`)
round-trip a `Scheme { version, name, brick_density, rows[], spawns[],
powerups[] }`. `to_text`/`write` reproduce the exact shipped grammar above
(verified round-trip: `load(write(s))` reproduces every field `load()`
reads, `tests/assets/test_sch_write.cpp`) so the hidden scheme editor
(`docs/re/results-and-options.md` #5) can save `.SCH` files the original
game (and this port) can read back.
