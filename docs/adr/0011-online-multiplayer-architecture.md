# ADR-0011 — Online multiplayer: rollback P2P + a minimal matchmaking service

Status: proposed
Date: 2026-07-24
Follows: ADR-0010 (netplay: deterministic lockstep over UDP), ADR-0003
(deterministic fixed-timestep simulation)

## Context

ADR-0010 shipped the netcode CORE — a new `libs/net` (`bomber::net`) that runs
the deterministic sim in lockstep between peers:

- An abstract `Transport` seam (`send`/`poll`, UDP-shaped: unreliable +
  unordered) with two implementations — the in-memory `LoopbackLink` /
  `LoopbackTransport` (headless tests) and `UdpTransport` (raw winsock / BSD
  sockets, no SDL). `UdpTransport` already **learns the peer's address from the
  first datagram** (`poll()` fills the peer when unset) — the germ of STUN-style
  reflexive discovery.
- A wire codec: `input_codec` packs `PlayerInput` to 6 bits and (de)serializes a
  `(tick_index, seat_mask, packed-seats)` frame **little-endian**; `protocol`
  wraps it in a one-byte `MsgType` tag (`Input` / `InputRange` / `Hash` /
  `Hello`) and adds the `HashFrame` desync digest, the `InputRangeFrame`
  redundancy window (loss tolerance), and the `HelloFrame` seed exchange.
- Two interchangeable session strategies over that seam:
  `LockstepSession` (input-delay; STALLS until every seat's input for a tick has
  arrived) and `RollbackSession` (GGPO-style: predict absent remote input,
  snapshot, re-simulate on misprediction; `max_prediction` caps the speculative
  window). Both detect divergence by exchanging `Simulation::hash()` and
  latching a **loud** desync (`desynced()`/`desync_tick()`) — no silent
  correction, unlike the 1997 host-authoritative model
  (`docs/re/multiplayer.md` §1.5).
- `SeedHandshake` — a pump-based pre-match exchange so a menu host/join needs no
  `--seed` on the command line; it rides the SAME `Transport` the match borrows.

On top sits an **MVP front-end** (`NetplayConnectScreen`, `GameApp::
run_netplay_match`, the `--host/--join` CLI): START NET GAME binds a fixed UDP
port and waits; JOIN NET GAME types a `host:port`; a `SeedHandshake` agrees the
seed; then a 2-player `LockstepSession` runs at **`input_delay=4` (200 ms)**.

That MVP proves the core but is not shippable as "online multiplayer" for three
reasons:

1. **LAN / typed-IP only.** "Type the host's IP:port" works across a LAN or with
   manual port-forwarding, but not across ordinary home routers — there is no
   NAT traversal, no discovery, no lobby.
2. **The wrong netcode for feel.** `input_delay=4` adds a flat 200 ms of input
   lag to *every* match regardless of RTT (`game_app.cpp` even comments it as
   "latency headroom"). Rollback (already written, `RollbackSession`) is the
   answer and is simply not wired into the GUI path yet.
3. **2 players only, no real lobby.** `local_seats` is hard-coded
   `host?0b01:0b10`; there is no waiting room, no way to share a match short of
   dictating an IP, and no path to the original's larger player count.

The sim itself is ready: integer-only + deterministic + cross-platform by
construction (ADR-0003), `Simulation(config)` from a shared seed → byte-identical
start, `state_hash` as the cross-machine integrity check, cheap value-type
`State` snapshots for rollback, and `TickInputs` = `array<PlayerInput,
kMaxPlayers(10)>` already sized for up to ten seats. The gap is entirely in the
**connection / lobby / topology** layer above `libs/net`, plus one small piece of
**server infrastructure** the P2P model cannot avoid.

## Decision

Build internet multiplayer as **rollback-over-UDP, peer-to-peer and fully
deterministic, brokered by one minimal matchmaking/signaling service**. The
server exists ONLY to introduce peers and (when they cannot reach each other) to
relay their packets; **it never simulates** — gameplay stays P2P on the
deterministic `tick()` path, exactly as ADR-0010 built it. Concretely:

1. **In-match netcode = `RollbackSession`, not input-delay lockstep.** Wire the
   already-written `RollbackSession` into `GameApp`'s match path in place of the
   `LockstepSession(delay=4)` MVP. `max_prediction` is derived per match from
   measured RTT (below). Keep a *small* shared `input_delay` (1–2 ticks) even
   under rollback to cut the misprediction rate on good links (standard GGPO).
   Input-delay lockstep is retained as an automatic fallback for very high RTT
   where a large prediction window mispredicts more than it hides.

2. **NAT traversal = STUN + UDP hole-punching, with a TURN-like relay
   fallback.** Peers discover their public (reflexive) address by sending a UDP
   probe to the signaling server, which echoes the source address it saw — the
   same "learn the address off the wire" trick `UdpTransport::poll()` already
   does, promoted to a server. The server hands each peer the others' candidate
   addresses; both sides then send simultaneously to punch matching NAT
   bindings. When punching fails (symmetric NAT / CGNAT), the peers fall back to
   a **relay allocation** on the server: a new `Transport` implementation
   (`RelayedTransport`) that sends to the relay, which forwards to the far peer.
   The session code above it is unchanged — relay vs direct is a `Transport`
   swap.

3. **Lobby = shareable code (private) + a public list.** A minimal
   **matchmaking/signaling service** (below) issues a short human-shareable
   **lobby code** (6 base-32 chars, e.g. `K7Q2MP`) for a private match, and
   keeps a **public list** open matches advertise and anyone can browse + join.
   The **host starts the match when ready** while players keep trickling into the
   **waiting room**; every client sees the live roster (who's in, ready state,
   RTT). Replaces the `NetplayConnectScreen` "type an IP" modal.

4. **N-player topology = a host-relay STAR for input distribution; the sim stays
   full P2P.** For >2 seats, one peer (the lobby host) is the **input hub**:
   guests send their owned seats to the host, the host **aggregates** all seats
   for a tick into one combined frame and fans it back out. This is a *transport*
   star, NOT a simulation authority — every peer still runs the identical
   deterministic sim and confirms a tick only when all seats are known
   (all-peers-must-confirm). The star is chosen over a full mesh because it makes
   NAT punching and packet count **linear** in seats (N−1 host↔guest paths)
   rather than quadratic (N·(N−1)/2 mesh paths, each needing its own punch +
   possible relay). 2 players is the degenerate star (direct P2P, no hub hop).

5. **Cross-platform parity is enforced at the door.** A compile-time
   **`build_hash`** (sim + wire-protocol + tunable-default digest) is checked
   TWICE: the server rejects a join whose `build_hash` differs from the lobby's
   host at lobby time (loud, before anyone waits), and the P2P `Hello` re-checks
   it before tick 0. Mismatched builds are rejected with a clear message, never
   silently allowed to desync. Endianness is already fixed LE on the wire
   (`input_codec`, `protocol`, `encode_hash`); the integer-only sim makes the
   arithmetic identical across x86/ARM and Windows/Linux/macOS.

## Components

Three cooperating pieces; only the middle one is new infrastructure.

- **Client lobby (`bomber::net` + `libs/game` screens).** A `LobbyClient`
  (control-plane: create/join/list/ready/start + candidate exchange over the
  signaling connection) and a `Rendezvous` helper (data-plane: run the punch,
  detect failure, allocate relay) that hands the finished `Transport` to a
  `RollbackSession`. New GUI screens replace `NetplayConnectScreen`: a lobby
  menu (Host Private / Host Public / Join by Code / Browse Public), a **waiting
  room** (live roster + Ready + host's Start), and a public-match browser. The
  seed still comes from `SeedHandshake` (server-assigned at Start); seats map onto
  the RE'd 10-slot roster (remote = input-type-4, `docs/re/multiplayer.md` §1.2).

- **Signaling / matchmaking server (new, minimal, stateless-ish).** A small
  service — a few hundred lines, hostable on one modest box — that does NOT
  simulate and holds only soft in-RAM lobby state (a `code → {roster,
  candidates, build_hash}` map, evicted on disconnect/timeout). Responsibilities:
  (a) STUN-style reflexive-address echo; (b) lobby registry — mint codes, accept
  joins, broadcast roster, list public lobbies; (c) rendezvous — relay each
  peer's candidate list to the others to coordinate the punch; (d) start — pick
  the seed + seat assignment + `input_delay`, validate all-ready + build match,
  broadcast Start. Control-plane messages are must-arrive + ordered
  (create/join/list/ready/start), so the control channel is reliable
  (WebSocket/TLS recommended — firewall-friendly and cross-platform; a reliable-
  UDP alternative is possible but is a maintainer decision, see Risks). STUN
  echo + relay run on a UDP port on the same host.

- **Relay (TURN-like, part of the server / co-located).** A public-IP UDP
  forwarder for peer pairs that cannot hole-punch. Per match it holds a small
  allocation `{allocationId → {seatA_addr, seatB_addr}}` and forwards datagrams
  between them; from the game's side it is just another `Transport`
  (`RelayedTransport`). This is the one component with a real ongoing **bandwidth
  cost** (all relayed game traffic transits it) — see Risks.

The protocol sketches (signaling messages, the punch sequence, the lobby state
machine) live in the companion note `docs/online-multiplayer-design.md` (kept
explicitly as forward design, not RE).

## Hard rules

Carried from ADR-0010 §3.2 and the determinism contract, plus the new
online-specific ones:

- **`tick()` only, never `frame()`.** Netplay lives entirely on the fixed 20 Hz
  deterministic `tick()` path. The F9 native-cadence live path
  (`Simulation::frame(inputs, delta_ms)`, ADR-0007) consumes a wall-clock delta
  and is explicitly non-deterministic — it stays a single-machine display lever
  and MUST NOT be used in a networked match.
- **The server never simulates and never sees `State`.** It brokers addresses
  and (optionally) forwards opaque datagrams. All gameplay authority is the
  deterministic sim running identically on every peer. This keeps the sim
  untouched (no I/O in `libs/sim`, rule 1) and makes the server cheap + stateless.
- **Bit-identical sim builds, enforced twice.** `build_hash` is checked at lobby
  join (server) and again in the P2P `Hello` (pre-tick-0). Any added / removed /
  reordered RNG draw changes `state_hash`, which now must agree across machines;
  a mismatch is rejected loudly, never played through.
- **Wire stays little-endian; only `State`/`state_hash` must agree.** The codec
  is already LE (`input_codec`, `protocol`, `encode_hash` = u64-LE). Presentation
  RNG (sound, death-anim, disease flash, interpolation) may diverge per peer
  without desyncing (contract rule 6) — only `State` (hence `state_hash`) is
  synchronized.
- **Seed / roster / tunable parity.** All peers enter `Simulation(config)` with
  an identical `MatchConfig` — including tunables that touch the RNG or the
  opening ticks. The Start message ships the authoritative config + seed +
  `input_delay`; guests adopt it wholesale. `input_delay` (which tick an input
  lands on) MUST be identical on every peer; `max_prediction` is a purely local
  display policy and MAY differ per peer (only confirmed ticks are hashed).
- **Desync + drop are deterministic events.** A `state_hash` mismatch aborts the
  match loudly (existing `RollbackSession::desynced()`). A peer that times out is
  handled by a host-scheduled control message applied at an agreed tick on EVERY
  peer (below) — never by one peer unilaterally changing `State`.

## Risks and hard parts

- **Symmetric-NAT relay cost is the one unavoidable server expense.** Hole-
  punching fails when a NAT rewrites the source port per-destination (symmetric
  NAT, carrier-grade NAT) — a real, non-trivial slice of home/mobile networks.
  Those matches MUST relay, and relayed traffic is continuous per-tick UDP for
  the whole match — bandwidth scales with concurrent relayed matches × seats.
  This caps how many simultaneous relayed games a given box can carry and is the
  only part that is genuinely "ops, not a static host." Decision to make: is
  relay in v1, or does v1 ship with "symmetric NAT can't connect (try later /
  use LAN)" and add relay in a later phase?
- **N-player rollback fan-out.** Rollback re-simulates the whole sim over the
  mispredicted window; snapshot memory is `sizeof(State)` × window, and re-sim
  cost is `window × tick()`. With up to 10 seats every seat is a prediction
  source, so misprediction rate (and thus re-sim frequency) rises with player
  count, and the star host additionally pays aggregate + fan-out per tick.
  Worse, **all-must-confirm means one late peer stalls everyone** past the
  prediction cap — a single bad connection degrades the whole lobby. Mitigated by
  the drop→AI timeout, but that is a visible cliff, not a smooth degrade.
- **Dropped/late peers.** Within the window: predict (repeat last input). Past
  the window: the session stalls (existing `max_prediction` behaviour). Past a
  hard timeout (≈2–3 s of silence): the host broadcasts a "seat S dropped at
  tick T" control message; **every** peer, at that exact tick, hands seat S to
  the deterministic `AISystem` (which already derives inputs from the shared
  `State`, so all peers compute identical AI inputs and the hash stays equal).
  This revives the RE'd Options **row 12 "Lost net players revert to AI"**
  (`lost_net_revert_ai`, `docs/re/results-and-options.md`) as its first real
  consumer. Host drop is the hard case — v1 ends the match (host-migration =
  re-punch to a new hub at an agreed tick, deferred).
- **Anti-cheat via `state_hash` is tamper-EVIDENT, not tamper-PROOF.** The hash
  exchange catches a client whose sim diverges (modified rules, desync) within
  one Hash round — good enough to detect a broken/hacked build and abort. It does
  NOT stop cheats that keep the sim honest: reading hidden state (hidden powerup
  locations, AI intent), input-timing bots, or two colluding peers. P2P
  determinism has **no referee** to punish anyone. Real anti-cheat needs a
  trusted authority the design deliberately avoids; ranked/public integrity is
  out of scope — flag it, don't pretend the hash solves it.
- **Signaling transport choice (maintainer decision).** WebSocket/TLS control-
  plane (recommended: reliable, ordered, traverses corporate/TCP-only firewalls,
  standard on all three OSes) pulls in a small WS/TLS client dependency, mildly
  at odds with the SDL-free / dependency-light `libs/net` ethos. The alternative
  — reliable-ordered messaging over the existing UDP `Transport` (reuse the
  `InputRange` redundancy pattern) — adds no dependency and keeps one socket
  type, but must reimplement ordered reliability and will not traverse UDP-
  blocking networks. Recommend WS/TLS control + UDP STUN/relay; leave the final
  call to the maintainer.
- **Hosting / ops.** The signaling half is tiny and could even be serverless /
  edge; the relay half needs a persistent public IP + a bandwidth budget. Someone
  has to run and pay for it. A self-host option (point the client at your own
  server URL, LAN needs none at all) keeps the project's no-lock-in ethos.

## Phased implementation plan

Ordered so each phase ships something playable and reuses the prior one. Nothing
below touches `libs/sim` or any golden hash.

- **Phase 0 — Smooth 2P (rollback in the GUI, LAN/typed-IP).** Swap
  `GameApp::run_netplay_match` from `LockstepSession(delay=4)` to
  `RollbackSession` with an RTT-derived `max_prediction` (measured by a short
  Ping/Pong on the game transport during the existing `SeedHandshake` warm-up).
  Keep the current `NetplayConnectScreen` (bind-port / type-IP) as the entry.
  Ships the feel fix immediately using ONLY already-written `libs/net` code — no
  server. (Directly answers goal 4.)
- **Phase 1 — Signaling server + lobby codes (private 2P over the internet).**
  Stand up the minimal signaling service (control-plane + STUN echo). Add
  `LobbyClient` + `Rendezvous` to `bomber::net`, a waiting-room screen, and the
  hole-punch. "Share a 6-char code" replaces "type an IP"; the server assigns the
  seed at Start (still delivered via `SeedHandshake`). `build_hash` handshake
  lands here. (Goals 1-partial, 2-partial, 3.)
- **Phase 2 — Relay fallback (reach everyone).** Add `RelayedTransport` + the
  server relay allocation; detect punch failure (no P2P datagram within N s) and
  transparently fall back to relay. Symmetric-NAT / CGNAT players can now play.
  (Completes goal 1.)
- **Phase 3 — Public match list.** `ListPublic` + a browser screen; public
  lobbies anyone can see and join; host starts while players trickle in. (Completes
  goal 2.)
- **Phase 4 — N>2 up to the original's count (~10, pending the native audit).**
  Host-relay star input distribution with per-tick aggregation; the deterministic
  peer-drop → AI handoff (Options row 12) scheduled by the host; N-peer punch
  (each guest ↔ host) with per-link relay fallback; seat assignment extended to
  the full 10-slot roster. (Goal 5.)

Cross-cutting and present from Phase 1 on: the `build_hash` door check, the loud
desync abort (already in `RollbackSession`), and the LE-wire reaffirmation.

## Consequences

- **The seam holds.** Rollback vs lockstep is already a `Session` choice; direct
  vs relay is already a `Transport` choice; STUN reflexive discovery generalizes
  `UdpTransport`'s existing peer-learning. The new code is a *lobby/rendezvous
  layer above* `libs/net` plus a small server — the codec, the sessions, and the
  sim are untouched (ADR-0010's constraint honoured).
- **The determinism stakes rise from "across a golden file" to "across arbitrary
  internet peers."** The `build_hash` door + the loud `state_hash` desync are the
  guard; the integer-only sim + LE wire make cross-OS/arch parity structural, not
  hoped-for.
- **One new operational dependency.** The project gains a server it must host
  (signaling cheap, relay not free) — the first non-static-hostable piece. A
  self-host / LAN-only path preserves the no-lock-in ethos.
- **Decisions deferred to the maintainer:** (1) signaling transport (WS/TLS vs
  reliable-UDP); (2) confirm the host-relay star over a full mesh; (3) relay in
  v1 or later + its bandwidth budget / who hosts; (4) pin the exact max seat count
  once the native audit lands; (5) accept "host drop ends the match" for v1
  (host-migration deferred).
