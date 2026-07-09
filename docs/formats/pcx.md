# PCX format (`DATA/RES/*.PCX`)

Standard ZSoft PCX, the format used for every 2D background/UI image the
install ships (62 files: menu backgrounds, `FIELD<n>.PCX` stage backdrops,
`POW*.PCX` static powerup icons, dialog art). Not used for player/animation
sprites — those are `.ANI` (`docs/formats/ani.md`), which carries a *retained*
palette-index buffer the `.RMP` recolour pipeline needs; PCX is decoded
straight to RGBA and never player-recoloured. All integers little-endian.

## Layout

```
Header (128 bytes):
  u8  manufacturer      ; 0x0A (ZSoft)
  u8  version            ; ignored
  u8  encoding           ; 1 = RLE
  u8  bits_per_pixel     ; 8
  u16 xmin, ymin, xmax, ymax   ; image bounds, INCLUSIVE
  ...4 bytes dpi, 48 bytes EGA palette, 1 reserved byte... (all ignored)
  u8  planes             ; 1
  u16 bytes_per_line     ; scanline stride, >= width

Scanline data: byte-run RLE, starts at offset 128, one row of
  bytes_per_line bytes per output row (see RLE below)

VGA palette (last 769 bytes of the file):
  u8  marker              ; 0x0C
  u8[768]  palette         ; 256 * (R,G,B)
```

`width = xmax - xmin + 1`, `height = ymax - ymin + 1`.

### RLE packet encoding

Each packet is a header byte `b` optionally followed by a data byte:

- `(b & 0xC0) == 0xC0`: a **run** — `count = b & 0x3F`, followed by one data
  byte repeated `count` times.
- otherwise: `b` **is** the pixel value (a literal, run length 1).

Rows are decoded independently; a row's packets are read until
`bytes_per_line` bytes have been produced, then the next row starts fresh
(the encoder is not required to pack a run across a row boundary and the
decoder does not assume one).

### Only variant loaded

The parser accepts exactly the variant every shipped file uses and rejects
anything else: `manufacturer == 0x0A`, `encoding == 1`, `bits_per_pixel == 8`,
`planes == 1` — 8-bit palettized, one plane, RLE-compressed. 24-bit or
uncompressed PCX (other manufacturers/encodings support these) is out of
scope; the loader throws `std::runtime_error("PCX unsupported variant")`
rather than silently misreading the pixel stream.

## Validation the parser enforces

1. File must be at least `128 + 769` bytes (header + minimum palette region),
   else `"PCX too small"`.
2. `width`/`height` (derived from `xmax/ymin` etc.) must both be positive,
   else `"PCX bad dimensions"`.
3. The palette marker byte at `size - 769` must be `0x0C`, else `"PCX palette
   marker missing"` — this is how the loader distinguishes the trailing VGA
   palette from ordinary scanline data without a length field for the
   compressed run.
4. While decoding a row, if the RLE cursor reaches the palette offset before
   `bytes_per_line` bytes have been produced, the stream is short:
   `"PCX data ended early"`.

Treats the 1997 file as untrusted input throughout — every read goes through
`BinaryReader`, which bounds-checks and throws `std::out_of_range` on a
truncated buffer (`libs/assets/include/bomber/assets/binary_reader.hpp`).

## No transparency

The loader always writes alpha 255 (fully opaque) for every pixel — PCX
carries no key-colour concept in this codebase's model, unlike ANI's paletted
`CIMG` frames (`docs/formats/ani.md`), which do. The red border visible on
some `POW*.PCX` menu icons is that ORIGINAL asset's own chroma-key convention
for a different (ANI-based) render path and is not applied by `pcx::load`;
the static PCX icon is a plain opaque fallback tile-fill, the live in-round
powerup art is drawn from `POWERS.ANI` instead (`docs/re/facts.md`
"Per-element blit anchors").

## Parser

`bomber::assets::pcx::load` (`libs/assets/src/pcx.cpp`) decodes directly to
`bomber::assets::Image { width, height, rgba }` (`image.hpp`) — RGBA8,
row-major, top-left origin. Unlike the ANI loader, `pcx::load` never fills
`Image::indices`/`Image::palette` (so `Image::paletted()` is false for every
PCX): PCX-sourced textures are UI-only and never go through the `.RMP`
player-colour remap, so retaining the source indices would be dead weight.
