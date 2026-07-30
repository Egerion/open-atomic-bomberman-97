# BM text-screen format (`*.BM`)

Plain ASCII help/credits screens shown by the front-end menu. Ten ship with the
game, all in the install root: `CREDITS.BM`, `MANUAL.BM`, `OPTIONS.BM`,
`NETWORK.BM`, `INPUT.BM`, `EDITOR.BM`, `ROULETTE.BM`, `README.BM`, plus the
low/high-memory notes `LOWMEM.BM` / `HIGHMEM.BM`.

The original renderer is `sub_41302D` (BM95.EXE, imagebase 0x400000). It opens
the file with `fopen(path, "rt")` (text mode), makes two passes — pass 1 counts
lines, pass 2 lays them out — and draws one line per screen row, scrolling
vertically. Credits is invoked from the front-end menu at `sub_413(...)`
(`sub_41302D((int)aCreditsBm)`); the memory notes and the help screens hang off
the same routine with their own filenames.

## Line model

- Records are **lines**, terminated by `\n` / CRLF. `sub_41302D` reads each with
  `fgets`, then `strchr(line,'\n')` and NUL-terminates at the newline, so the
  trailing `\r` of the shipped CRLF files is stripped by the same cut (the
  files are DOS-CRLF).
- The file ends at a DOS EOF marker `0x1A` (`^Z`); the original's read loop
  stops on the stream's EOF flag (`*(FILE+12) & 0x10`). Everything from the
  first `0x1A` onward is not screen content.
- **Tabs** (`0x09`) expand to spaces up to the next 4-column tab stop
  (`sub_41302D`: `do { *dst++ = ' '; ++col; } while (col & 3);`). Text art in the
  files (`CREDITS.BM`, `NETWORK.BM`) relies on this 4-wide expansion.

  **The stop drifts, and the drift is part of the format.** The expander keeps
  ONE counter for both the tab stop and the 255-character buffer limit, and the
  loop bumps it once per SOURCE character *on top of* the once-per-space the tab
  branch already did:

  ```c
  for (col = 0; *src && col < 255; src++, col++)
      if (*src == '\t') do { *dst++ = ' '; col++; } while (col & 3);
      else *dst++ = *src;
  ```

  So after a tab the counter sits one column AHEAD of the text actually written,
  and every further tab on the same line resolves against that inflated column.
  The shipped files were authored against this: in `CREDITS.BM` the three
  `(for ...)` annotations (`Tim Cain`, `John Price`, `Darren Monahan`) land at
  x = 179 / 189 / 190 px in FONT6 under the real rule; the third has no tab
  before its parenthesis, so it is a fixed anchor, and the textbook
  "next multiple of 4" rule tears the middle one 37 px out of the column
  (191 / 153 / 190). Our parser reproduces the drift (`bmtext::expand_tabs`).

- Expansion runs over the **whole raw line**, `<IMGname>` tags included, before
  the render pass scans for `<IMG` — so a tag's own characters occupy columns for
  any later tab on the line. Same order in our parser.

- The scratch buffer is 256 bytes and the loop stops at `col == 255`. No shipped
  `.BM` line expands past 63 columns, so this never fires on real data; the port
  keeps it as a bound on hostile input.
- There is **no header line** and no global directives — line 1 is already
  content (e.g. `Atomic Bomberman Credits`). Help screens conventionally frame a
  title with rows of `*` or `~`, but that is ordinary text, not markup.
- One record per line, `fgets`-style: a **trailing newline does not** add a
  final empty line, and an **empty file yields no lines**. Interior blank lines
  are kept as empty rows (screen spacing). Our parser matches this.

## Inline markup

Exactly one tag form exists: an inline image reference.

```
<IMGname>
```

### Transparency

The inline image is blitted with `sub_4428E4` → `sub_44AED5`, whose inner loop is

```c
v = *src++;  if (v) *dst = v;        /* BM95.EXE @ 0x44AED5 */
```

— a source byte of **zero is skipped**, so **palette index 0 is the key colour**.
The key is an *index*, not a colour: `CREDBAR.PCX` is 81% index 0,
`BOMBDUDE.PCX` 88% and `QALOGO.PCX` 67% (all three would otherwise be black
rectangles), while `JERM.PCX` and `KURT.PCX` — the two photographs — contain no
index 0 at all and store their real blacks at index 255. Keying on RGB would eat
holes out of those two. In the three images that do use the key, index 0 is the
only source of RGB(0,0,0), which is what makes the `DATA_HD` rule below exact.

The full-screen backdrops (`MAINMENU`, `GLUE<n>`, the `WINZ` border) are copied
opaquely by a different primitive and must NOT be keyed. `DATA_HD` ships 24-bit
upscales with no palette at all; those paint the keyed region as literal black,
so the port keys RGB(0,0,0) there — but only for assets whose classic image
actually used index 0.

### Palette

`sub_41302D` loads each image with `sub_4150F0` (pseudo.c 16370) =
`sub_41BE63` = raw decode + `sub_41BBBD`, the **master-palette snap** (the same
snapping loader `MAINMENU`/`GLUE<n>` use, not the own-palette `sub_415120` path
`TITLE`/`DRAW`/`WINZ` use). `sub_41BBBD` builds its 256-entry remap through the
15-bit reverse LUT starting at entry 1, leaving **entry 0 fixed** — the key
survives the snap.

It cannot be otherwise: the viewer never uploads a palette, so on 8-bit hardware
these images are physically displayable only through the palette already active.
That the active palette is `COLOR.PAL`'s master is corroborated by the art —
`JERM.PCX` and `KURT.PCX` are already 100% master-palette colours, so the snap is
the identity for them, while the other three shift (`CREDBAR` by up to 39/255 on
its yellows).

`sub_41302D` scans each line with `strstr(line,"<IMG")`; on a hit it advances 4
bytes past `<IMG`, finds the closing `>` with `strchr`, and NUL-terminates
there. The bytes between `<IMG` and `>` are an image **base name**
(no extension, no spaces). The original then builds `"%s.plt"` from the name
(`sub_4518D0(..., aSPlt, name)` where `aSPlt = "%s.plt"`) to load the image's
palette, loads the image into a name-keyed table, and in the render pass splits
the line at each tag, drawing the preceding text run and then blitting the named
image inline before continuing with the rest of the line.

Evidence from the shipped files (base name → asset):
`<IMGCREDBAR>` `<IMGJERM>` `<IMGKURT>` `<IMGBOMBDUDE>` `<IMGQALOGO>` in
`CREDITS.BM` (e.g. `CREDBAR.PCX`); the powerup gallery in `MANUAL.BM` uses
`<IMGpowbomb>` `<IMGpowflame>` `<IMGpowgold>` `<IMGpowdisea>` `<IMGpowbad>`
`<IMGpowkick>` `<IMGpowgrab>` `<IMGpowpunch>` `<IMGpowjelly>` `<IMGpowtrig>`
`<IMGpowskate>` `<IMGpowspoog>` `<IMGpowslow>`. Names appear both UPPERCASE and
lowercase; DOS filesystem lookup was case-insensitive, so the name is used
verbatim and case-folding is left to the asset lookup.

### Not markup

Other angle-/square-bracketed tokens in the prose are **literal text**, not
tags — they document keys/buttons inside sentences and never start with the
`<IMG` prefix the scanner looks for:

- `<ESC>`, `<button>`, `<action>`, `<drop bomb>` (`MANUAL.BM`, `NETWORK.BM`,
  `EDITOR.BM`)
- `[TAB]`, `[up]`, `[down]`, `[left]`, `[right]` (`MANUAL.BM`, `EDITOR.BM`)

The scanner only reacts to the literal `<IMG`; these pass through as ordinary
characters. Our parser mirrors that: a run is a tag only if it begins with
`<IMG`.

## Assumptions / edge-case rules

Marked here because they are not directly observable from a clean disassembly of
the single call site:

1. **Tag name character set.** The original delimits the name purely by the
   next `>`; it imposes no character restriction. We accept any bytes up to
   `>`. (In practice the names are `[A-Za-z0-9]`.)
2. **Unterminated `<IMG` (no closing `>` on the line).** In the original,
   `strchr(...,'>')` returns null and the copy runs to the line's NUL, so the
   tag silently swallows the rest of the line. Because CLAUDE.md treats 1997
   files as untrusted and forbids silent truncation, our parser instead
   **throws `std::runtime_error`** on an unterminated `<IMG`. This is the one
   deliberate divergence from the binary; it only affects malformed input the
   shipped files never contain.
3. **`<IMG` with an empty name (`<IMG>`).** Treated as an image segment with an
   empty base name (the original would form `".plt"` and fail the lookup,
   drawing nothing); callers may skip empty names. Not present in any shipped
   file.
4. **Encoding.** Bytes are passed through as-is (Latin-1/ASCII); no transcoding.
