# Player-colour remap format (`0.RMP`..`9.RMP`, install root)

Ten fixed-name files (`0.RMP` .. `9.RMP`, index = player-colour slot: 0 =
white, 1 = black, 2 = red, 3 = blue, 4 = green, 5 = yellow, 6 = cyan,
7 = magenta, 8 = orange, 9 = purple), each a **palette-INDEX remap table**
for one player colour. Every pre-rendered player sprite (walk/stand/bombs/
deaths/etc.) is authored once in a neutral "green" armour; the engine
retargets it to a player's actual colour at blit time by rewriting each
pixel's palette index through this table before the palette lookup —
`sub_415A1C` (blit), table built/loaded by `sub_414A65` (@ 0x414A65). This
is the byte-layout companion to `docs/re/player-colour.md`, which has the
full RE narrative (the green-dominant recolour formula, the builder
fallback when a `.RMP` is missing, the setup-screen slot-ink derivation);
this page documents only the on-disk format and the parser.

## Layout (259 bytes, fixed size)

```
[0..255]    256-byte index -> index remap table.
            Confirmed against the install: the non-identity "colour band"
            is exactly indices 100..174 in every shipped file; every entry
            outside that band is stored as 0x00.
[256..258]  3 tail bytes = R, G, B, each 0..100 (PERCENT, not 0..255) —
            the slot's nominal colour, e.g. 2.RMP (red) = 64 00 0a hex
            (100, 0, 10).
```

Total file size is always exactly 259 bytes; there is no length field or
magic — the format is implicit from context (the loader is only ever
called with a `<digit>.RMP` path).

## Backfill convention — CONFIRMED (`sub_414A65`, decompile 17498-99)

A raw `0x00` table entry means **"not remapped"**, not literally "map index
`i` to index 0". The loader backfills every zero entry to identity right
after reading:

```
for (i = 0; i < 256; ++i)
    if (table[i] == 0) table[i] = i;
```

so after loading, `dst = table[src]` is total: identity outside the colour
band, the colour ramp inside it. This is what lets a plain per-pixel remap
leave shadow/casing/transparent indices exactly where they were — the
band-only design means the file only needs to encode 75 non-trivial
entries (100..174), but the loader still ships/reads the full 256-byte
table since that is what the blit indexes into directly.

## Parser

`bomber::assets::res::load_rmp` (`libs/assets/src/rmp.cpp`) reads the
256-byte table (applying the `if (t[i]==0) t[i]=i` backfill inline, so
callers never see a raw zero entry) and the 3 tail bytes into `RemapTable {
map: array<uint8_t,256>, rgb: array<uint8_t,3> }`
(`libs/assets/include/bomber/assets/rmp.hpp`). Every read goes through
`BinaryReader`, so a file shorter than 259 bytes throws `std::out_of_range`
(1997 files are untrusted input) — the caller in `AssetStore` catches this
and falls back to a truecolour approximation of the same recolour formula
(`docs/re/player-colour.md` "Our port") rather than treating a missing/
corrupt `.RMP` as fatal.

The `.RMP` files are the user's own game-install data (loaded at runtime
from the install root) and are never committed to this repo; tests use
synthetic 259-byte buffers only (`tests/test_rmp.cpp`).

See `docs/re/player-colour.md` for: the green-dominant recolour formula
that BUILDS a table when the file is absent, the blit itself
(`sub_415A1C`), and the setup-screen slot-colour derivation from the tail
RGB (`sub_41672F`).
