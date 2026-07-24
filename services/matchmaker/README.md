# Open Bomberman matchmaker

The online-multiplayer **signaling / lobby control plane + STUN echo** for Open
Bomberman (ADR-0011, design `docs/online-multiplayer-design.md`). One small,
self-contained Go service. It is **Phase 1a**: lobby brokering and reflexive
address discovery only — the UDP relay forwarder (Phase 2) is stubbed
(`relay.go`).

**Hard invariant:** the server **never simulates and never sees game `State`**.
It holds only soft in-RAM lobby state (`code → {roster, candidates, build_hash,
…}`) and echoes UDP source addresses. No gameplay logic, no persistence, no game
data ever touches it. All match authority is the deterministic P2P sim on the
peers.

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
| `-heartbeat-interval` | `MATCHMAKER_HEARTBEAT_INTERVAL` | `10s` | expected client heartbeat cadence |
| `-heartbeat-miss` | `MATCHMAKER_HEARTBEAT_MISS` | `3` | missed heartbeats (K) before dropping a member |
| `-locked-grace` | `MATCHMAKER_LOCKED_GRACE` | `10s` | grace after StartMatch before the lobby goes `IN_PROGRESS` |
| `-log-level` | `MATCHMAKER_LOG_LEVEL` | `info` | `debug`｜`info`｜`warn`｜`error` |
| `-tls-cert` | `MATCHMAKER_TLS_CERT` | _(unset)_ | optional cert for standalone `wss://` |
| `-tls-key` | `MATCHMAKER_TLS_KEY` | _(unset)_ | optional key for standalone `wss://` |

A member that misses `K` heartbeats (`interval × miss`, default 30 s) is dropped
with a RosterUpdate; a drained lobby is evicted and its code freed.

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
docker run -p 8080:8080 -p 8081:8081/udp ob-matchmaker
```

Multi-stage build → a static `CGO_ENABLED=0` binary on a distroless nonroot base
(tiny image, no shell). TCP 8080 (WS) + UDP 8081 (STUN) exposed.

### Fly.io (free tier)

See [`fly.toml`](./fly.toml). TLS is terminated at Fly's edge (`force_https`).
UDP needs a dedicated IPv4 and binding the Fly address:

```sh
fly launch --no-deploy         # uses fly.toml
fly ips allocate-v4            # required for the UDP STUN listener
fly deploy
# → wss://<app>.fly.dev/ws  and  udp <app> :8081
```

The manifest sets `MATCHMAKER_STUN_ADDR=fly-global-services:8081` so the STUN
listener observes real client reflexive addresses.

### Render

Deploy the Dockerfile as a **Web Service** (Render terminates TLS at its edge →
clients use `wss://<svc>.onrender.com/ws`). Render's HTTP services do not expose
arbitrary UDP; STUN/relay want a host with a public UDP port (Fly, or a plain
VPS). The control plane alone is Render-friendly.

## Security / ops notes

- **No game data, no PII beyond a chosen display name.** State is soft, in-RAM,
  and evicted on disconnect/timeout — nothing is persisted.
- **Authn is capability-based:** knowing a 6-char lobby `code` lets you join;
  the opaque `host_token` (128-bit, `crypto/rand`) authorises `StartMatch`.
  Codes and handles are unguessable and never sequential.
- **`build_hash` is the loud cross-platform door** (ADR-0011): a mismatched sim
  build is rejected at join, before anyone waits. The P2P `Hello` re-checks it
  at tick 0 as defence in depth.
- **Origin is not checked** on the WebSocket upgrade — native clients send none,
  and authn is by code/token, not Origin. Put the service behind the edge TLS
  proxy; do not expose the plain `:8080` port publicly if you can avoid it.
- **DoS surface:** lobby maps are bounded by live connections and reaped on
  timeout; slow WebSocket consumers are dropped rather than blocking the server.
  Rate-limiting / connection caps are a reverse-proxy concern (out of scope here).
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
| `wsserver.go` | HTTP/WebSocket adapter (`coder/websocket`) → the Manager |
| `stun.go` | UDP STUN reflexive-address echo |
| `relay.go` | Phase 2 relay drop-in seam (stub) |
| `*_test.go` | code, manager, STUN, and in-process WebSocket tests |
