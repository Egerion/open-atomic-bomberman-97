# Matchmaker wire protocol (FROZEN)

This is the single source of truth the C++ client (IXWebSocket, ADR-0011)
implements against. It realises `docs/online-multiplayer-design.md` §§1–2, §4
and §5.2. Field names and JSON types here are the contract — do not change one
side without the other.

Three channels:

| channel | transport | carries |
|---|---|---|
| **control** | WebSocket text frames, `ws(s)://<host>:8080/ws` | lobby create/join/list/ready/start/roster/candidates/reanchor/relay allocation/chat |
| **discovery** | UDP datagrams, `<host>:8081` | STUN reflexive-address echo |
| **relay** | UDP datagrams, `<host>:8082` | opaque per-tick game traffic, forwarded when the punch failed (§6) |

TLS terminates at the hosting edge (Fly/Render) → clients use `wss://`. The
binary can also serve `wss://` directly with `-tls-cert`/`-tls-key`.

---

## 1. Envelope

**One JSON object per WebSocket text frame.** The object is discriminated by a
top-level `"type"` string; **all payload fields are flattened alongside it**
(never nested under a `payload` key). Example:

```json
{"type":"CreateLobby","visibility":"private","name":"Ege's game","max_seats":4,"build_hash":"0xA1B2C3D4","player":"Ege"}
```

Rules:

- `type` is always present and is one of the names in §3/§4.
- Unknown fields are ignored (forward-compatible); missing optional fields take
  their documented default.
- `build_hash` is a **hex STRING** in the human-readable form `"0xA1B2C3D4"`.
  Comparison is case-insensitive and the `0x` prefix is optional
  (`"0xA1B2C3D4"`, `"a1b2c3d4"`, `"A1B2C3D4"` all compare equal). The wire form
  SHOULD keep the `0x` prefix.
- `seed` and `local_seats_mask` are JSON **numbers** (unsigned; `seed` is a
  uint32, fits exactly in a double). `match_config_digest` is a **hex string**
  like `build_hash`.
- `seat` is a 0-based index into the ≤10-slot roster.

---

## 2. STUN echo (UDP, design §2)

One JSON object per UDP datagram. `nonce` is an **opaque string** echoed back
verbatim (the server never interprets it; a string avoids JSON-number precision
pitfalls). The probe MUST be sent from the **same UDP socket** the match will
punch/relay on, so the reflexive address the server reports is the one the peer
will actually use.

```
C→S   {"type":"StunProbe","nonce":"<opaque token>"}
S→C   {"type":"StunReply","nonce":"<same token>","your_addr":"81.2.3.4:52001"}
```

`your_addr` is the `ip:port` source the server observed (IPv6 is
`"[::1]:52001"`). Malformed or non-`StunProbe` datagrams get no reply.

---

## 3. Client → server messages

### CreateLobby → LobbyCreated
```json
{"type":"CreateLobby","visibility":"private","name":"Ege's game","max_seats":4,"build_hash":"0xA1B2C3D4","player":"Ege"}
```
- `visibility`: `"private"` | `"public"` (anything else ⇒ `"private"`).
- `max_seats`: int, clamped to `[2,10]`.
- Reply: **LobbyCreated** (the creator becomes seat 0, the host).

### JoinByCode → JoinAccepted | JoinRejected
```json
{"type":"JoinByCode","code":"K7Q2MP","build_hash":"0xA1B2C3D4","player":"Ada"}
```
- `code`: 6-char Crockford base-32 (case-insensitive; normalised to upper).
- Reply: **JoinAccepted** (assigned the lowest free seat) or **JoinRejected**.

### ListPublic → PublicList
```json
{"type":"ListPublic","build_hash":"0xA1B2C3D4"}
```
- `build_hash` optional; when present, rows are flagged `build_ok`.

### SetReady  (server pushes RosterUpdate)
```json
{"type":"SetReady","ready":true}
```

### Heartbeat → HeartbeatAck
```json
{"type":"Heartbeat"}
```
Presence keep-alive. A member that misses `K` heartbeats
(`heartbeat-interval × heartbeat-miss`, default 30 s) is dropped and a
RosterUpdate is broadcast; an emptied lobby is evicted (its code freed).

### Candidates → PeerCandidates (fan-out)
```json
{"type":"Candidates","lobby_id":"…","seat":1,"list":[
  {"kind":"host","addr":"192.168.1.9:41234"},
  {"kind":"reflexive","addr":"81.2.3.4:52001"},
  {"kind":"relay","addr":"relay.example:3478","alloc":"…"}
]}
```
The server stores this seat's list and pushes a **PeerCandidates** to every
other member; it also back-fills the sender with any peer lists already known,
so the exchange converges regardless of arrival order. `kind` priority mirrors
ICE: `host` > `reflexive` > `relay`.

### StartMatch (host only) → StartMatch (broadcast)
```json
{"type":"StartMatch","lobby_id":"…","host_token":"…","input_delay":2,"match_config_digest":"0xC0FFEE01"}
```
- `host_token` must equal the lobby's token AND the sender must be the host seat.
- `input_delay` (optional, default 2, clamped `[1,8]`) and `match_config_digest`
  (optional, default `"0x00000000"`) are **echoed** into the broadcast — the
  server is config-agnostic (ADR-0011: it never sees `MatchConfig`); the host
  owns these parity values.
- Server validates: valid host_token + host seat, lobby `OPEN`, ≥2 seats, **all
  seats ready**, **all build_hash equal**. Then it generates `seed`, locks the
  lobby, and broadcasts **StartMatch** to every seat.

### ReanchorLobby → ReanchorAccepted (host migration, design §8.3)
```json
{"type":"ReanchorLobby","code":"K7Q2MP","roster_digest":"<hex sha-256>"}
```
- Sent by a surviving peer promoted to hub after the host dropped.
- `roster_digest` proves continuity: it must equal the server's digest of the
  **surviving** roster (see §5). On success the server keeps the same `code`
  resolvable, mints a **fresh host_token**, promotes the sender to host seat, and
  replies **ReanchorAccepted** + broadcasts RosterUpdate. On mismatch it replies
  **Error** `{"code":"reanchor_rejected"}`.

### MatchOver  (rematch, design §5.2)
```json
{"type":"MatchOver","lobby_id":"…"}
```
Any member may send it. The lobby returns to `OPEN`, ready flags clear, roster
is kept, and a RosterUpdate is broadcast. Enables rematch and re-opens joins.

### Chat → Chat (fan-out — see §7)
```json
{"type":"Chat","text":"gl hf"}
```
- `text` is the whole message. **No other field is read**: a `seat` or `name` in
  a client frame is ignored, so nobody can speak as another seat.
- The sender must hold a seat, else **Error** `{"code":"not_in_lobby"}`.
- Validated, never repaired: over `120` bytes ⇒ **Error** `{"code":"chat_too_long"}`;
  a control rune, a U+FFFD, or a blank/empty body ⇒ **Error** `{"code":"chat_invalid"}`.
- Over the rate limit ⇒ **dropped silently and logged** (no reply). See §7.

### AllocateRelay → RelayAllocated  (relay fallback — see §6)
```json
{"type":"AllocateRelay","lobby_id":"…","seat":1}
```
Sent once the hole-punch has failed. `seat` is **advisory**: the allocation
always belongs to the sender's own seat. See §6 for the full contract.

---

## 4. Server → client messages

### LobbyCreated
```json
{"type":"LobbyCreated","code":"K7Q2MP","lobby_id":"…","host_token":"…","your_seat":0}
```
`lobby_id` and `host_token` are 128-bit opaque hex handles (32 chars).

### JoinAccepted
```json
{"type":"JoinAccepted","lobby_id":"…","your_seat":1,
 "roster":[{"seat":0,"name":"Ege","ready":false,"is_host":true},
           {"seat":1,"name":"Ada","ready":false,"is_host":false}],
 "host_candidates":[]}
```

### JoinRejected
```json
{"type":"JoinRejected","reason":"not_found"}
```
`reason` ∈ `"not_found" | "full" | "build_mismatch" | "in_progress"`.

### PublicList
```json
{"type":"PublicList","lobbies":[
  {"code":"K7Q2MP","name":"Ege's game","players":2,"max":4,"host_region":"","server_rtt_ms":0,"build_ok":true}
]}
```
Only `public` + `OPEN` lobbies appear. `build_ok` is false when the browser's
`build_hash` differs (the row is still listed so the UI can grey it out).
`host_region`/`server_rtt_ms` are placeholders in Phase 1a.

### RosterUpdate (pushed on any roster change)
```json
{"type":"RosterUpdate","roster":[{"seat":0,"name":"Ege","ready":true,"is_host":true}]}
```
`RosterEntry`: `seat`(int), `name`(string), `ready`(bool), `is_host`(bool),
optional `rtt_to_host_ms`(int). Roster is always sorted by ascending seat.

### HeartbeatAck
```json
{"type":"HeartbeatAck"}
```

### PeerCandidates (one per other seat)
```json
{"type":"PeerCandidates","seat":0,"list":[{"kind":"host","addr":"192.168.1.9:41234"}]}
```
`seat` is the seat these candidates belong to.

### StartMatch (broadcast)
```json
{"type":"StartMatch","seed":1592371220,"seat_assign":[0,1],
 "match_config_digest":"0xC0FFEE01","input_delay":2,
 "topology":{"hub_seat":0},"local_seats_mask":2}
```
- `seed`: uint32, authoritative match seed (same value to every peer).
- `seat_assign`: ascending list of occupied seats (final seat→player binding).
- `input_delay`: shared, identical on all peers.
- `topology.hub_seat`: the star input-hub seat (the host; for 2P a direct pair).
- `local_seats_mask`: **per-recipient** — the bitmask of seats this recipient
  owns (`1 << seat`; each connection owns exactly its own seat in Phase 1a).
- `max_prediction` is deliberately NOT sent — it is a local per-peer display
  policy (design §6).

### Chat (relay, one per member — see §7)
```json
{"type":"Chat","seat":1,"name":"Ada","text":"gl hf"}
```
`seat` and `name` are the SERVER's, read from the roster — never echoed from the
sender's frame. Sent to every member of the sender's lobby, **the sender
included**, so all members hold one identically-ordered transcript.

### ReanchorAccepted
```json
{"type":"ReanchorAccepted","lobby_id":"…","code":"K7Q2MP","host_token":"…"}
```

### RelayAllocated
```json
{"type":"RelayAllocated","relay_addr":"relay.example:8082","alloc_id":"7f3a…"}
```
See §6.

### Error (out-of-band failures)
```json
{"type":"Error","code":"not_host","message":"only the host with a valid host_token may start"}
```
`code` values: `bad_json`, `bad_message`, `unknown_type`, `already_in_lobby`,
`not_in_lobby`, `not_host`, `already_started`, `not_enough_players`,
`not_all_ready`, `build_mismatch`, `reanchor_rejected`, `chat_too_long`,
`chat_invalid`, `internal`.

---

## 5. roster_digest algorithm (host migration)

Both server and the promoted hub MUST compute this identically. Over the
**surviving** roster, in **ascending seat order**, using only the `(seat, name)`
pairs (ready/is_host are excluded so the digest identifies "the same players in
the same seats"):

```
buf = ""
for entry in roster sorted by seat ascending:
    buf += str(seat) + ":" + str(byte_length(name)) + ":" + name + ";"
digest = lowercase_hex( SHA-256( utf8(buf) ) )    // 64 hex chars, no "0x"
```

The length prefix makes the encoding injective even if a name contains `:`/`;`.

---

## 6. Relay fallback (FROZEN)

The TURN-like UDP forwarder for peers whose hole-punch failed (symmetric NAT /
CGNAT). Design §4, ADR-0011 decision 3; server side in `relay.go`.

**The server never simulates and never decodes a game datagram.** Everything
past the fixed header is opaque bytes: the relay forwards, it does not inspect.
It adds no reliability, no ordering and no rate shaping — the game's own netcode
is loss-tolerant and this stays a dumb forwarder.

### 6.1 Control plane (WebSocket)

```
C→S  {"type":"AllocateRelay","lobby_id":"…","seat":1}
S→C  {"type":"RelayAllocated","relay_addr":"relay.example:8082","alloc_id":"7f3a…"}
```

- `alloc_id` is a 128-bit opaque handle rendered as **32 lowercase hex chars**
  (same generator as `lobby_id` / `host_token`).
- **One allocation per (lobby, seat).** Re-allocating for the same seat returns
  the SAME `alloc_id` — idempotent, and the seat's already-learned address is
  preserved, so retrying after a lost reply is safe mid-match.
- `seat` is **advisory**: an allocation always belongs to the SENDER's own seat,
  so no peer can mint or steal another seat's handle. It may be omitted; if it
  is present and disagrees with the sender's seat the request is rejected with
  `Error{code:"bad_message"}`.
- `relay_addr` is the publicly reachable `host:port` of the UDP relay
  (server flag `-relay-advertise`, defaulting to the relay listen address —
  same host/port pattern as the STUN listener).
- Errors: `not_in_lobby` (no seat for this connection), `bad_message`
  (undecodable frame or a foreign `seat`), `internal`.
- Allocations are freed when the member disconnects, when the heartbeat reaper
  drops it, when the lobby is evicted, or after `-relay-idle` (default 60 s)
  without traffic from that seat.

### 6.2 Data plane (UDP, `<host>:8082`)

Every datagram, **in both directions**, is:

```
 offset  size  field
   0      16   alloc_id   (BINARY — the 32 hex chars decoded)
  16       1   seat
  17     ...   opaque payload
```

The header is therefore **exactly 17 bytes** each way.

| direction | `alloc_id` is… | `seat` is… |
|---|---|---|
| client → relay | the **sender's** allocation | the **destination** seat |
| relay → client | the **receiver's own** allocation | the **sender's** seat |

So a client always prepends its own `alloc_id` + the seat it is writing to, and
always strips a fixed 17-byte prefix on receipt, reading the seat byte to learn
who sent it.

On receipt the relay:

1. looks up `alloc_id` → the sending lobby + seat;
2. **learns/refreshes that sender's public address from the datagram source**
   (the same trick as the STUN echo — this is what makes the return path work
   through a NAT that no peer can predict);
3. finds the destination seat's allocation **in the same lobby** and its learned
   address;
4. forwards `[dst's own alloc_id][sender's seat][payload]` to it, payload
   byte-identical.

**Dropped silently** (untrusted input — the relay never panics, never replies to
a malformed datagram, and never logs per datagram; drops are counted and
reported in one aggregated line per interval):

- shorter than 17 bytes;
- unknown `alloc_id` (including an expired one);
- destination seat has no allocation in the sender's lobby — a lobby therefore
  cannot address any other lobby's seat;
- destination's address not learned yet (it has not sent anything).

Datagrams larger than **2048 bytes** are dropped and counted as oversize — never
truncated.

On the client this is one `Transport` swap decided by the `Rendezvous` outcome;
the `RollbackSession` above it is byte-for-byte identical to the direct-P2P case.

---

## 7. Lobby chat (FROZEN)

**A PORT-ONLY FEATURE. Atomic Bomberman (1997) has no chat** — no chat window,
no in-game text entry, and no network message that could carry a line of text
(`docs/re/network-screens.md`). This exists because the maintainer asked for it,
and it is called out as an addition everywhere it appears so nobody later
mistakes it for reverse-engineered behaviour. Server side is `handleChat` in
`manager.go`; the client's overlay is `libs/game/.../chat_overlay.hpp`.

It rides the **control plane** rather than the game's UDP path because players
chat *in the lobby* — before any hole punch has happened, when the WebSocket is
the only link they share. It is therefore available from the moment a seat is
taken right through the pre-match setup screens; it is not part of the in-match
data plane and the relay (§6) never carries it.

```
C→S  {"type":"Chat","text":"gl hf"}
S→C  {"type":"Chat","seat":1,"name":"Ada","text":"gl hf"}
```

### 7.1 What the server guarantees

- **Containment.** A message is fanned out to every member of the SENDER's own
  lobby and to nobody else. A connection holding no seat gets
  `Error{code:"not_in_lobby"}` and no relay.
- **Attribution.** `seat` and `name` are read from the server's roster. Fields
  of those names in a client frame are ignored outright, so a peer cannot speak
  as another seat.
- **Echo to the sender.** The author receives its own line back through the same
  fan-out, so every member holds one identically-ordered transcript and a
  delivered message is visibly delivered.
- **No history.** The server stores nothing; a member that joins late sees only
  what is said after it arrives.
- **Any live lobby state.** `OPEN`, `LOCKED` and `IN_PROGRESS` all relay — the
  online roster/level screens run after `StartMatch`, and chat stays live there.
  An `EVICTED` lobby is already gone.

### 7.2 Validation — reject, never repair

Chat is the one place a player's own typing reaches other people, so a body is
either relayed **byte-for-byte** or refused. Nothing is truncated, stripped or
substituted.

| condition | answer |
|---|---|
| `len(text)` > **120 bytes** | `Error{code:"chat_too_long"}` |
| any control rune, or a U+FFFD (see below) | `Error{code:"chat_invalid"}` |
| empty, or whitespace only | `Error{code:"chat_invalid"}` |
| otherwise | relayed verbatim |

Non-ASCII passes through untouched: what a client can *draw* is its own
business, and the relay does not get to decide which alphabets exist. (The
game's 1997 FON covers printable ASCII, so its own overlay drops the rest at
BOTH ends — that is a font limit in one client, not a wire rule.) `encoding/json`
substitutes U+FFFD for invalid UTF-8 while decoding, so a body arriving with one
is a body that did not survive the trip intact; forwarding it would be passing
on a silently repaired string, hence the refusal.

### 7.3 Rate limit — drop and log

Per **connection**, a token bucket measured in milliseconds of credit: one
message costs **2000 ms**, and at most **4** may be banked (the bucket starts
full). A normal exchange never notices it; a flood settles at one line every two
seconds.

An over-rate message is **dropped whole and logged** (`chat dropped (rate
limit)`, with the lobby code and seat). It is never queued, never shortened, and
**no `Error` is sent back** — answering every dropped line would amplify the
flood it exists to damp. The C++ client runs the identical bucket locally, so a
well-behaved client refuses (and says so) exactly where the server would drop.
