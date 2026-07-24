# Multiplayer / netplay — RE audit + path to online play

RE audit of the original 1997 **Atomic Bomberman** (BM95.EXE, Watcom C,
imagebase `0x400000`) multiplayer / network code, written as groundwork for
possibly making Open Bomberman online-playable. This is a **docs/RE** document
— it changes no code. ADR-0003 deferred netplay but built the sim as
deterministic fixed-timestep lockstep *precisely so this door stays open*; this
file distils what the existing RE reveals about the original's net model, what
the port has today, and what a modern online mode would take.

> **Deep-dive companion:** `docs/re/audit/multiplayer-deep.md` (2026-07-24)
> extends this doc with the four things left thin here — the **max player/machine
> count** (the 10-slot roster + type-4 remote seats, the `0x430C00` 5-slot
> packet-queue clue), the **full network screen map** ("the extra pages":
> `sub_42B0CE`/`sub_42B47D` + the `sub_40798B`/`sub_407F4F` config sub-screens),
> a **deeper mechanics diff**, and a **port-MVP-vs-original gap table**.

Sources: `docs/re/` (`facts.md`, `coverage-audit.md`, `dark-matter.md`,
`frontend-flow.md`, `setup-screens.md`, `campaign.md`, `goldman-roulette.md`,
`enclosure.md`, `audit/*`), the port's own net-aware code (`libs/`, `apps/`),
`docs/adr/0003`, `docs/adr/0007`, and CLAUDE.md. The binary itself and the
`native/` transliteration are **not** in this worktree; every claim that would
need the binary to settle is marked **NEEDS-VERIFY (binary)**.

> **Motivation.** Per the project's own working notes the original BM95.EXE has
> already been made to **run natively on modern Win11** (CFG.INI path fixes + a
> compatibility layer; free-runs at ~184 fps, the measurement behind the port's
> canonical `kSubFrames=9` cadence — ADR-0006/0007). So "is the 1997 game
> playable on a modern PC?" is **yes, single-machine, with fixes**. The thing
> that does *not* survive the jump to modern hardware is its **networking**:
> the original's transport is 1990s LAN/IPX + null-modem/dial (below), all of
> which is effectively dead on today's networks. **LAN/online multiplayer is
> the actual capability gap** a modern port can uniquely close — and the
> deterministic sim (ADR-0003) is the asset that makes closing it tractable.

---

## 1. What the original did

### 1.1 The network-role flag — `dword_460058` / `sub_40C06A` / `sub_40C035`

The whole net model hangs off one global mode word, `dword_460058`, read
through the bare accessor `sub_40C06A` (pseudo.c 11138-11142: `return
dword_460058;`) and written by the setter `sub_40C035` (pseudo.c 11123-11133):

| value | role | meaning |
|---|---|---|
| `0` | **local** | not networked (hotseat / shared-keyboard / all-CPU) |
| `1` | **host** | the authoritative machine |
| `2` | **guest** | a joining client |

Provenance for the tri-state: `facts.md` "Overpowered-powerup relocation"
disassembly note (`0x425184` `call 0x40c06a … -> dword_460058 (network role)`)
and its prose "the local/host/guest network role flag (0 = not networked,
1 = host, 2 = guest — set by `sub_40C035`)". `frontend-flow.md` §"SFX 40"
independently corroborates: "the game-mode global (0 = local, 1/2 = the two
network roles)", and pins mode 2's **only** non-zero writer — `sub_40C839(2)`
at the top of the net-game screen `sub_42B0CE` (i.e. entering JOIN sets guest).
There is **no fourth "demo" role** — attract mode is a *separate* flag
(`dword_464938`, §1.6), a correction `frontend-flow.md` calls out explicitly.

`sub_40C06A()` is the single predicate every net-vs-local branch in the binary
keys on. The port ports the local branch of each and omits the net branch
(§2.2); this file is the catalogue of where those branches are.

### 1.2 Session entry & setup flow — the "NET GAME" menu rows

The 7-row main menu (`sub_42B9CE`, selection `v10` 0..6, `frontend-flow.md`
§"main menu") dispatches two adjacent rows straight into net setup:

| row | handler | address | screen |
|---|---|---|---|
| 1 | `sub_42B0CE` | `0x42B0CE` | **START NET GAME** — setup screen A (host); `sub_42741E(0x410)`, `sub_40C839(2)` at head |
| 2 | `sub_42B47D` | `0x42B47D` | **JOIN NET GAME** — setup screen B (guest sibling, same `sub_42741E(0x410)`) |

(`frontend-flow.md` menu table rows 1-2; `coverage-audit.md` §4 lists both as
"net-game setup screens".) The port's `menu_screen.cpp` comments name them
"START NET GAME" / "JOIN NET GAME" and route both to the `NETWORK.BM` help
overlay because the real screens are netplay (§2.1).

`NETWORK.BM` itself is **not** the transport — it is one of the `.BM` text/help
screens (`CREDITS.BM`, `OPTIONS.BM`, `NETWORK.BM`, `INPUT.BM`, …) all rendered
by the single viewer `sub_41302D` (`frontend-flow.md` §"the `.BM` text-screen
viewer"). It is the *documentation* screen about networking, distinct from the
`sub_42B0CE`/`sub_42B47D` interactive net-setup screens.

Once in a match, remote participants occupy roster slots whose **input-type
byte `+16` == 4** — the "network-remote" / "network-spectator" input kind
(the local player-setup screen `sub_410F81` cycles slot types
OFF→CPU→KBD0→KBD1→JOY… and *skips* type-4 slots: Right/Left do nothing "unless
type==4", `setup-screens.md` key table). Type 4 sits alongside 0=off,
1=computer, 2=keyboard, 3=joystick as the fifth input source, filled by the net
layer rather than by local hardware. The player-setup screen's **Space** key is
"bind detect" **only** in net mode (`sub_40C06A()==1`), inert locally
(`setup-screens.md`).

### 1.3 Transport — Winsock (no DirectPlay) + a modem/serial-COM driver

`coverage-audit.md` §4 ("Netplay boundary") pins the transport, confirmed there
by a `grep -n "socket\|ioctlsocket\|bind(" pseudo.c`:

- **Winsock** (`WSOCK32.dll` import). **No DirectPlay.**
- The socket/IPX/modem transport primitives live in the
  **`sub_43BA06`–`sub_43C668`** cluster (`0x43Bxxx`–`0x43Cxxx`).
- Net-game screens `sub_42B0CE` / `sub_42B47D`; net-mode checks `sub_40C06A`;
  network-protocol calls `sub_40FB44` / `sub_40FB90` / `sub_40CE27` /
  `sub_40FCE6`; network stats dump `sub_40C678` — all catalogued net-only and
  explicitly **not ported**.

`dark-matter.md` §4 adds a second, transport-adjacent region the §4 list had
missed: a **65-function modem/serial-COM driver at `0x445AC6`–`0x4500C0`**
(baud-rate tables, COM-port open/close, a `GetTickCount`-based poll loop) plus
a **5-slot packet-queue helper cluster at `0x430C00`–`0x4331EC`** that calls
directly into the Winsock cluster. Both are transport-layer, same ADR-0003
exclusion.

The **Options** screen exposes the transport knobs (all preserved as
display-only chrome in the port, §2.1): row 14 **Modem: P/I/B/#**
(`modemport`/`modemirq`/`modembaud`/`modemdial`, defaults `2`/`3`/`19200`/
`"555-1212"`) and row 16 **Set Default Network Protocol** (`netprotocol`,
clamped `0..3` — i.e. **four** protocol choices). The classic 1997 quartet for
a Winsock+COM stack of this shape is IPX (Winsock `AF_IPX`), TCP/IP (Winsock),
serial null-modem, and dial-up modem — but the exact `netprotocol` 0..3 →
transport mapping is **NEEDS-VERIFY (binary)** (the docs give only the clamp
range and the "socket/IPX/modem" + modem/COM-cluster split).

### 1.4 The per-frame network loop — where remote input enters

The in-match frame driver `sub_42A191` / round loop `sub_4293E5`
(pseudo.c 29488-29557; `facts.md` "Tick order audit" / `audit/tick_order.md`)
runs, **in order**:

1. `29506` — frame delta `dword_464958` = elapsed ms, clamped to `getvalue(31)`
   (50 ms at the locked 20 Hz rate).
2. **`29516` `sub_40E765` — the network RECEIVE PUMP** (pseudo.c 12922-13008),
   dispatching **remote input** through the `funcs_40E9D7` table. Runs *before*
   the frame stamp and every gameplay pass, so remote commands are folded into
   the very same frame's simulation. "No local-game state" — a no-op at role 0.
3. `29517` `++dword_464994` — the frame stamp everything "once per frame" gates
   on.
4. `29518` `sub_4105D2` — the **match clock**. Its elapsed-seconds subtrahend is
   **`dword_4601B0` when hosting**, or a fresh wall-clock read `sub_43ACF8()`
   (`timeGetTime`) otherwise — i.e. **the host is the clock authority**; guests
   take the clock from the host, not their own wall time (`facts.md`
   "Overpowered-powerup relocation", `sub_4105B0`/`sub_4105D2`).
5. … the bomb/flame/movement/enclosure passes (the deterministic gameplay).

So remote input arrives via a **pump at the top of each frame** (`sub_40E765` →
`funcs_40E9D7`), and the **host owns the master clock**.

### 1.5 The model is host-authoritative replication, NOT pure lockstep

Several findings show the original did **active state replication with desync
correction**, not the clean input-only lockstep the port is built for:

- **Guest→host packet sends** gated on `dword_460058 == 2`: `sub_40CE27`
  (packs `(x,y,type)` into a buffer), and the sibling senders `sub_40FE88` /
  `sub_40FF14` / `sub_40FDE8` "nearby in the same source region" (`facts.md`
  "Per-level tile regeneration" §visual/sound note). These carry *position/tile
  state*, not just button presses.
- **Tile-sync replication**: five call sites of the board-tile writer family
  (pseudo.c 12149, 12189, 12483, 12637, 22406) are "gated on `dword_460058`'s
  netplay flag, **corrects a remote player's tile if it desyncs onto a brick**"
  (`facts.md` "spawn-pocket clear" search). A system that *corrects desync* is
  by definition not trusting determinism to keep peers in step.
- **Net-only extra RNG draws**: the disease roll runs a **200-try
  reroll-away-from-Swap** loop *only* when `sub_40C06A()` is non-zero
  (`diseases.cpp`, `audit/diseases.md`) — a net game draws a *different number*
  of `rand()` values per roll than a local game. Under a genuine deterministic
  lockstep that alone would guarantee desync; that the original does it anyway
  is more evidence its net path re-synced state rather than relying on
  bit-identical simulation.
- `bombs.md` flags "remote-client bomb reconciliation" bookkeeping as net-only,
  distinct from local gameplay.

**Working characterisation (NEEDS-VERIFY (binary) on the exact protocol):** the
host runs the authoritative simulation and clock; guests forward local input
(`sub_40CE27` & siblings) and are periodically **corrected** toward the host's
state (tile-sync, position fixups); the receive pump `sub_40E765` folds the
other side's messages in at the top of each frame. This is **host-authoritative
replication**, closer to a client/server-with-correction shape than to
GGPO-style deterministic rollback lockstep. The port should treat the original's
netcode as *history, not a reference implementation* (§3).

### 1.6 Full catalogue of `sub_40C06A()` / net-flag gates

Every place the RE has found the net-role predicate gating behaviour (the port
takes the local side of each):

| site | gate | effect | cite |
|---|---|---|---|
| Overpowered-powerup relocation (Punch/Grab/SuperDisease hide-relocate in opening 40 s) | `!sub_40C06A()` | local only | `facts.md`, `flames.cpp` |
| Flame hidden→floor powerup reveal | `!sub_40C06A()` | local only | `audit/flames.md`, `flames.cpp` |
| Disease reroll-away-from-Swap (200-try) | `sub_40C06A()` net-only | extra RNG draws in net | `audit/diseases.md`, `diseases.cpp` |
| Goldman roulette wheel (`sub_410F81` head, `sub_4034BC`) | `!sub_40C06A()` | local only | `goldman-roulette.md` |
| Campaign `.cam` picker ('C'×5 on player-setup) | `!sub_40C06A()` | local only | `campaign.md`, `setup-screens.md` |
| Campaign round-continuation | `sub_40C06A()==1` treated parallel to campaign | keep looping under external control | `campaign.md` |
| Player-setup Space "bind detect" | `sub_40C06A()==1` | net only; inert locally | `setup-screens.md` |
| SFX 40 "you can't do that here" buzz (results/draw/setup waits) | `sub_40C06A()==1` | net non-host feedback | `frontend-flow.md` §"SFX 40" |
| Per-kind spawn-count `k==12` zeroing | `sub_40C06A() && k==12` | net-only, N/A locally | `audit/setup.md` |
| Player-kill dispatcher / enclosure finder | `+16 == 4` / `type != 4` | skip network-remote players | `facts.md` (1012/1030), `enclosure.md` (`sub_421D3F`, `sub_41DE63`) |
| Match-clock subtrahend | host vs wall-clock split | host is clock authority | `facts.md` (`sub_4105D2`) |
| Guest state packet sends | `dword_460058 == 2` | guest→host replication | `facts.md` (`sub_40CE27`/`sub_40FE88`/`sub_40FF14`/`sub_40FDE8`) |
| Tile-sync desync correction | `dword_460058` netplay flag | correct remote player tile | `facts.md` (12149/12189/12483/12637/22406) |

**Not a net flag (common confusion):** attract/demo mode is `dword_464938`, a
*separate* idle-timeout flag (`frontend-flow.md` §"Attract mode",
`goldman-roulette.md` gate 1, `setup-screens.md`) — it auto-fills an all-CPU
roster for the boot demo and has nothing to do with `dword_460058`. Campaign is
`dword_46489C`/`dword_4648B0` (`campaign.md`). Neither is networking.

---

## 2. What the port has today

### 2.1 Netplay is stubbed / deferred (ADR-0003), confirmed

ADR-0003 ("Deterministic fixed-timestep simulation; netplay deferred",
Accepted 2026-07-02): *"Netplay is explicitly out of scope for now… without
writing any netcode. … Netcode (lockstep would fit this shape naturally) is a
future ADR."* Confirmed by an exhaustive grep turning up **zero transport /
host / guest code** in `libs/`/`apps/` (`facts.md` "Overpowered-powerup
relocation" §"Which network gate"): *"this port has no netplay concept at
all… currently a local-only, hotseat/shared-keyboard build."*

The front-end keeps the *chrome* of networking, wired to dead ends:

- **Menu START/JOIN NET GAME → help overlay.** `menu_screen.cpp` maps main-menu
  rows 1 and 2 (`AppInput::OpenNetwork`) to `AppState::Network`, which
  `game_app.cpp` renders as `present_bm_screen("NETWORK")` — the `NETWORK.BM`
  text screen, *not* the real `sub_42B0CE`/`sub_42B47D` setup screens
  (`app_flow.hpp`, `menu_screen.cpp` comments: "the real START NET GAME screen
  = netplay, deferred").
- **Options net rows are display-only, round-tripped.** `options_screen.hpp`
  keeps every RE'd Options row including the net ones — row 2 **Node Name** (net
  identity string; empty here, "no net-identity concept"), row 12 **Lost net
  players revert to AI**, row 14 **Modem: P/I/B/#**, row 16 **Set Default
  Network Protocol** (`netprotocol` 0..3). `install.{hpp,cpp}` parse and
  re-serialize `modemport`/`modemirq`/`modembaud`/`modemdial`/`netprotocol`
  typed but **unconsumed** ("Not consumed by this port (no network play);
  round-tripped typed"). Row 12 is the exception and is no longer display-only:
  it now drives `net::DropPolicy::revert_to_ai` (the peer-drop → AI handoff,
  ADR-0011 Risks). The value still lives in `assets::Options`; the netplay
  caller copies it into the session's policy, since `libs/net` depends on
  `bomber::sim` alone and cannot read CFG.INI.
- **Debug info** shows `"Network id: %u"` as **0** (`debug_info_screen.cpp`) —
  "no net id".

### 2.2 Net branches are omitted, not ported as always-true no-ops

Because every match the port runs *is* the original's role-0 "local" case,
`!sub_40C06A()` is permanently true and `sub_40C06A()==1` permanently false, so
the port **drops** each net branch rather than materialising a
permanently-fixed flag field. Representative in-code markers (all say the same
thing — "no netplay, guard omitted"):

- `libs/sim/src/systems/flames.cpp`: *"No network-role gate: the original also
  requires a non-networked game (`!sub_40C06A()`), but this port has no netplay
  concept yet (ADR-0003 defers it)."*
- `libs/game/src/screens/setup_screen.cpp`: *"no netplay (ADR-0003), so that
  guard is always-true and [omitted]."*
- `libs/game/src/screens/match_runner.cpp`: *"(no network gate needed here
  since this port has no network play)."*
- `libs/game/src/screens/results_screens.cpp`: *"&& local game (always true, no
  network play) …"*
- `libs/game/include/bomber/game/game_app.hpp`: *'"not net mode"
  (`sub_40C06A()`); this port has no netplay (ADR-0003 …)'.*
- `libs/sim/src/setup.cpp`: notes the net tile-sync replication call sites as
  *out of scope*.

This is deliberate and clean: there is no dormant net state machine to revive —
adding netplay is *new* code on top of a sim that was *shaped* to accept it, not
the un-stubbing of a half-written one.

### 2.3 The determinism assets a netcode can build on

The determinism contract (CLAUDE.md, ADR-0003) already provides everything an
input-synchronised netcode needs:

- **One RNG stream, ordered draws.** `State::rng` is a xorshift32 advanced only
  through `next_random`/`random_below` (`rng.hpp`); the *order and count* of
  draws per tick is a pinned part of the contract. No `rand()`, no clock, no I/O
  in `libs/sim` (rule 1). Cosmetic randomness (sound picks, death-anim,
  disease-flash) uses **presentation-side** RNGs, never `State::rng` (rule 6) —
  so those can differ per peer without desyncing the sim.
- **A full state digest.** `state_hash()` (`hash.cpp`) is an FNV-1a over *every*
  gameplay field of `State`/`Player`/`Bomb`/brains/rovers/pending-chain;
  `simulation.hpp` documents it verbatim as "for tests and **future netplay**".
  Events are per-tick derived outputs, never hashed (rule 4). This is exactly
  the per-tick checksum a lockstep desync detector exchanges.
- **A pure tick function.** `Simulation::tick(const TickInputs&)` is a pure
  function of `(State, inputs)` — the fixed 20 Hz entry (`simulation.hpp`).
- **Trivially serializable input.** `TickInputs` = `array<PlayerInput,
  kMaxPlayers(10)>`; `PlayerInput` = **6 bools** (`up`/`down`/`left`/`right`/
  `action1`/`action2`, `types.hpp`) → **6 bits/player**, ~1 byte/player, ≤8
  bytes for a whole 10-slot frame.
- **Value-type snapshots.** `State` is a plain copyable aggregate (CLAUDE.md:
  "no virtuals, no heap-owning members beyond `std::vector`"), so
  save/restore for rollback is `State snap = sim.state();` /
  `sim.state() = snap;` — cheap, and re-sim is just replaying `tick()`.
- **Proven lockstep pinning.** `tests/sim/test_golden.cpp` pins the exact
  `state_hash` **and** RNG-stream position across full multi-thousand-tick
  scenarios; the determinism-test pattern (two sims, same seed+inputs → equal
  hash over N ticks — ADR-0003 action item 2) is the single-process proof that
  the cross-machine lockstep invariant already holds.
- **A single deterministic match seed.** `Simulation(const MatchConfig&)` is the
  one entry that builds initial state (arena, spawns, hidden powerups) from a
  seeded RNG — feed both peers the same `MatchConfig` and they start
  byte-identical.

---

## 3. Path to online play

The port is **better placed than the original**: the 1997 game needed active
desync *correction* (§1.5); the port has a *provably* deterministic sim
(`state_hash` + golden). That means clean **input-only lockstep** — which the
original could not fully rely on — is viable. Recommended shape: **deterministic
lockstep over UDP**, with rollback the natural target for feel.

### 3.1 What to add

1. **A transport component (`libs/net`).** New target, dependency-pointing like
   the rest (SDL-free, or SDL3_net). **UDP** with a thin reliability layer for
   input frames (sequence numbers, resend-unacked). Keep it strictly **outside
   `libs/sim`** — determinism rule 1 forbids I/O in the sim. `apps/game` /
   `libs/game` own the socket; the sim only ever sees a fully-assembled
   `TickInputs`.
2. **Input serialization.** Pack `PlayerInput` to 6 bits; a frame is a
   `(tick_index, seat_mask, bits…)` datagram of a handful of bytes. Only the
   local seat(s) go on the wire; remote seats are filled from received frames
   before `tick()`.
3. **A session/lobby.** Reuse the RE'd role concept (host=1/guest=2) and the
   10-slot roster. Host advertises and owns the authoritative `MatchConfig` +
   RNG seed; guests join and seed an *identical* `Simulation(config)`. Remote
   seats map onto the original's **input-type-4** slot concept (§1.2).
4. **Desync detection via `state_hash`.** Exchange `Simulation::hash()` every
   tick (or every N ticks to save bandwidth); a mismatch is an immediate,
   loud desync. The golden infra already guarantees identical
   seed+inputs+build → identical hash, so `state_hash` *is* the on-wire
   integrity check, no new machinery required.
5. **Synchronization strategy** — two options on the same core:
   - **Input-delay lockstep** (simplest, GGPO's baseline). Each peer buffers
     local input `d` ticks ahead and stalls tick `T` until all peers' inputs
     for `T` have arrived. Fixed delay hides RTT; cost is a hard stall on
     packet loss and delay tuned to worst-case latency.
   - **Rollback (GGPO-style, best feel).** Predict absent remote input (repeat
     last), simulate ahead, and on a misprediction **restore the snapshot and
     re-run `tick()`** through the corrected inputs. The port already has the
     two things rollback needs and that are usually the hard part: **cheap
     full-state snapshots** (value-type `State`) and a **fast pure re-sim**
     (`tick()`), plus a checksum (`state_hash`) to validate. This is the
     natural fit for the port's architecture.

### 3.2 Risks and hard rules

- **MUST use `tick()`, NEVER `frame()`.** The F9 native-cadence live path
  (`Simulation::frame(inputs, delta_ms)`, ADR-0007) consumes a **wall-clock
  delta** and is *explicitly non-deterministic* — `simulation.hpp` labels it
  "PORT-ONLY, NON-DETERMINISTIC … NOT for tests/oracle". Two peers on `frame()`
  desync on the first differing `delta_ms`. Netplay lives entirely on the fixed
  20 Hz `tick()` path; the live-feel toggle stays a **single-machine** display
  lever (ADR-0007: "must never be used to derive or validate gameplay").
- **Bit-identical sim builds required.** Any added/removed/reordered RNG draw
  changes `state_hash` (contract rules 2-5). Today that only has to hold across
  a golden file; netplay makes it hold **across machines**. Peers must run the
  same sim build; a version/hash handshake at session start is mandatory.
- **Don't reimplement the original's netcode.** §1.5: the original did
  host-authoritative replication *and* took net-only extra RNG draws (the
  disease reroll) — porting that verbatim would fight the port's determinism,
  not use it. Build fresh lockstep on `tick()` + `state_hash`; treat the
  original protocol as historical context only.
- **Cross-platform integer determinism.** The sim is integer-only (no floats —
  ADR-0003, verified), so the usual float-nondeterminism trap is already
  avoided. Fixed-width integer overflow is well-defined in C++20; remaining risk
  is only the usual "same compiler-independent arithmetic" discipline the
  determinism contract already enforces.
- **Seed/roster parity.** All peers must enter `Simulation(config)` with an
  identical `MatchConfig` — including tunables that touch the RNG or the opening
  ticks (e.g. `input_freeze_ticks`, spawn counts). The host's config is the
  authority; guests adopt it wholesale before seeding.
- **Presentation divergence is fine.** Sound choice, death-anim pick, disease
  flash, and the whole render/interpolation path use non-sim RNG/state and never
  feed back into the sim (rule 6, ADR-0007) — peers may render differently
  without desyncing. Only `State` (and thus `state_hash`) must agree.

### 3.3 Suggested sequencing

1. `libs/net` skeleton: UDP socket + framed, sequence-numbered datagrams; no
   sim coupling.
2. `TickInputs` (de)serialization + a two-process **loopback** lockstep harness
   that runs two `Simulation`s from one `MatchConfig`, exchanges packed inputs,
   and asserts `hash()` equality every tick (the golden determinism test,
   promoted to a socket).
3. Input-delay lockstep for real matches (2-player first), with a
   `state_hash` mismatch surfaced as a visible desync error.
4. Rollback on top, exploiting the value-type snapshot + pure `tick()`.
5. Lobby/session UI: revive the host/guest roles and the 10-slot roster as a
   real (not stubbed) START/JOIN flow; retire the `NETWORK.BM`-only stub.

A future ADR should record the lockstep-over-UDP decision (ADR-0003 already
earmarks "netcode design when netplay becomes in-scope" as the revisit point).

---

## 4. Open questions — NEEDS-VERIFY (binary)

1. **`netprotocol` 0..3 → transport mapping.** Only the 0..3 clamp and the
   "socket/IPX/modem" + modem/COM-cluster split are documented; the exact
   ordinal→(IPX / TCP-IP / serial / modem) mapping is unconfirmed.
2. **Lockstep vs. replication (exact protocol).** §1.5's evidence (host clock
   authority, guest state-packet sends, tile-sync desync correction, net-only
   RNG draws) strongly implies **host-authoritative replication**, not
   deterministic lockstep — but the wire protocol (does the host ship inputs, or
   full/partial state?) is unconfirmed. (Bears only on history; the port should
   do clean lockstep regardless.)
3. **Wire packet formats.** The layout of `sub_40CE27` / `sub_40FE88` /
   `sub_40FF14` / `sub_40FDE8` payloads (fields, whether tick-stamped, how
   corrections are keyed) is not RE'd.
4. **`funcs_40E9D7` remote-command table.** The receive pump `sub_40E765`
   dispatches through it; the set of remote commands (movement/bomb only, or
   more — chat, pause, mid-match join?) is uncatalogued.
5. **Max networked players / machines.** 10 roster slots exist locally and type
   4 marks remote seats; whether all 10 can be remote and across how many hosts
   is unconfirmed.
6. **Guest simulation depth.** Whether guests run a full local sim reconciled to
   the host, or a thin render+input client fed corrected state, is unconfirmed
   (the clock-authority split + tile correction lean toward the latter).

---

*Scope note: this audit reads only `docs/re/` + the port code + the ADRs — the
binary and the `native/` transliteration are outside this worktree. It ports no
mechanic and changes no code (CLAUDE.md RE workflow: an RE writeup, not an
implementation). Netplay itself remains deferred per ADR-0003; this file is the
map for if/when it becomes in-scope.*
