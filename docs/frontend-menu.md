# The main menu

`MenuScreen::run()` (`libs/frontend/src/menu_screen.cpp`) — `sub_42B9CE`
@0x42B9CE. `docs/re/frontend-flow.md` is the RE source; this page holds the
rationale that used to sit inline (`docs/coding-standards.md` §10).

## The rows

The original highlights one of seven rows (selection index 0..6) over
MAINMENU.PCX and dispatches on Enter. The port keeps the row set and order:

| row | dispatch | port |
|---|---|---|
| 0 | Play (`sub_42A3F6`) | `StartMatch` |
| 1 | START NET GAME (`sub_42B0CE`) | `OpenNetHost` — our own UDP netplay (ADR-0010), not the original's IPX/serial link |
| 2 | JOIN NET GAME (`sub_42B47D`) | `OpenNetJoin` |
| 3 | Options (`sub_4080DC`) | `OpenOptions` |
| 4 | Credits (.BM) | `OpenCredits` |
| 5 | the generic `*.BM` help browser (`sub_41431C`) | handled INLINE |
| 6 | Quit (`sub_412987`) | the quit-confirm modal |

Row 3's target is a CORRECTION (`docs/re/results-and-options.md`): it goes to
`sub_4080DC` = the OPTIONS screen, not an editor. Row 5's is another: the row's
old "Roulette" label was wrong — it lists every `*.BM` in the install root,
ROULETTE.BM among them.

Row 5 is special-cased ahead of the row table because `sub_42B9CE`'s row switch
calls it DIRECTLY (`case 5: sub_41431C(); break;`), staying inside the menu loop,
unlike rows 0-4/6 which return through the AppState flow.

**INPUT.BM has no dedicated menu row and there is no controller-setup screen** —
a CONFIRMED negative (2026-07-09): no "controller" string and no "INPUT.BM"
literal exist anywhere in the decompile. It is one of ~10 topics the generic help
browser already globs. `AppInput::OpenControllers` is exercised by the flow
doctest but deliberately bound to no row, because there is nothing in the seven
rows to bind it to.

## The music re-arm is per VISIT, not per process

`sub_42B9CE` has an OUTER loop (one iteration per menu visit, pseudo.c ~30744)
wrapping an INNER one (the per-frame input poll, ~30768). The music flag is
raised once per OUTER iteration, right before the inner loop starts; inside the
inner loop it gates the `sub_42741E(0x3F2)` call and is cleared the moment that
call is made. All it does is stop the track re-firing on every polled FRAME
within one visit.

Every switch case at the bottom of the outer loop (Play/setup cancel, Credits,
Options, Quit-confirm-cancel, Results, idle-timeout→attract, …) falls through
back to the top of the outer loop, which RE-ARMS the flag. So
`sub_42741E(0x3F2)` — a full free + reload-from-disk + restart-from-sample-0
(`sub_4273A4`, with no same-id no-op) — fires again on EVERY return to the menu.

`MenuScreen::run()` is called once per menu (re-)entry, so an unconditional
`start_music()` at its head is the faithful port. It is also what switches back
from the round/results tracks (1020/1130) to 1010 — the audible "menu music
reclaims the loop when you back out" behaviour.

**Retraction.** A once-per-process gate (`menu_music_started_`, removed) broke
that: after the first call it never started 1010 again, so returning from a match
or the results screen left 1020/1130 stuck looping forever.

## Escape does not quit — it selects row 6

PINNED 2026-07-09. Escape's full path in `sub_42B9CE`: the "any real key" line
fires the nav blip (SFX 20) for EVERY key including 27, then Escape reaches the
accept branch — whose key test admits exactly code 17 (Ctrl+Q) and code 27 —
which plays `sub_427961(10)` and forces the cursor to row 6. The row-6 dispatch
calls `sub_412987`, which pops a modal yes/no dialog BEFORE anything exits.

`sub_41456C`'s own key loop (pseudo.c ~17220-17270) accepts Y/y/Enter/Space as
Yes (returns 1) and N/n/Q/q/Escape as No (returns 0) — confirmed by the raw
key-code ranges (0x1B/78/110 → No; 13/32/89/121 → Yes; the Q pair is 17253-17267,
missing from frontend-flow.md's old list). The dialog resolves SILENTLY: no
accept sting on either answer, so the old `play(10)`s there were invented.

Only on Yes does `sub_412987` FREE the music (`sub_427342`) first — MENU.RSS must
not keep looping under the sting — then play the exit sting
(`sub_427BFB(2600)`, skip-logos-gated) and `Sleep(0xFA0)` = 4 s before the real
process exit (`sub_4128C9(0)`), so the sting is audible rather than cut off by
window teardown. No just closes the dialog. The cancel path exits through the
OUTER loop: cursor home to row 0 (case 6 resets the selection index, 30920-30922)
and MENU.RSS reloaded from sample 0.

### The quit dialog's chrome

RE-PINNED 2026-07-10. The SAME `sub_43C734` chrome as the loading dialog — the
WINZ.PCX 9-patch (`sub_41726B` @ pseudo.c 17200), NOT a flat grey — sized from
the prompt extent (inner width = max(prompt width, 80); dialog width = that + 64;
height = 4·fontheight + 64 + fontheight for a one-line prompt), centred on both
axes. Prompt at y = fontheight+32 (window-relative, centred) in `sub_412987`'s
OWN ink `byte_49A390` — LUT offset 0x5000 → idx 248 → (164,0,0), a dark red
(pseudo.c 16027 passes it as the a3/foreground argument; this corrected an
earlier white). Two `sub_432298` buttons at the pinned y = height-32-fontheight-6,
x = width/2-80 (Yes) / width/2+22 (No).

The prompt is drawn as that red FILL over a GOLD 1-px outline — (252,248,88) =
`byte_49D37A` "percent readout yellow". The RE records `sub_41696C`'s outline
colour (`sub_412987`'s a2/a7) as decompiler-lost ("black per every sibling call
site"), so **this gold is PHOTO-DERIVED pending a binary re-read**; only the quit
prompt overrides the default-black outline, since the photo evidence is for this
dialog alone.

## Hidden triggers

- **Ctrl+E ×6** — the scheme editor (`docs/re/results-and-options.md` §5,
  CONFIRMED). `sub_42B9CE`'s input loop tracks a same-key repeat counter on raw
  key code 5 (ASCII Ctrl+E); any OTHER key resets it; `++counter > 5` fires on the
  6th CONSECUTIVE press (plays accept SFX 10, then `sub_40330E` → `sub_403184`).
  Time spent in the editor must not count toward the attract idle trigger.
- **Ctrl+Q** (raw 17) behaves exactly like Escape (30861-30867).
- **Alt+O** (280) jumps to and selects Options (30806-30812); case 3's
  fall-through resets the cursor to row 0 for the NEXT visit (30910-30912).
- **F1** (315) selects row 5 = the help browser (30826-30832).
- **Alt+A** (286) starts an attract demo match directly (30813 → the 30888-30894
  attract path). frontend-flow.md's old "run the current selection" label for 286
  was wrong.
- **Alt+D** (288) pops the hidden debug-info window (`sub_413D45`, 30819-30822).
- **F10** is PORT-ONLY: the Video Settings panel, which keeps the modern
  video/cadence toggles off the faithful Options screen. It still blips —
  `sub_42B9CE` fires `sub_427961(20)` @0x42BB2E for EVERY real key before its
  dispatch, and F10 (raw 0x144) is simply unmapped there, falling to the default
  arm. An unmapped key clicking IS the original's behaviour; an early `continue`
  used to swallow it.

Every one of these rides the any-real-key blip 20 first. The blip fires for EVERY
key `sub_42B9CE` reads, mapped or not (30787-30790) — an unbound letter still
clicks. The port gates that on the press edge so SDL's key repeats don't buzz.

## Enter/Space on a row

`sub_42B9CE` plays the any-key blip 20 first (30787-30790, for every real key
including Enter/Space), then the accept sting (SFX 10) for BOTH Enter (13) and
Space (32) on EVERY row — there is no "inert row" concept in the original; each
row 0..6 is a live dispatch. A row this port has not built still plays the accept
sting to stay faithful but has no leaf to jump to, so it stays put.

The selection is handed to the flow by a CUT — no wipe. The original's dispatch
calls the selected handler directly and the next screen's first frame replaces
the menu. A HEADWIPE.ANI wipe used to play here (~3.5 s: 211 frames, one per
vsynced frame — the "long pause into the player-setup screen" report), but that
file is dead art the original never even loads: absent from MASTER.ALI, with no
headwipe string or transition call site anywhere in the decompile
(`docs/re/frontend-flow.md` "HEADWIPE.ANI is dead art").

## Pad navigation

`sub_4102B7` @14286-14333: whenever the key queue is empty, the original's getkey
polls every joystick and SYNTHESIZES key codes from it — axis-threshold crossings
become up (328)/down (336) and any button rising edge becomes Enter (13) — so the
whole menu, and the quit confirm on top of it (which reads the same getkey), is
pad-navigable. SDL gives dpad/button edges directly; the port re-injects them as
the synthetic keys so every key path is shared rather than duplicated.

## Attract

`getvalue(92)` = 30 (VALUELST `92,30`), gated `> 5` (the file's own legend:
values < 5 disable attract entirely) — distinct from the waited-screen
`getvalue(12)`. The idle clock is seeded on every fresh visit to the menu,
including a re-entry after an attract match, so an unattended machine cycles demo
matches forever one getvalue(92)-second gap apart; `sub_42B9CE`'s idle counter is
never suppressed after firing once. ANY real input resets it.

Once the timer elapses the port fires the SAME Play dispatch a real Enter-on-row-0
would: `sub_42B9CE` forces the selection index to 0 regardless of the highlighted
row (30894). The quit-confirm dialog is modal (`sub_41456C` blocks `sub_42B9CE`'s
own loop until answered), so the attract idle trigger is held off while it is up
— a demo match must not yank the confirm away mid-decision. For the same reason
the trigger-cursor animation phase is frozen under the modal.

`roll_attract_match()` does the `sub_4224E2` save (snapshot the CURRENT
selections so `restore_from_attract()`/`sub_422552` can put them back untouched)
plus two presentation-LCG rolls in the doc's read order — roster count first,
then the stage, matching `sub_410F81`. It also forces team play OFF: a demo match
is never team mode regardless of what the player configured. The campaign trigger
is inert during attract, because campaign state can only be armed by the setup
screen's 'C'×5 trigger and the attract short-circuit never visits it.

## The cursor

CONFIRMED `getvalue(700/701/702)`: X from 700, Y from 701, the Y-step from 702;
the bomb-trigger sprite is blitted at x=X, y=Y + Ystep·row. Read live from
VALUELST row 700's columns (its own legend reads "X, Y - first item / YS -
y-spacing"), with this install's values (332,140,38) as the fallback so a
stripped VALUELST still positions sanely.

The sequence comes from TRIGANIM.ANI, the file MASTER.ALI actually loads — the
original's menu resolves the name from the same global pool the in-match trigger
bomb uses, and TRIGBOMB.ANI's 7-step twin is dead art (`docs/re/facts.md` "ANI
sequence-name audit"). If it is absent the port draws a pulsing highlight bar so
the selection stays visible.

The "V1.0" version string draws every menu frame (pseudo.c 30779:
`sub_41696C(root, aV10, x=0, W=50, y=0, byte_49A624, black)`) — the ink is the
general grey (168,168,164) and the literal is hardcoded in the binary (`aV10`,
pseudo.c 1622), not a MESSAGES.TXT entry.

## Pacing

Refresh-boundary pacing (`platform::FrameClock`). The blind `SDL_Delay(2)` this
replaced let the loop free-run at 300-500 Hz on Windows (present does not block),
so the `frame`-driven trigger cursor animated far too fast. One animation step
per real refresh.
