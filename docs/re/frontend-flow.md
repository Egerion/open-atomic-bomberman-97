# BM95.EXE — front-end boot & screen-flow facts

Facts distilled from static analysis of `BM95.EXE` (Watcom C, imagebase
0x400000) and the shipped install. **Facts only — addresses and observed
behaviour, never copied code.** Provenance cited per entry. Companion to the
Phase 3 spine (`docs/adr/0004-frontend-screen-flow.md`).

## There is no FMV — the whole front-end is the assets we already parse

The install ships **no** `.smk` / `.avi` / `.mve` / `.flc` / `.fli` — confirmed
by an exhaustive `find` over `BOMBRMAN/`. Every front-end screen is one of the
formats the asset pipeline already decodes:

| Screen art | File | Format | Pipeline status |
|---|---|---|---|
| Interplay logo | `DATA/RES/IPLOGO.PCX` | PCX | loads today |
| High Score / studio logo | `DATA/RES/HSLOGO.PCX` | PCX | loads today |
| Title | `DATA/RES/TITLE.PCX` | PCX | loads today |
| Main menu | `DATA/RES/MAINMENU.PCX` | PCX | loads today |
| Draw / no-winner | `DATA/RES/DRAW.PCX` | PCX | loads today |
| (dead asset — see BONUS.PCX note) | `DATA/RES/BONUS.PCX` | PCX | loads today, but UNREFERENCED by the binary |
| Credits image bar | `DATA/RES/CREDBAR.PCX` | PCX | loads today |
| Head-to-head wipe | `DATA/ANI/HEADWIPE.ANI` | ANI | loads today |
| Boot/title music | `DATA/SOUND/TITLE.RSS` | RSS (id 1000, loop) | plays today, continuous across logos+title |
| Menu music | `DATA/SOUND/MENU.RSS` | RSS (id 1010, loop) | plays today, from menu entry |
| Menu-exit / accept sting | `DATA/SOUND/MENUEXIT.RSS` | RSS (id 10, one-shot) | plays today |
| Nav blip | `DATA/SOUND/LETTER1.RSS` | RSS (id 20, one-shot) | plays today |
| Title intro sting | `DATA/SOUND/GEN8A.RSS` | RSS (id 2800, one-shot) | plays today, before the title |

The text screens (`OPTIONS.BM`, `NETWORK.BM`, `CREDITS.BM`, `INPUT.BM`,
`MANUAL.BM`, `README.BM`, …) are **plain ASCII** with inline `<IMGxxx>` tags
that name a PCX to embed (e.g. `CREDITS.BM` opens `Atomic Bomberman
Credits\r\n…<IMGCREDBAR>…`). This `.BM` markup is the **one front-end format
the pipeline cannot parse yet** — but it only backs the deep menu leaves
(options/credits/manual), which are out of scope for the spine. (Provenance:
`head -c 200 CREDITS.BM`; `find` for codec extensions.)

## The boot LOADING dialog — PINNED (2026-07-09), BEFORE `sub_42B060` ever runs

The user's memory of a boot-time "loading" screen is CONFIRMED, but it is
**not** a fourth full-screen PCX bolted onto the IPLOGO/HSLOGO/TITLE chain —
it is a small **modal progress dialog**, shown **twice**, entirely before
`sub_42B060` (the logo/title chain) starts. The real app entry is
`sub_42BE22` (`__noreturn`, decompile 30947-30953):

```
void __noreturn sub_42BE22()
{
  sub_41095A();   // init — INCLUDES the two LOADING dialogs, below
  sub_42B060();   // IPLOGO -> HSLOGO -> TITLE (see "Top-level flow" below)
  sub_42B9CE();   // the menu
}
```

`sub_41095A` (the config/subsystem-init routine, decompile 14602-14663) calls,
in order: … `sub_42971F()` → **`sub_41D695()`** → **`sub_42896E()`** → …
(decompile 14622-14624). Those two calls are the loading dialogs:

1. **`sub_41D695`** (decompile 21653-21689) — `sub_4124A4(201)` (getmessage
   201 = **"Loading data..."**, MESSAGES.TXT), then opens `MASTER.ALI` and
   reads it in a loop, calling `sub_412E33(100*read/total, ...)` each
   iteration — a **live percent** readout tied to real bytes read.
2. **`sub_42896E`** (decompile 28498-28556) → **`sub_4287B9`**
   (decompile 28395-28423) — `sub_4124A4(200)` (getmessage 200 = **"Loading
   sound..."**), then FIVE `sub_412E33(pct, ...)` calls at fixed percents
   (5/20/40/60/80/100) as it preloads SOUNDLST groups 200/400/700/1400/2300 —
   a **coarse, hardcoded** percent sequence (not measured), matching this
   phase's much smaller/faster workload.

Both route through the **same generic dialog-window primitive**
`sub_43C734` (the same one the Yes/No confirm below and several other
overlays use) and the **same percent-bar renderer** `sub_412E33`
(decompile 16157-16211): a small window (`sub_43C734(200, 8·h, 360, 256, 4)`)
captioned **"Completion"** (`aCompletion`), with a `%d` readout and a
two-tone filled/unfilled bar (`sub_43D1C0` × 2, widths `3·pct` /
`3·(100-pct)`). **This is drawn programmatically — there is no LOADING*.PCX
or similar asset anywhere in the install or the decompile** (confirmed by an
exhaustive `find`/grep over both); it is a DOS4GW-style dialog box, not a
full-screen `sub_42A088` image. Neither `sub_41D695` nor `sub_42896E`
touches the boot music (`sub_42741E(0x3E8)`, started later inside
`sub_42B060` — see "Top-level flow" below), so the two LOADING dialogs run
in silence.

**Port status: DONE, chrome now pixel-pinned (2026-07-09 chrome pass).**
`GameApp::init` (`game_app.cpp`) flashes a small dialog
(`draw_boot_loading_dialog`) at the same two points in the same order: once
captioned "Loading data..." immediately before `assets_.load()`, and once
captioned `getstring(200)` ("Loading sound...", read from the now-loaded
MESSAGES.TXT) immediately before `audio_.init()`. Both flashes now render
through the pinned `sub_43C734` chrome (below) with the real FONT6 glyph
textures — FONT6 is loaded standalone before the first flash, matching the
CONFIRMED init order (`sub_41095A` pins FONT6 via `sub_414DF4` before it
calls either loading dialog; see "FONT6 timing" below), correcting the
earlier port's assumption of a font-readiness gap at the first flash.
**Documented simplification (unchanged):** our loaders have no per-file/
per-byte progress callback (`AssetStore::load` is one monolithic try-block)
and complete in well under a second on modern hardware, so each flash
presents the bar already full (100%) for one rendered frame rather than
animating a real or synthetic percent — inventing progress data we don't
have would be less faithful than a same-order same-caption flash. Skipped in
`--demo` mode (matching that `audio_.init` is also skipped there). No PCX
asset dependency, no `libs/sim` involvement.

### The `sub_43C734` dialog-chrome primitive — PINNED (2026-07-09)

`sub_43C734` is the shared window-object constructor both the boot LOADING
dialog (`sub_412E33`'s percent-bar window) and the Yes/No confirm
(`sub_41456C`) open through. Reading its body plus every accessor that
touches the same struct offsets (`sub_43DE0C`/`sub_43DE28` — width/height
getters; `sub_43D398` — the bounds-clamp/placement step; `sub_43D4D0`/
`sub_4428E4` — the generic dirty-rect blit, not a border draw) resolves the
full geometry/colour contract, and CORRECTS the parameter order an earlier
investigation pass guessed:

**Signature (CORRECTED): `sub_43C734(a1=y, a2=height, a3=width, a4=colormode,
a5=flags)`** — NOT `(x, y, width, ...)`. Proof: the master 640×480 root
window is created at `sub_414DF4` (@0x417523, the graphics-init routine) as
`sub_43C734(0, 480, 640, byte_495390[0], 1)` — only readable as
`(y=0, height=480, width=640, black, flags)`; as `(x, y, width, ...)` it would
place a 640-wide window at `y=480` on a 480-tall screen, entirely off-screen.
Every other call site is consistent with this reading once re-parsed:
`a3` always lands on the literal pixel width (200/360/450/300/600/…), matches
`sub_43DE0C`'s offset-24 "width" accessor at every use, and the two dialogs
this task cares about both compute `a2` as an explicit height expression
(`8·fontheight` for the loading dialog, `4·fontheight+64+promptheight` for the
confirm) rather than a Y screen-position. **`a1` (the "y" slot) is X in the
percent-bar's own reading only insofar as that call hardcodes it (`200`); the
confirm dialog instead computes `a1 = (screenH − windowHeight) / 2` —
i.e. `a1` genuinely is used as the window's Y origin, vertically centering the
box.** (Provenance: `sub_414DF4` @ 0x414DF4/0x417523 window-create line;
`sub_43DE0C`/`sub_43DE28` @ 0x43DE0C/0x43DE28 struct-offset accessors,
confirmed against `sub_4327DC`'s own-bounds check `x+w <= sub_43DE0C(win)`,
`y+h <= sub_43DE28(win)`.)

**X placement is NOT an explicit parameter anywhere in this family.**
Neither `sub_43C734` itself nor either dialog caller (`sub_412E33`,
`sub_41456C`) ever computes/passes a horizontal position — `sub_41456C`
computes ONLY the vertical center `(dword_464A6C − height)/2` (`dword_464A6C`
= 480, confirmed screen height, corrected from an earlier pass's "screen
width" misreading — `dword_464A70` = 640 is the width, set one line above it
in `sub_414DF4`). `sub_43D398` (the placement/clamp step, called from the
constructor right after the background fill) DOES clamp an X value (its
internal `v3`) against the screen-width bound `dword_4A3BE8`, so a
horizontal position genuinely exists and is genuinely clamped — but its
SOURCE register is one IDA's decompiler explicitly marks lost
(`// 43D3C7: variable 'v3' is possibly undefined`, i.e. a stack-spilled value
across the constructor's slot-search loop that the decompiler failed to
recover). `TODO(RE): sub_43D398`'s exact X-default formula needs a
disassembler pass (not available in this environment) to pin bit-for-bit; the
port instead centers each dialog horizontally against the 640-px screen
width, which is faithful to every visual call site's evident intent (nothing
in the binary ever repositions these dialogs off-center) even though the
literal source expression for that default isn't independently confirmed.**

**There is NO border/frame draw anywhere in this primitive.** The
"chrome" is exactly two steps, both already in the constructor:
1. `sub_43D1C0(id, 0, width, 0, height, fillColour)` — ONE flat filled
   rectangle spanning the whole window, in `fillColour`.
2. `sub_43D398(id, y)` — clamps/stores the final on-screen rect and blits the
   (still just flat-filled) buffer via `sub_43D4D0`/`sub_4428E4`, a generic
   dirty-rect pixel copy, not a decorative draw.

No outline, no drop shadow, no bevel anywhere in the WINDOW body — the
earlier port's guessed light-frame `SDL_RenderRect` outline around the
loading-dialog panel has no basis in the decompile and is removed. (The
BUTTON widget, `sub_432298` below, *does* have a real bevel — that is a
property of buttons, not of `sub_43C734` windows in general.)

**Fill colour resolution (a4=colormode).** `sub_43C734`/`sub_43D1C0` share
one decode: `a4==256` → the THEME DEFAULT fill, `(unsigned
__int8)byte_495390[dword_45C46C]`; `BYTE1(a4)!=0` (high byte set) → an
indexed palette lookup via `dword_45C068[a4 & 0xFFFF]`; otherwise `a4` is
used as a raw palette index directly. Both boot-chrome dialogs here pass
literal `256` — the theme default. `dword_45C46C` is a **fixed literal LUT
offset set once at window-system init** (`sub_414DF4`'s constructor block,
same as the button bevel colours below) — **`10570`**, which decodes via the
established RGB555-offset formula (`docs/re/results-and-options.md` "screen-
ink byte globals") to `r5,g5,b5 = 10,10,10` → **RGB (82, 82, 82)**, a
mid-dark grey. This is the ONE fill colour both boot dialogs use.

**Flags (a5) do not affect visible appearance.** Loading dialog passes `4`;
the confirm dialog passes `20` (0x14 = bits 2|4). Every flags-bit tested
anywhere in the constructor or its siblings (`sub_43D2A4`, `sub_43D4D0`)
governs internal z-order/redraw-callback bookkeeping (e.g. `a5&1` merges in
a default-flags global before storage; `a5&4==0` triggers a stacking-reorder
insertion pass) — none gates a border, shadow, or fill-colour branch. So the
flags difference between the two dialogs (4 vs 20) is a genuine but
INVISIBLE difference (window-manager stacking behaviour only); the port does
not need to (and cannot meaningfully) reproduce it.

**Window default fill and the button bevel colours are siblings from the
SAME init block** (`sub_414DF4`, pseudo.c ~43614-43626 — all four are
literal LUT offsets assigned once, never reassigned elsewhere):

| global | LUT offset | r5,g5,b5 | decoded RGB | role |
|---|---|---|---|---|
| `dword_45C46C` | 10570 | 10,10,10 | **(82, 82, 82)** | window default fill (`a4==256`) |
| `dword_45C470` | 15855 | 15,15,15 | **(123, 123, 123)** | button bevel "light" tone |
| `dword_45C474` | 8456 | 8,8,8 | **(66, 66, 66)** | button bevel "dark" tone |
| `dword_45C478` | 21140 | 20,20,20 | **(165, 165, 165)** | button label text-shadow ink |

(Provenance: literal assignments @ pseudo.c ~43614-43626, inside the same
window-subsystem init block that also sets `dword_45C468=1`
(`dword_45C468` gates every accessor in this family — "is the window system
initialized"); decode formula per `docs/re/results-and-options.md` "screen-
ink byte globals".)

### `sub_432298` — the button widget (PINNED)

Buttons ARE where a real 3D bevel exists. `sub_432298(win, x, ?, y, ?, ?,
hotkeyChar, labelPtr, ?)` (9 params; the `?` slots are `-1` sentinels at both
confirm-dialog call sites and not needed to reproduce the visible geometry):

- **Size is text-derived, not caller-specified:** width = `measure(label) +
  16`, height = `fontheight + 6` (pseudo.c ~35197-35199, `v13`/`v15`).
- **Two 8-bit-alpha bitmaps are built per button — "up" and "down"** — each
  gets the SAME two-ring inset bevel (`sub_44240C` called twice, insets 1px
  and 2px from the edge) using the two grey tones above, **colour order
  swapped between the two bitmaps**: light-then-dark for "up" (top/left lit,
  bottom/right shadowed — a raised look), dark-then-light for "down"
  (a pressed/sunken look). A 1px flat outline in black (`byte_495390[0]`)
  closes each bevel (`sub_442384`, the rect-fill primitive, drawn at the
  bitmap's own edge). The port draws only the "up" state (there is no
  press-animation requirement here) as a 2px inset bevel: `(123,123,123)`
  top/left ring, `(66,66,66)` bottom/right ring, 1px black outline.
- **Label ink uses the text-shadow colour** `dword_45C478` = **(165, 165,
  165)** (`dword_45C378(...)|0x10000` call immediately before each bevel
  pass) — a lighter grey than pure white, distinct from the window-fill grey.
- **Confirm-dialog button positions (from `sub_41456C`, both CONFIRMED
  literal expressions):** Yes at `x = width/2 − 80`, No at `x = width/2 +
  22` (both window-relative, `width` = the dialog's own computed width
  `v30`), same `y = height − 32 − fontheight − 6` for both (window-relative,
  bottom-anchored). Hotkey chars are ASCII `'Y'`=89, `'N'`=78 — cosmetic
  labels only; the REAL accept/cancel logic lives in `sub_41456C`'s own key
  loop (already pinned below), not in the button widget.

(Provenance: `sub_432298` @ 0x432298 pseudo.c 35167-35276; `sub_44240C` @
0x44240C two-colour inset-rect primitive; `sub_442384` @ 0x442384 filled-rect
primitive; button geometry read directly from `sub_41456C` @ 0x41456C
pseudo.c 17211-17216.)

### The percent-bar dialog, `sub_412E33` — geometry CONFIRMED, one residual gap

Re-verifying against the corrected `sub_43C734` signature: the window is
`sub_43C734(200, 8·h, 360, 256, 4)` = **y=200, height=8·fontheight, width=360**
(`h` = the active font's char-height getter, `dword_45C37C`), themed grey
fill `(82,82,82)`, x auto-centered (see the X-placement gap above). Contents,
window-relative (all CONFIRMED by direct read, pseudo.c 16181-16204):

- **Caption "Completion"** (`aCompletion`): `sub_41696C(win, text, (360−tw)/2,
  tw, 1.5·h, ink=white(255,255,255), bg=black)` — horizontally centered, y =
  `h/2 + h` = 1.5·fontheight.
- **"%d" percent readout:** drawn via a SECOND `sub_41696C` call at **y =
  3.5·fontheight** (`v12+3·v13`, `v12=h/2`) — **CORRECTION to an earlier
  investigation pass:** its ink is `byte_49D37A` (**yellow**, RGB ≈
  (255,255,90) — a fifth screen-ink global, same LUT-offset decode, offset
  `32747` → r5,g5,b5=31,31,11), NOT the caption's white. The two text draws
  use two different inks.
- **Two-tone bar** (`sub_43D1C0` × 2, already pinned): filled segment `x=31,
  width=3·pct, y=5·h+1+h/2, height=h−1`, colour `byte_49A624` mid-grey
  (168,168,164); unfilled remainder `x=3·pct+31, width=3·(100−pct)`, same
  rect, colour black.
- **`TODO(RE): sub_43D080`'s exact role** — a call between the caption and
  the readout (`sub_43D080(win, 5·h, 6·h, white)`) that this pass could not
  fully resolve: its signature takes only (win, a2, a3, colormode) yet reads
  a THIRD geometry value from the window's own stored fields internally
  (offsets 6/11, "possibly undefined" in the decompile — the same
  register-spill loss as `sub_43D398`'s X value above), consistent with
  either a full-width horizontal rule/divider or a redundant background
  strip in the 5.5h–6.5h band, immediately overpainted by the bar draw one
  line below it. Visually inconsequential either way (the bar redraws that
  same band), so the port omits it rather than guess a specific 1px line;
  flagged here rather than silently dropped.

**Port status: chrome now pinned and reimplemented** (`draw_boot_loading_dialog`,
`libs/game/src/game_app.cpp`) — grey fill window, "Completion" caption in
white, "%d" readout in yellow, two-tone bar in the decoded greys, all via
FONT6 (see the timing trace below), keeping the already-documented
"presented already-complete" percent simplification (no per-byte load
callback) and the boot-order/caption content unchanged.

### FONT6 timing — CONFIRMED ready before BOTH loading-dialog flashes

`sub_41095A` (pseudo.c 14602-14663, the config/subsystem-init routine) calls,
in exact order: … `sub_413B1C()` (14618) → `sub_4127FF()` (14619) →
**`sub_414DF4()` (14620)** → `sub_406086()` (14621) → `sub_42971F()` (14622)
→ **`sub_41D695()` (14623, "Loading data...")** → **`sub_42896E()` (14624,
"Loading sound...")** → … `sub_414DF4` (@0x414DF4, pseudo.c 17523-17602) is
the graphics/window-system init: it creates the master 640×480 root window
(`sub_43C734(0,480,640,black,1)`, confirming the corrected signature above)
AND, as its very last statement, pins the active font via **`sub_431E9C(6)`**
(pseudo.c 17600) — i.e. FONT6 is loaded/pinned and the window system is live
**before `sub_414DF4` even returns**, which is itself two calls before either
loading dialog runs.

**Conclusion (CORRECTS the earlier port comment):** FONT6 is ready for the
**FIRST** loading-dialog flash too, not just the second. The earlier
`draw_boot_loading_dialog` comment ("no PCX/font asset is loaded yet at this
point … uses SDL's built-in debug font") was wrong about FONT6 specifically —
it was right that MESSAGES.TXT isn't loaded yet (so the first flash's caption
falls back to literal text, unchanged by this pass), but the FONT ASSET
ITSELF has no such gap. **Port fix:** `GameApp::init()` now calls
`AssetStore::load_frontend_font()` (a small standalone FONT6.FON loader,
`asset_store.cpp`) and builds `front_font_` from it immediately after
creating the SDL renderer, before the first `draw_boot_loading_dialog` call —
matching `sub_41095A`'s real order. Both loading-dialog flashes now render
with the real FONT6 glyph textures; `SDL_RenderDebugText` is no longer used
by either dialog. (Provenance: call order pseudo.c 14602-14663 read in full;
`sub_414DF4` body pseudo.c 17523-17602, `sub_431E9C(6)` @ 17600.)

### Escape/Quit-row confirm dialog — chrome now exact

The quit-confirm modal (`sub_412987` → `sub_41456C`, already pinned above for
its behaviour/sound path) now uses the SAME `sub_43C734` chrome as the
loading dialog (grey `(82,82,82)` fill, no border) sized from the ACTUAL
button label extents (`FontTextures::measure` on getstring(26)/(25) = " Yes
"/" No ", matching `v29 = max(text-width, 80)` and `v30 = v29+64`, `v32 =
4·fontheight+64+fontheight` for a one-line prompt), vertically centered
(`y=(480−v32)/2`) and horizontally centered against the 640-px screen width
(the X-placement gap above), with the two buttons drawn via the pinned
`sub_432298` bevel geometry and colours instead of a single inline text
line. The prompt text itself (getstring(10)) is drawn window-relative at
`y = fontheight+32`, horizontally centered, in the general white ink
(`byte_49D38F`, already pinned) — replacing the previous ad hoc
`" Y/ENTER =… N/ESC =…"` single-line hint with the real prompt-plus-two-
buttons layout. Behaviour (Y/Enter/Space confirm, N/Escape cancel, the sound
path, the 4 s exit delay) is UNCHANGED — this pass is chrome-only.

## Top-level flow — `sub_42B060` then `sub_42B9CE`

The two calls that make up the whole app entry sit back-to-back
(`sub_42B060(); sub_42B9CE();`, decompile ~30951):

1. **`sub_42B060` — the boot presentation.** Body (paraphrased):
   ```
   sub_42741E(0x3E8)              ; START THE BOOT MUSIC (1000), FIRST OF ALL
   if (!sub_413D01())            ; skip-logos gate (returns global dword_460260)
       show("iplogo", wait=1)    ; sub_42A088(aIplogo, 1)
       show("hslogo", wait=1)    ; sub_42A088(aHslogo, 1)
   sub_427BFB(2800)              ; play SOUNDLST group 2800 (title intro sting)
   show("title", wait=1)         ; sub_42A088(aTitle, 1)
   ; then teardown, return to caller
   ```
   So the boot order is **IPLOGO → HSLOGO → TITLE**, each a full-screen image
   that **waits for a keypress or the `getvalue(12)` = 7 s timeout** (below), and
   the two logos are skipped entirely when the skip-logos flag (`dword_460260`, a
   `-nologo`-style global set at startup, `sub_413D01` — confirmed a bare `return
   dword_460260;`) is set. The title sting fires *before* the title image is
   drawn. (Provenance: `sub_42B060` @ 0x42B060; `sub_413D01` @ 0x413D01.)

   **The title sting is a GROUP random pick, not a fixed clip (FIXED).**
   `sub_427BFB(2800)` picks a random member of the **contiguous** SOUNDLST run
   starting at 2800 — the "ATOMIC BOMBERMAN!" intro group **2800..2810** (GEN8A,
   GEN8B, GEN8C, GEN8C2, ZAI08A..ZAI08G; the file's `; 2899 is the last intro`
   comment bounds it). Each boot can therefore voice a different take. The port's
   `run_boot_attract` now uses `play_random_in_range(2800, 2899)` (one-shot SFX
   voice, over the still-running boot music), which hits exactly the loaded ids in
   that span — matching the group pick. (An earlier port played the single id 2800
   via `play(2800)` — correct clip, no variety; widened to the group.)

   **The boot flow is STRAIGHT-LINE — there is NO attract loop.** `sub_42B060` is
   a single pass: start music → (skip-gate) IPLOGO, HSLOGO → sting → TITLE →
   **return**. It does not re-show the logos/title; after the title it returns to
   its caller, which enters the menu (`sub_42B9CE`, `__noreturn`, starting the
   1010 menu music). Each screen advances on a key OR the 7 s timeout, and the
   **title's timeout synthesizes Enter and proceeds to the MENU** — it does NOT
   restart the intro. The port's `run_boot_attract` therefore presents IPLOGO →
   HSLOGO → TITLE once and returns `Advance` (key or 7 s timeout) so `run_app`
   drops into `present_menu`; Back/Quit short-circuit out. (An earlier port
   looped the chain on the title timeout — removed; it was never in `sub_42B060`.)

   **The boot MUSIC model (corrected).** The very first line is *not* misc init:
   it is `sub_42741E(0x3E8)` — **the music player** (`sub_42741E` @ 0x42741E)
   starting SOUNDLST **1000** (`0x3E8` = TITLE.RSS). It is called **once**, before
   any screen, and the track then **plays CONTINUOUSLY across IPLOGO → HSLOGO →
   TITLE** — the logos are *not* silent. `sub_42741E` is the ONLY music call in
   `sub_42B060`; nothing restarts it per screen. `sub_42A088` (the screen
   primitive, below) never touches music. A naive port that starts music inside
   the screen primitive would (re)start the track on every screen, leaving the
   logos silent until the title — the bug this note exists to prevent. The menu
   (`sub_42B9CE`) later replaces it with the menu track (1010); see below.

   **Music is a LOOPING channel, distinct from one-shot SFX (CONFIRMED).**
   `sub_42741E` builds `<name>.rss` from the SOUNDLST id table
   (`dword_463094 + 4*id`), loads it (`sub_411D17`), and plays it via
   `sub_4273A4`, which **frees the currently playing music first**
   (`sub_427342`: releases the single music handle `dword_463064`) and starts the
   new one with **loop count `0xFFFF`** (`sub_41A3C9(handle, 0xFFFF)`) — i.e. an
   infinitely looping track, and starting a new one *replaces* the old. Contrast
   the SFX players `sub_427961` / `sub_427BFB` (the sting/blip): identical bodies
   that pick a random group member, but play through `sub_427859` / `sub_427B36`
   which set **loop count `0`** — one-shot voices on a separate pool, mixed via
   `rand_()` (C `rand`, presentation-side; never the sim RNG). So: **music = one
   looping channel that new tracks replace; stings/blips = one-shot voices** —
   exactly the split `AudioEngine::start_music` (looping music stream, re-queued
   on drain) vs `AudioEngine::play` (one-shot SFX pool) implements.
   (Provenance: `sub_42741E`/`sub_4273A4`/`sub_427342` @ 0x42741E/0x4273A4/
   0x427342; `sub_427961`/`sub_427BFB`/`sub_427B36` @ 0x427961/0x427BFB/0x427B36.)

2. **`sub_42B9CE` — the main menu (`__noreturn`).** Never returns; it *is* the
   application loop the logos/title fall through into. It draws `MAINMENU.PCX`
   (palette `mainmenu.plt`, loaded at ~30760) and drives the menu; entering a
   sub-screen dispatches to routines like `sub_41302D(aCreditsBm)` (a `.BM`
   text-screen viewer). Match start and the results screens are reached from
   inside this loop. (Provenance: `sub_42B9CE` @ 0x42B9CE; `aCreditsBm` use
   @ 30915.)

   **The MENU music id is `0x3F2` = 1010 (CONFIRMED, id corrected).** On entering
   the menu, `sub_42B9CE` plays the menu track **once** via `sub_42741E(0x3F2)`
   guarded by a run-once flag (`v14`: `if (v14) { sub_42741E(0x3F2); v14 = 0; }`).
   `0x3F2` = **1010** = MENU.RSS (SOUNDLST label `menu`) — this *switches* the
   looping music from the boot track (1000) to the menu track and keeps it
   playing while in the menu. **Do not confuse this with `0x3FC` (1020):** that id
   is played by the **Play/round handler `sub_42A3F6`** at its entry
   (`sub_42741E(0x3FC)`, decompile ~29696) — the "win" track for a game round,
   NOT the menu; the same handler plays `0x46A` (1130, `draw`) on the draw branch
   (~29820). The SOUNDLST labels (1000=title, 1010=menu, 1020=win, 1130=draw)
   therefore **agree with the code** — there is no label/code mismatch here; the
   only correction is that the menu-entry track is 1010, played in `sub_42B9CE`,
   while 1020 belongs to `sub_42A3F6`'s round path. (Provenance: `sub_42B9CE`
   `sub_42741E(0x3F2)` call; `sub_42A3F6` `sub_42741E(0x3FC)`/`(0x46A)` calls.)

## The generic Screen primitive — `sub_42A088(name, wait)`

Every full-screen image goes through one routine, `sub_42A088(a1=name,
a2=wait)` @ 0x42A088. It is the exact primitive the spine's `Screen` mirrors:

```
sub_415CE3()                     ; clear the back buffer (memset framebuffer 0)
sub_415C1F()                     ; bump frame + flip bookkeeping
palette = load(sprintf("%s.plt", name))   ; aSPlt_0 = "%s.plt"
apply_palette(palette)           ; sub_41522D: copies + (>>2) to VGA 6-bit
sub_429FF1()                     ; draw pass (sets dword_464994|=0x40; sub_429F1A)
sub_415C1F(); sub_41043C()       ; flip / present
key = getkey()                   ; sub_4102B7
if (wait) {
    t0 = time()
    loop {
        key = getkey()                          ; sub_4102B7
        if (elapsed + getvalue(12) < deadline)  ; ATTRACT TIMEOUT via getvalue(12)
            key = 13                            ; force "advance" (Enter)
        if (key != -1 && key != -2) sound(20)   ; sub_427961(20) nav blip
        if (key >= 0x1B) break                  ; Esc(0x1B) / others exit wait
        if (key == 13) { sound(10); return }    ; Enter selects, plays sound 10
        ; Space (32) also selects (checked after the >=0x1B break)
    }
}
```

Key facts the spine reproduces:

- **Palette is derived from the screen name** (`<name>.plt`), the image itself
  is drawn by the shared draw pass — so a "screen" is fundamentally just a
  name plus a wait flag, exactly the data-driven shape we adopt.
- **A waited screen appears INSTANTLY — sub_42A088 CUTS, it does not wipe or
  fade.** The present is exactly three steps: set the palette (`sub_41522D`,
  instant — its `>>2` is the **8→6-bit VGA palette conversion**, NOT a fade
  loop), blit (`sub_429FF1`; the `sub_429F1A` it calls is a ROT13 debug-watermark
  drawer, NOT a transition), and flip (`sub_41043C`). There is no HEADWIPE / fade
  on a waited screen — logos, title, results, and the `.BM` viewer all just
  replace the previous image. So the port presents these screens with a **cut**;
  the only screen-to-screen wipe in the front-end is the menu→match select wipe
  (`present_menu`), which is a separate, intentional effect. (Provenance:
  `sub_41522D` @ 0x41522D; `sub_429FF1`/`sub_429F1A` @ 0x429FF1/0x429F1A;
  `sub_41043C` @ 0x41043C.)
- **`sub_42A088` does NOT touch music** — it loads a palette, blits, and runs
  the wait loop. The background track is a separate concern (`sub_42741E`), so
  the port's `Screen` carries no music id and never starts a track; the boot
  caller owns the continuous music (see the boot-music note above).
- **The wait is "keypress OR timeout"**: the timeout is **`getvalue(12)` = 7
  SECONDS** — CONFIRMED. VALUELST ships the line **`12,7`**, and `sub_42A088`'s
  wait loop compares C `time_()` (whole seconds) against `start + getvalue(12)`,
  so the auto-advance fires 7 s after the screen appears. When it elapses the
  code synthesizes Enter (13) and advances; on the title that means it proceeds
  to the **menu** (the flow is linear — no re-run). The port sets both the logo
  and title dwell to **7000 ms** (`getvalue(12)·1000`), keypress-skippable.
  (Provenance: VALUELST `12,7`; `sub_42A088` wait loop @ 0x42A088.)
- **The timeout auto-advance is AUDIBLE — it plays SFX 20 + 10, not silence
  (FIXED).** The synthesized Enter (13) is NOT a quiet exit: it re-enters the
  same sound path a real accept takes. The wait loop runs `if (key != -1 && key
  != -2) sub_427961(20)` for the forced `key=13` (the nav blip fires), then the
  `key==13` branch reaches `sub_427961(10)` (the accept sting). So a screen that
  times out plays **BOTH the blip (20) and the accept (10)**, exactly like a
  keypress. The port's `Screen::update` previously just set `done_` on the dwell
  (silent); it now plays `play(20)` then `play(10)` on the timeout (guarded by
  `!done_` so a key that already accepted this frame does not double-fire), so the
  dwell advance is audible. The blip/accept are one-shot SFX, so the looping boot
  music is untouched. (Provenance: `sub_42A088` wait loop: `if (result < v12) v8
  = 13;` then the shared `sub_427961(20)`/`(10)` path.)
- **Skip keys + their SFX (ported exactly).** Reading the wait loop precisely:
  ANY real key (`key != -1 && key != -2`) fires `sub_427961(20)` (the nav blip);
  then the **accept keys Enter (13), Space (32), Escape (0x1B/27)** each reach the
  accept path `sub_427961(10)` and leave the wait, while other codes 1..26 keep
  waiting after their blip. So the confirmed behaviour is: **Enter/Space/Escape
  accept → SFX 10; any other key → SFX 20 blip only**. The port's
  `Screen::on_key(key)` mirrors this — blip 20 on every key, sting 10 + finish on
  Enter/Space/Escape — and crucially **plays these as one-shot SFX (`play`), so
  the looping music is never stopped by a skip; only the screen changes.**
- `sub_427961` and `sub_427BFB` are **identical SOUNDLST group players** (pick
  a random member of the contiguous id run starting at the argument, load the
  named `.RSS`, play it **one-shot** via `sub_427859`/`sub_427B36`, loop count 0)
  — so the numeric arguments (2800, 1700, 10, 20) are **sound ids**, matching
  `AudioEngine::play`'s one-shot id model (distinct from the looping
  `start_music`). (Provenance: `sub_427961` @ 0x427961, `sub_427BFB` @ 0x427BFB —
  same body.)

## The main-menu items — `sub_42B9CE` (CONFIRMED)

The `__noreturn` menu loop draws `MAINMENU.PCX` (palette `mainmenu.plt`) with
an **animated cursor sprite** blitted on top — `sub_41D957(aBombTriggerGre)` /
`sub_41DAA7` resolve `"bomb trigger green"` (the trigger-bomb ANI) as the
highlight, positioned at `getvalue(700)` x / `getvalue(701)+getvalue(702)` y.
The **item labels are baked into the PCX art**; only the cursor moves. The
selection variable (`v10` in the decompile) runs **0..6** — seven rows — and
Enter/keys dispatch it (Provenance: `sub_42B9CE` @ 0x42B9CE):

| `v10` | handler | address | what it is | `.BM`? |
|---|---|---|---|---|
| 0 | `sub_42A3F6` | 0x42A3F6 | **Play/Start** — runs a match then the results flow (owns DRAW/RESULTS/VICTORY, below) | no |
| 1 | `sub_42B0CE` | 0x42B0CE | setup screen A (`sub_42741E(0x410)`) | no |
| 2 | `sub_42B47D` | 0x42B47D | setup screen B (sibling, same `sub_42741E(0x410)`) | no |
| 3 | `sub_4080DC` | 0x4080DC | **CORRECTED: the Options screen** (19-item settings list — team play, random start, key-remap, …), NOT a map editor; see `docs/re/results-and-options.md` §3 | no |
| 4 | `sub_41302D(aCreditsBm)` | — | **Credits** — the `.BM` text viewer on `credits.bm` | **yes** |
| 5 | `sub_41431C` | 0x41431C | **CORRECTED: a generic HELP FILE BROWSER**, not Roulette — lists every `*.BM` in the install (incl. ROULETTE.BM as one topic among ~10) via a real DOS glob; see `docs/re/results-and-options.md` §4 | (BM) |
| 6 | `sub_412987` | 0x412987 | **Quit** — exit confirm + exit sting `sub_427BFB(2600)` | no |

**Row 3 and row 5 corrected (2026-07-08).** A prior pass mislabeled
`sub_4080DC` as the map editor and `sub_41431C` as "roulette" — reading
their bodies shows neither is true. `sub_4080DC` has no editor code and
never references `EDITOR.BM`; it is the interactive Options screen (team
play / random start / conveyor speed / key-remap / …), fully RE'd in
`docs/re/results-and-options.md`. `sub_41431C` has no roulette-wheel
drawing code either; it globs `*.BM` and opens whichever the player picks
in the existing `.BM` viewer — ROULETTE.BM is just one of ~10 listed help
topics. The **real** Goldman Roulette Wheel mini-game is a separate,
menu-unreachable routine (`sub_4034BC`) that runs at the head of the Play
flow (top of `sub_410F81`) when `goldman=1` and a gold player is pending —
fully RE'd in `docs/re/goldman-roulette.md`. **The map editor is now
LOCATED (2026-07-08, second pass):** it hangs off the hidden Ctrl+E ×6
trigger in this very menu loop (raw code 5, six consecutive presses →
`sub_40330E` → `sub_403184` → the full scheme editor `sub_4028D2` with a
real `.SCH` writer `sub_403C16`) — chain and controls in
`docs/re/results-and-options.md` §5. `EDITOR.BM` remains a help text only
(the string appears nowhere in the binary).

**Menu key → sound → action table (raw `sub_4102B7` codes, EXHAUSTIVE, CONFIRMED
`sub_42B9CE`).** The nav blip fires *first* for every real key, then the dispatch:

| key (v8) | sound | effect |
|---|---|---|
| any real key (`≠ -1, ≠ -2`) | `sub_427961(20)` blip | resets idle timer, then falls to the dispatch below |
| `13` Enter | `sub_427961(10)` accept | select current row (v9=1) |
| `32` Space | `sub_427961(10)` accept | select current row (v9=1, via `LABEL_59`) |
| `17` / `27` Escape | `sub_427961(10)` accept | set v10=6 (**Quit**) and select |
| `328` Up | (blip only) | `--v10`, wraps 0→6 |
| `336` Down | (blip only) | `++v10`, wraps 6→0 |
| `280` | `sub_427961(10)` accept | jump v10=3 (**Options**, corrected) and select |
| `315` | `sub_427961(10)` accept | jump v10=5 (**Help browser**, corrected) and select |
| `286` | (blip only) | break the loop → run the current selection |
| `288` | (blip only) | `sub_413D45()` — a toggle, no accept |
| `5` = **Ctrl+E** (×6 in a row) | `sub_427961(10)` accept | `sub_40330E()` — **the MAP EDITOR** (corrected: code 5 is the Ctrl+E ASCII control code, the counter is `++v15 > 5` = six consecutive presses, and the target is the scheme editor, NOT a campaign — full chain in `docs/re/results-and-options.md` §5) |
| idle > `getvalue(92)` s | (none) | **ATTRACT MODE** (corrected — it does NOT simply run the current row): sets the attract flag `dword_464938`, saves the roster/level/team config, forces v10=0 and dispatches Play as an AI-only demo match — see "Attract mode" below |

`getvalue(92)` = **30** (VALUELST `92,30`) — the menu idle timeout, gated by
`getvalue(92) > 5` (the file's own legend documents it as the attract-mode
delay, with < 5 disabling attract entirely), distinct from the waited-screen
`getvalue(12)` = 7. Only Up (328) and Down (336) plus 286/288 stop at the
blip; **every SELECT (Enter/Space/Escape/280/315) plays the accept sting
10**. On the Quit row (6, whether reached
by Escape/17/27 or Enter on the row), the dispatch calls `sub_412987`, which pops
a confirm dialog and — on confirm — plays the exit sting `sub_427BFB(2600)` (the
2600..2699 "go outside and play now!" group) before sleeping 4 s and exiting.

So the confirmed public item set + order is **Play, net-setup A, net-setup B,
Options, Credits, Help browser, Quit** with an animated bomb-trigger cursor
(the map editor and the roulette wheel exist but have no menu row — hidden
trigger / automatic respectively).

### Escape / Quit-row dispatch — `sub_412987` -> `sub_41456C` — PINNED (2026-07-09)

Escape does **not** exit the app directly, and neither does Enter on the Quit
row — both just *select* row 6, and row 6's dispatch is `sub_412987`
(decompile 16018-16040), which pops a real **Yes/No confirm dialog** before
anything is torn down:

```
void sub_412987()
{
  sub_431178();                       // freeze/enter-dialog bracket
  v3 = sub_41456C(getmessage(10), byte_49A390, ...);  // "Are you sure...?" Y/N
  sub_431360();                       // thaw/leave-dialog bracket
  if (v3 == 1) {                      // Yes
    sub_427342();                     // stop the current music
    if (!sub_413D01())                // skip-logos flag NOT set
      sub_427BFB(2600);               // exit sting group (2600..2699)
      sub_452012(0xFA0);              // Sleep(4000 ms) — let the sting finish
    sub_4128C9(0);                    // __noreturn — the REAL process exit
  }
  // v3 == 0 (No): falls straight through, back to the menu loop. Nothing
  // else happens — no sting, no sleep, no exit.
}
```

`sub_41456C` (decompile 17129-17278) is the generic Yes/No dialog primitive
(also used by several other confirm sites in the binary): it draws a
`sub_43C734` window with **getstring(10)** = `"Are you sure you want to
exit?"` as the prompt and two buttons, **getstring(26)** = `" Yes "` and
**getstring(25)** = `" No "`. Its own key loop (decompile ~17220-17270) blips
(SFX 20) on every real key, then resolves the answer from the raw key code:
**Y/y (89/121), Enter (13), Space (32) → Yes** (returns 1); **N/n (78/110),
Escape (0x1B) → No** (returns 0); every other key is ignored and the dialog
stays up. So the terminal "confirm" sting (SFX 10) and the 2600 exit-sting
group only ever fire on an explicit **Yes** — Escape *inside* the dialog is a
**cancel**, not a second-level exit.

**Port fidelity of the menu sounds (FIXED 2026-07-08, RE-FIXED 2026-07-09
with the real confirm dialog).** `present_menu` now matches the table: Up/Down
play the blip 20; **Enter/Space play the accept 10 on EVERY row** including the
"inert" Editor/net-setup stubs (the original has no inert-row concept — every
row 0..6 is a live dispatch); **Escape plays blip 20 + accept 10, sets the
highlighted row to 6, and opens the SAME Yes/No confirm modal Enter-on-row-6
opens** (`quit_confirm`, `game_app.cpp`) — since 2026-07-09 drawn with the
pinned `sub_43C734` chrome ("Escape/Quit-row confirm dialog" below): a
centered grey window sized from the actual `getstring(26)`/`getstring(25)`
(" Yes "/" No ", fallback text if MESSAGES.TXT lacks those ids) button label
extents, `getstring(10)` as the prompt, and two real `sub_432298`-bevel
buttons — not the earlier flat single-line hint. Inside
that modal: **Y/Enter/Space confirm** — blip 20 (already fired on keydown) +
accept 10 + the exit-sting group `play_random_in_range(2600, 2699)` + a 4 s
`SDL_Delay` (mirroring `sub_452012(0xFA0)`, so the sting is audible instead of
being cut off by window teardown) before returning `AppInput::Quit`; **N/Escape
cancel** — accept 10 (the dialog's own dismiss sound) and the menu resumes,
nothing else happens. The attract-mode idle trigger is held off while the
modal is up (the original's dialog is itself modal/blocking, so an idle-timeout
demo match could not interrupt it either). The **Ctrl+E ×6 editor trigger**
(key 5 → `sub_40330E`; formerly mislabelled
here as a "campaign easter egg" — it opens the map editor, see
`docs/re/results-and-options.md` §5; the REAL campaign trigger is 'C'×5 on
the player-setup screen, `docs/re/campaign.md`) and the **280/315 direct
jumps to Editor/Roulette** targeted features not yet built at the time, so
they were documented gaps — not faked. The **WASD nav aliases** (`SDLK_W`/`SDLK_S` = Up/Down) are a
deliberate modern convenience: in the binary raw 'w'(119)/'s'(115) fall through to
`LABEL_44` (blip only, no move), so binding them to nav is a superset, not a
misrepresentation.

**Cursor anchor — PINNED.** The bomb-trigger cursor's blit is
`x = getvalue(700)`, `y = getvalue(701) + getvalue(702)*row` (decompile: `v11 =
getvalue(700); v1 = getvalue(701); v12 = getvalue(702)*sel + v1; blit("bomb
trigger green", v11, v12)`). VALUELST stores these as the **columns of one
multi-value row** — `700,332,140,38,0` — and the file's own legend labels them
`X, Y - first (top) item / YS - y-spacing / W - width`. The original
`getvalue(id)` (`sub_412135`) reads a **flat array** the loader (`sub_4121FF`)
fills by splitting each `id,a,b,c` line into consecutive slots, so
`getvalue(700/701/702/703)` == columns 0/1/2/3 of row 700 == `{332, 140, 38,
0}`. Confirmed cursor: **x = 332, y = 140 + 38·row**. (The menu's own idle
timeout is `getvalue(92)`, not the waited-screen `getvalue(12)`.)

**Spine mapping.** The polished menu keeps the seven rows in the original v10
order so the cursor anchor lands on the baked labels: Play→`Match`,
setup A→Options `.BM` help, setup B→Network `.BM` help, row 3 (Options,
corrected above)→inert stub, Credits→Credits `.BM`, row 5 (Help browser,
corrected above)→**live**: the generic `*.BM` help browser
(`GameApp::present_help_browser`, `HelpBrowser` in bmscreen.hpp — the
`sub_41431C`→`sub_414235` glob+list+viewer loop of
`docs/re/results-and-options.md` §4, dispatched inline with no wipe exactly
as `sub_42B9CE`'s `case 5` does), Quit→app exit. **Play, Credits, Help
browser, Quit are live**; row 3 remains a documented inert stub — its real
screen is the RE'd Options screen (`docs/re/results-and-options.md` §3, a
separate implementation effort), not a map editor as an earlier pass
assumed. The same browser also opens mid-round on F1
(`docs/re/in-match-shell.md` §1's `sub_42A16F(1)/(0)` bracket,
`GameApp::present_help_browser_modal`). The cursor is now the actual animated `"bomb trigger
green"` sprite (TRIGBOMB.ANI) at the pinned anchor, read live from VALUELST
row 700's columns (`ValueList::column_or`), with `{332,140,38}` as the
fallback; it falls back to a highlight bar only if TRIGBOMB.ANI is absent. The
Controllers/INPUT.BM help has no dedicated main-menu row in the original (it
hangs off the interactive controller-setup screen), so its `OpenControllers`
edge exists in the flow graph but is not bound to a menu row here — a documented
gap belonging to the deferred controller-setup UI.

## Attract mode — the menu idle timeout runs a LIVE AI demo match (CONFIRMED)

The question "does the original run a recorded DEMO match after the title
timeout, like other 1997 games?" is now settled — in two halves:

**1. There is NO recorded-input demo subsystem.** Negative, with evidence:
`sub_42B060` (boot) is a single straight line — music, logos, sting, title,
teardown, return — with no demo branch; the whole-decompile greps for
`demo`, `.dem`, `.rec`, `record`, `playback`, `attract` return zero relevant
hits (the only match is an unrelated ANI-loader diagnostic about a STAT
record); none of the ~93 `fread`/`fwrite` sites has a per-tick input-log
shape (they are stats/config/resource I/O); and the install contains no
`.DEM`/`.REC`/`.INP` or other unexplained files. The port's straight
IPLOGO → HSLOGO → TITLE → menu chain is faithful for the boot portion.

**2. But the MENU idle timeout is a real attract mode — a live, AI-only
match, not a plain "select the current row".** The idle path in
`sub_42B9CE` (pseudo.c 30887-30894) does five things before dispatching:
increments an attract counter (`dword_4642D8`), sets the **attract flag
`dword_464938` = 1**, snapshots the 10 slots' input-type/sub bytes and the
team flag via `sub_4224E2` @ 0x4224E2 (pseudo.c 24605-24620) plus the level
into `dword_4646B8`, forces team play off, and forces **v10 = 0** — so it
ALWAYS dispatches Play (`sub_42A3F6`), regardless of the highlighted row.
The flag then reroutes the whole Play flow:

- **`sub_410F81` (player setup) short-circuits** (pseudo.c 15125-15143):
  goldman wheel skipped (`!dword_464938` gate at 15048), all 10 slots set
  OFF, then `rand()%10 + 1` (clamped to a minimum of 3) slots are set to
  **COMPUTER**, the level is set to `rand() % getvalue(35)` **directly**
  (a specific random stage — bypassing the VALUELST 1150-1160 random-level
  enable flags, so attract can pick hockey rink / coal mine), and the
  function returns immediately — neither the player screen nor the LEVEL &
  ROUNDS screen (`sub_406DDE`, called at `sub_410F81`'s tail, 15516) is
  shown.
- **The round runs live** — the normal sim with AI players; nothing is
  scripted or replayed.
- **Any dispatched-through keypress aborts**: the round loop's key handler
  tail has `if (dword_464938) goto LABEL_34` (pseudo.c 29788 → 29737),
  which clears the pending gold player and sets `dword_464A68 = 2` — back
  to the menu.
- **Round end skips ALL outcome screens**: before the DRAW/RESULTS tiers,
  `if (dword_464938)` tears down and returns (pseudo.c 29812-29819,
  `LABEL_204`) — an attract match never shows DRAW/RESULTS/VICTORY. (The
  related 6 s auto-advance on those screens is gated by `sub_42247A` — an
  **all-AI-roster test**, pseudo.c 24586-24602 — which covers the
  human-configured all-CPU case, not the attract flag.)
- **Menu re-entry restores everything** (pseudo.c 30747-30754): the flag is
  cleared and `sub_422552` (24627-24642) writes the saved roster bytes,
  team flag, and level back.

`getvalue(92)` (VALUELST `92,30`) is the attract delay in seconds; the
file's own legend documents that values < 5 never enter attract mode —
matching the code's `getvalue(92) > 5` gate.

**Port status: DONE** (2026-07-09, "Port attract-mode demo match" `d83cd9a`,
merged `96be2e4`; coverage-audit.md table row #18). The menu-idle attract
match is fully reproduced: after 30 s of menu idle (`getvalue(92)`, gated
> 5), `present_menu` saves the configured roster/level/team
(`GameApp::AttractSaved`), rolls a random 3..10-slot COMPUTER-only roster and
a random stage bypassing the VALUELST 1150-1160 enable flags
(`attract_computer_count`/`fill_attract_roster`/`attract_stage_pick` in
`input.hpp`, pure/SDL-free and unit-tested), and dispatches the same
Menu->StartMatch edge a real Play selection uses (no new `AppState`/
`AppInput`). Any key/mouse/gamepad-button input during the demo aborts
`run_match` immediately; either exit (natural end or abort) skips DRAW/
RESULTS/VICTORY entirely and restores the saved selections before returning
to the menu. All of it presentation/config-level (the sim just receives an
all-AI `MatchConfig`); the two rand draws are on a dedicated presentation
LCG, never `State::rng` — ADR-0003 untouched. (Provenance: `sub_42B9CE` idle
path pseudo.c 30747-30754/30887-30894; `sub_410F81` attract branch
15125-15143; `sub_42A3F6` gates 29788/29812; `sub_4224E2`/`sub_422552`
24605-24642; `sub_42247A` 24586-24602; VALUELST 92.)

## The results / DRAW / VICTORY flow — inside `sub_42A3F6` (CONFIRMED)

The end-of-round path lives at the tail of the **Play** handler (`sub_42A3F6`,
~29820-30130), not a standalone screen. **The in-round portion of the same
handler — the auxiliary key loop (Ctrl+Q forfeit, F1 help, debug keys, the
Esc-is-inert and no-pause negative findings), the per-tick callback
`sub_42A191`, the countdown-clock HUD (`sub_4105D2`) + "hurry" flash, and the
round-end shell constants (hardcoded 6000/3000/1500 ms, no getvalue ids) — is
RE'd separately in `docs/re/in-match-shell.md`.** It is a **three-tier**
outcome:

1. **DRAW — no survivor.** `if (sub_4219B0(v69) != -1) goto RESULTS;` — the
   survivor query returns the lone survivor's index, or **-1 for none**. With no
   survivor it draws `sub_42A088(aDraw, 0)` (`aDraw = "draw"` → DRAW.PCX) then
   `sub_427BFB(1700)` (the **draw sting, SOUNDLST 1700**), and runs a bespoke
   wait: nav-blip on key, Esc→27, and **`(attract || flag) && time > t0+6000`
   → key=13** — a 6 s auto-advance in attract mode.
2. **RESULTS scoreboard — a survivor exists.** Loads `aResultsPlt`
   (`"results.plt"` → RESULTS.PCX) and prints each player's win tally, computing
   `v73` = the index that reached the match-win threshold (`dword_464A7C` =
   wins-needed), or **-1 if nobody has clinched the match yet**.
3. **VICTORY — the match is won (`v73 != -1`).** Team game → `aTeamU`
   (`"team%u"` → TEAM0/TEAM1.PCX); else → `aVictoryU` (`"victory%u"` →
   **VICTORY0..VICTORY9.PCX**, one per winner index). Draws that named screen
   `sub_42A088(v66, 0)`, dwells `sub_413CB0(3000)` (3 s), and plays the "we have
   a winner" voice group `sub_427BFB(2000)`. (Provenance: `aDraw`/`aResultsPlt`/
   `aTeamU`/`aVictoryU` string table @ 1612-1615; blit sites @ 29825/29888/30130.)

**Note on BONUS.PCX — CONFIRMED DEAD ASSET (2026-07-08).** The install ships
`BONUS.PCX`, but it is **not** referenced by this flow — the per-round outcome
is DRAW (no survivor) or the RESULTS tally (a survivor), and the match winner
is VICTORY%u. The earlier spine guess "round-win = BONUS" was wrong; the
faithful round/match-win screen is `VICTORY<player>`. A binary-wide search now
closes the question: a case-insensitive grep for `bonus` over the whole
decompile returns **zero hits** (no `aBonus*` string constant exists at all),
DATA/RES ships no `bonus.plt` companion palette, VALUELST/MESSAGES contain no
bonus-screen legend or string, and no `.RES` list names it (the only install
file containing the word is ROULETTE.BM's help prose). BONUS.PCX is leftover
art from a cut feature — nothing in the shipped binary can display it, so the
port owes it nothing.

**Spine mapping.** The sim does not yet model teams, so Results shows **DRAW**
(no survivor / time-up, `round_winner()` returns -1) or **`VICTORY<player>`**
(one survivor, naming that player), each as a normal `Screen` with a bounded 6 s
dwell (the `sub_42A3F6` attract auto-advance) then a return to the menu. The
winner voice group (2000) is played by `run_match` on match-over, matching
`sub_427BFB(2000)`. TEAM%u is a documented future hook (needs sim team state).

**Results MUSIC — CORRECTED (2026-07-08, second pass): 1130 under ALL outcome
screens; 1020 is the SETUP music, not victory music.** An earlier pass read
`sub_42741E(0x3FC)` = 1020 at handler entry as "played under the VICTORY
screen" — wrong. The round-end `sub_42741E(0x46A)` = **1130** fires
**unconditionally, BEFORE the survivor test** (pseudo.c 29820 precedes the
`sub_4219B0` check at 29823), so DRAW, the RESULTS tally, **and**
VICTORY/TEAM all play under **1130**; nothing ever re-starts 1020 in the
outcome tier. 1020 (started at Play entry, 29696) is in fact the
**setup-screens track** (player select / LEVEL & ROUNDS) — it is replaced at
every round init by the **per-level stage track** (`sub_4293E5` @ 0x4293E5:
SOUNDLST id `1100+level`, fallback 1120/0x460; suppressed entirely by the
Options "Disable music during gameplay" toggle `dword_4648C0`, which frees
the music instead — `sub_410B6E` pseudo.c 14847-14850). Full call-site-
exhaustive model in `docs/re/in-match-shell.md` "round-end shell" step 2.
**RESOLVED.** The port now matches: `game_app.cpp`'s `kDrawMusicId` (1130)
is started for DRAW, RESULTS, **and** VICTORY/TEAM alike, and `kWinMusicId`
(1020) is scoped to the Play/setup path only (never started for VICTORY).
See the `kDrawMusicId`/`kWinMusicId` comment block and every
`audio_.start_music(kDrawMusicId)` call in the outcome branches of
`run_match`/`run_app`. (0x3FC=1020, 0x46A=1130 — hex confirmed; the SOUNDLST
labels `win`/`draw` describe the clips, not where the code plays them.)

**SFX 40 (enrt1, "you can't do that here") — NETWORK-ONLY, N/A to our build.**
The results/draw wait loops fire `sub_427961(40)` when a key is pressed but
`sub_40C06A() == 1`. `sub_40C06A` is a bare `return dword_460058;` — the game-mode
global (0 = local, 1/2 = the two network roles; CORRECTED: mode 2 is NOT a
"demo" mode — its only writer is the setter `sub_40C035` @ 0x40C035, whose only
non-zero call is `sub_40C839(2)` at the top of the net-game screen `sub_42B0CE`;
no code path ever sets it for an unattended/demo run — see "Attract mode" below
for what the real demo path uses instead). So enrt1 is the "can't dismiss this
yet" buzz a **networked non-host** hears instead of the accept sting; every other
SFX-40 site in the front end (`sub_42B0CE`/`sub_42B47D` network-setup screens,
lines ~6102/8230/15390) is likewise gated on `sub_40C06A() == 1`. Our front end is
local-only (mode 0), so **SFX 40 can never trigger in the boot/menu/results path**
— it is correctly absent, not a missing sound. (Provenance: `sub_40C06A`
@ 0x40C06A = `return dword_460058`; SOUNDLST `40,enrt1` under the `; some kind of
"you can't do that here" sound` comment.)

**Draw-sting fidelity (fixed).** `sub_427BFB(1700)` plays a random member of the
contiguous SOUNDLST run beginning at 1700 (the file's own `; tie game/draw game`
comment) **once**. The spine previously routed 1700 through `ScreenDef.music_id`,
which *loops* the track — a wrong, repeating sting. It is now a **one-shot**:
`run_app` calls `audio_.play_random_in_range(1700, 1799)` when entering a DRAW
and the DRAW `ScreenDef` has `music_id = -1` (no looping music). The winner path
was already a one-shot (2000-group in `run_match`).

**RESULTS tally tier — RE'd, port DONE.** The middle tier (a survivor
exists but nobody has clinched the match: `sub_42A3F6` loads RESULTS.PCX and
prints each player's cumulative win tally against `dword_464A7C` = wins-needed)
is fully RE'd — layout, getvalue ids, message ids, the two independent
packed win/kill counters (`sub_421AC8`/`sub_421B0F`), and the clinch/outcome
logic — in `docs/re/results-and-options.md` §1. It now has the **multi-round
match structure with a running scoreboard** it needed: ROADMAP "Multi-round
best-of-N loop + RESULTS tally 1:1 — DONE 2026-07-08" tracks per-player
cumulative wins + the match-win threshold across rounds and renders the tally
on RESULTS.PCX (with the FONT6 glyph draw, below). The per-round DRAW/VICTORY
screens + their stings are correct today.

## The `.BM` text-screen viewer — `sub_41302D` (CONFIRMED) + the FON font

The credits/help screens (`CREDITS.BM`, `OPTIONS.BM`, `NETWORK.BM`, `INPUT.BM`,
…) all render through **one routine, `sub_41302D`** (@ 0x41302D). Confirmed
behaviour, now implemented in `libs/game` (`bmscreen.{hpp,cpp}` + the `bmtext`
parser + the new `bmfont` parser):

- **Two passes over the file** (pass 1 counts lines, pass 2 lays out), reading
  `fgets`-style in text mode — exactly what the `bmtext` parser already mirrors
  (CRLF/`0x1A`-EOF handling, 4-column tab stops, one `<IMG>` tag form).
- **Layout (confirmed literals):** text starts **34 px** from the top of the
  scroll region, one line per row at the **font cell height**; the left inset is
  **34 px**; the on-screen row count is `v60 = 344 / line_height`; each `<IMG>`
  segment blits its named PCX inline and advances the pen past it.
- **Scroll is keyboard-driven, one line at a time — there is NO auto/timed
  scroll.** Up (`328`) `--v54`, Down (`0x150`) `++v54`, PgUp (`0x149`) `v54 -=
  v60-1`, PgDn (`337`) `v54 += v60-1`, clamped to `[0, count - v60]`. **Enter
  (13) or Escape (27) dismiss** the viewer (`LABEL_100` sets the done flag on
  both). So the earlier task assumption of a "scroll speed" constant does not
  exist — nothing to guess. (Provenance: `sub_41302D` scroll loop @ ~16420-16590.)
- **Font:** `sub_41302D` draws with the **active font**, which graphics-init
  pins to **FONT6** via `sub_431E9C(6)` at the end of `sub_414DF4` (@ 0x417600).
  The text colour is the global draw index `byte_49D38F`. The `FONT<n>.FON`
  files live in the **install root** (not under DATA/). See `docs/formats/fon.md`
  for the decoded format; the port renders glyphs in truecolour (the paletted
  `byte_49D38F` index → a fixed light ink on a dark panel — a cosmetic port
  choice, layout/advance are faithful).

**Interactive settings — Options screen BUILT (clean-room) and now RE'd;
controller-remap RE'd, port still DEFERRED.** These `.BM` files are the **HELP
overlays** listed by the help browser (`sub_41431C`, corrected above). The
**Options screen** (`libs/game/src/options_screen.cpp`) is a fully-interactive
Team Play / Conveyor Speed editor persisting to `options.ini`
(read-modify-write, `bomber::assets::save_options`); its F1 key still reaches
OPTIONS.BM. It was built clean-room against the glue-screen conventions
(random `GLUE<n>` backdrop, FONT6 text, SFX 20 nav / 10 accept) BEFORE the
real screen was RE'd; the original (`sub_4080DC` — a 19-item list including
team play, random start, conveyor speed, "Define keyboard layouts", persisting
to `options.ini` only on app exit via `sub_405DE3`/`sub_410EBF`) is now fully
pinned in `docs/re/results-and-options.md` §3 (+ the complete 22-key
`options.ini` table), so aligning our screen's item list/layout/write-timing
to it is a tracked follow-up. The **key-remap UI** (`sub_407B9D`, reached from
the Options screen's "Define keyboard layouts" row — NOT the
`sub_42B0CE`/`sub_42B47D` net-game screens) is RE'd in the same doc §2;
porting that widget is still a separate implementation effort. The `AppState`
hooks (`Options`, `Network`, `Controllers`) are in place; Network/Controllers
currently show the help text.

## The transition — HEADWIPE.ANI, driven by the standard ANI pacer

There is **no `headwipe` string** anywhere in the decompile, yet
`DATA/ANI/HEADWIPE.ANI` ships. The engine therefore loads it the same generic
way it loads every other ANI — by a runtime-built `DATA/ANI/<name>.ani` path
(`aSSSAni = "%s/%s/%s.ani"`) — and plays it as a screen-to-screen wipe overlay.
Its internal contents (via `strings`) are a **single sequence** built from one
`HEAD`/`HEAD0` STAT with a run of FRAM leaves (`FOA0000.TGA`, …): a growing
mask, exactly a horizontal head-to-head wipe.

Playback obeys the already-confirmed universal ANI rule (`docs/re/facts.md`
"ANI per-step timing — CONFIRMED INERT", `sub_41DAA7`): the shown step is
**`counter % statecnt`**, the owner advancing `counter` once per rendered
frame. There is no per-step duration; the wipe simply steps one frame per tick
until it has shown every step once. The spine's transition primitive advances a
frame counter over the `HEADWIPE` sequence at the front-end frame rate and is
"done" after `statecnt` frames — faithful to that driver. When HEADWIPE.ANI is
absent the spine falls back to a wall-clock alpha fade (cosmetic only).

**Scope: the wipe is NOT played on waited screens.** `sub_42A088` cuts (palette
+ blit + flip, above) — it does not invoke HEADWIPE. So the logos, title,
results, and `.BM` viewer all appear/dismiss by a cut, with no wipe. The port
keeps the HEADWIPE transition only for the **menu→match select** step
(`present_menu`), which is the one place a head-to-head wipe belongs; the earlier
spine also ran the wipe out of every waited screen, which was wrong and has been
removed.

## Audio: looping music vs one-shot SFX — the port model (matches the binary)

The binary keeps **one looping music channel** and a **separate one-shot SFX
pool** (both facts confirmed above from `sub_4273A4` loop `0xFFFF` vs
`sub_427B36` loop `0`). `AudioEngine` mirrors this exactly and needed no new
plumbing for this flow:

- **`AudioEngine::start_music(id)`** owns the single looping music stream
  (`music_stream_`): it loads the id's `.RSS`, clears the stream, and queues the
  samples; `update_music()` re-queues the clip when the buffered audio drops
  below a threshold — a faithful stand-in for the original's `0xFFFF` infinite
  loop. Calling it again with a new id **replaces** the current track (clears +
  re-queues), matching `sub_4273A4`'s "free the old handle, start the new one".
- **`AudioEngine::play(id)`** plays a one-shot clip on an 8-voice SFX pool,
  flushing so the voice frees when it ends — the analogue of the SFX players
  (`sub_427859`/`sub_427B36`, loop 0). Stings (2800/1700/2000/2600) and blips
  (10/20) all go through here, so **they never disturb the looping music**.

The **boot-music model** at the port level (`GameApp::run_boot_attract`,
`present_menu`):

1. `run_boot_attract` calls `start_music(1000)` **once**, before presenting
   IPLOGO — the boot track then loops continuously through the logos and the
   title (no per-screen restart, because `Screen` never touches music).
2. The one-shot title sting `play(2800)` fires right before the title image,
   over the still-playing boot track.
3. `present_menu` calls `start_music(1010)` on menu entry, which replaces the
   boot track with the menu track and keeps it looping while in the menu.
4. Screen skips (`Screen::on_key`) only ever call `play(20)` / `play(10)` — one
   shots — so no skip stops the music; only the presented image changes.

This is presentation-only (no `libs/sim` involvement); the front-end's cosmetic
randomness (SFX group pick) uses `AudioEngine`'s own LCG, never `State::rng`.

## Tunables (all front-end / presentation, zero determinism impact)

| id | meaning | value | status |
|---|---|---|---|
| `getvalue(12)` | attract / auto-advance delay for a waited screen | **7 s** (VALUELST `12,7`) | CONFIRMED value + source: `sub_42A088` waits `time_()` (seconds) to `start + getvalue(12)`; port uses 7000 ms for logos + title. Timeout synthesizes Enter → plays SFX 20 + 10 (audible advance) |
| `getvalue(92)` | main-menu attract-mode delay | **30 s** (VALUELST `92,30`) | CONFIRMED: after `getvalue(92)` s idle (gated `> 5`; legend: < 5 disables attract) `sub_42B9CE` enters ATTRACT MODE — saves config, forces Play, runs a live all-CPU demo match (see "Attract mode"); distinct from `getvalue(12)` |
| `getvalue(700/701/702)` | main-menu cursor x / y-base / y-step | **332 / 140 / 38** (VALUELST `700,332,140,38,0`) | CONFIRMED anchor + values (`sub_42B9CE`): x=getvalue(700), y=getvalue(701)+getvalue(702)·row |
| SOUNDLST 1000 | boot/title music (`title`), looping, started ONCE in `sub_42B060`, continuous across logos+title | TITLE.RSS | CONFIRMED (`sub_42741E(0x3E8)` @ boot, loop 0xFFFF) |
| SOUNDLST 1010 | main-menu music (`menu`), looping, started on menu entry (`sub_42741E(0x3F2)`, v14-gated) — replaces the boot track | MENU.RSS | CONFIRMED (`sub_42B9CE`); NOT 0x3FC/1020 (that is `sub_42A3F6`'s round "win" track) |
| SOUNDLST 1020 | **setup-screens music** (label `win`), looping, started at `sub_42A3F6` entry (`sub_42741E(0x3FC)`) — CORRECTED: plays under player/level setup, replaced at round init by the stage track; it does NOT underlie VICTORY | WIN.RSS | CONFIRMED (corrected 2026-07-08); port fixed — `game_app.cpp`'s `kWinMusicId` (1020) is now scoped to the Play/setup path only, never started for VICTORY (see "Results MUSIC") |
| SOUNDLST 1130 | **outcome-tier music** (label `draw`), looping, started unconditionally at round end (`sub_42741E(0x46A)` BEFORE the survivor test) — under DRAW **and** RESULTS **and** VICTORY | DRAW.RSS | CONFIRMED (corrected 2026-07-08); port fixed — `game_app.cpp`'s `kDrawMusicId` (1130) now starts under DRAW, RESULTS, **and** VICTORY/TEAM alike (`audio_.start_music(kDrawMusicId)` in every outcome branch) |
| SOUNDLST 1100+level, 1120 | per-level in-round stage music (`sub_4293E5` @ 0x4293E5, called from `sub_410B6E` round init unless the "Disable music during gameplay" option frees the music instead); 1120 (0x460) is the fallback when the level has no entry | per-level RSS | CONFIRMED (`docs/re/in-match-shell.md` step 2); port plays no in-round music — gap |
| SOUNDLST 10 | menu-exit / accept sting (`menuexit`), one-shot | MENUEXIT.RSS | CONFIRMED (`sub_427961(10)` accept path in `sub_42A088` + every menu select in `sub_42B9CE`) |
| SOUNDLST 20 | nav blip (`letter1`), one-shot, on ANY key | LETTER1.RSS | CONFIRMED (`sub_427961(20)` in `sub_42A088`/`sub_42B9CE`/`sub_42A3F6`) |
| SOUNDLST 40 | "you can't do that here" buzz (`enrt1`), one-shot | ENRT1.RSS | CONFIRMED **NETWORK-ONLY** (`sub_427961(40)` gated on `sub_40C06A()==1` in the results/setup wait loops); never fires in local play — correctly absent in the port |
| SOUNDLST 1700 | draw-screen sting group (`draw`/gump1, 1700..1999), one-shot random pick | GUMP1.RSS + group | CONFIRMED (`sub_427BFB(1700)` in `sub_42A3F6`); port uses `play_random_in_range(1700, 1999)` |
| SOUNDLST 2000 | "we have a winner" voice group (2000..2299), one-shot random pick | PROUD.RSS + group | CONFIRMED (`sub_427BFB(2000)` on VICTORY); port `play_random_in_range(2000, 2299)` |
| SOUNDLST 2600 | menu-quit / exit sting group (2600..2699), one-shot random pick | QUITGAME.RSS + group | CONFIRMED (`sub_427BFB(2600)` in quit handler `sub_412987`); port plays it on menu Quit/Escape |
| SOUNDLST 2800 | title intro sting group (2800..2810 "ATOMIC BOMBERMAN!"), one-shot random pick | GEN8A.RSS + group | CONFIRMED (boot `sub_427BFB(2800)`); port `play_random_in_range(2800, 2899)` |

The logo/title dwell is **no longer a tunable — it is CONFIRMED `getvalue(12)` =
7 s** (VALUELST `12,7`), the same waited-screen timeout for all three boot
screens; the port hard-codes 7000 ms from it. The one remaining **our tunable**
in the front-end is the HEADWIPE frame cadence in ms for the surviving
menu→match select wipe (the original steps per rendered frame; the spine uses a
fixed ms-per-frame so it is resolution-independent). Neither touches the sim.
