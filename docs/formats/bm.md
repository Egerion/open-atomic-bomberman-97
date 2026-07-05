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
  (`sub_41302D`: `while (col & 3) emit ' '`). Text art in the files
  (`CREDITS.BM`, `NETWORK.BM`) relies on this 4-wide expansion.
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
