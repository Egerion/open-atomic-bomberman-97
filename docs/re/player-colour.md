# Player colour — the `.RMP` palette-remap pipeline — RE

Reverse-engineered from `BM95.EXE` (pseudo.c) + the shipped install. Every
pre-rendered player sprite (walk / stand / bombs / duds / flames / trigger bomb /
death anims / cornerheads / carry-bomb poses) is authored ONCE in a neutral
"green" armour and retargeted to each of the ten player colours at load time by a
**palette-INDEX remap table** (`%u.rmp`), applied by the blit before the palette
lookup. This doc pins the file format, the builder, the apply/backfill, the blit,
and the setup-screen slot ink, and records how our clean-room port mirrors them.

Records STRUCTURE only. The `.RMP` files are the user's own game data (loaded at
runtime from the install ROOT); their bytes are never committed. Tests use
synthetic tables.

## Why index-remap, not a truecolour tint

The sprites are stored 8-bit **paletted** (ANI CIMG type 11: a raw index buffer +
a 1024-byte RGBA palette per frame). The engine recolours by rewriting each
pixel's palette INDEX through the colour's table, then doing the palette lookup —
so only the indices in the "colour band" move; the shadow, casing, outline and
transparent indices are untouched. Our earlier port instead scaled the decoded
RGB by a green-excess TINT (`recolor_image`), which on DESATURATED armour pixels
preserved the achromatic baseline and washed every non-black colour toward WHITE
(black collapsed to the baseline and happened to look right). The fix is to do
the real index remap; the tint survives only as the missing-file fallback.

## `.RMP` file format — CONFIRMED (259 bytes)

`0.RMP`..`9.RMP` in the install ROOT, one per player colour (index = player
number: 0 = white, 1 = black, 2 = red, 3 = blue, 4 = green, 5 = yellow,
6 = cyan, 7 = magenta, 8 = orange, 9 = purple). Each file is **259 bytes**:

```
[0..255]   256-byte index -> index remap table. The player colour band is
           non-zero; every other entry is 0x00.
[256..258] 3 tail bytes = R, G, B as 0..100 PERCENT (the slot's nominal colour).
```

Confirmed against the install: the colour band is exactly indices **100..174** in
every file, and every entry outside it is 0x00 (so after the identity backfill,
the table is identity outside 100..174). Tail examples (structure, not committed):
0.RMP = white `64 64 64` (100,100,100); 1.RMP = black `0f 0f 0a`; 2.RMP = red
`64 00 0a` (100,0,10); 9.RMP = purple `32 00 64` (50,0,100). The tails track
VALUELST 200-247 / `Tuning::color_rgb`, but the **tail is the authoritative
per-colour value** — the engine overwrites the VALUELST-derived percents with the
file's tail when a `.RMP` is present (see the builder below).

## Apply + backfill — CONFIRMED (`sub_414A65` @0x414A65, decompile 17498-99)

When a `.RMP` exists, `sub_414A65` reads the 256-byte table into the colour's
in-memory table `dword_460564[player]`, reads the 3 tail bytes into the per-slot
colour bytes `byte_460BD0/BDA/BE4[player]`, then **backfills the table to
identity**:

```
fread(table, 256);                      // the file's remap band
byte_460BD0[p] = fgetc();               // tail R
byte_460BDA[p] = fgetc();               // tail G
byte_460BE4[p] = fgetc();               // tail B
for (i = 0; i < 256; ++i)
    if (table[i] == 0) table[i] = i;    // 0 == "not remapped" -> identity
```

So after load the table is TOTAL: `dst = table[src]` is identity outside the
band and the colour ramp inside. This is what lets a plain remap blit leave every
non-band pixel (shadow / casing / transparent) exactly where it was.

## Builder (fallback when a `.RMP` is ABSENT) — CONFIRMED (`sub_414A65`)

If the `%u.rmp` file is missing (or a forced rebuild), `sub_414A65` BUILDS the
table from the base palette and writes the file out (with the 3 tail bytes). Init
feeds the args from VALUELST (`sub_412135` = getvalue), in the loop over the ten
players `k`:

```
G% = getvalue(201 + 5k);  R% = getvalue(200 + 5k);  B% = getvalue(202 + 5k);
sub_414A65(k, R%, B%, G%, 0);          // a2=R%, a3=B%, a4=G%
```

and for each base-palette entry `[R,G,B]` (base palette getter `sub_42C570`):

```
if (G > R && G > B) {                  // green-dominant (strict, no margin)
    baseline = (R + B) / 2;            // v33
    excess   = G - baseline;           // v32 - v33  (lum = G)
    outR = R% * excess / 100 + baseline;
    outG = G% * excess / 100 + baseline;
    outB = B% * excess / 100 + baseline;
    table[i] = argmin_j |basePalette[j] - (outR,outG,outB)|^2;   // nearest entry
} else {
    table[i] = i;                      // non-green: identity
}
```

The **baseline `(R+B)/2` is preserved**, so the sprite's shading/casing survives.
Our `recolor_image` (`libs/game/src/sprites.cpp`) is the truecolour port of THIS
formula (it keeps the computed RGB instead of snapping to a palette entry). It is
used ONLY as the fallback for a colour whose `.RMP` failed to load.

## Blit — CONFIRMED (`sub_415A1C`, wrapper @ decompile 18002)

The standard sprite blit takes the colour's remap table and applies it to each
pixel's index before the palette lookup. The wrapper clamps the colour and
selects the table:

```
if (colour < 0) colour = 0;  if (colour >= 10) colour = 9;
return sub_415A1C(surface, src, dword_460564[colour], dst);
```

`dword_460564[10]` are the ten in-memory remap tables (identity-backfilled).
`sub_415A1C` walks the source indices, does `dst_index = table[src_index]`, and
looks the recoloured pixel up in the palette. The diseased-player strobe reuses
the SAME machinery with a per-frame random colour (`rand()%10`, facts.md disease
visual) — presentation RNG, not `State::rng`.

## Setup-screen slot ink — CONFIRMED (`sub_41672F` @0x41672F, `sub_416867`)

The PLAYER INPUT screen (`sub_410F81`, docs/re/setup-screens.md) inks each of the
ten slot labels with that slot's own colour via `sub_41696C(surface, text, x,
getvalue(713), y, fg = sub_41672F(i), bg = sub_416867(i))`. The foreground colour
`sub_41672F(i)` derives from the slot's stored RGB (`byte_460BD0/BDA/BE4[i]` ==
the `.RMP` tail):

```
r5 = min(tailR / 3, 31);  g5 = min(tailG / 3, 31);  b5 = min(tailB / 3, 31);
return palette_LUT[ b5 | (g5 << 5) | (r5 << 10) ];   // 15-bit RGB555 -> index
```

i.e. quantise each channel to 5 bits and look the RGB555 up in the palette. The
background `sub_416867(i)` is the outline colour (`palette[0]` in normal play;
the team colour in team mode). So the slot colour is the **`.RMP` tail**, not the
raw VALUELST percent.

## Our port

- **Parser** `libs/assets` `res::load_rmp` (`rmp.hpp`/`rmp.cpp`): reads the 256
  byte table (backfilling `if (t[i]==0) t[i]=i` per the apply convention above)
  and the 3 tail bytes into `RemapTable{ map, rgb }`. Bounds-checked
  (`BinaryReader`), throws on a short file. Doctests use synthetic 259-byte
  buffers only.
- **Recolour** `libs/game` `recolor_image_rmp(img, rmp)` + `AniTextures::
  recolored(ren, rmp)`: for each non-transparent pixel of a paletted frame,
  `dst = rmp[indices[i]]`, RGB = frame `palette[dst]`, alpha preserved (the ANI
  loader already baked the key-colour transparency into the alpha). A
  non-paletted (16bpp type 4) frame is returned unchanged. This mirrors
  `sub_415A1C` directly on the frame's retained indices + palette.
- **AssetStore** loads `0.RMP`..`9.RMP` from the install ROOT into `rmp_[10]`
  (+ `rmp_ok_[10]`, + `rmp_rgb_[10]` the tails). `build_player_sets` recolours
  player slot `p` (colour index = `p`) with `recolored(rmp_[p])` when the file
  loaded, else the truecolour `recolored(color_rgb[p])` fallback. A missing/short
  `.RMP` is logged, never fatal.
- **Setup screen** tints slot `i` via `AssetStore::slot_color(i)`, the truecolour
  equivalent of `sub_41672F` (`min(v/3, 31)` then `expand5` of the 5-bit channels
  instead of the palette LUT), from the loaded tail — so each slot reads as its
  real in-game colour (white 255/255/255, red 255/0/24, blue 0/24/231, green
  0/255/0, yellow 255/255/0, cyan 0/247/255, magenta 255/0/247, orange
  255/132/0, purple 132/0/255).

## Determinism / golden — NO IMPACT

Recolour and slot colour are entirely PRESENTATION-side. `libs/sim` `State`,
`state_hash()`, and `tests/test_golden.cpp` are byte-identical — player colour is
NOT a hashed field (it is `MatchConfig`/asset data, never mixed into the hash).
Confirmed unchanged: no sim file was touched.

Sources: `sub_414A65` (0x414A65) builder + apply/backfill (decompile 17420-17500,
17580-17600), `sub_415A1C` blit (0x415A1C, wrapper @18002), `dword_460564[10]`
tables, base palette `sub_42C570` (0x42C570), setup ink `sub_41672F` (0x41672F) /
`sub_416867` (0x416867), the `%u.rmp` layout confirmed against the install
(band 100..174 + 3 tail percent bytes).
