# Matchmaker security review

The matchmaker is **deployed, internet-facing and unauthenticated**. Anyone can
open a WebSocket to it and send anything; anyone can send it UDP. This file is
the record of what was checked, what was found, what was fixed and what was
accepted — written so the next reader can tell the difference between "we
thought about it" and "we did something about it".

**Threat model.** An attacker is any stranger who can reach the three listeners,
plus — for a relayed match specifically — an observer on the network path
between a peer and the server. There are no accounts and no user identity: the
only secrets are the 6-character lobby `code`, the 128-bit `host_token`, and the
128-bit relay `alloc_id`. Adding authentication is explicitly out of scope; where
something genuinely needs it, this file says so rather than half-building it.

**Standing rules the code follows.**

1. **Reject, never repair.** A frame that breaks a rule is refused whole. Nothing
   is truncated, stripped or substituted — a trimmed display name would put a
   name in the roster that the player never chose.
2. **Bound anything a stranger can grow.** Every map, list, buffer and timer
   reachable without credentials has a ceiling.
3. **Never key a growing map on an attacker-chosen value.** A UDP source address
   is chosen by the sender and forgeable; it indexes a fixed-size table, never a
   map that allocates.
4. **Aggregate the logs.** A log line per hostile packet is itself an amplifier.
   Refusals are counted and reported in one periodic line.

---

## Serious, and NOT fixed here

### S1. The deployed control plane runs in cleartext, so `host_token` is readable

`fly.toml` sets `force_https = false`, and the C++ client dials `ws://` because
IXWebSocket v11.4.6's mbedTLS backend does not build against any single mbedTLS
release (`cmake/BomberIXWebSocket.cmake`). The existing comment says the
signalling "carries no credentials". **That is wrong.** In the clear, on every
lobby session, go:

| value | what it grants an observer |
|---|---|
| `host_token` | `StartMatch` authority over that lobby |
| `code` | the ability to join a "private" lobby |
| `alloc_id` (in `RelayAllocated` and in `Candidates`) | the relay handle for a seat |
| `lobby_id`, roster, display names, candidate `ip:port`s | who is playing, from where |

So a passive on-path observer of the *control plane* gets the host's credential
outright — it does not need the datagram-header trick that F1 below is about.
This is the single most serious issue in the service and **it cannot be fixed
inside `services/matchmaker/`**: the fix is to give the client a working TLS
backend and then flip `force_https = true`. Until then, treat "private" lobbies
as private-by-obscurity and the host token as public to anyone on the path.

The misleading part of the `fly.toml` comment has been corrected in place.

### S2. A seat squatter can block a public lobby indefinitely

`StartMatch` requires **every** seat to be ready, and the protocol has no kick
message. Anyone who joins a public lobby and never readies up prevents the host
from ever starting, and the host has no recourse but to abandon the lobby.

Not fixed: every available fix changes the frozen wire contract (a `KickPlayer`
message, or letting the host start with only the ready seats). It needs a
protocol revision, so it is recorded here rather than patched around.

---

## Found and fixed

### F1. Relay seat hijack — `relayTable.forward` learned the return path from any source

**Severity: high. Exploitable by: an observer on the path of a relayed match.**

`forward` did this, once per datagram:

```go
from, ok := t.byID[id]   // alloc_id straight off the wire
if !ok { drop }
from.addr = src          // learn/refresh — UNCONDITIONALLY
```

`alloc_id` is the only thing identifying a sender on the data plane, and it
travels **in cleartext as the first 16 bytes of every relayed datagram**
(PROTOCOL.md §6.2). One datagram carrying a seat's `alloc_id` therefore repointed
that seat's entire inbound match traffic at an address of the sender's choosing.
The victim goes deaf; the attacker receives everything aimed at them.

The usual defence — "the handle is 128 bits and only its owner ever sees it" —
does not hold here. A relayed match is by definition one where the direct path
failed and traffic is crossing hostile ground, so an observer reading the header
is inside the threat model, not outside it. (And per S1, on the current
deployment the `alloc_id` is also readable straight off the control plane.)

**Fix — pin the address, and only move the pin when the pinned peer has gone
quiet.** `forward` now runs:

| situation | outcome |
|---|---|
| no address learned yet | learn it and **pin** it |
| datagram from the pinned address | forward, and re-assert the pin |
| different source, pinned address spoke within `kRelayRebindQuiet` (5 s) | **drop**, pin unchanged, counted as `rebind_refused` |
| different source, pinned address quiet ≥ 5 s, rebind budget left | move the pin (one per 10 s, 3 banked) |
| different source, budget exhausted | **drop**, counted as `rebind_throttled` |

A live seat sends ~20 datagrams a second, so while the real peer is on the air
the pin is re-asserted continuously and the window in which it can be taken never
opens. A genuine mid-match NAT remap still recovers, because the peer's *old*
mapping necessarily goes silent the moment it starts sending from the new one:
after 5 s (roughly the same stall the game's own netcode already tolerates as
packet loss) the new address is accepted.

Two supporting changes come with it: `lastSeen` now moves only on an **accepted**
datagram, so a stranger holding an `alloc_id` cannot keep a dead allocation alive
past the idle reaper; and the pinned address is **copied** out of the read path
rather than aliasing the receive buffer.

**What this does NOT close — stated plainly:**

- **A full on-path MITM still wins.** An attacker who can *drop* the victim's
  datagrams, not merely read them, can manufacture the 5-second silence and then
  take the pin. That attacker already controls the traffic, so the relay was
  never going to be the thing that saved the match — but the fix narrows the
  attack from "one datagram, any time" to "sustained suppression of the victim".
- **A first-datagram race, in theory.** The pin goes to whoever speaks first. In
  practice an off-path attacker cannot know the `alloc_id` before the first
  datagram exists, and an on-path one cannot read it before it is sent. Under S1
  (cleartext control plane) an observer *can* learn the `alloc_id` from
  `RelayAllocated` and race the real peer's first datagram. Fixing S1 closes this.
- **A stranger can still cost a victim a stall.** After a genuine quiet window an
  attacker who wins the rebind pins their own address; the returning peer then
  has to wait out its own quiet window to take it back — and cannot, if the
  attacker keeps sending. The rebind budget bounds the flapping but does not
  arbitrate who deserves the seat.

Closing the remaining gap properly needs the datagram to be authenticated, not
just addressed — a MAC over the payload under a per-allocation key handed out on
the control plane. That is a wire-format change to a frozen protocol and a real
design decision, so it is named here rather than smuggled in.

Regression tests: `relay_rebind_test.go`.

### F2. `player` and lobby `name` went verbatim into the roster and out to everyone

**Severity: medium. Exploitable by: anyone.**

`CreateLobby`/`JoinByCode` took `player` straight into the roster, and the roster
is broadcast to every member and echoed as the `name` of every relayed Chat
frame; `name` is served to *strangers* by `ListPublic`. With a 64 KiB read limit
that was up to 64 KiB of arbitrary bytes — control characters, ANSI escapes,
whatever — stored and re-sent on every roster change.

Fixed: `validateText` screens every inbound free-text field for size, valid UTF-8
and control runes, and **rejects** rather than trims. Limits are in PROTOCOL.md
§8; they are sized off what the client can actually produce (the 1997 node name
is 39 bytes, `assets::kNodeNameMax`), so no real player meets them. Non-ASCII
still passes untouched — the relay does not get to decide which alphabets exist.

Screened fields: `player`, `name`, `code`, `build_hash`, `host_token`,
`roster_digest`, `match_config_digest`, and every `kind`/`addr`/`alloc` in a
`Candidates` list.

### F3. Unbounded connections, lobbies and per-connection state

**Severity: medium. Exploitable by: anyone.**

Nothing capped how many WebSockets one stranger could open. Each cost two
goroutines, a 64-slot send buffer and a socket, and a connection that never took
a seat was **never reaped** — the heartbeat reaper only walks lobby members. The
lobby table inherited that: one lobby per connection, connections unbounded.

Fixed:

| bound | default | flag |
|---|---|---|
| concurrent connections | 2000 | `-max-conns` |
| concurrent connections per client IP | 16 | `-max-conns-per-ip` |
| live lobbies | 5000 | `-max-lobbies` |
| seatless, silent connection closed after | 120 s | `-conn-idle-timeout` |

Admission happens **before** the WebSocket upgrade — a refusal costs one small
`503`, not a socket. The per-IP counter is a map keyed by client IP, which is
normally exactly the thing rule 3 forbids; it is safe here only because an entry
exists solely while a connection is open, is deleted at zero, and the total is
capped by `-max-conns`, so it can never hold more than that many keys.

Closing a seatless connection is invisible to the real client: it reconnects
lazily on the next action (`LobbyFlow`: `if (!client_.is_open()) client_.connect(...)`).

### F4. No rate limit on anything but chat

**Severity: medium. Exploitable by: anyone.**

Fixed with per-connection token buckets, all in milliseconds of credit (the
generalisation of the chat bucket, PROTOCOL.md §7.3 — chat's arithmetic is
unchanged). Every one is orders of magnitude above what the real client produces,
which sends a heartbeat every 5 s and a handful of one-shot frames:

| budget | sustained | burst | why it exists |
|---|---|---|---|
| every inbound frame | 20/s | 40 | the ceiling under everything: parse cost, broadcasts, timers, logs |
| `ListPublic` | 2/s | 4 | the one request where the smallest frame buys the largest answer |
| `Candidates` | 4/s | 8 | fans out to every other member — one frame in, up to nine out |
| failed `JoinByCode`, per connection | 1 per 2 s | 5 | see F5 |
| failed `JoinByCode`, per source IP | 2/s | 30 | see F5 |
| relay ingress, per source IP | 250/s | 500 | in front of the allocation table, so a flood cannot contend its mutex with live matches |
| STUN ingress, per source IP | 20/s | 40 | the client re-probes every 250 ms |

Over-rate frames are dropped **silently**. Answering each one with an `Error`
would turn the limiter into the amplifier it exists to prevent.

The UDP-side budgets use `ipBuckets`: a **fixed-size array** of buckets indexed
by a keyed hash of the source address. It never grows, never evicts and never
allocates per source, so a flood from a million forged addresses costs exactly
the same memory as one client; colliding sources share a budget, which is the
price of never letting a stranger allocate. The hash is keyed with a per-process
random seed, so an attacker cannot choose addresses that land in a victim's slot.
Loopback, private and link-local sources are exempt — behind an edge proxy every
connection arrives from one of those, and capping there would squeeze the whole
world into one bucket.

### F5. Lobby-code guessing was unmetered

**Severity: medium. Exploitable by: anyone. Verdict: yes, it needs its own limit.**

A code is 6 Crockford base-32 symbols — 32⁶ ≈ 1.07 × 10⁹. That is a large space,
but it is *searchable*, and the search was free: `JoinByCode` is unauthenticated,
a failed attempt does not seat you, and one socket could retry forever. A hit
puts the attacker inside somebody's private lobby, where they can then squat a
seat (S2), read the roster and chat, and `MatchOver` the lobby.

Fixed: a rejected join spends credit; an accepted one spends none, so a player
who mistypes a code or retries a full lobby never notices. Charged against the
connection **and** the source address, because the per-connection budget is reset
simply by reconnecting. Past the budget the server answers **nothing at all** —
any reply, including a rejection, is an oracle telling the searcher its rate.
Codes that are not even the right shape are rejected before the map lookup, but
still cost a guess: a guess is a guess.

Residual, and accepted: behind an edge proxy the per-IP half is inert unless
`-client-ip-header` is configured (see "Operator notes"), and an attacker with
many source addresses gets a budget per address. At the per-IP ceiling one
address buys ~2 guesses/second, so a full sweep of the code space from one
address takes on the order of 17 years. Closing it completely would need
per-account limits, i.e. accounts, which are out of scope.

### F6. One bad UDP datagram could kill a listener

**Severity: medium (platform-dependent). Exploitable by: anyone, on a non-Linux host.**

Both UDP read loops did `if err != nil { return }`. On Linux the kernel truncates
an oversized datagram silently and this never fires, so the deployed instance was
not affected — but on Windows `ReadFromUDP` returns `WSAEMSGSIZE` for an
oversized datagram and `WSAECONNRESET` when an earlier reply drew an ICMP
port-unreachable. Either ended the listener goroutine **permanently**, taking
STUN or the relay down for the life of the process. The second one needs no
attacker at all: a peer that quits mid-match is enough.

Fixed: only `net.ErrClosed` ends the loop. Every other read error is counted and
the loop continues, with a consecutive-error ceiling so a genuinely broken socket
cannot spin hot. Regression tests:
`TestRelayStillForwardsAfterAnOversizedDatagram`,
`TestStunStillAnswersAfterAnOversizedDatagram`.

### F7. `ReanchorLobby` could depose a live host

**Severity: medium. Exploitable by: any member of the lobby, including a stranger who was given the code.**

Re-anchoring mints a **fresh `host_token`**, which invalidates the sitting host's,
and promotes the sender to the host seat. The only checks were "you are a member
of this lobby" and "your roster digest matches" — and the roster is broadcast to
every member, so the digest is not a secret. Any member could therefore take the
host's authority away from a live, connected host with one frame.

Fixed: re-anchoring is host **recovery** (design §8.3), not a host election, so it
now requires the host seat to actually be vacant. The case the feature exists for
is unchanged and still tested (`TestReanchorWorksOnceTheHostIsGone`).

### F8. Error replies reflected the caller's own string back

**Severity: low. Exploitable by: anyone.**

`unknown_type` echoed `"unknown message type: " + env.Type` — up to a whole 64 KiB
frame, reflected verbatim, including control characters that would then land in
the server's log.

Fixed: `clipEcho` echoes a short, printable, valid-UTF-8 type as-is (that is the
whole diagnostic value) and replaces anything else wholesale with `(rejected)`.

### F9. Fan-out amplifiers on `SetReady` and `Candidates`

**Severity: low. Exploitable by: any member.**

One `SetReady` frame produced a `RosterUpdate` to **every** member, as fast as a
member could repeat itself, even when the value did not change. `Candidates` had
the same shape with a larger payload and no size limit on the list.

Fixed: a no-op `SetReady` now broadcasts nothing (it changes no roster, so there
is nothing to report), and `Candidates` is bounded in both dimensions — at most
`kMaxCandidates` (16) entries with bounded fields, at most 4 publications/second.

### F10. `wsConn.disconnect` did network I/O under the Manager's lock

**Severity: low (availability). Exploitable by: any connected client.**

`disconnect` is called from inside `Manager.mu` — by a broadcast that overflows a
stuck consumer's send buffer, and directly by the heartbeat reaper. It called
`websocket.Conn.Close`, which **writes a close frame**. A peer that stops reading
would therefore stall every lobby on the server behind one mutex for the write
timeout.

Fixed: `disconnect` cancels the connection's context inline (immediate) and
performs the close handshake on its own goroutine, with a fallback to `CloseNow`
if the peer will not complete it. One goroutine per connection at most, via
`closeOnce`.

### F11. StartMatch cycling stacked one timer per round trip

**Severity: low. Exploitable by: a host with one confederate.**

Each `StartMatch` armed a `time.AfterFunc` for the LOCKED→IN_PROGRESS transition,
and `MatchOver` returns the lobby to OPEN, so `Start → MatchOver → Start` left a
live timer behind every time. Fixed: at most one is outstanding per lobby.

### F12. Secrets compared with `==`

**Severity: informational.**

`host_token` and `roster_digest` were compared with Go's string `==`, which exits
early on the first differing byte. Against a 128-bit `crypto/rand` handle over a
network this is not a realistic attack, but there is no reason to leave it
standing between a member and the host's authority. Both now use
`subtle.ConstantTimeCompare`.

---

## Checked and found OK

- **Attribution cannot be forged.** `Chat` reads `seat` and `name` from the
  server's roster and ignores any in the frame (PROTOCOL.md §7.1). `Candidates`
  ignores `msg.Seat` and always stores under the sender's own seat.
  `AllocateRelay` allocates for the sender's seat and *rejects* a `seat` field
  that disagrees. `MatchOver`, `SetReady` and `Candidates` all resolve the lobby
  through the sender's own membership, never through a `lobby_id` in the frame,
  so no member can act on a lobby it is not in.
- **Cross-lobby relay addressing is impossible.** The destination seat is looked
  up under the *sender's* lobby code (`relay_test.go`).
- **Relay allocations are idempotent per (lobby, seat)** and freed on member
  disconnect, heartbeat timeout, lobby eviction and idle expiry — so the table
  cannot be grown by re-allocating.
- **Codes and handles are unguessable.** `crypto/rand`, never sequential;
  `byte & 0x1f` over a 32-symbol alphabet is unbiased because 32 divides 256.
- **Deeply nested JSON is refused, not fatal.** Go's `encoding/json` caps nesting
  at 10 000 levels, which an 8 KiB frame cannot reach; the reply is `bad_json`.
- **HTTP surface is two routes.** `/ws` and `/healthz`; everything else, including
  path traversal, is a `ServeMux` 404. `/healthz` returns `ok` and nothing else —
  no version, no build, no counts.
- **Binary WebSocket frames are ignored**; the control plane is JSON text only.
- **Slow consumers are dropped, not waited on** — a full send buffer disconnects
  rather than blocking the Manager.
- **No persistence, no PII beyond a chosen display name.** Everything is soft,
  in-RAM, and evicted on disconnect or timeout.

---

## Accepted, with reasoning

### A1. STUN is a ~2× reflector, and stays one

Measured against the live deployment: a 35 B probe drew a 68 B reply (**×1.94**);
a 1234 B probe drew 1267 B (**×1.03**). The nonce is echoed verbatim, so the reply
grows in step with the request and an attacker cannot choose a better ratio — the
gain *falls* as the probe grows. A usable reflector runs 50–500×; at 2× it is
cheaper for an attacker to send the traffic directly.

Accepted rather than fixed, because the echo is the feature: a client learns its
own reflexive address by being told what source the server saw. Two caps were
added anyway so the worst case stays at the small end of that curve: a probe over
512 bytes is dropped unparsed, and a nonce over 128 bytes is refused (the real
client's is ~38: `"seat<N>-<32 hex lobby id>"`).

Per-source rate limiting is applied but does **not** stop a spoofing attacker, by
definition. A global reply-rate ceiling was considered and rejected: it would let
anyone with a spoofed source deny STUN to every real player, which is a worse
outcome than a 2× reflector.

### A2. The relay is a paid-for, unshaped forwarder

There is deliberately no per-allocation rate limit (README "Relay: bandwidth &
cost"): shaping a lockstep game stream would create the desync the whole design
exists to avoid. Two seats in one lobby can therefore use the relay as a 1:1 UDP
forwarder for the price of holding a lobby, and the server pays egress both ways.
This is the documented cost model, bounded by the lobby/connection caps, the
2048-byte datagram cap and the 60 s idle expiry. Cap it outside (firewall,
provider quota) if it matters.

A seat addressing **itself** is now dropped — that had no legitimate meaning and
turned the forwarder into a 1:1 reflector for its own sender.

### A3. `ListPublic` discloses public lobby codes

That is what a public lobby *is*. A public row carries `code`, `name`, player
count and capacity to any caller. Private lobbies never appear. The disclosure is
bounded in rate (2/s) and in size (200 rows per answer, sorted by code so the cut
is deterministic).

### A4. A stranger who learns a code can disrupt a *pending* match, not a live one

Concretely:

- **Before `StartMatch`:** yes. They can join (if a seat is free), see the roster
  and chat, refuse to ready up and block the start (S2), or `MatchOver` the lobby.
- **After `StartMatch`:** joins are refused with `in_progress`, so a stranger who
  learns the code *after* the lock cannot get in at all.
- **A stranger who joined earlier and stayed** can still send `MatchOver`
  mid-match, which returns the lobby to OPEN and clears the ready flags. That
  does not touch the match — the match is peer-to-peer and the server holds no
  part of it — but it does disturb the rematch flow. `MatchOver` is documented as
  "any member may send it" in the frozen protocol; restricting it to the host
  would be a contract change.

### A5. Origin is not checked on the WebSocket upgrade

Native clients send no `Origin`, and authorisation is by code and token rather
than by browser origin. Unchanged, and now the deliberate choice is recorded
alongside its consequence: any web page can open a socket to this service, which
matters only to the extent that everything a socket can do is already
unauthenticated.

### A6. Successful actions are still logged one line each

`lobby created`, `join accepted`, `member left`, `match started` remain one line
per event. They are not hostile-shaped — each costs the caller a full TCP +
WebSocket handshake or a successful join, both of which are capped — and they are
what makes the service operable. Everything *refused* is aggregated instead
(`control-plane drops`, `relay stats`, `stun stats`), which is where an attacker
could otherwise write to the log at their own chosen rate.

---

## Operator notes

- **Set `-client-ip-header` behind a proxy.** Behind Fly, Render, Cloudflare or
  any reverse proxy, every WebSocket arrives from a private address, so
  `-max-conns-per-ip` and the per-IP join budget are inert. Point the flag at a
  header the proxy **overwrites** (`Fly-Client-IP`, `CF-Connecting-IP`, an nginx
  `proxy_set_header X-Real-IP`). Never point it at a raw `X-Forwarded-For` chain:
  the leftmost entry there is attacker-controlled. The header is consulted only
  when the direct peer is itself loopback/private, so a directly connected client
  cannot forge its own source with it. The service warns at startup when the flag
  is unset. `fly.toml` sets it.
- **The UDP listeners are not proxied** and see real client addresses, so their
  per-source budgets are live regardless.
- **Watch the aggregated lines.** `rebind_refused` in `relay stats` is the
  signature of F1 being attempted — no well-behaved client ever produces one.
  `join_guess` in `control-plane drops` is the signature of F5.
