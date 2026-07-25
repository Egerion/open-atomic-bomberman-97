# Open Bomberman matchmaker

The online-multiplayer **signaling / lobby control plane + STUN echo + UDP relay
forwarder** for Open Bomberman (ADR-0011, design
`docs/online-multiplayer-design.md`). One small, self-contained Go service.

**Hard invariant:** the server **never simulates and never sees game `State`**.
It holds only soft in-RAM lobby state (`code → {roster, candidates, build_hash,
…}`), echoes UDP source addresses, and forwards relayed datagrams whose payload
is **opaque bytes** to it. No gameplay logic, no persistence, no game data ever
touches it. All match authority is the deterministic P2P sim on the peers.

- **Language / runtime:** Go 1.21+ (module `github.com/egedemirbas/open-bomberman/matchmaker`).
- **WebSocket library:** [`github.com/coder/websocket`](https://github.com/coder/websocket)
  (the maintained successor to `nhooyr.io/websocket`) — MIT, stdlib-only deps.
- **Wire contract:** [`PROTOCOL.md`](./PROTOCOL.md) (frozen; the C++ client
  matches it byte-for-byte).

## Run locally

```sh
cd services/matchmaker
go run .
# WebSocket control plane on ws://localhost:8080/ws
# UDP STUN echo on        udp  localhost:8081
# UDP relay forwarder on  udp  localhost:8082
# health check            GET  http://localhost:8080/healthz
```

Test / vet / format:

```sh
go test ./...          # unit + in-process WebSocket + UDP STUN tests
go test -race ./...    # race-clean (needs a C toolchain for the race runtime)
go vet ./...
gofmt -l .             # (empty == formatted)
```

## Configuration (flags, with env fallbacks — flags win)

| flag | env | default | meaning |
|---|---|---|---|
| `-ws-addr` | `MATCHMAKER_WS_ADDR` | `:8080` | WebSocket control-plane listen address |
| `-stun-addr` | `MATCHMAKER_STUN_ADDR` | `:8081` | UDP STUN-echo listen address |
| `-relay-addr` | `MATCHMAKER_RELAY_ADDR` | `:8082` | UDP relay-forwarder listen address |
| `-relay-advertise` | `MATCHMAKER_RELAY_ADVERTISE` | _(unset ⇒ `-relay-addr`)_ | public `host:port` put in `RelayAllocated` |
| `-relay-idle` | `MATCHMAKER_RELAY_IDLE` | `60s` | drop a relay allocation after this long without traffic (`0` disables) |
| `-heartbeat-interval` | `MATCHMAKER_HEARTBEAT_INTERVAL` | `10s` | expected client heartbeat cadence |
| `-heartbeat-miss` | `MATCHMAKER_HEARTBEAT_MISS` | `3` | missed heartbeats (K) before dropping a member |
| `-locked-grace` | `MATCHMAKER_LOCKED_GRACE` | `10s` | grace after StartMatch before the lobby goes `IN_PROGRESS` |
| `-log-level` | `MATCHMAKER_LOG_LEVEL` | `info` | `debug`｜`info`｜`warn`｜`error` |
| `-tls-cert` | `MATCHMAKER_TLS_CERT` | _(unset)_ | optional cert for standalone `wss://` |
| `-tls-key` | `MATCHMAKER_TLS_KEY` | _(unset)_ | optional key for standalone `wss://` |
| `-max-conns` | `MATCHMAKER_MAX_CONNS` | `2000` | concurrent WebSocket connections (`<0` disables) |
| `-max-conns-per-ip` | `MATCHMAKER_MAX_CONNS_PER_IP` | `16` | concurrent connections from one client IP (`<0` disables) |
| `-max-lobbies` | `MATCHMAKER_MAX_LOBBIES` | `5000` | live lobbies (`<0` disables) |
| `-conn-idle-timeout` | `MATCHMAKER_CONN_IDLE_TIMEOUT` | `120s` | close a connection holding no seat that has said nothing (`<0` never) |
| `-client-ip-header` | `MATCHMAKER_CLIENT_IP_HEADER` | _(unset)_ | trusted edge header carrying the real client IP |

A member that misses `K` heartbeats (`interval × miss`, default 30 s) is dropped
with a RosterUpdate; a drained lobby is evicted and its code freed.

**`-client-ip-header` matters behind a proxy.** Fly, Render, Cloudflare and any
reverse proxy make every WebSocket arrive from a *private* address, so the per-IP
caps see one source for the whole world and are deliberately switched off. Point
this flag at a header the proxy **overwrites** — `Fly-Client-IP`,
`CF-Connecting-IP`, an nginx `proxy_set_header X-Real-IP` — and they come back to
life. Never point it at a raw `X-Forwarded-For` chain: the leftmost entry of an
appended chain is attacker-controlled. The header is consulted only when the
direct peer is itself loopback/private, so a directly connected client cannot use
it to forge its own source. The server warns at startup when it is unset;
`fly.toml` sets it.

The full set of protocol-visible limits (frame sizes, field ceilings, rates,
capacity) is [`PROTOCOL.md` §8](./PROTOCOL.md); the reasoning behind each is in
[`SECURITY.md`](./SECURITY.md).

## TLS

Two options — pick one:

1. **Terminate at the edge (recommended).** Fly.io / Render provide HTTPS/WSS at
   their proxy and forward plain `ws://` to the container. Leave `-tls-cert`/
   `-tls-key` unset; clients still connect over `wss://`.
2. **Standalone `wss://`.** Pass `-tls-cert`/`-tls-key` (or the env vars) and the
   binary serves TLS itself. Useful for self-hosting / LAN with your own cert.

## Deploy

### Docker

```sh
docker build -t ob-matchmaker services/matchmaker
docker run -p 8080:8080 -p 8081:8081/udp -p 8082:8082/udp \
  -e MATCHMAKER_RELAY_ADVERTISE=relay.example:8082 ob-matchmaker
```

Multi-stage build → a static `CGO_ENABLED=0` binary on a distroless nonroot base
(tiny image, no shell). TCP 8080 (WS) + UDP 8081 (STUN) + UDP 8082 (relay)
exposed. Set `MATCHMAKER_RELAY_ADVERTISE` to the **publicly reachable**
`host:port`; without it the server hands clients its wildcard listen address and
warns at startup.

### Fly.io (free tier)

See [`fly.toml`](./fly.toml). TLS is terminated at Fly's edge (`force_https`).
UDP needs a dedicated IPv4 and binding the Fly address:

```sh
fly launch --no-deploy         # uses fly.toml
fly ips allocate-v4            # required for the UDP STUN + relay listeners
fly deploy
# → wss://<app>.fly.dev/ws  and  udp <app> :8081 (STUN) :8082 (relay)
```

The manifest sets `MATCHMAKER_STUN_ADDR=fly-global-services:8081` and
`MATCHMAKER_RELAY_ADDR=fly-global-services:8082` so both listeners observe real
client addresses, and `MATCHMAKER_RELAY_ADVERTISE=<app>.fly.dev:8082` so clients
get a dialable relay — **edit that hostname to your app's** before deploying.
Mind the egress: see "Relay: bandwidth & cost" above before opening a public
relay on a free tier.

### Render

Deploy the Dockerfile as a **Web Service** (Render terminates TLS at its edge →
clients use `wss://<svc>.onrender.com/ws`). Render's HTTP services do not expose
arbitrary UDP; STUN and the relay want a host with public UDP ports (Fly, or a
plain VPS). The control plane alone is Render-friendly.

## Relay: bandwidth & cost

The UDP relay (`:8082`) is the TURN-like fallback for peers whose hole-punch
fails (symmetric NAT / CGNAT). It is a **dumb forwarder**: it reads
`[16B alloc_id][1B seat][opaque payload]`, learns the sender's public address
from the datagram source, and re-addresses the payload at the destination seat's
learned address. It never decodes the payload, and adds no reliability, ordering
or rate shaping — the game's netcode is loss-tolerant by design.

**This is the expensive component** (ADR-0011 Risks). A relayed match routes
*all* per-tick traffic through the server for the match's whole duration, and
the server pays for it **twice** — once inbound, once outbound:

```
bytes/s ≈ 2 × seats × tick_rate × (17 + payload + 28)
                                   ↑header  ↑IP+UDP overhead
```

At the sim's 20 Hz, a 2-seat relayed match with ~50-byte payloads is roughly
**7 KB/s (~58 kbit/s) counting both directions** — about 2 MB per 5-minute
match. 100 concurrent relayed 2-seat matches ≈ **6 Mbit/s sustained**. Budget
against your host's egress allowance before advertising a public relay; direct
P2P (the common case) costs the server nothing.

Two caps bound the damage; **both log what they drop** — nothing is truncated:

| cap | value | behaviour |
|---|---|---|
| idle allocation expiry | `-relay-idle` (default `60s`) | an allocation with no traffic **from that seat** is freed, so the table cannot grow unbounded when a client vanishes without a clean disconnect. Logged as `relay allocations expired (idle)`. |
| max datagram | 2048 bytes (`kRelayMaxDatagram`) | oversized datagrams are **dropped**, never truncated, and reported at WARN (`relay dropped oversized datagrams (not truncated)`). |

Allocations are also freed on member disconnect, heartbeat timeout, and lobby
eviction.

Drop reasons (short datagram, unknown `alloc_id`, unknown destination seat,
destination address not learned yet, write error, oversize) are **counted, not
logged per datagram** — untrusted input must not be able to flood the log. One
aggregated `relay stats` line is emitted every 10 s when a tally moves.

There is deliberately **no per-allocation rate limit**: shaping a lockstep game
stream would create the desync the whole design avoids. If you need to protect a
host, cap it outside (firewall / provider quota) rather than inside the
forwarder.

## Security / ops notes

**Read [`SECURITY.md`](./SECURITY.md) first** — it is the full review: what was
checked, what was found, what was fixed and what was accepted, with the residual
risk of each spelled out. The short version:

- **The deployed control plane is in the clear**, because the client cannot yet
  speak `wss://` (`fly.toml`, `force_https = false`). That means the `host_token`
  and the lobby `code` are readable by anyone on the path. This is the most
  serious open issue and its fix lives on the client side. SECURITY.md S1.
- **No game data, no PII beyond a chosen display name.** State is soft, in-RAM,
  and evicted on disconnect/timeout — nothing is persisted.
- **Authn is capability-based:** knowing a 6-char lobby `code` lets you join;
  the opaque `host_token` (128-bit, `crypto/rand`) authorises `StartMatch`.
  Codes and handles are unguessable and never sequential, and a failed
  `JoinByCode` is rate-limited per connection *and* per source address so the
  code space cannot be searched.
- **`build_hash` is the loud cross-platform door** (ADR-0011): a mismatched sim
  build is rejected at join, before anyone waits. The P2P `Hello` re-checks it
  at tick 0 as defence in depth.
- **Origin is not checked** on the WebSocket upgrade — native clients send none,
  and authn is by code/token, not Origin. Put the service behind the edge TLS
  proxy; do not expose the plain `:8080` port publicly if you can avoid it.
- **DoS surface is bounded in the service, not delegated to a proxy.**
  Connections, lobbies, per-connection state, request rates, frame sizes and
  field sizes all have ceilings (PROTOCOL.md §8); UDP-side budgets use a
  fixed-size table so a forged-source flood cannot make the server allocate.
  Slow WebSocket consumers are dropped rather than blocking the server.
- **The relay pins each seat's address.** A datagram carrying a seat's
  `alloc_id` from any other source is dropped while the real peer is still
  sending, so an observer who reads a header off the wire cannot steal a seat's
  return path. SECURITY.md F1 records exactly what that does and does not close.
- **Refusals are counted, not logged per event.** One periodic line each for
  `control-plane drops`, `relay stats` and `stun stats` — a log line per hostile
  packet is itself an amplifier. `rebind_refused` and `join_guess` are the two
  counters worth alerting on.
- **Not tamper-proof.** P2P determinism has no referee (ADR-0011 Risks):
  `state_hash` catches a diverging build, not an honest-but-cheating peer.

## Layout

| file | role |
|---|---|
| `main.go` | config wiring, signal-driven graceful shutdown |
| `config.go` | flags + env, slog logger |
| `protocol.go` | wire message types, envelope, `build_hash`/`roster_digest` helpers |
| `code.go` | Crockford base-32 lobby codes + opaque handles (`crypto/rand`) |
| `manager.go` | lobby state machine, all control-plane handlers, heartbeat reaper |
| `ratelimit.go` | token buckets, the fixed-size per-source table, client-IP resolution |
| `wsserver.go` | HTTP/WebSocket adapter (`coder/websocket`) → the Manager, connection admission |
| `stun.go` | UDP STUN reflexive-address echo |
| `relay.go` | UDP relay forwarder: allocation table + opaque datagram forwarding |
| `*_test.go` | code, manager, STUN, relay, rate-limit, security and in-process WebSocket tests |
