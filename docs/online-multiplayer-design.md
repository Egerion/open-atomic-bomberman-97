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
relay vs direct is one `Transport` swap. The relay never inspects the payload (it
forwards opaque `libs/net` datagrams), so it still "never simulates."

Cost (ADR-0011 Risks): relayed matches put *all* per-tick traffic through the
server for the match's whole duration; bandwidth = concurrent relayed matches ×
seats × per-tick frame size. This is the expensive, ops-heavy component and the
reason relay is a later phase.

### 4.1 The swap is NOT decided by the `Rendezvous` outcome

It used to be, and that was a bug — a live one, match `9V9BHE`, Turkey ↔
Lithuania. The punch's verdict is **per-peer and unsynchronised**: the 2-peer
`Rendezvous` declares `Connected` on receiving the PONG for its own PING, which
is a genuine round-trip proof *for the peer that received it* and says nothing
about whether the other peer completed its own round before `kPunchTimeoutMs`.
Worse, the winner then went **silent** — `LobbyFlow` stops pumping the punch once
it is connected, and `Rendezvous::step()` returns early when it is no longer
`Punching` — so the peer's still-in-flight PINGs stopped being echoed and its
failure became self-fulfilling. One peer played direct; the other asked for a
relay allocation the first never made; every datagram it pushed into the relay
was discarded for want of a destination (`drop_unknown_dst`). The match connected
and delivered 147 bytes.

`AllocateRelay` is answered **only to the sender** (PROTOCOL.md §6 is frozen and
carries no broadcast that could tell the other seat), so the peers have to
converge by themselves. They do it with a **mutual path verification** between
the punch and the match (`libs/net/link_probe.hpp`, `Phase::Verifying`):

    MsgType::Probe { nonce, seen_peer }, over the CHOSEN Transport

    receiving a probe            ⇒ peer→me carries
    receiving one with seen_peer ⇒ me→peer carries too

Both together are `verified()`, and only a verified path is handed to the match
layer. Three properties make it converge rather than merely usually work:

1. **The probe answers hole-punch PINGs.** Whoever is verifying is still on the
   air for a peer that is still punching, so the winner's silence can no longer
   starve it. Most of the production failure never happens now.
2. **A direct path that does not verify escalates to the relay**, on a budget
   measured from the *punch's* start so it outlasts the peer's own punch window.
3. **The relay is absorbing.** Nobody ever leaves it, so a peer that goes there
   makes the other's direct verification fail, which sends it to the relay too.
   The relay's own verification just waits for the other seat to allocate.

**Every topology verifies, including the star** (corrected 2026-07-31). It
shipped gated on `can_relay()` — "only a match that can escalate is worth
verifying" — on the reasoning that a star's punch already required both halves
per guest. That is true of the **hub**, which runs the multi-peer `Rendezvous`,
and false of a **guest**, which punches the hub with the 2-peer form: exactly the
single-sided latch above, with the probe skipped. The failure it allowed: the
hub's punch needs *every* guest confirmed, so one unreachable guest fails it and
a star cannot escalate — while a reachable guest had already latched Ready and
started a match its hub had abandoned. For a star guest the probe carries the one
fact its own round trip cannot: a `Probe` can only arrive once the hub is pumping
over its `StarHubTransport`, which it builds only after confirming every guest.
When it expires with no relay to escalate to, the flow fails with `A PLAYER COULD
NOT BE REACHED`. Pinned in `tests/net/test_lobby_flow.cpp`.

`Transport` is the seam that makes this invisible: which one delivers changes,
the session above never sees it (determinism rule 1).

**Residual, honestly stated.** The peer that completes the exchange last cannot
know its final probe arrived — the two-generals shape, which TCP answers with
TIME_WAIT and this answers with `kProbeLingerMs` of continued probing after
mutual proof. Losing *every* probe in that window still splits the pair, and past
`Phase::Ready` there is no detector: `LobbyFlow` is pumped through the setup
screens but not during the match, and setup traffic is host→guest only, so
"silence" is not a valid signal there. Closing that fully needs a guest-side
keepalive in `SetupSession` plus a late transport swap behind an indirection.

### 4.2 A path that dies MID-MATCH (open; analysis only)

Observed live, deep into a healthy session (round 53, ~1124 ticks in): `RX 0/s`,
`~100% LOSS`, `BAD 0` — not malformed datagrams, *none at all* — with `PRED 8/8`
and `STALL 10/s` as consequences of nothing arriving. Consistent with a NAT
binding expiring or a transient network event. §4.1's verification runs before
tick 0 and nothing re-verifies afterwards, so the match freezes rather than
failing over or ending cleanly.

Host migration supplies part of the answer and is **not** the whole of it:

- If the vanished peer was the **hub of a star (>2 seats)**, §8 covers it:
  survivors elect a new one and play on with the lost seat on AI, instead of
  stalling forever.
- **For the 2-seat match this capture came from, §8 deliberately does nothing.**
  Migration is gated to a star precisely because the failure above is
  indistinguishable from a dead peer, and acting on the guess would give each
  side a private divergent game rather than a visible stall. The observed
  configuration therefore behaves exactly as it did before host migration
  existed. This section stays open.

**What the frozen protocol already allows, verified against the Go source** — so
the remaining work needs no server change:

- **Mid-match relay allocation is ungated.** `handleAllocateRelay` checks
  membership and seat only; it never reads lobby state. Allocation is idempotent
  per (lobby, seat), and PROTOCOL.md §6.1 explicitly blesses the mid-match retry.
  The relay learns each seat's address from its first datagram, so no candidate
  exchange is needed — which matters, because §8.3 notes candidates are *not*
  relayed mid-match.
- **The control plane is still there**, provided somebody keeps pumping
  `LobbyFlow` during the match (§8.3). Nothing does today; the same driver host
  migration needs would supply it.
- **`RosterUpdate` is the peer-liveness oracle**, broadcast unconditionally
  mid-match from both the disconnect path and the reaper. A peer that QUIT loses
  its seat immediately; a peer whose UDP path died keeps heart-beating and never
  produces one. A member that loses its socket **cannot rejoin** (joins are
  refused once IN_PROGRESS), so the signal is one-way and needs no false-positive
  handling.
  **But the timing asymmetry is the catch:** a clean quit shows up instantly,
  while a hard crash or a machine going dark takes the 30 s deadline plus up to
  10 s of reaper granularity. So "no roster change yet" is *inconclusive*, not
  proof the peer is alive, and a failover that fires in ~2 s must not read it as
  such. A correct design either waits out that window before deciding, or acts
  only on the fast arm (an explicit disconnect) and treats everything else as
  undetermined.

Deliberately NOT used: `MatchOver` would flip the lobby back to OPEN and
re-enable candidate relay, which looks like a free renegotiation and is a trap —
it clears every ready flag, broadcasts a roster update every client's UI reads as
"match over", re-opens the lobby to joins, and a second `StartMatch` would mint a
new seed and desync the running match.

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
    RENDEZVOUS  (gather candidates → §2 STUN → §3 punch)
     │
     ▼
    VERIFYING   (§4.1 mutual probe over the chosen path; not carrying → §4 relay,
     │           which is absorbing: the relay verifies but never escalates)
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
    the host decides and ships it. **This applies to `LockstepSession` only.**
    Under `RollbackSession` input delay is *local* and needs no agreement at all
    (ADR-0011's 2026-07-30 amendment): there is no schedule mapping a sample to a
    tick, so a peer may lead its own input and change that lead mid-match without
    a wire message. It does so ADAPTIVELY, off measured arrival variance rather
    than off RTT — see `rollback_session.hpp`'s "arrival variance" note. Nothing
    keys the rollback path to the `input_delay` the lobby ships.
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
hard silence window a survivor broadcasts, on the data plane, a
`HostLost { seat, at_tick: T }` to the surviving hub-reachable peers (for 2P, the
lone guest simply schedules it locally and elects itself — it is not gated on
having anyone to tell).

`MsgType::HostLost` (opcode 11, wire v9) is a **separate tag from `Drop` despite
an identical payload**, and the difference is authority rather than bytes: a
`Drop` is the hub's decree and only the hub may send one, while a `HostLost` is
an observation about the hub, which by definition cannot come from it. Keeping
them apart is what lets a peer accept "the hub is gone" from a non-hub without
also accepting an ordinary seat-drop decree from one. A `HostLost` naming any
seat other than the one we currently believe is the hub is refused.

Three corrections this section needed once it met the code:

- **`T` is RETROACTIVE, not "near-future".** It is the announcer's first tick
  with no input from the hub, exactly as `DropFrame`'s is. A future `T` cannot
  work for the same reason it cannot work there: the ticks between the hub's last
  input and `T` could never be *confirmed*, so the session would speculate past
  the cap and never unstall — the hang the message exists to cure.
- **Not "the peer that first notices" — every survivor announces, and they
  disagree.** `T` is a *local* quantity: peers hold different amounts of a dying
  hub's output, so their proposals differ by however many of its last datagrams
  each happened to lose. Everyone adopts the **lowest** `T` seen. Monotone, so no
  agreement protocol is needed. Because the hub cannot re-send its own death
  notice, *every* holder re-sends it each pump, not just one designated peer.
- **"Lowest wins" alone is not sufficient.** A peer that received MORE of the
  hub's dying output can already have **confirmed** past the tick it must adopt.
  Adopting therefore has to *un-confirm* back to `T` — the one place the session
  moves its frontier backwards — which needs snapshots retained below
  `confirmed_`. It must also discard every hash at or above `T`, on **both**
  sides: the lagging peer never rolls back at all, so without the purge it
  compares its correct post-handoff hash against the pre-handoff one the other
  peer already broadcast, and reports a divergence that never happened. If `T` is
  older than the retained window the session says so loudly rather than guessing.

**Migration arms for a STAR ONLY — matches of more than two seats.** This is a
safety gate, not an unfinished edge case, and it was added after the two-seat
behaviour was measured rather than reasoned about.

With two seats the data plane cannot distinguish a **dead peer** from a **dead
path**: the peer that stopped arriving may be gone, or may be alive, still
playing, and merely unreachable (§4.2 — observed live as `RX 0/s`, `~100% LOSS`,
`BAD 0` on a direct match deep into a session). With nobody else at the table
there is no third party whose view could settle it. Electing on that guess makes
*each* side hand the *other's* seat to the AI and play on, inside two private
divergent games that neither player can tell from a real one — strictly worse
than the freeze it would replace, because a freeze is at least visible.

Measured before the gate existed, on a severed 2-seat direct path with **both
peers alive and pumping**: the guest set `host_lost_seats() == 1`, promoted
itself (`hosting() == true`), scheduled the host's seat to AI at tick 39 and set
`players[0].ai`. The host does the mirror image. Both tests are in
`tests/net/test_host_migration.cpp`, including the deliberately awkward one: a
2-seat host that is *genuinely* dead is **also** left alone, because from inside
the survivor the two runs are byte-for-byte identical.

A star differs in the one way that matters: the survivors can still hear *each
other* once rewired, so "the hub is unreachable from everyone" is a conclusion
the remaining peers reach together rather than a guess one peer makes alone.
Lifting the gate needs §4.2's oracle (the lobby's `RosterUpdate`), not a better
guess on the data plane.

### 8.2 Deterministic re-election

With no messages exchanged, every surviving peer computes the new hub as **the
seat that started as hub while it is still live, else the lowest surviving seat
index**. Survivorship is derived from the agreed drop schedule alone —
deliberately *not* from `State`: a player who has been blown up still runs a
machine and can still be the hub, and keeping the election out of hashed state is
what makes it recomputable after any rollback. So all peers elect the same hub
with zero coordination — the election is a pure function of (schedule, tick).

The rule **chains**: if the elected hub dies too, the next-lowest survivor takes
over by the same rule.

**A refinement that RETRACTS the obvious design.** Evaluating the role at the
peer's *confirmed frontier* — so that a peer takes it only once the migration is
agreed history — is unimplementable over the very topology the role exists for,
and it deadlocks. The frontier cannot cross `T` until the survivors exchange
input; their input only ever reached each other through the dead hub's
reflection; and it will not flow again until somebody *with the role* rewires the
star. The frontier gate therefore gates the migration on itself. The election
reads the **schedule** (`live_seats()`), not the frontier. That is safe for the
reason the schedule exists: an entry only ever comes from an announcement or a
hard-timeout detection, it is idempotent and earliest-wins, and the role decides
nothing hashed — only which peer emits `Drop` frames.

**Nothing new is declared while a migration is still healing**, and this is a
correctness guard rather than a nicety. When the hub dies the star is severed, so
every survivor goes silent to *every* other survivor at the same instant — not
just the corpse. Ungated, that silence is read as evidence twice over: the seat
just elected is itself declared lost one timeout later (the election chains all
the way down until every peer has elected *itself* and the table has split into
as many one-player games as there are survivors), and the moment a survivor does
take the role it decrees an ordinary `Drop` on the other survivor, which is
equally silent and equally alive. Both were measured in
`tests/net/test_host_migration.cpp` before the guard existed.

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

#### What is implemented, and what is not

**Implemented in `libs/net` and covered by `tests/net/test_host_migration.cpp`:**
detection (`MsgType::HostLost`), the election and its chaining, the un-confirm +
hash purge that makes disagreeing survivors converge, the healing guard that
stops the election cascading, and `MigratingTransport` — the indirection the
session borrows so the far end can be re-pointed without it noticing.
`RollbackSession::set_migration_hold()` is the stall across the rewire.

**NOT implemented:** the front-end half. Nothing yet calls
`LobbyClient::reanchor()`, no `roster_digest` is computed C++-side, no fresh
punch to the new hub is driven, and no `StarHubTransport` is rebuilt on the
promoted guest. So the mechanism is proven against a modelled rewire
(`StarBus::set_hub`) and not against a real one. **The session-level claims below
are loopback-only** — see the report's honest-limits section.

#### Constraints the frozen protocol imposes (verified against the Go source)

- **The lobby survives a match.** `HeartbeatInterval` (10 s) × `HeartbeatMiss`
  (3) = a 30 s per-member deadline, and a lobby is deleted **only** when its
  member map empties. There is no age check and no IN_PROGRESS-specific timer —
  `createdAt` is written and never read. One member frame every <30 s keeps the
  lobby, its code and its roster alive indefinitely, in any state. So the
  re-anchor is available mid-match *provided somebody keeps pumping `LobbyFlow`
  during the match*, which nothing currently does.
- **`ReanchorLobby` works while IN_PROGRESS.** Its handler checks membership,
  host-seat vacancy and the roster digest — **not** lobby state. It is expected
  to be refused at first ("the host seat is still occupied") until the dead
  host's socket is reaped, and is never load-bearing for the match itself; what
  it buys is the code staying resolvable and a rematch having an anchor.
- **Candidates are the pre-match ones.** `relaysCandidates()` is
  `OPEN || LOCKED`, so a mid-match `Candidates` frame is accepted, validated and
  then **silently dropped**. No fresh exchange is available and none can be
  published. Each survivor already holds its new hub's addresses from the
  pre-match fan-out, so the re-punch has something to aim at; the cost is that a
  peer whose mapping has since changed cannot republish, and for it the re-punch
  fails.
- **Relay is still two seats only.** `RelayedTransport` addresses one destination
  seat, so a relayed star cannot exist. Applied to the **survivors**, so two
  survivors of a four-seat match do get the relay. Above two there is no
  fallback and one unreachable peer ends the match for the table. Lifting it
  needs a relay allocation that fans out — a matchmaker change, and therefore a
  PROTOCOL.md change, out of scope by construction.

## 9. Match setup after the punch (host-authoritative)

The punch produces a connected `Transport`; the match itself needs a
`sim::MatchConfig`. Between the two sits `libs/net`'s `SetupSession` — the
online equivalent of the original's roster and level screens.

**Why it exists.** Netplay used to build a hard-coded canonical config (2 seats,
no AI, stage derived from the seed) purely so the two peers could not disagree.
That bought determinism at the price of no map choice, no AI slots and no
roster. The original does not need that compromise: after its two network
screens every peer lands in the *ordinary* roster (`sub_410F81`) and level
(`sub_406DDE`) screens, the **host drives them and broadcasts each change**
(slot kind 40, team kind 58, level kind 43, rounds kind 44), and **guests are
read-only** (SFX 40 on any edit attempt) — `docs/re/network-screens.md` §7.

**Shape.** Same polarity, our transport, after the punch, so the matchmaker
stays config-agnostic (ADR-0011: the server never sees a `MatchConfig`).

| direction | message | payload |
|---|---|---|
| host → guest | `MsgType::SetupPreview` | live, DISPLAY-ONLY subset: per-slot kind, level index + name, rounds, team flags. Re-sent every 250 ms. |
| host → guest | `MsgType::SetupChunk` | one 1024-byte slice of the serialized `MatchConfig` + revision, total length and a whole-blob checksum. Re-sent every 200 ms until acked. |
| guest → host | `MsgType::SetupAck` | revision + blob checksum, so the host learns the guest holds *its* bytes. |

**The final payload is the whole resolved config, never a level index.** A level
index would have each peer build its own board, and two installs disagree
constantly — a different `.SCH`, a different `EXTRA<n>.RES`, a custom map at
that index, a hand-edited `VALUELST`. Shipping the resolved config (cells, stage
actors, warp destinations, spawns, per-slot `active`/`ai`/`team`, seed, the whole
`Tuning`, the powerup override/forbid/born-with arrays, the campaign fields)
makes every peer feed `Simulation` byte-identical input. The preview may be
lossy; the final may not.

**Chunked, not fragmented.** A full 10-spawn config serializes to 1842 bytes —
past a safe UDP payload — so it travels as two datagrams of at most 1039 bytes
each rather than relying on IP fragmentation surviving an arbitrary path. A
reassembly is decoded only when every slice of one revision is present and the
blob checksum matches, so a lost chunk yields *no* config rather than half of
one.

**Scope limits, both deliberate.** (1) Like `SeedHandshake`, `SetupSession`
models ONE remote peer: `peer_acked()` flips on the first matching ack, so over
a `StarHubTransport` (>2 seats) one guest's ack would wrongly mean "everybody
has it". The star needs either a session per guest or a seat id in the ack plus
a per-seat mask. (2) The original's guest→host slot upload (kind 40) — a guest
contributing its own local humans/AI to the shared roster — is not built; it is
a new `MsgType` plus a host-side merge, and another `kWireProtocolVersion` bump.

## 10. The match shell (host-authoritative), wire v8

Everything above is about the *content* of a match. Two transitions around it
used to be **local** decisions, and both ended the connection:

1. **Esc during a round** returned to the menu, and the caller answered by
   destroying the transport. It was also a divergence: the peer that pressed Esc
   stopped its sim while the other kept ticking, so by the time anything read the
   frozen state the two had simulated a different number of ticks (and tallied a
   different slice of the round's kill events).
2. **A finished match** dropped the transport at the VICTORY screen, so two
   players who wanted another game had to go back through the lobby — which by
   then has been reaped anyway (the server drops a lobby about 30 s into a match,
   `HeartbeatInterval` 10 × `HeartbeatMiss` 3).

Both are now carried by one new message, `MsgType::MatchCtl` — a `kind` byte and
a `u32` tick, 6 bytes. The polarity is the original's (`docs/re/network-screens.md`
§7): the machine driving the game decides and broadcasts, a `sub_40C06A() == 1`
client may only ask — and this port went one step further and removed even the
asking (see the `EndRoundRequest` row).

"The machine driving the game" is `RollbackSession::hosting()` — the **elected**
hub, not whoever pressed Host. In a star the role moves when the hub dies (§8.2),
and every one of the decisions below asks that function rather than the
`DropPolicy::is_host` seed. Two of them did not until 2026-07-31, which left a
migrated star with no machine anywhere able to abandon a round.

| direction | kind | meaning |
|---|---|---|
| hub → all | `EndRound` | "this round stops at `at_tick`; it is a DRAW". Re-sent every pump; earliest tick wins, so duplicates and reordering are no-ops. |
| — (retired) | `EndRoundRequest` | Was "the player here pressed Esc", a guest's request the host granted — i.e. a lever letting any guest force-end any round with no host confirmation. **Removed 2026-07-30 on both sides**: nothing sends it, and a hub ignores an inbound one. The value and its `decode()` stay because `kWireProtocolVersion` did not move — a peer on the previous build is turned away rather than disconnected on an unknown kind. |
| hub → all | `RematchWait` | liveness while the host reads the post-match RESULTS/VICTORY screens. |
| hub → all | `Rematch` | "I am walking back to the setup screens now." |

**Why the end tick is in the FUTURE**, unlike `MsgType::Drop`'s deliberately
retroactive one. A drop tick must be reachable when a seat's input will never
arrive; here every seat is live and still sending, so any near-future tick is
reachable by definition — and it *must* be future, because a peer that had
already speculated past it would stop having simulated more of the round than the
host did. The host picks `its own head + max_prediction + slack`: a peer cannot be
more than `max_prediction` past its confirmed frontier, and its confirmed frontier
cannot be above the host's head (it needs the host's input to get there).

**A DRAW BY DECREE.** The abandoned round's outcome is *not* read out of the
frozen state — `run_netplay_match_seats` forces `w = -1` when
`end_round_scheduled()`. That is what makes the transition immune to the peers'
last speculative ticks differing at all: there is no per-machine observation left
in the path. It then falls into the same DRAW → RESULTS → next-round rotation an
ordinary drawn round takes, which is exactly the local flow's behaviour (Esc →
DRAW GAME → Enter → replay).

**The rematch needs no new mechanism**, only a door. Both peers reach "the match
is decided" with no traffic (same sim, same tally, same clinch), and
`SetupSession` needs nothing but a connected `Transport` — so returning to map
selection is just *running the setup stage again over the link that is already
there* (`GameApp::run_netplay_session`). What does need agreeing is **when** to
leave the outcome screens, because the guest's next `SetupSession` starts a
liveness timeout the moment it is built: a guest that walked into the roster
screen ahead of a host still reading VICTORY would time out and report THE HOST
LEFT THE GAME. So `net::RematchSession` gates the VICTORY screen on the host's
`Rematch`, and keeps `RematchWait` flowing under both outcome screens so silence
never has to be guessed at. It self-heals against UDP loss twice over: `Rematch`
is re-sent on an interval, and a guest also follows any inbound **setup** traffic
(the host's first preview says the same thing implicitly, and is re-broadcast for
as long as it is on those screens).

**Ctrl+Q is untouched** — it stays the faithful unilateral forfeit
(`docs/re/in-match-shell.md`'s "Esc negative finding": raw key 0x11 is the only
key that aborts a round in the original), and it still ends the session.

## 7. What is reused vs new

| reused unchanged (ADR-0010) | new (this design, ADR-0011) |
|---|---|
| `Transport` seam, `UdpTransport`, `LoopbackLink` | `RelayedTransport` (a `Transport` impl) |
| `input_codec`, `protocol` (`Input`/`InputRange`/`Hash`/`Hello`) | `PunchPing`/`PunchPong` + `Stun*` tags (extend `MsgType`); `SetupPreview`/`SetupChunk`/`SetupAck` + the `MatchConfig` codec (§9) |
| `RollbackSession`, `LockstepSession` | `LobbyClient`, `Rendezvous` (control + punch) |
| `SeedHandshake`, `state_hash` desync | `build_hash` door (server + Hello) |
| the whole `libs/sim` (untouched) | the minimal signaling/relay **server** |

The server is the only wholly-new artifact; everything on the client is a
lobby/rendezvous layer *above* the existing, tested `libs/net`.
