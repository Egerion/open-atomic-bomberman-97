# Multiplayer deep-dive audit — original netplay end-to-end (2026-07-24)

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

**Bottom line for the roadmap:** treat **"up to 10 players"** as the target and
**machine topology (1 host + up to ~4-9 guests, 1+ humans each)** as the open
variable to pin from the packet-queue cluster and the `sub_40CE27`/`funcs_40E9D7`
seat-indexing. The port's current **hard 2** (host=seat 0, guest=seat 1;
`run_netplay_match` builds a canonical 2-human config) is the #1 gap.

---

## 2. The network menu / screens in full — "the extra pages"

The net path is reached from **two adjacent main-menu rows** (`sub_42B9CE`, the
7-row menu, `frontend-flow.md` menu table):

| menu row | handler | VALUELST legend | mode-set at head | music | backdrop |
|---|---|---|---|---|---|
| 1 **START NET GAME** | `sub_42B0CE` @0x42B0CE | 765-778 | `sub_40C839(2)` | 1040 (`sub_42741E(0x410)`) | random `GLUE<n>` (`sub_4148E5`) |
| 2 **JOIN NET GAME** | `sub_42B47D` @0x42B47D | 750-763 | `sub_40C839(1)` | 1040 | random `GLUE<n>` |

> **[NEEDS BINARY] — a real internal inconsistency in the existing docs.** The
> naming says row 1 = **START** (host) and row 2 = **JOIN** (guest), but the
> recorded `sub_40C839` argument at each screen's head is the **opposite** of
> that reading: `setup-screens.md` records `sub_42B0CE` (row 1, "START") entering
> **mode 2** and `sub_42B47D` (row 2, "JOIN") entering **mode 1**, and
> `multiplayer.md`/`frontend-flow.md` both note `sub_40C839(2)` (=guest) at the
> top of `sub_42B0CE`. Either the START/JOIN labels or the mode-set is
> mis-attributed. **The decisive read is the `sub_40C839(1)` vs `(2)` literal at
> the head of `sub_42B0CE` and `sub_42B47D`** — that byte alone says which screen
> is host and which is guest. Flagging because the port's `present_net_host`
> (role 1) / `present_net_join` (role 2) mapping should follow whichever the
> binary confirms.

### 2.1 Screen: START NET GAME — `sub_42B0CE` (the game-options pane)

A **4-item options list** (`for i in 0..3`, `sub_40F1E9(i)` active? →
`sub_40F217(i)` value; string 71 active / 72 off) at getvalue `775 (x) /
776+777*i (y) / 778 (colour)`, plus two headers at 765/766/768 and 770/771/773
(strings 73 and 70). Same getkey model as the local screens (SFX 20 on any key,
Enter proceeds). This is the **game-type/options** pane for a net match (team
play etc.) — the network equivalent of the local PLAYER-INPUT screen's siblings.
Fields/defaults of the 4 items are **[NEEDS BINARY]** (`sub_40F1E9`/`sub_40F217`
enumerate them). (`setup-screens.md` "Screen A".)

### 2.2 Screen: JOIN NET GAME — `sub_42B47D` (the controller/roster assignment)

The **10-slot roster** screen for the net game: `for i in 0..9`, `sub_40F163(i)`
(slot active?) → if active, controller type/index via `sub_40F191(i)`/
`sub_40F1BD(i)`, line = string 62 formatted with the controller index; else
string 63 (off). Title string 66; header string 60. Row at getvalue `760 (x) /
761+762*i (y) / 763 (colour)`; selection cursor via `sub_413BD6(760-20,…)`.
Player-config store `unk_4632CC`, **404 B/player × 10** (`setup-screens.md`
"Screen B"). Keys:

- **Up 328 / Down 336** — move the selection (wrap).
- **Space 0x20** — **BIND DETECT** (the net-only control): SFX 10, then a ~3000 ms
  detect loop (`sub_40EC6F(sel)` polls controllers; `sub_40F386()` = a control was
  pressed) showing the **animated "press a control now" prompt** (string 80,
  cycling `dword_45BFB4[c&3]`) at (150,400); on detect, bind that controller to
  the slot; on timeout, cancel (overlays strings 100/110 + 95). This is how a
  machine **claims its local seat(s)** into the shared roster — the mechanism that
  makes a slot a *local* human on this box (the others show as remote/off).
- **Any key < 0x20 (Enter 13 / Esc 27)** — leave the screen (proceed / back).

Message ids: 60/62/63/66/80/95/100/110 (`setup-screens.md` id table). This is the
closest thing the original has to a **lobby/roster view** — a live list of the 10
slots, each OFF / a local controller / (implicitly) a remote seat.

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

The **`netprotocol` 0..3 → transport** mapping is **[NEEDS BINARY]**: only the
clamp (0..3, so four options) and the "Winsock socket/IPX + modem/serial-COM"
split are documented (`multiplayer.md` §1.3, `dark-matter.md` §4). The classic
1997 quartet for a `WSOCK32`+COM stack of this shape is **IPX / TCP-IP / serial
null-modem / dial-up modem**, but the exact ordinal order is unconfirmed
(`sub_407F4F`'s option table + how `netprotocol` selects a transport in the
`sub_43BA06`–`sub_43C668` cluster is the read).

### 2.5 The "waiting for players / lobby" screen and host-start / join-while-waiting

**[NEEDS BINARY] — the single biggest screen-flow unknown.** The RE has **not**
located a distinct "waiting for players / lobby" screen in the original. The
plausible model, given the pieces above, is that **`sub_42B47D` (the 10-slot
roster) IS the waiting room**: the host sits on it while guests join, each join
lighting up a slot as a remote seat (arriving via the receive pump `sub_40E765` →
`funcs_40E9D7`), and the host presses Enter to start once the roster is set. But
whether there is a separate waiting state, how a mid-wait join is announced/seated,
and how host-start is broadcast to guests are all unconfirmed. To settle it, read:

- `sub_42B0CE` / `sub_42B47D` frame loops — is there a "wait for host-start"
  branch on the guest, and a "wait for all joins" branch on the host?
- `funcs_40E9D7` (the `sub_40E765` dispatch table) — the message kinds; a *join*
  and a *start* command would show up here.
- the SFX-40 gate (`sub_40C06A()==1`, `frontend-flow.md` §SFX 40): the net
  non-host "you can't do that here" buzz fires on the setup/results wait loops,
  which is *evidence a guest sits in a wait state* the host controls — worth
  reading `sub_42B0CE`/`sub_42B47D` lines ~6102/8230/15390 (the SFX-40 sites) to
  see exactly what the guest is blocked from doing while waiting.

**Port note.** The port has **no lobby at all**: `present_net_host` shows a
"WAITING FOR A PLAYER..." acknowledge modal (port-invented, `netplay_connect_
screen.cpp`) and `present_net_join` a "host:port" text-entry, then a **seed
handshake** (`SeedHandshake`), then straight into a 2-seat match. There is no
roster view, no slot list, no join-while-waiting for a 3rd+ peer.

---

## 3. Netplay MECHANICS — everything the original does differently under `dword_460058 != 0`

This deepens `multiplayer.md` §1.5/§1.6. All gates key on `sub_40C06A()` (=
`return dword_460058`) or the raw `dword_460058 == 2` (guest) comparison.

### 3.1 Host is the clock authority (`sub_4105D2` / `sub_4105B0`)

The match-clock update `sub_4105D2` (pseudo.c 14456-14552) computes elapsed time
against a subtrahend that is **`dword_4601B0` when HOSTING**, or a fresh
wall-clock read `sub_43ACF8()` (`timeGetTime`) otherwise (`facts.md` 2262-2274).
Guests therefore **take the match clock from the host, not their own wall time** —
the defining property of a host-authoritative session. The receive pump runs at
frame top (step 2, 29516) *before* the clock update (step 4, 29518), so a guest's
frame is stamped with host-derived timing folded in first.

### 3.2 Guest → host state-packet sends (`sub_40CE27` & siblings), gated `dword_460058 == 2`

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
  seats mapped onto the type-4 concept**, the **host(1)/guest(2) roles**, a **real
  lobby/roster screen** (revive `sub_42B47D`'s list), **best-of-N round rotation**
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
| **Human players per match** | up to **10** (10-slot roster, type-4 seats; RE-consistent, cap **[NEEDS BINARY]**) | **2** (host=seat0, guest=seat1; canonical 2-human `MatchConfig`) | **HIGH — the #1 gap** |
| **Machines / endpoints** | 1 host + N guests (N **[NEEDS BINARY]**, ~5 per the `0x430C00` queue) | 1 host + 1 guest (single UDP peer) | **HIGH** |
| **Multiple local humans per box in net** | plausible (per-machine bind-detect claims local controllers) — **[NEEDS BINARY]** | no (one local arrow-key seat) | MEDIUM |
| **Lobby / waiting-room screen** | `sub_42B47D` 10-slot roster as the wait room (distinct lobby **[NEEDS BINARY]**) | none (a connect modal + text-entry only) | **HIGH** |
| **Roster view in net mode** | yes — `sub_42B47D` live 10-slot list, per-slot bind | no (config built from seed, roster ignored) | **HIGH** |
| **Round rotation (best-of-N) in net** | yes (same match loop as local) | no ("ONE match then exit — no round rotation yet") | **HIGH** |
| **Team play in net** | yes (roster `+84` team byte, red/white) | no (`cfg.team[i]=0` forced) | MEDIUM |
| **AI seats mixed with humans in net** | yes (unclaimed active slots → computer) | no (`cfg.ai[i]=false` forced; only 2 humans) | MEDIUM |
| **Lost net player → revert to AI** | yes (Options row 12, `dword_464928`) | no (option round-tripped, unconsumed) | MEDIUM |
| **Node Name (net identity)** | yes (Options row 2, `sub_4074DC`) | field parsed, unused; no identity in lobby | LOW |
| **Protocol options (IPX/TCP/serial/modem)** | **4** (`netprotocol` 0..3, mapping **[NEEDS BINARY]**) | UDP only | LOW (dead transports; keep UDP) |
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
2. **A real net lobby/roster screen** reviving `sub_42B47D`'s 10-slot list, with
   **remote seats shown as the type-4 concept**, per-machine seat claim (bind), and
   host-start. This is the highest-value UX gap and the natural home for Node Name.
3. **Round rotation + team + AI seats in net** — drop the `run_netplay_match`
   forcing of one round / `team=0` / `ai=false`; reuse the local `MatchRunner`
   best-of-N loop.
4. **Lost-net → AI** dropout handling (Options row 12 finally gets a consumer):
   on a peer timeout, flip its seat to the AISystem and continue deterministically.
5. **Multi-peer transport** (the current `UdpTransport` is one-peer): a host that
   fans input out to K guests and collects K input streams — informed by the
   machine-cap read (§1.3).

---

## 5. Biggest [NEEDS BINARY] unknowns (ranked)

1. **The player/machine CAP.** Read the roster loop bound in `sub_42B0CE`/
   `sub_42B47D`, the **5-slot packet-queue cluster `0x430C00`–`0x4331EC`** (is it
   a 5-peer connection table?), and whether `sub_40CE27`/`funcs_40E9D7` carry a
   **seat index**. Settles "up to 10 players across how many machines".
2. **host vs guest labeling of `sub_42B0CE`/`sub_42B47D`.** The `sub_40C839(1)`
   vs `(2)` literal at each screen's head — the existing docs are internally
   inconsistent (START labeled row 1 but recorded entering mode 2 = guest).
3. **`funcs_40E9D7` remote-command set.** The `sub_40E765` dispatch table — is it
   movement/bomb only, or does it include join, host-start, chat, pause,
   mid-match join? Bears on lobby flow *and* the chat/pause/mid-join gaps.
4. **Waiting-room / lobby flow.** Whether a distinct wait screen exists and how
   host-start + join-while-waiting are sequenced (read the `sub_42B0CE`/
   `sub_42B47D` frame loops + the SFX-40 guest-block sites ~6102/8230/15390).
5. **Wire packet formats** of `sub_40CE27`/`sub_40FE88`/`sub_40FF14`/`sub_40FDE8`
   (fields, tick-stamping, how corrections are keyed) and **guest simulation
   depth** (full reconciled sim vs thin corrected client).
6. **`netprotocol` 0..3 → transport mapping** (`sub_407F4F` option table).
7. **Lost-net → AI** drop-detection + handover site (near `sub_40E765` / the
   `0x430C00` queue's per-connection timeout).

*Historical-context only (do not port): the exact 1997 protocol. The port's clean
lockstep on `tick()` + `state_hash` (ADR-0010) is the correct replacement; this
audit exists to copy the game's multiplayer **shape and scope**, not its netcode.*
