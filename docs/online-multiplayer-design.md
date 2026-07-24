# Online multiplayer — protocol design sketch

**Forward design, NOT reverse engineering.** Everything here is a proposal for
Open Bomberman's own online mode (ADR-0011); none of it exists in the 1997
binary. The RE audit of the *original's* dead netcode is the separate
`docs/re/multiplayer.md`. This note fleshes out ADR-0011's protocol: the
signaling messages, the NAT-punch sequence, and the lobby state machine. Wire
field widths and message names are a starting point for implementation, not a
frozen contract.

Grounding: the in-match wire codec, sessions, and transport seam already exist
(`libs/net`, ADR-0010) and are reused unchanged. This note designs only the
**lobby / rendezvous layer above them** and the **minimal server**.

---

## 0. Two planes, two channels

| plane | carries | channel | reliability |
|---|---|---|---|
| **control** | create / join / list / ready / start / roster / candidate exchange | client ↔ **server** | reliable + ordered (WebSocket/TLS recommended; reliable-UDP alt — ADR-0011 Risks) |
| **discovery** | STUN reflexive-address echo | client ↔ **server** (UDP) | best-effort, retried |
| **data** | per-tick `Input` / `InputRange` / `Hash` / `Hello` | peer ↔ peer (or peer ↔ relay ↔ peer) | the existing `libs/net` UDP model (unreliable, redundancy-tolerant) |

The **data plane is untouched** — it is exactly ADR-0010's `Transport` +
`protocol.hpp`. Only the control + discovery planes are new, and they only ever
run *before* a match (and, for a relayed match, forward opaque data-plane bytes
during it). The server never decodes an `Input`/`Hash` frame — to a relay they
are opaque payload.

---

## 1. Control-plane messages (client ↔ server)

Sketched as JSON for readability; the reliable-UDP alternative would TLV-encode
the same fields LE. `build_hash` is the compile-time sim+protocol digest
(ADR-0011); `seat` is a 0-based index into the 10-slot roster.

### 1.1 Create a lobby

    C→S  CreateLobby {
           visibility: "private" | "public",
           name:       "Ege's game",      // shown in the public list
           max_seats:  4,                  // 2..10
           build_hash: 0xA1B2C3D4,
           player:     "Ege"
         }
    S→C  LobbyCreated {
           code:        "K7Q2MP",          // 6 base-32 chars, private share code
           lobby_id:    "…",               // opaque server handle
           host_token:  "…",               // proves "I am the host" for StartMatch
           your_seat:   0
         }

`code` is a 6-char base-32 (Crockford, no I/L/O/U) string — ~1 billion
codes, human-readable, phone/chat-shareable. `public` lobbies also get a code
(joinable directly) AND appear in `ListPublic`.

### 1.2 Join by code

    C→S  JoinByCode {
           code:       "K7Q2MP",
           build_hash: 0xA1B2C3D4,
           player:     "Ada"
         }
    S→C  JoinAccepted {
           lobby_id:  "…",
           your_seat: 1,
           roster:    [ {seat:0, name:"Ege", ready:false, is_host:true},
                        {seat:1, name:"Ada", ready:false, is_host:false} ],
           host_candidates: […]            // if already known, else via CandidatesUpdate
         }
    S→C  JoinRejected { reason: "not_found" | "full" | "build_mismatch" | "in_progress" }

**`build_mismatch` is the loud cross-platform door** (ADR-0011): a client on a
different sim/protocol build is turned away here, before anyone waits, with the
reason surfaced in the UI. The P2P `Hello` re-checks it at tick 0 as defence in
depth.

### 1.3 Browse the public list

    C→S  ListPublic { build_hash: 0xA1B2C3D4 }   // optional filter to compatible lobbies
    S→C  PublicList {
           lobbies: [ {code:"K7Q2MP", name:"Ege's game", players:2, max:4,
                       host_region:"eu", server_rtt_ms:38, build_ok:true}, … ]
         }

`build_ok` lets the browser grey-out incompatible lobbies instead of failing the
join. `server_rtt_ms` is the host's measured RTT to the server — a coarse
"how far away is this host" hint (true P2P RTT is measured during the punch).

### 1.4 Roster / presence updates (server push)

    S→C  RosterUpdate {                     // broadcast on any join/leave/ready change
           roster: [ {seat, name, ready, is_host, rtt_to_host_ms?}, … ]
         }
    C→S  SetReady { ready: true }
    C→S  Heartbeat {}   ⇄   S→C  HeartbeatAck {}   // presence + server-RTT sample

A client that misses K heartbeats is dropped from the lobby (RosterUpdate to the
rest). This is the waiting-room "who's in the lobby" feed (goal 2).

### 1.5 Candidate exchange (rendezvous)

After join, each client gathers its ICE-lite candidate set and posts it; the
server fans each peer's candidates to the others.

    C→S  Candidates {
           lobby_id: "…",
           seat:     1,
           list: [ {kind:"host",      addr:"192.168.1.9:41234"},   // LAN
                   {kind:"reflexive", addr:"81.2.3.4:52001"},      // from STUN echo
                   {kind:"relay",     addr:"relay.example:3478", alloc:"…"} ]  // if pre-allocated
         }
    S→C  PeerCandidates { seat: 0, list: [ … ] }   // one per other seat

`reflexive` comes from §2. `relay` is present only if the client pre-allocated a
relay slot (§4). Candidate *priority* mirrors ICE: host > reflexive > relay.

### 1.6 Start the match (host only)

    C→S  StartMatch { lobby_id, host_token }       // host presses START
    S→C  StartMatch {                              // broadcast to every seat
           seed:        0x5EED1234,                // authoritative match seed
           seat_assign: [0,1,2,3],                 // final seat → player binding
           match_config_digest: 0x…,               // guards MatchConfig parity
           input_delay: 2,                         // SHARED, identical on all peers
           topology:    { hub_seat: 0 },           // star hub for N>2 (self for 2P)
           local_seats_mask: 0b0010                 // per-recipient: the seats you own
         }

The server validates *all seats ready* and *all build_hash equal* before
broadcasting. `seed` + `match_config_digest` + `input_delay` are the parity
payload (ADR-0011 hard rules): every peer seeds an identical
`Simulation(config)`. `max_prediction` is deliberately NOT here — it is a local
per-peer display policy each client sets from its own RTT.

The host can Start while the lobby is below `max_seats`; late joiners after Start
are refused (`in_progress`) in v1 (mid-match join = future).

---

## 2. STUN-style reflexive discovery (client ↔ server, UDP)

The game's own `UdpTransport` socket sends a tiny probe to the server's UDP
port; the server replies with the **source address it observed** — i.e. this
socket's public `ip:port` as seen from outside the NAT. This is exactly
`UdpTransport::poll()`'s existing "learn the peer address off the wire," moved to
a server.

    C→S(udp)  StunProbe  { nonce }
    S→C(udp)  StunReply  { nonce, your_addr: "81.2.3.4:52001" }

Critically the probe MUST leave the **same local socket** the match will use, so
the NAT binding the server observes is the one the peer will punch on. The
reflexive `addr` becomes the `reflexive` candidate in §1.5.

---

## 3. The P2P hole-punch sequence

Two peers A and B, each behind a NAT, after both have each other's candidate
lists (§1.5). Driven by a new `Rendezvous` helper over the same `Transport` the
`RollbackSession` will borrow.

    A knows: B.host, B.reflexive        B knows: A.host, A.reflexive

    t0  A ──punch──▶ B.reflexive        B ──punch──▶ A.reflexive
        (both send a PunchPing to every candidate of the peer, simultaneously;
         the outbound packet opens this NAT's binding so the peer's inbound
         packet is allowed back through)

    t1  first candidate pair that gets a PunchPing THROUGH replies PunchPong;
        the first pair to complete a Ping→Pong→Ping round-trip WINS and becomes
        the chosen path (host pair wins on a LAN; reflexive pair across the net)

    t2  A and B both call transport.set_peer(winning_addr) and hand the SAME
        UdpTransport to SeedHandshake → RollbackSession. Punch bytes double as
        the RTT sample that picks max_prediction.

    ── if NO candidate pair completes within ~3 s → declare punch FAILURE ──▶ §4

`PunchPing/PunchPong` are two more tiny tagged datagrams (a natural extension of
`protocol.hpp`'s `MsgType`; the game already redundantly resends, so lost punch
packets self-heal). Simultaneous sending is what opens both NATs — the same
technique STUN/ICE call "UDP hole punching." It succeeds for full-cone,
restricted-cone, and port-restricted NATs; it fails for **symmetric** NAT
(the port the server saw ≠ the port used toward the peer), which is §4.

For N>2 the punch runs **N−1 times**: each guest punches only to the **hub seat**
(the star, ADR-0011), not to every other guest — linear, not quadratic.

---

## 4. Relay fallback (TURN-like)

When the punch fails (symmetric NAT / CGNAT on one or both ends), the pair
routes through a public-IP forwarder on the server.

    C→S  AllocateRelay { lobby_id, seat }
    S→C  RelayAllocated { relay_addr:"relay.example:3478", alloc_id:"…" }

    In-match data plane, per datagram:
      A ──▶ relay_addr   [alloc_id][opaque libs/net datagram]
      relay looks up alloc_id → forwards the opaque bytes to B's learned addr
      B ──▶ relay_addr   [alloc_id][…]  → forwarded to A

`RelayedTransport` implements the `Transport` interface: `send()` prepends
`alloc_id` and aims at `relay_addr`; `poll()` strips the header. **The
`RollbackSession` above it is byte-for-byte identical to the direct-P2P case** —
relay vs direct is one `Transport` swap decided by the `Rendezvous` outcome. The
relay never inspects the payload (it forwards opaque `libs/net` datagrams), so it
still "never simulates."

Cost (ADR-0011 Risks): relayed matches put *all* per-tick traffic through the
server for the match's whole duration; bandwidth = concurrent relayed matches ×
seats × per-tick frame size. This is the expensive, ops-heavy component and the
reason relay is a later phase.

---

## 5. Lobby state machines

### 5.1 Client

    IDLE
     │  Host Private / Host Public → CreateLobby
     │  Join by Code               → JoinByCode
     │  Browse Public → LIST → pick → JoinByCode
     ▼
    IN_LOBBY (waiting room: live roster, my Ready toggle)
     │  (host only) all ready & press Start → StartMatch
     │  receive StartMatch(seed, seats, …)
     ▼
    RENDEZVOUS  (gather candidates → §2 STUN → §3 punch → §4 relay if needed)
     │  Transport ready → SeedHandshake(seed) → build_hash Hello check
     │  build mismatch / unreachable → error → IN_LOBBY (or IDLE)
     ▼
    IN_MATCH  (RollbackSession on tick(); loud desync → POST_MATCH(error))
     │  match ends (win/draw) or desync/drop
     ▼
    POST_MATCH → results → IN_LOBBY (rematch, roster kept) | IDLE

### 5.2 Server (per lobby)

    OPEN            accepting JoinByCode up to max_seats; broadcasting RosterUpdate;
                    public lobbies appear in ListPublic
      │  host StartMatch (valid host_token, all ready, all build_hash equal)
      ▼
    LOCKED          no new joins; broadcast StartMatch; relay candidate sets;
                    hold relay allocations if requested
      │  all peers report connected  |  timeout
      ▼
    IN_PROGRESS     lobby dormant on the control plane (only relay data-plane, if any,
                    is live); new joins → JoinRejected(in_progress)
      │  a client posts MatchOver / all clients disconnect
      ▼
    OPEN (rematch)  |  EVICTED (empty / all timed out → free the code)

The server holds only this soft state per lobby (`code`, roster, candidates,
`build_hash`, optional relay allocations) — a small in-RAM map, evicted on
timeout. No `State`, no `tick`, no gameplay logic ever lives on the server.

---

## 6. RTT → rollback parameters

- **Server RTT** (Heartbeat/HeartbeatAck) is a coarse lobby hint only
  (`PublicList.server_rtt_ms`, "how far away is this host").
- **P2P RTT** is the real input: the §3 punch Ping/Pong round-trips *are* the
  first RTT samples on the exact path the match uses; the `Rendezvous` keeps
  sampling into the warm-up.
- **Derivation** (tick = 50 ms): one-way ≈ RTT/2.
  - `input_delay` (shared, in StartMatch) — a small constant, 1–2 ticks, chosen
    by the host from the *worst-seat* RTT; it must be identical on all peers, so
    the host decides and ships it.
  - `max_prediction` (local, per peer) — `ceil(one_way_ms / 50) + margin`, clamped
    to roughly 2..8. Each peer sets its own from its own RTT; it is a display
    policy and does not affect determinism (only confirmed ticks are hashed).
  - If a peer's derived window would exceed the cap (very high RTT), that peer
    falls back to `LockstepSession` with `input_delay` = the needed ticks —
    smoother than perpetually mispredicting a huge window.

---

## 8. Host migration (ADR-0011 decision 5, IN v1)

The host owns no `State` — every peer holds the full deterministic sim — so
migration moves only two *roles*: the **signaling anchor** and (for N>2) the
**star input-hub**. There is no state transfer.

### 8.1 Detecting host loss

The host's *seat* times out like any peer (§ drop→AI, ADR-0011 Risks): after a
hard silence window the peer that first notices broadcasts, on the data plane, a
`HostLost { at_tick: T }` to the surviving hub-reachable peers (for 2P, the lone
guest simply schedules it locally). `T` is a near-future tick chosen the same way
the drop→AI handoff picks its tick, so **every** peer acts at the identical tick.

### 8.2 Deterministic re-election

At tick `T`, with no messages exchanged, every surviving peer computes the new
hub as **the lowest surviving seat index**. Survivorship is derived from the same
per-seat liveness the drop→AI handoff already tracks, which is identical on every
peer (it is a function of the shared `State` + the agreed drop schedule). So all
peers elect the same hub with zero coordination — the election is a pure function
of shared state, exactly like an AI input.

### 8.3 Re-anchoring + reconnecting

    new hub ──▶ S  ReanchorLobby { code, host_token', roster_digest }
    S ──▶ new hub  ReanchorAccepted   // same code stays resolvable for browsers
    guests ──▶ (Rendezvous to new hub: reuse existing punched path if the guest
               already had a direct pair to it; else §3 punch / §4 relay afresh)

The new hub proves continuity with the `roster_digest` it already holds (the
server verifies it against the lobby's last known roster). The lobby code is
preserved so a mid-match `ListPublic`/`JoinByCode` still resolves. `host_token'`
is a fresh token minted for the new hub.

The hard sub-case (ADR-0011 Risks): a guest that reached the *old* hub only via
relay must re-punch/re-allocate to the new hub, which can exceed the drop timeout
and surface as a brief "migrating…" overlay. v1 accepts the stall; a later
refinement pre-warms a backup-hub path so migration is seamless.

## 7. What is reused vs new

| reused unchanged (ADR-0010) | new (this design, ADR-0011) |
|---|---|
| `Transport` seam, `UdpTransport`, `LoopbackLink` | `RelayedTransport` (a `Transport` impl) |
| `input_codec`, `protocol` (`Input`/`InputRange`/`Hash`/`Hello`) | `PunchPing`/`PunchPong` + `Stun*` tags (extend `MsgType`) |
| `RollbackSession`, `LockstepSession` | `LobbyClient`, `Rendezvous` (control + punch) |
| `SeedHandshake`, `state_hash` desync | `build_hash` door (server + Hello) |
| the whole `libs/sim` (untouched) | the minimal signaling/relay **server** |

The server is the only wholly-new artifact; everything on the client is a
lobby/rendezvous layer *above* the existing, tested `libs/net`.
