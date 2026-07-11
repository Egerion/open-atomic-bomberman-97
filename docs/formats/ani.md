# ANI format (`DATA/ANI/*.ANI`)

Custom chunked container, magic `CHFILEANI ` (10 bytes). Derived empirically from all 95 original files (survey: 2327 frames, 249 sequences, zero parse errors) with structural hints from [ab_aniex](https://github.com/mmatyas/ab_aniex). All integers little-endian.

## Layout

```
File header:  "CHFILEANI " | u32 payload_len | u16 file_id
Item header:  char tag[4]  | u32 payload_len | u16 item_id      (payload follows)
```

Top-level items: `HEAD`(48) · `PAL `(8192) · `TPAL`(1028) · `CBOX`(4: u16 cell_w, u16 cell_h) · `FRAM`×N · `SEQ `×M. `PAL `/`TPAL` semantics unknown, unused so far.

### FRAM (top-level) — one frame

Children: `HEAD`(2: `03 64`), `FNAM` (NUL-terminated original TGA name), `CIMG` (image).

### CIMG

```
u16 type        4 = 16bpp X1R5G5B5 (2299/2327), 11 = 8bpp paletted (28/2327)
u16 unknown
u32 additional_size    24 = no palette; >=32: palette block present, palette_size = additional_size - 32
u32 unknown
u16 width, height
u16 hotspot_x, hotspot_y
u16 key_color          transparent pixel value (type 4) / index (type 11)
u16 unknown
[if palette: u32 unknown ×2, then palette_size bytes = 256 × RGBx]
u16 unknown ×2
u32 compressed_size    includes these 12 special-header bytes; data = compressed_size - 12
u32 uncompressed_size  == width*height*bytes_per_pixel
data[...]              TGA type-10 style RLE, optional 1 terminator byte
```

RLE: packet header byte `b`; count = `(b & 0x7F) + 1`; if `b & 0x80` one pixel value repeated count times, else count literal pixel values. Pixel = u16 (type 4) or u8 (type 11).

### SEQ — one named animation

Children: `HEAD`(96: NUL-terminated name at offset 0, remainder uninitialized stack garbage + unknown fields), then `STAT`×K (one per animation step).

### STAT — one step

Children: `HEAD`(46: u16 field0 — `0x001E` or `0xFFFF`, timing-related, semantics TBD; rest zero), `FRAM`(12, **leaf**, not the top-level container):

```
u16 unknown (=1)
u16 frame_index        into top-level FRAM order
s16 offset_x, offset_y per-step blit offset
u32 unknown (=0)
```

## Rendering a step

Blit `frames[frame_index]` at `pos - hotspot`, treating `key_color` pixels as
transparent. X1R5G5B5 → RGB8 via `(c5 << 3) | (c5 >> 2)`.

**The per-STAT `offset_x/offset_y` (FRAM-leaf `dx/dy`) is NOT applied by the
original's standard blit** (`sub_415920` / `sub_415A9F` take only the frame
index; the offset getter `sub_41DB41` is a separate, rarely-used path). Do NOT
fold it into the hotspot — these offsets are large (tile 10 brick dy=18, stand
south dy=19), so applying them shoves sprites that many pixels DOWN (bricks leak
below their cell, players sink below their shadow; dy=0 sprites like bombs/shadow
stay correct). Confirmed 2026-07-04 against a live build. Parse `dx/dy` for
inspection but render by the frame hotspot alone.

**Confirmed exception: real flame arms.** `sub_41DB41`'s one confirmed caller
is `sub_426D06` (the per-tick flame-cell animator), and only on its
non-brick-burn branch (`off_45BEA0` kind 0-8, the `"flame <piece> green"`
sequences). Resolved by disassembly (0x426ee7-0x426f46), the full anchor it
passes to the blit — *before* the frame's own `pos - hotspot` subtraction — is:

```
X = X_base + dx                (X_base = tile_left + tileW/2, i.e. tile centre)
Y = Y_base - tileH/2 + dy       (Y_base = tile_top + tileH-1, i.e. tile bottom)
```

Both `dx` and `dy` are ADDED (rec+4→X, rec+8→Y); the flame-only `- tileH/2`
re-anchors the piece from tile-bottom to tile-centre before the offset. The
SAME function's brick-burn branch (kind 9, `"flame brick <n>"`) does not call
`sub_41DB41`, applies no `dx/dy`, and no `-tileH/2` — it blits at the raw
`Y_base` base anchor, per the general rule above. `libs/game/src/sprites.cpp`'s
`resolve_sequence` carries `dx/dy` through onto `Sprite` (still inert by
default) specifically so `Renderer::draw_world`'s flame-arm draw can apply the
formula above; every other draw site must keep ignoring them. See
`docs/re/facts.md` "Flame draw offset" (2026-07-11) for the byte-level citation
and the arithmetic sanity check.
