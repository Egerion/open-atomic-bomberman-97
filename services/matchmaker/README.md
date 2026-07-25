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
- **Logging:** [`go.uber.org/zap`](https://github.com/uber-go/zap), structured,
  JSON by default (`-log-format console` for a human at a terminal).
- **Health endpoints:** [`github.com/alexliesenfeld/health`](https://github.com/alexliesenfeld/health).
- **Wire contract:** [`PROTOCOL.md`](./PROTOCOL.md) (frozen; the C++ client
  matches it byte-for-byte).

Three direct dependencies, none of them a framework. Why the usual suspects
(DI container, mock generator, a faster JSON codec) are *not* here is written
down in "Shape of the service" below, so it does not get re-argued.

## Run locally

```sh
cd services/matchmaker
go run ./cmd/matchmaker
# WebSocket control plane on ws://localhost:8080/ws
# UDP STUN echo on        udp  localhost:8081
# UDP relay forwarder on  udp  localhost:8082
# liveness                GET  http://localhost:8080/healthz
# readiness               GET  http://localhost:8080/readyz
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
| `-log-format` | `MATCHMAKER_LOG_FORMAT` | `json` | `json`｜`console` |
| `-drain-delay` | `MATCHMAKER_DRAIN_DELAY` | `0` | pause between `/readyz` going down and the listener closing |
| `-shutdown-timeout` | `MATCHMAKER_SHUTDOWN_TIMEOUT` | `10s` | bound on the whole graceful drain |
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
  `control-plane drops`, `relay stats`, `stun stats` and `ws admission` — a log
  line per hostile packet is itself an amplifier. `rebind_refused`, `join_guess`
  and `refused` are the three counters worth alerting on.
- **`/healthz` can fail now.** It used to be a constant 200. It reports the two
  UDP read loops and the lobby reaper, all restart-only failures — but that also
  means a false positive costs a machine restart and every live lobby on it.
  `fly.toml`'s check points at `/healthz`; point it at `/readyz` instead if you
  would rather nothing but a deliberate drain ever mark the machine unhealthy.
- **Not tamper-proof.** P2P determinism has no referee (ADR-0011 Risks):
  `state_hash` catches a diverging build, not an honest-but-cheating peer.

## Layout

Packages are split by **domain**, not by technical layer — the Go convention, and
the one that keeps the dependency graph readable. There is a lobby, a relay, a
STUN echo, and the wire contract they all speak. The two packages that *are*
named after a layer, `handler` and `app`, are the process edge rather than a tier
over the domain: one owns the HTTP route table, the other owns the object graph
and the shutdown order.

```
cmd/matchmaker/     main.go — the process boundary only: flags, a logger, a
                    signal-scoped context, an exit code
internal/
  config            flags + env + defaults, the zap logger
  protocol          the FROZEN wire types and the screens that decide whether an
                    inbound field may be acted on. No state, no dependencies.
  ratelimit         token bucket + the fixed-size per-source table + client-IP
                    resolution. A mechanism the other packages spend.
  lobby             the domain core: lobby state machine (state.go), every
                    control-plane handler, per-connection accounting, the
                    reapers. Transport-agnostic — it pushes frames through a
                    ClientConn seam.
  wsapi             HTTP/WebSocket adapter (coder/websocket) → the Manager, plus
                    the connection admission control in front of it. The /ws
                    handler only; it has no opinion about the route table.
  stun              the UDP reflexive-address echo, self-contained
  relay             the UDP forwarder: allocation table, address pinning, listener
  handler           the HTTP surface: /ws, /healthz (liveness), /readyz
                    (readiness), the drain switch, and a 404 for everything else.
                    Knows nothing about lobbies — probes arrive as closures.
  app               composition root + lifecycle: bind, serve, drain in order
```

Dependencies point one way only:

```
cmd/matchmaker ──► app ──► handler
                    ├────► wsapi ──► lobby ──► relay ──┐
                    ├────► stun        ├──► protocol   ├──► ratelimit
                    └───────────────────┴──► config ◄──┘
```

`lobby` owns the allocations `relay` stores, and `relay` never calls back — so
the only lock order that can occur is `Manager.mu → relay.Table.mu`. Tests live
beside the package they exercise: the address-pinning suite is in `relay`
(it needs a table, not a lobby), the admission tests are in `wsapi`, the
control-plane hardening tests are in `lobby`, the route table and probe
semantics are in `handler`, and the startup/drain order is in `app` — which runs
the whole service on ephemeral ports rather than asserting about main().

## Shape of the service

### Health, and the split that matters

`/healthz` is **liveness**: "is this process broken in a way only a restart
fixes?" It reports three components, and each one is a failure the service could
previously suffer while still answering `ok`:

| component | failure it catches |
|---|---|
| `stun_listener` | the STUN read loop has stopped (SECURITY.md F6) |
| `relay_listener` | the relay read loop has stopped — still listening on TCP, deaf on UDP |
| `lobby_reaper` | no reaper pass in six heartbeat periods: members and lobbies stop being evicted |

It reports **nothing about load**. Capacity, connection count and lobby count are
not liveness signals, and a failing liveness probe is an instruction to kill the
machine — reporting "busy" there would restart the server exactly when it is
working hardest.

`/readyz` is **readiness**: "should the edge send new connections here?" It is
`up` until the process starts draining, and that is deliberately all it is: with
one instance, a readiness failure has nowhere to shed load to.

Both bodies are `{"status":"up"}` / `{"status":"down"}` and nothing else.

### Graceful shutdown

`SIGTERM`/`SIGINT` runs one ordered drain (`app.drain`), bounded end to end by
`-shutdown-timeout`:

1. `/readyz` goes down, so the edge stops routing new connections here;
2. wait `-drain-delay` (0 by default — with a single instance the pause only
   lengthens the outage; set it to a couple of health-check intervals if you run
   more than one);
3. close the HTTP listener and let in-flight plain requests finish;
4. close the live WebSocket sessions with a real close frame. This step is
   separate because a WebSocket is a **hijacked** connection and
   `http.Server.Shutdown` neither closes nor waits for those;
5. stop the UDP listeners — each `Close` now returns only once its read loop has
   actually exited, instead of racing it.

A closed session costs a player nothing: the C++ `LobbyFlow` reconnects lazily on
the next action.

### What was deliberately NOT adopted

| suggested | verdict | reason |
|---|---|---|
| `google/wire` (DI) | **no** | the object graph is nine constructor calls in `app.New`, no cycles, no interfaces to select between. A generated injector would restate it in a second file and add codegen to the build to save nothing. |
| `gomock` | **no** | the one seam worth faking is `lobby.ClientConn`, and the existing `fakeConn` is a *recorder* — the suites assert on the content of frames the Manager sent, decoded from JSON. gomock expresses "these calls, these arguments, this order", so adopting it would mean rebuilding `fakeConn` on top of `EXPECT().Do(...)` plus a codegen step. Strictly more machinery for a strictly weaker assertion. |
| `gorilla/websocket` | **no** | `coder/websocket` is context-native, which the adapter depends on throughout (`Read(ctx)`, per-write deadlines, cancel-to-disconnect). gorilla is deadline-based and needs its own writer discipline, so the swap is a rewrite of the connection lifecycle on a **deployed** server, for zero behavioural gain. |
| `sonic` (JSON) | **no** | this is a control plane, not a data plane: a client sends a heartbeat every 5 s and a handful of one-shot frames, all under 8 KiB. `encoding/json` is not on any hot path here (the hot path is the relay, which never parses anything). Sonic would trade that for a JIT/assembly parser with narrower platform support, applied to **untrusted internet input** against a frozen protocol. |
| a `model` package | **no** | the shared value types already live in `protocol`, which for this service is not a DTO layer but the frozen contract itself. The only other entity, `lobby`, is guarded by `Manager.mu`; moving it across a package boundary would mean exporting the fields that mutex protects. |
| a `mapper` package | **no** | the mapping is `rosterOfLocked` and the `PublicLobby` row build — a few lines each, both of which read `lobby` state *while holding the lock*. Extracting them would export the internals for no gain. |
| a ~10-worker pool with drop-oldest | **no** | see below. |

### Backpressure

There is backpressure; it is just not a worker pool, and the shape it has is
deliberate.

- **Admission** caps concurrent connections, per-IP connections and lobbies
  (SECURITY.md F3), *before* the WebSocket upgrade.
- **Per-connection token buckets** cap inbound frame rate, `ListPublic`,
  `Candidates`, failed joins and chat (F4/F5).
- **Per-connection bounded egress**: a 64-slot send buffer, and a consumer that
  fills it is **disconnected rather than waited on**, so one stuck peer can never
  block the Manager (F10).

A shared pool of ~10 workers in front of `Manager.Dispatch` would be a
regression, for two independent reasons. Control-plane frames are **not
fungible** — dropping the oldest queue entry could drop a `StartMatch` or a
`JoinByCode` and leave a lobby wedged, which is a correctness bug, not shedding.
And it would not buy throughput: `Dispatch` takes one global `Manager.mu`, so the
effective concurrency is already 1 and a queue in front of it adds latency and
head-of-line blocking without removing the serialisation. If control-plane
throughput ever becomes the constraint, the fix is to shard the Manager by
lobby, not to queue in front of the same lock.
