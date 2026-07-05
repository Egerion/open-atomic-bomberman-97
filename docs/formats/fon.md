# Bitmap font format (`FONT<n>.FON`)

The original's bitmap fonts, shipped in the **install root** (not under DATA/):
`FONT0.FON`, `FONT1.FON`, `FONT6.FON`. The engine draws every text string through
the *active* font; graphics-init pins it to **FONT6** for the front-end via
`sub_431E9C(6)` (BM95.EXE @ 0x417600), so FONT6 is what the `.BM` help/credits
screens (`sub_41302D`) render with.

Provenance: the loader `sub_431BBC` (@ 0x431BBC), the active-font selector
`sub_431E9C` (@ 0x431E9C), and the metric routines `sub_432120`/`sub_432164`
(@ 0x432120). All fields verified byte-for-byte against the shipped FONT6.FON.

## Layout (little-endian)

```
Header (20 bytes):
  u32 glyph_count     ; 128 (FONT0, FONT6) or 256 (FONT1)
  u32 glyph_height    ; common cell height in px (16 for FONT1/FONT6, 17 FONT0)
  u32 spacing         ; extra advance added between glyphs (0 in the shipped fonts)
  u32 magic0          ; integrity hash — the renderer ignores it
  u32 magic1          ; integrity hash — the renderer ignores it

Glyph table (glyph_count entries, 8 bytes each):
  u32 width           ; glyph advance width in px (table column 0)
  u32 bitmap_offset   ; byte offset of this glyph's bitmap, from the bitmap base

Bitmap block (starts right after the table, at 20 + 8*glyph_count):
  for each glyph, at bitmap_base + bitmap_offset:
    glyph_height rows of ((width + 7) / 8) bytes, 1 bit per pixel, MSB-first;
    a set bit is an inked pixel.
```

The loader computes the block size from the LAST table entry the same way
(`height * ((width+7)>>3) + offset`), then reads exactly that many bitmap bytes.

## Indexing and metrics

- A character's glyph is indexed **directly by its byte value** (`8 * c` into the
  table), so **glyph index == char code** and the file covers codes
  `0 .. glyph_count-1` (0..127 for FONT6, 0..255 for FONT1). Non-printable codes
  simply have a width and an all-clear bitmap (a blank advance).
- **Advance width** of one character = `glyph[c].width + spacing` (`sub_432120`:
  `spacing + glyph_table[8*c]`). A string's pixel width is the sum of its
  characters' advances; a code `>= glyph_count` contributes nothing (the
  original's `if (c < count)` guard).
- `sub_432164(c)` returns just `glyph[c].width` (the glyph box width, no spacing).

## Worked example (FONT6.FON)

`glyph_count = 128`, `glyph_height = 16`, `spacing = 0`,
magics `0x1D4C68C7 / 0x1D4C7E95`. Table (code → width, offset):
`0→(7,0)`, `1→(8,0x10)`, `2→(8,0x20)`, `3→(8,0x30)`, `4→(9,0x40)`, `5→(8,0x60)`,
… `' '(32)→(12,…)`, `'A'(65)→(13,…)`, `'.'(46)→(5,…)`. Width-8 glyphs step by
`16` bytes (1 byte/row × 16), width-9 glyphs by `32` (2 bytes/row × 16) — exactly
the offsets in the file. Rasterizing `'A'` reproduces a legible capital A, `'.'`
a bottom-corner dot, `' '` all-clear — confirming stride, MSB order, and the
code-indexed table.

## Parser

`bomber::assets::bmfont::parse/load` (`libs/assets/src/bmfont.cpp`) decodes this
into `Font { glyph_height, spacing, glyphs[] }`, each `Glyph { width, pixels }`
where `pixels` is one byte per pixel (0 = transparent, 255 = inked), ready for the
presentation layer to expand into an RGBA texture. SDL-free; every field is
bounds-checked via `BinaryReader`, and a truncated file or a glyph whose bitmap
runs past the buffer throws (1997 files are treated as untrusted input). The
front-end presentation (`FontTextures` in `libs/game`) uploads one alpha texture
per glyph and tints it per draw.
