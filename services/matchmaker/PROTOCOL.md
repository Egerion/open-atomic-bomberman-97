# Matchmaker wire protocol (FROZEN — Phase 1a)

This is the single source of truth the C++ client (IXWebSocket, ADR-0011)
implements against. It realises `docs/online-multiplayer-design.md` §§1–2 and
§5.2. Field names and JSON types here are the contract — do not change one side
without the other.

Two channels:

| channel | transport | carries |
|---|---|---|
| **control** | WebSocket text frames, `ws(s)://<host>:8080/ws` | lobby create/join/list/ready/start/roster/candidates/reanchor |
| **discovery** | UDP datagrams, `<host>:8081` | STUN reflexive-address echo |

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

### AllocateRelay → Error (Phase 2 stub — see §6)
```json
{"type":"AllocateRelay","lobby_id":"…","seat":1}
```
Currently answers **Error** `{"code":"not_implemented"}`.

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

### ReanchorAccepted
```json
{"type":"ReanchorAccepted","lobby_id":"…","code":"K7Q2MP","host_token":"…"}
```

### Error (out-of-band failures)
```json
{"type":"Error","code":"not_host","message":"only the host with a valid host_token may start"}
```
`code` values in Phase 1a: `bad_json`, `bad_message`, `unknown_type`,
`already_in_lobby`, `not_in_lobby`, `not_host`, `already_started`,
`not_enough_players`, `not_all_ready`, `build_mismatch`, `reanchor_rejected`,
`not_implemented`, `internal`.

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

## 6. Phase 2 TODO — relay allocation (NOT frozen yet)

The UDP relay forwarder (design §4, ADR-0011 decision 3) is deferred. The
control-plane shapes below are the intended contract; they are **provisional**
until Phase 2 lands (`relay.go` holds the server-side drop-in seam). Today the
server accepts `AllocateRelay` and replies `Error{code:"not_implemented"}`.

```
C→S  {"type":"AllocateRelay","lobby_id":"…","seat":1}
S→C  {"type":"RelayAllocated","relay_addr":"relay.example:3478","alloc_id":"…"}
```

In-match data plane (opaque to the server; it never decodes an Input/Hash
frame): each relayed datagram is `[alloc_id][opaque libs/net datagram]`; the
relay looks up `alloc_id` and forwards the opaque bytes to the far seat's
learned address. This is a `RelayedTransport` swap on the client — the
`RollbackSession` above it is byte-for-byte identical to the direct-P2P case.
