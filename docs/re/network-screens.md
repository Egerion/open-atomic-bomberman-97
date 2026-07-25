# The two NETWORK SETUP screens — complete RE (2026-07-25)

Definitive spec for `sub_42B0CE` (**START NET GAME**) and `sub_42B47D`
(**JOIN NET GAME**), the original's only interactive netplay screens. Written
so the port can reproduce them 1:1: geometry table, per-state line contents,
key table, LUT-decoded ink table, and the full join/wait state machine.

Sources: `BM95.EXE` (imagebase 0x400000, Watcom register convention — args in
EAX, EDX, EBX, ECX), `DATA/RES/VALUELST.RES`, `MESSAGES.TXT`, `COLOR.PAL`.
Every signature and literal below was verified against **raw bytes**
(capstone), not the decompiler alone — Hex-Rays drops register arguments and
did so in five load-bearing places here (flagged inline as *HR-dropped*).
Text and art stay in the install; only ids, formats, and geometry are here.

---

## 0. TL;DR — what these screens actually are

| menu row | handler | net mode set | role | screen |
|---|---|---|---|---|
| 1 | `sub_42B0CE` @0x42B0CE | `sub_40C839(2)` | **HOST** | START NET GAME — the host's live list of up to **4 connected clients**, and the START button |
| 2 | `sub_42B47D` @0x42B47D | `sub_40C839(1)` | **GUEST** | JOIN NET GAME — a browser of up to **10 discovered servers**, with join + "waiting for server to start" |

Both are lobbies. Neither is an options pane, and neither picks a map. After
either one commits, the machine falls into the **shared local pre-match
flow** (`sub_42A3F6` → `sub_410F81` roster → `sub_406DDE` level/rounds) with
the host driving and the guest locked to read-only (§7).

---

## 1. The host/guest polarity — RESOLVED, and it CORRECTS the docs

`docs/re/audit/multiplayer-deep.md` §2 flagged a real conflict: the labels say
row 1 = START (host) but `sub_42B0CE` sets mode **2**, which every doc read as
"guest". **The labels were right; the mode table was wrong.**

`dword_460058` is:

| value | role |
|---|---|
| 0 | local (not networked) |
| **1** | **GUEST / client** |
| **2** | **HOST / server** |

Four independent proofs:

1. **`sub_40C035`** (the setter, 0x40C035): `dword_460058 = a1;` and **only**
   for `a1 == 2` it also does `dword_4600D4 = HIWORD(dword_46013C)` — i.e.
   mode 2 makes the machine *its own session authority*. On mode 1
   `dword_4600D4` stays 0 until a JOIN ACCEPT fills it with the host's node
   id (`sub_40D12D`).
2. **`sub_40CD1C`** (packet framing, 0x40CD1C): the datagram header's
   authority field is `dword_4600D4` when `dword_460058 == 1` (the host I am
   attached to) and `HIWORD(dword_46013C)` — my own id — when
   `dword_460058 == 2`.
3. **`sub_4105D2`** (match clock, 0x4105D2): mode 1 *copies* the clock from
   the received value; mode 2 computes it from the wall clock and broadcasts
   it (`sub_40FCA1`). The clock authority is mode 2.
4. **`sub_40EBC1` / `sub_40ED08` / `sub_40FE88`** are all `dword_460058 == 2`
   only: the game announce, the start broadcast, and the options push. Only a
   host does those.

Raw bytes at the two screen heads (settles the assignment itself):

```
0x42B0DC  mov eax, 2 ; call 0x40C839   ; sub_42B0CE → HOST
0x42B48B  mov eax, 1 ; call 0x40C839   ; sub_42B47D → GUEST
```

Corroborated three more ways: VALUELST's own section comments label 750-763
"JOIN NET GAME SCREEN" (the ids `sub_42B47D` reads) and 765-778 "START NET
GAME SCREEN" (the ids `sub_42B0CE` reads); MESSAGES id 60 (Screen B's header)
is an "available net games" list caption while id 70 (Screen A's header) is a
"connected network players" caption; and Screen A reads the 4-entry
client table while Screen B reads the 10-entry server table.

**Port mapping:** `present_net_host` ⇒ the `sub_42B0CE` screen (mode 2),
`present_net_join` ⇒ the `sub_42B47D` screen (mode 1).

**What `sub_40C839(n)` actually does** — it is not a setter, it is the whole
**net bring-up**, and it can fail (§2).

---

## 2. Entry gate — `sub_40C839(role)` @0x40C839

Called as the first statement of both screens; **a non-zero return aborts the
screen immediately** (`test eax,eax / jne <epilogue>`), returning to the menu
with no further drawing. Returns `dword_4600D0 == 0`, i.e. non-zero = failed.

Sequence:

1. `dword_4600D4 = 0`; `sub_40C035(role)` (the real mode set, §1);
   `sub_40C26E()`; `sub_40CC87()` (queue reset).
2. Read **`CFG.INI` key `netonoff`** (`sub_41739C` + `sub_4516C1` = atoi). If
   0 → two-line modal `getstring(96)` / `getstring(340)` ("networking is
   disabled in CFG.INI"), `sub_40C035(0)`, **return 1**.
3. Protocol selection into `dword_46012C`:
   - if `options.ini`'s `netprotocol` (`dword_464828`) is non-zero, use it;
   - else pop the **protocol picker**: 5 strings `getstring(630+i)` are
     loaded, the widget is called as
     `sub_41485A(prompt=getstring(620), items, 0, count=getvalue(1110), 150,
     150, ink=byte_49D38F)` and it renders exactly `count` rows
     (`sub_42FEF0(list, count)`'s `for (i = 0; i < a2; …)`).
4. **`if (dword_46012C < 1 || dword_46012C > 3) → fail`** (raw:
   `cmp [46012C],1 / jl` … `cmp [46012C],3 / jle`): `dword_4600D0 = 0`,
   `sub_40C035(0)`, **return 1** — silently, no modal.
5. Bind the transport (`sub_43B81D(dword_46012C, &table@0x4600F0)` fills a
   60-byte vtable at 0x4600F0), open it (`dword_4600F4()`), and for
   `dword_46012C == 2` (modem) / `3` (serial) copy the Options modem knobs
   (`dword_464970` port, `dword_46482C` baud, `dword_4648B8` irq). On **modem**
   the two roles differ: the **host** (`dword_460058 == 2`) sets
   `dword_460124 = 0` and *answers*; the **guest** is prompted for the number
   with `getstring(648)` (`sub_42E938`, 30 chars, width 180) and *dials* it.
6. Connect wait: `dword_4600FC()`, caption `getstring(57)`, percent dialog
   `sub_412E0C`/`sub_412E33(0)`, loop until `dword_460110()` reports ready or
   **Esc** cancels. On cancel/failure → modal `getstring(96)`/`getstring(291)`
   and `sub_40C035(0)`.
7. Success → `sub_40E474()` (registers the **51** packet handlers into
   `funcs_40E9D7[100]`) and return 0.

### The `netprotocol` ordinals — RESOLVED (was [NEEDS BINARY])

VALUELST's own comment block at 1100 names them, and MESSAGES 630-634 matches
one-for-one:

| id | protocol | VALUELST retransmit ms (`1100+id`) |
|---|---|---|
| 0 | none / cancel | 200 |
| 1 | **IPX** | 200 |
| 2 | **modem** | 750 |
| 3 | **serial** | 400 |
| 4 | **TCP/IP** | 400 |

`getvalue(1110) = 4` ("how many different protocols are supported") — so the
picker draws rows 0..3 only and **TCP/IP is never offered**; and even if it
were, step 4's `<= 3` clamp would reject it. The Options-screen picker
(`sub_407F4F`) has the same shape and stores the result only `if (r >= 0 &&
r <= 3)`, and the `options.ini` reader clamps `netprotocol > 3 → 3`. So the
shipped executable supports **IPX / modem / serial** and TCP/IP is dead data.
(This is the code behind the folklore that AB netplay is IPX-only.)

---

## 3. Session model the two screens sit on

| global | meaning |
|---|---|
| `dword_46013C` | HIWORD = **this machine's node id**, a nonzero random 16-bit value drawn once at boot (`sub_40C74C`: `do { HIWORD = rand(); } while (!HIWORD);`). LOWORD = the machine count once a session forms. |
| `unk_460140` | **this machine's node name**, ≤40 chars. `sub_40FE34()` returns `&unk_460140`. Loaded from install-root **`NODENAME.INI`** (first line, `fgets(buf,40)`, `\n` stripped — `sub_40C08C`, called from the boot init `sub_40C74C`); if the file is missing, a random default `getstring(500 + rand() % getvalue(47))`, `getvalue(47) = 49` names. Written back by `sub_40C140` (`fopen("nodename.ini","wt")` + `fputs`) from the **shutdown hook `sub_40C4DB`** (registered with `sub_410EBF`, the same atexit-style registrar `options.ini`'s `sub_405DE3` uses) — so an edited name persists, and a randomly-assigned one becomes permanent after the first run. Editable from Options row 2 (`sub_4074DC` → `sub_40FE55`, 30-char edit field). |
| `word_45FFA4[4]` / `dword_45FFFC[4]` | **HOST-side client table**: node id + a 41-byte name buffer per connected client. Cap **4 clients**. Cleared by `sub_40F290()`. |
| `word_4600D8[10]` / `dword_45FF04[10]` / `dword_45FF54[10]` | **GUEST-side server table**: node id + 41-byte name + the announced client count, per discovered server. Cap **10 servers seen**. Cleared by `sub_40F243()`. |
| `dword_4600D4` | the node id of my session authority (host id on a guest; self on a host). `sub_40F364(v)` sets, `sub_40F386()` reads. Every in-session handler gates on `sub_40C497` = "packet came from `dword_4600D4`". |
| `dword_460068` | "the game has started" flag. `sub_40F320(v)` sets, `sub_40F342()` reads; set by the kind-14 handler. |
| `word_460130[5]` | the final session node list — `[0]` = host, `[1..4]` = clients. **The 5-machine cap**: `sub_40E765` drops any datagram whose sender is not one of these 5. |
| `dword_45BAC4` | **network packet version = 21356**; stamped into every datagram header and checked on receive (`sub_40E765`). Mismatched builds simply never see each other — the original's version of ADR-0011's `build_hash`. |

Accessors the two screens use (all `@<eax>` in, `@<eax>` out):

| fn | returns |
|---|---|
| `sub_40F1E9(i)` 0x40F1E9 | `(int16)word_45FFA4[i]` — client i's node id, 0 = empty slot |
| `sub_40F217(i)` 0x40F217 | `dword_45FFFC[i]` — **char\*** to client i's name |
| `sub_40F163(i)` 0x40F163 | `(int16)word_4600D8[i]` — server i's node id, 0 = empty slot |
| `sub_40F191(i)` 0x40F191 | `dword_45FF04[i]` — **char\*** to server i's name |
| `sub_40F1BD(i)` 0x40F1BD | `dword_45FF54[i]` — **how many clients server i already has** |

> **CORRECTION.** `setup-screens.md` reads `sub_40F1BD` as "returns <4 = a
> keyboard/joystick index" and describes both screens as controller
> assignment. There is no controller anywhere in either screen:
> `dword_45FF54[i]` is written by `sub_40CF93` from the announce payload's
> count word, and the `< 4` test is the **"is this server full?"** check
> (its failure message id 7 is a *server-full* message). See §9.

### Packet kinds used by the lobby (`sub_40E474`'s table)

| kind | sender | payload | handler | effect |
|---|---|---|---|---|
| 0 | host, every 1000 ms (`sub_40EBC1`, len **82**) | `int16` client count + 80-byte node name | `sub_40CF93` (guest only) | insert/refresh the server in `word_4600D8[]`, store name + count |
| 3 | guest on join (`sub_40EC6F(sel)`, len **43**) | `int16` target server node id + 41-byte node name | `sub_40D175` (host only) | if addressed to me: seat the requester in the first free `word_45FFA4[]`, copy the name, reply kind 18 (`sub_40ECC3`) |
| 18 | host (`sub_40ECC3`, len 2) | `int16` accepted node id | `sub_40D12D` (guest only) | **JOIN ACCEPT** → `dword_4600D4 = host id` |
| 14 | host on start (`sub_40ED08`, len 12) | the `word_460130[]` node list + count | `sub_40D251` | **GAME START** → `dword_460068 = 1`, copy the node list |
| 49 | host on start (`sub_40FE88`, len 16) | 8 option words (§6) | `sub_40DF95` | apply the host's game options |
| 1 | host on exit (`sub_40EC29`, len 0) | — | `sub_40D073` | drop that server from the guest's list; clear `dword_4600D4` if it was mine |
| 39 | guest on exit (`sub_40EDC9`, len 0) | — | `sub_40D2F8` | free that client's `word_45FFA4[]` slot |

---

## 4. Shared chrome (identical on both screens)

| element | detail |
|---|---|
| backdrop | `sub_4148E5()` — a **random** `GLUE<n>` full-screen picture + its `.plt` palette, `getvalue(16) = 7` (GLUE0..GLUE6). Re-rolled on every screen restart. |
| per-frame clear | `sub_415CA4()` memcpys the saved backdrop over the work surface — everything below is redrawn each frame. |
| present | `sub_415C1F()` — frame counter, flip, window-layer repaint. |
| music | `sub_42741E(0x410)` = track **1040** on entry (this is a genuine 1040 site; the 1020 correction in `setup-screens.md` applies to `sub_410F81`, not here). |
| footer | `sub_40FC44()` every frame: `getstring(55)` ("%u" = `dword_45BAC4` = **21356**) at **x=0, y=430, clip w=320**, white ink / black outline. |
| VALUELST | `sub_4121FF()` (a full VALUELST.RES reload) runs on both entry **and** exit. |
| roster save/restore | `sub_4224E2()` on entry saves all 10 slots' input type/sub bytes (`byte_461BD4`/`byte_461BD5`, +16/+17 of the 152-byte player record) plus team mode; `sub_422552()` restores them on exit. |
| options save/restore | `sub_40FF57()` on entry / `sub_40FFBC()` on exit for the 8 net-synced options (§6). |
| goldman | `dword_46492C = -1` on entry (and on every Screen B restart) — the standard pending-gold clear. |
| text | `sub_41696C(surface, text, x, clip_w, y, ink, outline)` — glyphs drawn 4× in `outline` then 1× in `ink` (confirmed order, `frontend-flow.md`). |

### The 4th VALUELST column is CLIP WIDTH, not colour — CORRECTION

`setup-screens.md` and `multiplayer-deep.md` §2 both record 763/778 as "the
colour". They are the **max-width / clip** argument. Raw bytes (Screen B's
title draw, mirrored exactly on Screen A):

```
push [0x495390]      ; a7 outline
push [0x497F8F]      ; a6 ink
eax=0x2EF(751); call getvalue; push eax   ; a5 = Y
eax=0x2EE(750); call getvalue; mov ecx,eax ; a3 = X      (HR-dropped)
eax=0x2F1(753); call getvalue; mov ebx,eax ; a4 = CLIP W
```

Inks are never VALUELST ids on these screens — they are the fixed COLOR.PAL
LUT byte globals (§8). This is the same correction the 2026-07-12 pixel pass
already made for the player-input screen's column 4.

### VALUELST geometry (both screens are pixel-identical)

`VALUELST.RES` rows are `id, X, Y, YSTEP, W` → consecutive ids.

| block | id → X | id → Y | id → YSTEP | id → W |
|---|---|---|---|---|
| Screen B title | 750 = **120** | 751 = **80** | 752 = 0 | 753 = **400** |
| Screen B header | 755 = **120** | 756 = **120** | 757 = 0 | 758 = **250** |
| Screen B rows | 760 = **120** | 761 = **140** | 762 = **20** | 763 = **400** |
| Screen A title | 765 = **120** | 766 = **80** | 767 = 0 | 768 = **400** |
| Screen A header | 770 = **120** | 771 = **120** | 772 = 0 | 773 = **250** |
| Screen A rows | 775 = **120** | 776 = **140** | 777 = **20** | 778 = **400** |

Row *i* is drawn at `y = Y + YSTEP*i`.

---

## 5. Screen A — START NET GAME (host), `sub_42B0CE` @0x42B0CE

### Entry

`sub_40C839(2)` → `sub_4121FF` → `dword_4646BC = 0` → `sub_411CF8`
(`dword_460244 = 1`) → `sub_4224E2` → `sub_40FF57` → **`dword_464990 = 1`**
→ music 1040 → `sub_4148E5` (backdrop) → `t_announce = timeGetTime()` →
`dword_46492C = -1` → **`sub_40F290()`** (clear the client table) →
`t_change = timeGetTime()`, `sum = last_sum = 0`.

> `dword_464990` is the `diseases_destroyable` option — **hosting a net game
> force-enables it**, and `sub_40FFBC` restores the local value on exit.

### Frame

1. `sub_415CA4` (restore backdrop), `sub_40EA1E` ×2 (net pump), `sub_40FC44`
   (footer).
2. **Announce**: if `t_announce + 1000 <= now` → `t_announce = now`,
   `sub_40EBC1()` (kind-0 broadcast), pump. So the host beacons at **1 Hz**.
3. Title: `sprintf(buf, getstring(73), sub_40FE34())` — **1 arg, `%s` = our
   node name** — at (120, 80), clip 400, ink `byte_497F8F`, outline black.
4. Header: `getstring(70)` at (120, 120), clip 250, ink `byte_49D38F`.
5. **Client list, `for i in 0..3`** — `v = sub_40F1E9(i)`:
   - `v != 0` → `sprintf(buf, getstring(71), sub_40F217(i), v)` and
     `sum += v`. Format is `%s`, `%u` = **client name, client node id**
     (raw: `push v ; push name ; push fmt ; push buf ; call sprintf ;
     add esp,0x10` — the second arg is *HR-dropped*).
   - `v == 0` → `sprintf(buf, getstring(72))`, **no args**.
   - drawn at (120, 140 + 20·i), clip 400, ink white, outline black.
   - **There is no selection cursor and no per-row interaction on Screen A.**
6. If `sum != last_sum` → `t_change = now; last_sum = sum` (the client set
   changed; restart the settle timer).
7. `sub_415C1F` (present), `key = sub_4102B7()`; any real key
   (`!= -1 && != -2`) plays SFX **20**.

### Keys

| key | code | action |
|---|---|---|
| **Enter** | 13 | START (below) |
| **Space** | 32 | START (identical path) |
| **Esc** | 27 | leave — set done, skip the match, run the epilogue |
| anything else | | ignored (the SFX-20 blip still fires) |

### The START path

```
now = timeGetTime()
if (now < t_change) t_change = now                  // clock-wrap guard
if (t_change + 1000*getvalue(13) >= now) -> ignore  // silently, no feedback
if (sub_410262() == 0)                              // = count of clients
    modal(title=getstring(95), body=getstring(105), ink=byte_49A390,
          outline=byte_49D37A)
    t_change = now                                   // restart the settle wait
else
    ++dword_4642C0                                   // play-telemetry counter
    SFX 10
    5 x { sub_40ED08(); pump; sub_413CB0(100); }     // kind-14 start x5
    sub_40FE88(); pump                               // kind-49 options
    done = 1  ->  sub_42A3F6()                       // the shared Play flow
```

`sub_4148AC()` = `sub_40C06A() ? getvalue(13) : 1`, and **`getvalue(13) = 3`**
— VALUELST's own comment: *minimum number of seconds to wait at screens so
that other computers can catch up*. So the host cannot start until the client
set has been stable for **3000 ms**, and an attempt inside that window is
silently swallowed.

The start broadcast is deliberately sent **5 times, 100 ms apart** (no
acknowledgement) before the options blob — the original's crude reliability
for the one message that cannot be missed.

### Exit / epilogue

`sub_40EC29()` (kind 1, "host is leaving"), pump, `sub_40FFBC` (restore
options), `sub_422552` (restore roster), `sub_40CBB9()` (10 pumps at 100 ms,
close the transport, `sub_43B86D`, `sub_40C035(0)`, `dword_4600D4 = 0`),
`sub_4121FF`. Note the teardown runs **after** the match returns.

---

## 6. Screen B — JOIN NET GAME (guest), `sub_42B47D` @0x42B47D

### Entry and the restart label

Entry: `sub_40C839(1)` → `sub_4121FF` → `dword_4646BC = 0` → `sub_411CF8` →
`sub_4224E2` → `sub_40FF57` → music 1040.
**`LABEL_3` @0x42B4C5** (jumped to by three of the error paths):
`dword_46492C = -1`, `sub_4148E5()` (a **new random backdrop**),
**`sub_40F243()`** (clear the server list), `sel = 0`, `done = 0`.

The guest originates nothing while browsing (only `sub_40EA1E`'s own
retransmit flush) — **discovery is passive**: the pump `sub_40EA1E` →
`sub_40E765` feeds the hosts' kind-0 announces into `sub_40CF93`, which
inserts/refreshes each server by node id and stores its name and client count.

### Frame

1. `sub_415CA4`, `sub_40EA1E` (pump ×1), `sub_40FC44` (footer).
2. Title: `sprintf(buf, getstring(66), sub_40FE34())` — `%s` = our node name —
   at (120, 80), clip 400, ink `byte_497F8F`, outline black.
3. Header: `getstring(60)` at (120, 120), clip 250, ink white.
4. **Server list, `for i in 0..9`** — `v = sub_40F163(i)`:

   | state | line |
   |---|---|
   | `v != 0` (a server is present) | `sprintf(buf, getstring(62), sub_40F191(i), sub_40F1BD(i), v)` — **3 args in this order: `%s` server name, `%u` how many clients it already has, `%u` its node id**. Raw: `push v ; push sub_40F1BD(i) ; push name ; push fmt ; push buf ; call sprintf ; add esp,0x14`. **Two of the three were HR-dropped** — the decompile shows only the name. |
   | `v == 0` | `sprintf(buf, getstring(63))` — **no args** (the decompile's third argument is a phantom). |

   drawn at (120, 140 + 20·i), clip 400, ink `byte_49D38F`, outline black.
5. **Selection cursor** — `sub_413BD6(x, y)` with, raw:
   `x = getvalue(760) - 20 = 100`,
   `y = getvalue(761) + 16 + getvalue(762)*sel = 156 + 20*sel`.
   The `+16` half-row offset was *HR-dropped* entirely (the EDX argument).
   `sub_413BD6` draws the shared blinking `cursor1` ANI (interval
   `getvalue(690)`/`getvalue(691)`), same primitive the player-input screen
   uses at `x-15`.
6. `sub_415C1F`, `key = sub_4102B7()`, SFX **20** on any real key.

### Keys

| key | code | action |
|---|---|---|
| **Up** | 0x148 (328) | `--sel`, wrap `< 0 → 9` |
| **Down** | 0x150 (336) | `++sel`, wrap `>= 10 → 0` |
| **Enter** | 13 | **JOIN** the selected server (below) |
| **Space** | 32 | **JOIN** — identical path |
| **Esc** | 27 | leave the screen |
| anything else | | ignored |

> **CORRECTION.** `setup-screens.md` / `multiplayer-deep.md` §2.2 record
> "any key < 0x20 (Enter 13 / Esc 27) → leave the screen". Raw bytes: keys
> `< 13` and `14..26` are ignored, **Enter is the join/commit key** (it lands
> on the same label as Space), and only **Esc** leaves.

### The JOIN state machine (this is the whole lobby)

```
[SELECT]  Enter/Space
   |  SFX 10
   |  v = sub_40F163(sel)
   +-- v == 0 ------------> modal(95, 65)  "must select a server"   -> [SELECT]
   +-- sub_40F1BD(sel) >= 4 -> modal(96, 7) "server already full"   -> [SELECT]
   |        (this one is NOT wrapped in sub_431178/sub_431360)
   v
[REQUEST]  sub_415CA4(); sub_415C1F();      // one flush frame
           sub_40F364(0);                   // clear "my host"
           t0 = timeGetTime()
           do { sub_40EC6F(sel);            // kind-3 join request
                pump;
                sub_413CB0(1000);           // 1000 ms  (10 x Sleep(100))
                t1 = timeGetTime(); }
           while (!sub_40F386() && t0 + 3000 > t1)     // ~3 attempts
   +-- timed out ---------> modal(95, 100) "unable to connect"  -> LABEL_3
   v   (accepted: dword_4600D4 = host id, via kind 18)
[WAIT]     sub_40F320(0)                    // clear the start flag
           loop {
             sub_415CA4()
             c = dword_45BFB4[dword_464AFC & 3]; sprintf(buf, getstring(80), c)
             ++dword_464AFC
             sub_41696C(surface, buf, x=150, clip_w=400, y=200,
                        ink=byte_49D38F, outline=byte_495390[0])
             pump; sub_415C1F(); pump
             if (sub_40F342()) started = 1              // kind-14 arrived
             if (!sub_40F163(sel))                      // host vanished
                 modal(95, 110) -> LABEL_3
             if (getkey() == 27) -> leave the screen
           } until started
   v
[START]    ++dword_4642C4 ; sub_42A3F6()  // the shared Play flow
           on return: leave the screen
```

Details that matter for a faithful port:

- **`dword_45BFB4[4] = { 47, 45, 92, 124 }` = `'/'`, `'-'`, `'\'`, `'|'`** — a
  classic 4-phase spinner. `getstring(80)`'s single `%c` takes it.
  `dword_464AFC` is a free-running global (never reset), advanced **once per
  rendered frame** — the animation is frame-rate paced, not time-paced.
- The prompt is at **x = 150, y = 200, clip width 400**. (`multiplayer-deep`
  and `setup-screens` record "(150,400)" — that is x and the clip width; the
  y is 200.)
- The request loop's sleep is **1000 ms per attempt** (`sub_413CB0(0x3E8)`),
  bounded by **3000 ms** total.
- `sub_431178()` / `sub_431360()` bracket three of the four modals (a
  save/restore of the framebuffer region behind the dialog). The
  "server full" modal is *not* bracketed — an original inconsistency, worth
  reproducing only if the port is chasing artefacts.
- Esc inside [WAIT] leaves the *screen*, not just the wait.

### Exit / epilogue

`sub_40EDC9()` (kind 39 "guest is leaving", then 10 pumps at 100 ms),
`sub_40EA1E`, `sub_40FFBC`, `sub_422552`, `sub_40CBB9()`, `sub_4121FF`.

---

## 7. What happens after either screen commits — where the map and the AI live

Both screens hand off to **`sub_42A3F6()`** — the *same* handler main-menu row
0 (PLAY) uses. There is no net-specific match setup:

```
sub_42A3F6:  100 x pump  ->  music 1020 (0x3FC)
             sub_410F81()            // PLAYER INPUT TYPE screen (roster + AI)
             if (!aborted) { sub_410B6E(); <round loop> }
sub_410F81 tail:            sub_406DDE()   // LEVEL / ROUNDS screen (the map)
```

So **map selection and AI/CPU slot assignment for a net game come from the
shared local screens**, not from the network screens. Their net behaviour:

- **`sub_410F81` head** — a guest (`sub_40C06A() == 1`) runs `sub_40F3A8()`
  (clear the handshake flag) then **blocks** in a pump loop until
  `sub_40F3C7()` (`dword_4600EC`, set by kind-41 `sub_40D4C8`); Esc aborts.
  Then it walks its own 10 slots and **uploads every local slot whose type is
  non-zero and != 4** with `sub_40EE16(i, type, sub)` (kind **40**). That is
  how a machine contributes its local humans/AI to the shared roster.
- **`sub_40D372`** (the kind-40 handler) applies a remote slot as
  `sub_421E33(slot, 4, 0)` — input type **4 = network-remote** — or type 0 to
  clear it. Type 4 is the only marker of "someone else's player".
- **Guests are read-only** on both shared screens: every edit path is guarded
  by `sub_40C06A() != 1`, and a guest that presses an edit key or Enter falls
  through to a shared `sub_427961(40)` label (`LABEL_159` in `sub_410F81`,
  `LABEL_97` in `sub_406DDE` — the SFX-40 sites `multiplayer-deep` §2.5 asked
  about). They mean exactly "a guest touched a host-only control", nothing
  about a wait state.
- The **host** makes every change and broadcasts it: roster type
  (`sub_40EE16`, kind 40), team flag (`sub_40EE59`, kind 58), **level index
  (`sub_40FA66`, kind 43)**, **round count (`sub_40FAD5`, kind 44)** — both
  host-only (`dword_460058 == 2`) with the guest reading them back through
  `sub_40FAB3()`/`sub_40FB22()` each frame — and the screen-advance commands
  `sub_40F064(901)` / `sub_40F064(902)` (kind **32**, handler `sub_40D48E` →
  `sub_410401`) that push guests out of the roster and level screens.
- The host's Enter on both shared screens repeats the **same 3000 ms settle
  gate** (`t + 1000*sub_4148AC() >= now`) and requires `sub_42223E() >= 2`
  players.
- Abort (Esc on the roster screen): SFX 10, `sub_40F064(27)` (kind 32 carrying
  27) then `sub_40EDC9()` — unconditionally, even on a host; the screen tail
  then sends the role-appropriate leave (`sub_40EC29` kind 1 for mode 2,
  `sub_40EDC9` kind 39 for mode 1).

**Port mapping** (2026-07-25, ADR-0011): the port reproduces this SHAPE, not the
1997 wire. `GameApp::present_net_setup` runs between the connect step (lobby
punch or direct seed handshake) and the match, over the same UDP socket, driving
the ORDINARY `SetupScreen` (`sub_410F81`) and `MapSelectScreen` (`sub_406DDE`)
with a `NetSetupLink` (`libs/game/include/bomber/game/screens/net_setup_link.hpp`):
the host publishes a `net::SetupPreviewFrame` after each edit, the guest renders
it read-only and buzzes SFX 40 at any edit key. Differences, all deliberate:

- **No guest->host slot upload** (kind 40 from `sub_410F81`'s tail). The port is
  host-drives-everything, so the two wire seats are LOCKED — the local one to
  KEYBOARD, the remote one to type 4 OTHER (`sub_40D372`'s own marker) — and
  every other slot cycles OFF <-> COMPUTER. AI slots are how an online match gets
  more than two players.
- **No kind-32 screen-advance command** (`sub_40F064(901/902)`). The preview's
  `rounds` field carries the cue instead: 0 while the host is on the roster
  screen, the real win target once it reaches LEVEL & ROUNDS.
- **The confirmed payload is the whole resolved `sim::MatchConfig`**, not the
  level index + round count the original sends as kinds 43/44 — see
  `match_config_codec.hpp` for why an index cannot be trusted across two installs.
- The 8 kind-49 options are not pushed separately; they are already baked into
  the confirmed config's `Tuning`.

**The 8 options a host pushes** (`sub_40FE88`, kind 49, 8 words in order) are
exactly the 8 `sub_40FF57`/`sub_40FFBC` save/restore: `dword_46497C`
(win_by_kills), `dword_464940` (stomped_bombs_detonate), `dword_464930`
(conveyor_speed), `dword_464964` (team_play), `dword_464974`
(enclosement_depth), `dword_464948` (playtime), `dword_464990`
(diseases_destroyable), `dword_464928` (lost_net_revert_ai). **Note what is
NOT in the list: the level/map and the round count** — those travel as their
own messages from the level screen.

---

## 8. Colour table — LUT-decoded RGB

Method (`frontend-flow.md` "COLOR.PAL decoded for real"): every `byte_49XXXX`
ink is element `addr - 0x495390` of COLOR.PAL's 32768-byte RGB555→index LUT;
`RGB = masterPalette[LUT[offset]] << 2`.

| global | LUT offset | idx | **RGB** | used for |
|---|---|---|---|---|
| `byte_497F8F` | 0x2BFF | **97** | **(96, 252, 252)** | the title line of **both** screens (bright cyan) |
| `byte_49D38F` | 0x7FFF | 72 | (240, 248, 252) | header, every list row, the waiting prompt, the footer |
| `byte_495390[0]` | 0 | 255 | (0, 0, 0) | the 4-way outline on all screen text |
| `byte_49A390` | 0x5000 | 248 | (164, 0, 0) | **ink** of every modal (the 5 in-screen ones + `sub_40C839`'s 2) |
| `byte_49D37A` | 0x7FEA | 182 | (252, 248, 88) | **outline** of those same modals |

`byte_497F8F`'s LUT-true value is new here — `results-and-options.md` only had
the superseded nearest-search reading ("cyan-ish").

The modal ink pair is unusual and was traced end to end:
`sub_414340(EAX=line1, EDX=line2, EBX=ink, ECX=outline)` →
`sub_4172BA(win, text, cx, y, [+0x10]=EBX, [+0x14]=ECX)` →
`sub_41696C(..., a6=[+0x10], a7=[+0x14])`, and a6 is the last (top) pass.
So the net modals are **dark-red glyphs with a bright-yellow outline**, not
the usual white-on-black. Both lines share the pair; the box is centred, width
`max(measure(l1), measure(l2))` clamped to ≥80 then +64, height
`2·fontheight + 4·fontheight + 64`, built by `sub_43C734(x, y, w, h, 256, 20)`
with an OK button (`sub_432298`) and a `getstring(27)` label.

---

## 9. Message ids — purpose and format signature only

| id | screen | args | purpose |
|---|---|---|---|
| 7 | B | — | "server already full" (the `sub_40F1BD(sel) >= 4` refusal) |
| 55 | both | `%u` | the packet-version footer (`dword_45BAC4` = 21356) |
| 57 | both (via `sub_40C839`) | — | transport connect caption |
| 60 | B | — | list header: available net games |
| 62 | B | `%s`, `%u`, `%u` | an occupied server row: **name, client count, node id** |
| 63 | B | — | an empty server row |
| 65 | B | — | "must select a server" |
| 66 | B | `%s` | title: our node name |
| 70 | A | — | list header: connected network players |
| 71 | A | `%s`, `%u` | a connected client row: **name, node id** |
| 72 | A | — | an empty client row |
| 73 | A | `%s` | title: our node name |
| 80 | B | `%c` | the waiting-for-start prompt (spinner char) |
| 95 | both | — | modal **line 1** for the four "note" cases (bodies 65, 100, 105, 110) |
| 96 | both | — | modal **line 1** for the three "problem" cases (bodies 7, 291, 340) |
| 100 | B | — | join attempt timed out |
| 105 | A | — | cannot start: no clients connected |
| 110 | B | — | the server stopped being a server while we waited |
| 291 | both (via `sub_40C839`) | — | could not establish the net connection |
| 340 | both (via `sub_40C839`) | — | networking disabled in CFG.INI |
| 500..548 | boot | — | the 49 random default node names (`getvalue(47) = 49`) |
| 620, 630..634 | `sub_40C839` | — | the protocol picker prompt and its 5 entries |

> **CORRECTION.** `setup-screens.md`'s id table assigns 62 "formatted with the
> controller index", 63 "an OFF/inactive slot", 70-73 "screen-A labels
> (active/inactive item, headers)", and lists "95, 100, 110" as the overlay
> set. The real set is **95 + {65, 100, 110}** and **96 + {7}**; there is no
> controller index anywhere; and 73/66 are titles carrying the node name.

---

## 10. `unk_4632CC` is NOT a player-config store — CORRECTION

`setup-screens.md` records: *"Player-config store: `unk_4632CC`, 404 bytes per
player × 10 … holds per-slot: active flag, the assigned controller/type … and
the team."* That is wrong, and neither net screen touches it.

`unk_4632CC` is the **Win32 joystick capabilities array**:
`sub_42965C` @0x42965C does `pjc = (LPJOYCAPSA)((char*)&unk_4632CC + 404*i);
joyGetDevCapsA(i, pjc, 0x194u);` — `0x194 = 404 = sizeof(JOYCAPSA)`. And
`sub_429A61(i)` (bounds `0..9`) returns `&unk_4632CC + 404*i + 4` =
`JOYCAPSA::szPname`, the stick's product name.

The real per-slot stores are: the **152-byte player records** at
`dword_461BC4` (type at +16, sub-index at +17 — `setup-screens.md`
`sub_421DD2`, and the pair the net screens save/restore) and the net tables in
§3.

---

## 11. Summary of corrections made to existing docs

| doc | said | actually |
|---|---|---|
| `multiplayer.md` §1.1, `frontend-flow.md`, `multiplayer-deep.md` §2/§3 | `dword_460058`: 1 = host, 2 = guest | **1 = guest, 2 = host** (§1, four independent proofs + raw bytes) |
| `multiplayer-deep.md` §2 | the row-1/row-2 host/guest labelling is contradictory, needs the binary | labels are correct; the mode table was inverted. Row 1 `sub_42B0CE` = host, row 2 `sub_42B47D` = guest |
| `multiplayer-deep.md` §2.1, `setup-screens.md` "Screen A" | "a 4-item options list … the game-type/options pane for a net match (team play etc.)" | the host's **4 connected-client slots**; no options at all. Net options travel as kind-49 from the host and come from the Options screen |
| `multiplayer-deep.md` §2.2, `setup-screens.md` "Screen B" | "the controller/roster assignment", "Space = BIND DETECT", "bind that controller to the slot" | a **server browser**; Space/Enter = *join the selected server*; the 3000 ms loop retransmits a join request, it does not poll controllers; the second loop waits for the host's start |
| both | `sub_40F1BD(i) < 4` = "a keyboard/joystick index" | = "that server has fewer than 4 clients" |
| both | 763 / 778 = colour | = **clip width**; inks are the fixed LUT globals |
| `setup-screens.md` | "(150,400)" for the prompt | x = 150, **clip w = 400, y = 200** |
| `setup-screens.md` | Enter/Esc both leave Screen B | **Enter joins**; only Esc leaves |
| `setup-screens.md` | `unk_4632CC` = 404 B/player player-config store | `JOYCAPSA[10]` from `joyGetDevCapsA` (§10) |
| `multiplayer-deep.md` §2.4 | netprotocol 0..3 mapping [NEEDS BINARY] | **0 cancel / 1 IPX / 2 modem / 3 serial / 4 TCP-IP**, and TCP/IP is unreachable (§2) |
| `multiplayer-deep.md` §2.5 | is there a separate lobby/waiting-room screen? | **No.** Screen A *is* the host's waiting room (its client list is live); Screen B's post-accept spinner loop is the guest's. Both are described completely above |
| `multiplayer-deep.md` §2.5 | the SFX-40 sites need reading | they are the guest's "denied" buzz on the **shared** roster/level screens, not on the net screens (§7) |

---

## 12. Still [NEEDS BINARY]

| question | what would settle it |
|---|---|
| Does the host's 4-client cap imply 4 *machines* or 4 *players*? `word_45FFA4[4]` is keyed by node id, so it is 4 remote **machines** (+ the host = the 5 of `word_460130[5]`); the per-machine human count is bounded only by the shared roster's 10 slots. The exact clamp when a 5th machine requests is unread. | `sub_40D175`'s fall-through when both `for` loops fail (0x40D175), and whether any reply is sent |
| Whether a guest that is *already* in [WAIT] is re-seated if the host restarts its screen | `sub_40D073`/`sub_40D251` interaction with `sub_40F243` |
| The exact glyph offsets of `sub_41696C`'s 4 outline passes | register-lost in `sub_41696C` (0x41696C ~0x416A5x); the four cardinal ±1 offsets remain the only consistent reading |
| The font in effect on these screens (the `dword_45C37C`/`dword_45C380` pair is a global font vtable; no net screen sets it) | the writer of `dword_45C37C`/`dword_45C380` in the window-system init |
| `dword_4646BC`, `dword_460244` (`sub_411CF8`) — both set on entry, purpose unread | writers/readers of 0x4646BC and 0x460244 |
| Whether the **host** ever surfaces a client's disconnect (the client row just vanishes; no sound, no modal on `sub_40D2F8`) | any presentation hook on kinds 1/39 outside the handlers |
