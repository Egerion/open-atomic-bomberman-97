# Multiplayer deep-dive audit — original netplay end-to-end (2026-07-24)

> **UPDATE 2026-07-25 — §2 has been superseded by a binary pass.** Both network
> screens were read end-to-end against the raw bytes; the definitive spec is
> **`docs/re/network-screens.md`**. It resolves this audit's biggest open items
> (host/guest polarity, the 4-item list, the "bind detect", the machine cap, the
> lobby flow, the `netprotocol` ordinals) and **corrects** several claims here —
> most importantly **`dword_460058` is `1 = guest`, `2 = host`**, the opposite of
> what §3 assumes. Corrections are marked inline; §1.4, §2.x and §5 carry the
> resolved state.

**Purpose.** A complete map of what the original 1997 **Atomic Bomberman**
(BM95.EXE, Watcom C, imagebase `0x400000`) offered in **network multiplayer** —
both the **menus/screens** and the **mechanics** — so the port's netplay
(currently a 2-player UDP-lockstep MVP, ADR-0010) can grow to match it. This
extends the existing `docs/re/multiplayer.md` (the base netplay audit) with the
four things the base doc left thin: the **max player/machine count**, the **full
set of network screens ("the extra pages")**, a **deeper mechanics diff**, and a
**concrete gap table** to drive the roadmap.

**Scope / method.** Docs-only RE. Reads `docs/re/multiplayer.md`,
`setup-screens.md`, `frontend-flow.md`, `facts.md`, `coverage-audit.md` §4,
`dark-matter.md` §4, `results-and-options.md` §2-3, `audit/{diseases,setup}.md`,
plus the port's own net path (`libs/net/`, `libs/game/src/game_app.cpp`
`run_netplay`/`run_netplay_match`/`present_net_host`/`present_net_join`,
`libs/game/src/screens/netplay_connect_screen.cpp`, `apps/game/main.cpp`). **The
binary and the `native/` transliteration are NOT in this worktree** (gitignored);
every claim that needs them to settle is marked **[NEEDS BINARY]** with the exact
`sub_XXXX` a trace must read. **No code was changed; no build is needed** (this is
an RE writeup per the CLAUDE.md RE workflow).

> **One-line verdict.** The original was a **host-authoritative,
> replication-with-correction** multiplayer for a **10-slot roster** in which
> remote humans occupy **input-type-4 ("network-remote") seats**; the port
> reproduces only the 2-seat, single-round, no-lobby slice of that. The three
> biggest gaps are **player count (2 → up to 10)**, **a real net lobby/roster
> screen (none → the 10-slot roster with type-4 seats)**, and **round rotation +
> dropout-to-AI**. The port should keep its own **clean deterministic lockstep**
> (it is provably better than the original's correction protocol) and copy only
> the *shape* — roster, roles, lobby, revert-to-AI — not the 1997 wire protocol.

---

## 1. MAX player count in netplay — how many remote humans, across how many machines

### 1.1 What the RE positively establishes

- **The roster is 10 slots**, everywhere, net or local. `sub_421DD2(i,&type,&sub)`
  reads player `i` from `dword_461BC4[38*i]` (152 B/player) for `i in 0..9`; the
  player-setup screen `sub_410F81` loops `for i in 0..9` (`setup-screens.md`
  "Screen 1"). There is no separate, smaller "network roster".
- **Remote humans are a first-class slot TYPE, not one reserved seat.** The
  input-type byte `+16` takes values `0=off, 1=computer, 2=keyboard, 3=joystick,
  **4=network-remote**` (`setup-screens.md` "State model — CONFIRMED
  `sub_421DD2`"). Type 4 is the fifth input *source*, filled by the net layer
  instead of local hardware — and nothing in the RE ties it to a single slot
  index. So **any of the 10 slots can in principle be a type-4 remote seat.**
- **The local setup screen cannot author type-4 slots** — it only *renders* them.
  On `sub_410F81`, Right (`sub_421E80`, cycle type forward) and Left
  (`sub_421E33(i,0,0)`, set OFF) both **skip a slot whose type == 4** ("unless
  type==4", `setup-screens.md` key table). Type-4 slots are populated by the net
  path (the join/receive machinery), confirming they are the seat a *remote*
  human drives.
- **A machine holds exactly ONE role.** `dword_460058` (via `sub_40C06A`, set by
  `sub_40C035`) is tri-state: `0=local, 1=host, 2=guest` (`multiplayer.md` §1.1,
  `facts.md` 2271-2274). There is **no** multi-guest role value — a box is the
  host, or it is *a* guest. This is the strongest structural clue that the model
  is **one host + N single-role guests**, not several peers each hosting locals.
- **The host owns the whole roster and the master clock** (§3 below), i.e. the
  10 slots are authoritative on the host and *replicated* to guests — the natural
  shape for "the host machine fills the roster, remote machines each claim a seat".

### 1.2 The finding

The RE is **consistent with up to 10 networked players** and does **not** support
a hard cap below 10 anywhere it has looked: the roster is 10, type-4 is a general
slot type, and the host owns all of it. This aligns with the game's historical
"up to 10 players" billing — but that alignment is *external* knowledge, not RE,
so it only corroborates, it does not prove.

What the RE does **not** settle, and a binary trace must:

- **The MACHINE cap (endpoints).** Roles are `{host, guest}`, so the question is
  "how many guests can one host service". The single most concrete numeric clue
  is the **5-slot packet-queue helper cluster at `0x430C00`–`0x4331EC`**
  (`dark-matter.md` §4) that calls straight into the Winsock cluster — a fixed
  *five* buffers. That could be five connection slots (host + up to 4 guests) or
  just a five-deep packet ring; **[NEEDS BINARY]** to tell which. If it is
  connection slots, the *machine* cap is ~5 even though the *player* cap is 10 —
  i.e. **multiple local humans per machine mixed with remote seats** (2 kbd sets
  + joysticks locally, others remote) would be how 10 players fit across ~5 boxes.
- **Whether one machine can seat multiple local humans in a net game.** Locally
  the setup screen binds up to two keyboard sets + joysticks per machine; whether
  the net path preserves that (so a box contributes 2-3 local seats) or forces
  one-human-per-machine is unconfirmed. The Space "bind detect" being net-only
  (`sub_40C06A()==1`, `setup-screens.md`) hints each machine *claims* its local
  controller(s) into the shared roster at join time, which would *allow* multiple
  locals — but the count is not RE'd.

### 1.3 [NEEDS BINARY] — exact functions to confirm the cap

| To confirm | Read | What to look for |
|---|---|---|
| Player-slot cap in the net setup UI | `sub_42B0CE` (0x42B0CE), `sub_42B47D` (0x42B47D) | the roster loop bound (is it `0..9`?) and any clamp when assigning/reserving a type-4 seat |
| Machine/endpoint cap | packet-queue cluster **`0x430C00`–`0x4331EC`** (the "5-slot" helper), transport cluster `sub_43BA06`–`sub_43C668` | is the "5" a per-peer connection table (→ ~5 machines) or a packet ring depth? how many socket/peer descriptors are allocated? |
| Remote-command fan-out | receive pump `sub_40E765` (0x40E765) + its dispatch table **`funcs_40E9D7`**; send flush `sub_40EA1E` (0x40EA1E) | does a message carry a **seat/player index** (multi-seat capable) or address a single fixed remote seat? |
| Per-seat guest sends | `sub_40CE27` (0x40CE27), `sub_40FE88`, `sub_40FF14`, `sub_40FDE8` | whether the packed payload includes a seat id (→ many seats) or is hard-wired to "this guest's one seat" |
| Roles | `sub_40C035` (0x40C035) writers of `dword_460058` | confirm only `{0,1,2}` exist (no 4th "guest #k" role); a guest-index must therefore live elsewhere (the roster / a per-connection field) |

### 1.4 RESOLVED 2026-07-25 — the machine cap is **5**, and multi-local is real

The 2026-07-25 net-screens pass (`docs/re/network-screens.md`) settles both
open variables:

- **1 host + up to 4 guests = 5 machines.** The host's client table is
  `word_45FFA4[4]` / `dword_45FFFC[4]` (`sub_40D175` seats a join request in
  the first free entry of a `for j in 0..3`); the assembled session node list
  is `word_460130[5]` (`sub_40ED08`: `[0]` = host, then each client); and the
  receive pump `sub_40E765` **drops any datagram whose sender is not one of
  those 5** (`for (i = 0; i < 5 && word_460130[i] != sender; ++i); if (i >= 5)
  discard`). The "5-slot packet-queue cluster" question also resolves: boot
  allocates `sub_418511(1004, 5)` = **five 1004-byte per-node dedupe rings**
  (500 sequence entries each), indexed by that same node slot — per-peer, not
  ring depth.
- **Multiple local humans per machine: YES.** `sub_410F81` (the shared roster
  screen) walks all 10 slots on a guest and uploads **every** slot whose local
  type is non-zero and != 4 with its own `sub_40EE16(i, type, sub)` (kind 40).
  Nothing clamps the count. So 10 players across ≤5 boxes is exactly the
  intended shape, and the per-box limit is just local hardware.
- **There is no "bind detect".** The Space key the previous pass read as a
  net-only controller bind is the JOIN key on the guest's server browser
  (§2.2); seats are claimed through the shared roster screen, not the lobby.

**Bottom line for the roadmap:** target **up to 10 players across up to 5
machines (1 host + 4 guests), any number of local humans per machine**. The
port's current **hard 2** (host=seat 0, guest=seat 1; `run_netplay_match`
builds a canonical 2-human config) is the #1 gap.

---

## 2. The network menu / screens in full — "the extra pages"

> **SUPERSEDED 2026-07-25 by `docs/re/network-screens.md`.** Both screens have
> since been read end-to-end against the raw bytes; that file is the definitive,
> porter-ready spec (geometry table, per-state line contents, key table,
> LUT-decoded inks, the full join/wait state machine). This section is kept as
> the index and now records the **corrections** that pass produced. Everything
> below that was marked [NEEDS BINARY] here is resolved there.

The net path is reached from **two adjacent main-menu rows** (`sub_42B9CE`, the
7-row menu, `frontend-flow.md` menu table):

| menu row | handler | VALUELST legend | mode-set at head | role | music | backdrop |
|---|---|---|---|---|---|---|
| 1 **START NET GAME** | `sub_42B0CE` @0x42B0CE | 765-778 | `sub_40C839(2)` | **HOST** | 1040 (`sub_42741E(0x410)`) | random `GLUE<n>` (`sub_4148E5`) |
| 2 **JOIN NET GAME** | `sub_42B47D` @0x42B47D | 750-763 | `sub_40C839(1)` | **GUEST** | 1040 | random `GLUE<n>` |

> **RESOLVED (2026-07-25) — the labels were right, the mode table was wrong.**
> The conflict this section flagged is settled: **`dword_460058` is `1 = guest`,
> `2 = host`**, the opposite of what `multiplayer.md` §1.1, `frontend-flow.md`
> and §3 below assume. Proofs: `sub_40C035` gives *only* mode 2 its own node id
> as session authority (`dword_4600D4 = HIWORD(dword_46013C)`); `sub_40CD1C`
> stamps mode 2's own id but mode 1's *host's* id into the packet header;
> `sub_4105D2` has mode 1 copy the match clock while mode 2 computes and
> broadcasts it; and the announce/start/options senders (`sub_40EBC1`,
> `sub_40ED08`, `sub_40FE88`) are all `== 2` only. Raw bytes at the heads:
> `0x42B0DC mov eax,2` / `0x42B48B mov eax,1`. VALUELST's own section comments
> ("JOIN NET GAME SCREEN" = 750-763 = the ids `sub_42B47D` reads; "START NET
> GAME SCREEN" = 765-778 = `sub_42B0CE`'s) corroborate.
> **Port mapping: `present_net_host` ⇒ `sub_42B0CE`, `present_net_join` ⇒
> `sub_42B47D`.** Full detail: `network-screens.md` §1.

### 2.1 Screen: START NET GAME — `sub_42B0CE` (the HOST's live client list)

**CORRECTED.** This is **not** an options pane. The `for i in 0..3` loop walks
the host's **connected-client table** `word_45FFA4[4]` / `dword_45FFFC[4]`
(`sub_40F1E9(i)` = client i's node id, `sub_40F217(i)` = a `char*` to its
name). An occupied row is `getstring(71)` with **two** args (`%s` name, `%u`
node id — Hex-Rays dropped the second); an empty row is `getstring(72)` with
**none**. So the host sees up to **4 connected machines**, and the cap of 4 is
where the 5-machine session limit (`word_460130[5]`) comes from.

The screen beacons `sub_40EBC1()` (kind 0, 82 B: client count + node name) at
**1 Hz**, and Enter/Space starts the game once the client set has been stable
for `1000*getvalue(13)` = **3000 ms** (VALUELST 13's own comment: "minimum
number of seconds to wait at screens so that other computers can catch up"),
requiring ≥1 client (`sub_410262()`), else modal 95/105. Start =
5× `sub_40ED08()` (kind 14) 100 ms apart, then `sub_40FE88()` (kind 49, the
8-word options blob), then `sub_42A3F6()`. Entry force-sets
`dword_464990` (diseases_destroyable) = 1. Full spec: `network-screens.md` §5.

### 2.2 Screen: JOIN NET GAME — `sub_42B47D` (the GUEST's server browser)

**CORRECTED.** Not a controller assignment and not a 10-player roster: the
`for i in 0..9` loop walks the guest's **discovered-server table**
`word_4600D8[10]` (filled passively by `sub_40CF93` from the hosts' kind-0
announces). An occupied row is `getstring(62)` with **three** args in this
order — `%s` server name (`sub_40F191(i)`), `%u` how many clients it already
has (`sub_40F1BD(i)`), `%u` its node id (`sub_40F163(i)`); two of the three
were Hex-Rays-dropped. An empty row is `getstring(63)`, no args. Title 66 and
header 60 both take the local node name. Rows at getvalue `760 (x) /
761+762*i (y) / 763 (**clip width**, not colour)`; cursor at
`sub_413BD6(760-20, 761+16+762*sel)` — the `+16` was dropped too.
`unk_4632CC` is **not** a player store; it is `JOYCAPSA[10]` from
`joyGetDevCapsA` (`network-screens.md` §10). Keys:

- **Up 328 / Down 336** — move the selection (wrap).
- **Enter 13 / Space 0x20** — **JOIN the selected server** (not a bind-detect):
  SFX 10; refuse with modal 95/65 if the row is empty or 96/7 if
  `sub_40F1BD(sel) >= 4` (server full); else send `sub_40EC6F(sel)` (kind 3,
  join request) once per second for up to **3000 ms** until `sub_40F386()`
  (the host's kind-18 accept) or time out with modal 95/100. Then a
  **wait-for-start** loop draws the animated `getstring(80)` prompt (spinner
  `dword_45BFB4[] = '/','-','\','|'`, advanced once per frame) at **x=150,
  y=200, clip w=400** until the host's kind-14 arrives (`sub_40F342()`), the
  host vanishes (modal 95/110), or Esc.
- **Esc 27** — leave the screen. Keys < 13 and 14..26 are **ignored**.

Message ids: 60/62/63/65/66/80/95/96/7/100/110. Full spec:
`network-screens.md` §6.

### 2.3 The `.BM` documentation screen — NETWORK.BM (NOT transport)

`NETWORK.BM` is one of the plain-ASCII `.BM` help/text screens
(`CREDITS.BM`/`OPTIONS.BM`/`INPUT.BM`/…) rendered by the single viewer
`sub_41302D` (`frontend-flow.md`). It is the *documentation about networking*,
**not** the interactive setup and **not** the transport. (The port currently maps
BOTH menu net rows to this help overlay as a stub in the pure-front-end path, but
the *real* rows are `sub_42B0CE`/`sub_42B47D` above.)

### 2.4 The nested CONFIG sub-screens off OPTIONS — the transport knobs

These are not on the net path itself but **feed** it; they hang off the **Options
screen** (`sub_4080DC`, `results-and-options.md` §2-3):

| Options row | id | label | backing global(s) / ini key | sub-screen |
|---|---|---|---|---|
| 2 | 252 | **Node Name** | `sub_40FE34` (net identity string; `options.ini`, not a keydef) | `sub_4074DC` — a text-entry edit |
| 12 | 262 | **Lost net players revert to AI** | `dword_464928` (`lost_net_revert_ai=`) | in-place toggle |
| 14 | 264 | **Modem: P/I/B/#** | `dword_464970`/`dword_4648B8`/`dword_46482C`/string (`modemport`/`modemirq`/`modembaud`/`modemdial`, defaults `2`/`3`/`19200`/`"555-1212"`) | **`sub_40798B` — a nested modem-config sub-screen** |
| 16 | 266 | **Set Default Network Protocol** | `dword_464828` (`netprotocol=`, clamped `<0→0`, `>3→3` — **four** choices) | **`sub_407F4F` — a nested protocol picker** |

So "the extra pages" that RE has located are: the two net-setup screens
(`sub_42B0CE`, `sub_42B47D`), the modem-config sub-screen (`sub_40798B`), and the
protocol picker (`sub_407F4F`), plus the NETWORK.BM help text. The **modem** and
**protocol** screens are the transport-selection UI; the **Node Name** is the
per-machine net identity; **Lost net players revert to AI** is a live gameplay
policy toggle (§3).

The **`netprotocol` → transport** mapping is **RESOLVED (2026-07-25)**:
VALUELST's own comment block at 1100-1104 (the per-protocol retransmit
timeouts) and MESSAGES 630-634 name the ordinals one-for-one —
**0 = none/cancel, 1 = IPX, 2 = modem, 3 = serial, 4 = TCP/IP**. But
`getvalue(1110) = 4` ("how many different protocols are supported") is the item
count handed to the picker widget (`sub_42FEF0(list, count)` iterates
`i < count`), so **TCP/IP is never drawn**, and `sub_40C839` accepts only
`1..3` anyway (raw: `cmp [46012C],1 / jl fail`, `cmp [46012C],3 / jle ok`) —
matching `sub_407F4F`'s `if (r >= 0 && r <= 3)` store and the `options.ini`
`>3 → 3` clamp. The shipped exe therefore supports **IPX / modem / serial**;
TCP/IP is dead data. Details: `network-screens.md` §2.

Also pinned there: entering either net screen runs the whole bring-up in
`sub_40C839` — the **`CFG.INI` `netonoff` gate** (0 → modal 96/340 and a
straight bounce back to the menu), the protocol picker, the transport bind
(`sub_43B81D` fills a 60-byte vtable at `0x4600F0`), and a cancellable
connect-wait — and a non-zero return **aborts the screen before it draws
anything**.

### 2.5 The "waiting for players / lobby" screen and host-start / join-while-waiting

**RESOLVED (2026-07-25). There is no separate lobby screen — the two net
screens ARE the waiting room, one per role**, and the wait is explicit in both:

- **Host** (`sub_42B0CE`): its 4-row client list is live (rebuilt every frame
  from `word_45FFA4[]`, which the kind-3 handler `sub_40D175` fills as guests
  arrive and the kind-39 handler `sub_40D2F8` empties as they leave), it
  beacons kind 0 at 1 Hz, and Enter is gated on a **3000 ms "client set
  unchanged"** settle timer plus ≥1 client. Host-start is broadcast as
  **kind 14 sent five times, 100 ms apart** (`sub_40ED08`), immediately
  followed by the 8-word options blob (kind 49, `sub_40FE88`).
- **Guest** (`sub_42B47D`): after its join is accepted (kind 3 → kind 18) it
  sits in a dedicated **wait loop** rendering the animated `getstring(80)`
  prompt until the host's kind 14 sets `dword_460068` (`sub_40F342()`), with
  live outs if the host disappears from the server list or Esc is pressed.
- A **mid-wait join** needs no special handling: the host's table simply gains
  a row (and its settle timer restarts, which is exactly what stops the host
  from starting a game a straggler has not finished joining).
- The **SFX-40 sites** are *not* on these screens. They are on the **shared**
  pre-match screens (`sub_410F81`, `sub_406DDE`), where a guest
  (`sub_40C06A() == 1`) that touches any host-only control gets the buzz —
  see §2.6 and `network-screens.md` §7.

### 2.6 After the lobby: the shared local screens carry map + AI (2026-07-25)

Both net screens commit into **`sub_42A3F6()`** — the very same handler
main-menu row 0 (PLAY) uses. There is **no net-specific match setup**:
`sub_42A3F6` → `sub_410F81` (PLAYER INPUT TYPE = the roster, where CPU slots
are set) → its tail `sub_406DDE` (LEVEL / ROUNDS = the map) → `sub_410B6E` +
the round loop. So **map selection and AI-slot assignment for a net game live
in the shared local screens**, host-driven:

- a guest blocks at `sub_410F81`'s head (`sub_40F3A8()` … until
  `sub_40F3C7()`), then **uploads each of its local non-zero, non-type-4
  slots** with `sub_40EE16` (kind 40); the receiver applies them as
  `sub_421E33(slot, 4, 0)` — input type **4 = network-remote**;
- every edit path on both screens is guarded by `sub_40C06A() != 1`, so a
  guest is read-only and gets SFX 40 on any attempt;
- the host broadcasts roster type (kind 40), team flag (kind 58), **level
  index (`sub_40FA66`, kind 43)** and **round count (`sub_40FAD5`, kind 44)**,
  and drives both screen advances with `sub_40F064(901)` / `(902)` (kind 32);
- the host's Enter on each shared screen repeats the same **3000 ms settle
  gate** and needs `sub_42223E() >= 2` players.

This is the answer to "where do map choice and AI slots go in the port's
online flow": **not into the lobby — into the existing shared setup screens,
with the host authoritative and the guest observing.**

**Port note.** The port has **no lobby at all**: `present_net_host` shows a
"WAITING FOR A PLAYER..." acknowledge modal (port-invented, `netplay_connect_
screen.cpp`) and `present_net_join` a "host:port" text-entry, then a **seed
handshake** (`SeedHandshake`), then straight into a 2-seat match. There is no
roster view, no slot list, no join-while-waiting for a 3rd+ peer.

---

## 3. Netplay MECHANICS — everything the original does differently under `dword_460058 != 0`

This deepens `multiplayer.md` §1.5/§1.6. All gates key on `sub_40C06A()` (=
`return dword_460058`) or a raw numeric comparison against it.

> **POLARITY CORRECTION (2026-07-25).** This section was written with
> `1 = host, 2 = guest`. **It is the other way round: `1 = guest,
> 2 = host`** (`network-screens.md` §1, four independent proofs plus raw
> bytes). Wherever §3 below names a role from the numeric value, swap it. The
> *behaviours* described are unaffected — only the labels are. The two
> subsection titles that stated it outright are fixed inline.

### 3.1 The clock authority is mode 2 = the HOST (`sub_4105D2` / `sub_4105B0`)

The match-clock update `sub_4105D2` (pseudo.c 14456-14552): when
`sub_40C06A() == 1` (**guest**) the displayed clock is *copied* from
`dword_4601B0`, the value received from the host; otherwise (local, or
`== 2` = **host**) it is computed from the wall clock `sub_43ACF8()`
(`timeGetTime`) — and a host whose value changed pushes it out with
`sub_40FCA1(clock)`. Guests therefore **take the match clock from the host,
not their own wall time** — the defining property of a host-authoritative
session. The receive pump runs at frame top (step 2, 29516) *before* the clock
update (step 4, 29518), so a guest's frame is stamped with host-derived timing
folded in first. (CORRECTED: the earlier reading had the roles swapped.)

### 3.2 HOST → guest state-packet sends (`sub_40CE27` & siblings), gated `dword_460058 == 2`

`sub_40CE27` packs `(x,y,type)` into a buffer (`sub_40CE27((__int16*)0x30,…)`);
the siblings `sub_40FE88` / `sub_40FF14` / `sub_40FDE8` sit in the same source
region and are the same shape (`facts.md` 3844-3849). **These carry position/tile
STATE, not button presses** — the guest is shipping *where things are*, which is
replication, not input lockstep. The send flush `sub_40EA1E` runs at the frame
tail (29550-29555, `facts.md` 1960-1961).

### 3.3 Tile-sync desync CORRECTION — five call sites

Five call sites of the board-tile-writer family (pseudo.c **12149, 12189, 12483,
12637, 22406**) are gated on `dword_460058`'s netplay flag and **"correct a remote
player's tile if it desyncs onto a brick"** (`facts.md` 5058-5060). A system that
*corrects* desync is, by definition, **not trusting determinism** to keep peers in
step — the clearest single proof the original is replication-with-correction, not
lockstep.

### 3.4 Net-only extra RNG draws — the disease reroll

The skull/disease roll runs a **200-try "reroll away from Swap" loop ONLY when
`sub_40C06A()` is non-zero** (`audit/diseases.md`, `diseases.cpp`). A net game
thus draws a **different number of `rand()` values per roll** than a local game —
which under a *genuine* deterministic lockstep would itself guarantee desync. That
the original does it anyway is further evidence its net path **re-synced state**
rather than relying on bit-identical simulation.

### 3.5 Type-4 (network-remote) seats are SKIPPED by the local kill/enclosure logic

- **Kill dispatcher `sub_41DE63`** early-outs for a player with `+16 == 4`
  (`facts.md` 1010-1012) — the host's flame/crush death routine does not "kill" a
  remote seat locally; its death is driven from the authoritative side.
- **Death-scatter `sub_41DBFE`** is skipped entirely for `+16 == 4` ("`if
  (player[+16] != 4)`", `facts.md` 1030) — no powerup scatter for a remote seat's
  death on the non-authoritative side.
- **Enclosure/crush finder** (`sub_421D3F` → `sub_41DE63`, `enclosure.md`)
  treats `type != 4` as the condition to act — remote seats are excluded from the
  local wall-crush check (`facts.md` 2661-2667).

These are all consistent with "the remote seat is a **spectator/replica** on this
machine — its life-and-death is decided elsewhere and mirrored in."

### 3.6 Local-only mechanics SUPPRESSED in net games

Two hidden-powerup mechanics are **local-only** (`!sub_40C06A()`), i.e. turned
**off** in any net game (`multiplayer.md` §1.6, `flames.cpp`, `audit/flames.md`):

- **Overpowered-powerup relocation** (Punch/Grab/SuperDisease hide-relocate in the
  opening 40 s).
- **Flame → hidden floor-powerup reveal.**

Plus **per-kind spawn-count `k==12` zeroing** is net-only (`sub_40C06A() && k==12`,
`audit/setup.md`), and the **Goldman roulette wheel** and **campaign `.cam`
picker** are local-only. (`SFX 40` net non-host feedback, §2.5, is presentation.)

### 3.7 "Lost net players revert to AI" (Options row 12, `dword_464928`)

A live toggle (`lost_net_revert_ai=`) with a real consumer in the original: when
a networked human **drops**, their seat is handed to the **AISystem** (`+16`
flips to the computer type `1`) so the match continues rather than stalling on a
dead connection. The exact drop-detection + handover site is **[NEEDS BINARY]**
(likely near the receive pump `sub_40E765` / a per-connection timeout in the
`0x430C00` queue cluster), but the policy is confirmed by the option's existence
and label.

**CONSUMED as of the peer-drop increment** (ADR-0011 Risks, "Dropped/late
peers"): `net::DropPolicy::revert_to_ai` in `libs/net/rollback_session.hpp`. ON,
the host broadcasts `MsgType::Drop` and every peer sets `Player::ai` for that
seat at one agreed tick; OFF, the drop ends the match (`aborted()`). The port's
timeout and handover point are OUR design (the original's are still [NEEDS
BINARY]) — only the *policy the toggle selects* is RE-derived.

### 3.8 Protocol shape — and what the port should reproduce vs replace

**Characterization (as far as RE allows): host-authoritative replication with
desync correction.** Evidence: host clock authority (§3.1) + guest sends carry
*position/tile state* not buttons (§3.2) + active tile-sync *correction* (§3.3) +
net-only extra RNG draws that would break any real lockstep (§3.4) + remote seats
treated as replicated spectators locally (§3.5). This is **closer to
client/server-with-correction than to GGPO-style deterministic rollback**. The
**exact wire format** (does the host ship inputs or full/partial state? are
packets tick-stamped? how are corrections keyed?) is **[NEEDS BINARY]** on the
`sub_40CE27`/`sub_40FE88`/`sub_40FF14`/`sub_40FDE8` payload layouts and the
`funcs_40E9D7` command set. **Guest simulation depth** (full local sim reconciled
to host, vs a thin render+input client fed corrected state) is likewise
unconfirmed — the clock-authority split + tile fixups lean toward the *thin
client* reading.

**Reconciliation with the port (agrees with `multiplayer.md` §3 and ADR-0010).**
The port has a **provably deterministic sim** (`state_hash` + goldens), so it can
do the **clean input-only lockstep the original could not rely on** — and it
already does (`libs/net` `LockstepSession`, ADR-0010). Therefore:

- **REPLACE, don't port, the wire protocol.** The original's replication +
  net-only RNG reroll would *fight* the port's determinism. The port sends only
  packed inputs (`input_codec.hpp`, 6 bits/seat) and uses `state_hash` as a
  **loud** desync check with **no silent correction** (the opposite of §3.3). Keep
  it.
- **DROP the net-only sim deviations.** The port already omits every
  `!sub_40C06A()`/`sub_40C06A()` gate as an always-local build (`multiplayer.md`
  §2.2); with true lockstep it correctly keeps the *local* behaviour uniformly —
  no suppressed powerup-relocation/reveal, no net-only disease reroll, no type-4
  kill skips (there are no "spectator" seats — every peer runs the same
  authoritative sim). This is *more* faithful to the game's *feel* than
  replicating the correction hacks would be.
- **REPRODUCE the shape, for feel/parity:** the **10-slot roster** with **remote
  seats mapped onto the type-4 concept**, the **guest(1)/host(2) roles**, a **real
  lobby** (both role screens, `network-screens.md`), **best-of-N round rotation**
  in net (the port runs one round then exits), **team play in net** (the roster's
  `+84` team byte works in net too — the port forces `team=0`), and **lost-net →
  AI** dropout handling (§3.7). These are gameplay/UX parity items, independent of
  the wire protocol.

---

## 4. Gap analysis — port MVP vs original

Port state read from `run_netplay_match` (`game_app.cpp` 930-1015),
`netplay_connect_screen.cpp`, `libs/net/*`, `main.cpp` netplay args, ADR-0010.

| Feature | Original had it | Port MVP has it | Priority to add |
|---|---|---|---|
| **Human players per match** | up to **10** (10-slot roster, type-4 seats) | **2** (host=seat0, guest=seat1; canonical 2-human `MatchConfig`) | **HIGH — the #1 gap** |
| **Machines / endpoints** | **1 host + 4 guests = 5** (CONFIRMED §1.4: `word_45FFA4[4]`, `word_460130[5]`, the 5 × 1004 B dedupe rings) | 1 host + 1 guest (single UDP peer) | **HIGH** |
| **Multiple local humans per box in net** | **yes** (CONFIRMED §1.4: the guest uploads every local slot with its own kind-40 message) | no (one local arrow-key seat) | MEDIUM |
| **Lobby / waiting-room screen** | **yes, one per role** — `sub_42B0CE` is the host's live client list, `sub_42B47D` the guest's browser + wait loop (`network-screens.md`) | none (a connect modal + text-entry only) | **HIGH** |
| **Roster view in net mode** | the *lobby* lists machines, not players; the **player roster** is the shared `sub_410F81` screen with remote seats as type 4 | no (config built from seed, roster ignored) | **HIGH** |
| **Map + AI chosen in net** | in the **shared** `sub_410F81`/`sub_406DDE` screens, host-only, broadcast as kinds 40/58/43/44 (§2.6) | no (map/AI fixed by the caller) | **HIGH** |
| **Round rotation (best-of-N) in net** | yes (same match loop as local) | no ("ONE match then exit — no round rotation yet") | **HIGH** |
| **Team play in net** | yes (roster `+84` team byte, red/white) | no (`cfg.team[i]=0` forced) | MEDIUM |
| **AI seats mixed with humans in net** | yes (unclaimed active slots → computer) | no (`cfg.ai[i]=false` forced; only 2 humans) | MEDIUM |
| **Lost net player → revert to AI** | yes (Options row 12, `dword_464928`) | no (option round-tripped, unconsumed) | MEDIUM |
| **Node Name (net identity)** | yes — `NODENAME.INI` / a random default from MESSAGES 500..548, edited at Options row 2; **it is the text every lobby row shows** | **done (2026-07-25)** — `assets::load_node_name`/`save_node_name` read/write install-root `nodename.ini`, the MESSAGES 500..548 random default seeds an absent file, Options row 2 edits it through `getstring(290)`'s 30-char prompt, and it is the ADR-0011 lobby's roster display name | done |
| **Protocol options (IPX/TCP/serial/modem)** | ordinals CONFIRMED **0 cancel / 1 IPX / 2 modem / 3 serial / 4 TCP-IP**; only 1..3 reachable | UDP only | LOW (dead transports; keep UDP) |
| **Modem / serial-COM config** | yes (`sub_40798B`, `0x445AC6` driver) | no | SKIP (obsolete hardware) |
| **Seed / config parity** | host authoritative (replicated) | host seed via `SeedHandshake`, canonical cfg both sides | done (port's is cleaner) |
| **Desync handling** | silent correction (tile-sync §3.3) | **loud** `state_hash` mismatch, no correction | done (port's is better) |
| **Chat** | **[NEEDS BINARY]** (`funcs_40E9D7` set uncatalogued) | no | LOW |
| **Pause / mid-match join** | **[NEEDS BINARY]** | no | LOW |
| **Rollback (feel)** | no (used correction instead) | `RollbackSession` exists in `libs/net` but **NOT wired into the game path** (only `LockstepSession` is) | MEDIUM (wire it in after N-player) |

### 4.1 What the port most needs to add (roadmap highlights)

1. **N-player seats (2 → up to 10).** Generalize `run_netplay_match`'s
   hard-coded `local_seats`/`kAllSeats`/2-human `MatchConfig` to an arbitrary
   seat set. `LockstepSession` is *already* N-seat-capable (it takes `local_seats`
   / `all_seats` bitmasks and merges per-seat frames; `input_codec` packs any seat
   subset) — the block is purely in the game-layer 2-seat assumptions and the
   transport being a single peer.
2. **The two real lobby screens** (`network-screens.md` is the 1:1 spec): a host
   pane listing connected machines by **node name** with a settle-gated START,
   and a guest pane browsing announced games (name / client count / id) with
   join + a "waiting for the server to start" spinner. Highest-value UX gap and
   the natural home for Node Name.
3. **Route the net flow through the SHARED setup screens** (§2.6) instead of
   inventing net-only ones: after the lobby, both roles enter the normal roster
   screen (remote seats as type 4, host-only edits, guest buzzed) and then the
   level/rounds screen. That is where **map selection and AI slots** belong —
   and it deletes, rather than adds, port-specific UI.
4. **Round rotation + team + AI seats in net** — drop the `run_netplay_match`
   forcing of one round / `team=0` / `ai=false`; reuse the local `MatchRunner`
   best-of-N loop.
5. **Lost-net → AI** dropout handling (Options row 12 finally gets a consumer):
   on a peer timeout, flip its seat to the AISystem and continue deterministically.
6. **Multi-peer transport** (the current `UdpTransport` is one-peer): a host that
   fans input out to **up to 4** guests and collects their input streams — the
   cap is now CONFIRMED (§1.4), so size for 5 machines.

---

## 5. Biggest [NEEDS BINARY] unknowns (ranked)

**Items 1, 2, 4 and 6 of the original list were RESOLVED on 2026-07-25** by the
net-screens pass (`docs/re/network-screens.md`): the machine cap is **5**
(§1.4), the roles are **1 = guest / 2 = host** with row 1 `sub_42B0CE` = host
(§2), the lobby flow is fully mapped (§2.5-2.6), and the `netprotocol` ordinals
are pinned (§2.4). What remains:

1. **`funcs_40E9D7` in-match command set.** The 51 registered kinds are now
   enumerated (`sub_40E474`) and the **lobby/setup** ones are decoded
   (`network-screens.md` §3: 0/1/3/14/18/32/39/40/43/44/49/58); the **in-match**
   kinds (4-13, 15-17, 33-38, 42, 45-48, 50-61) are still uncatalogued — that
   is where chat / pause / mid-match join would live if they exist.
2. **Wire packet formats** of `sub_40CE27`/`sub_40FF14`/`sub_40FDE8` (fields,
   tick-stamping, how corrections are keyed) and **guest simulation depth**
   (full reconciled sim vs thin corrected client).
3. **Lost-net → AI** drop-detection + handover site (near `sub_40E765` / the
   `0x430C00` queue's per-connection timeout).
4. **The 5th machine's refusal path** — what `sub_40D175` does when both of its
   `for j in 0..3` scans fail (silent drop, or a reply the guest surfaces?).

*Historical-context only (do not port): the exact 1997 protocol. The port's clean
lockstep on `tick()` + `state_hash` (ADR-0010) is the correct replacement; this
audit exists to copy the game's multiplayer **shape and scope**, not its netcode.*
