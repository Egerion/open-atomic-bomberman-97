# ADR-0010 — Netplay: deterministic lockstep over UDP

Status: accepted (in progress)
Date: 2026-07-24
Follows: ADR-0003 (deterministic fixed-timestep simulation; netplay deferred)

## Context

ADR-0003 deferred netplay but shaped the sim to accept it: integer-only, one
ordered RNG stream, a pure `Simulation::tick(const TickInputs&)`, a full
`state_hash()` digest, and value-type `State` snapshots. `docs/re/multiplayer.md`
then audited the 1997 game's own net model and reached two conclusions:

- The original is **playable single-machine on modern Windows** (it already runs
  natively), but its **networking is dead** — 1990s Winsock-IPX + null-modem +
  dial-up, none of which survives modern networks. LAN/online is the real gap.
- The original did **host-authoritative replication with desync correction**
  (guest state-packet sends, tile-sync fixups, and net-only extra RNG draws),
  *not* clean input lockstep — because it could not rely on bit-identical
  simulation. Our port **can** (`state_hash` + the golden determinism tests
  prove it), so we are better placed than the original.

## Decision

Add netplay as **deterministic input-only lockstep over UDP**, built fresh on
`tick()` + `state_hash` — NOT a port of the original's replication protocol
(treated as historical context only). A new **`libs/net`** (`bomber::net`) owns
all of it; `libs/sim` stays free of I/O (determinism rule 1) and only ever sees
a fully-assembled `TickInputs`. Dependency direction: `apps/game → libs/net →
libs/sim`.

Hard rules (from `docs/re/multiplayer.md` §3.2):

- Netplay lives **only on the fixed-20 Hz `tick()` path**, never on `frame()`
  (the F9 native-cadence live path, ADR-0007, is wall-clock-driven and
  explicitly non-deterministic — two peers on `frame()` desync on the first
  differing `delta_ms`).
- Peers must run **bit-identical sim builds**; a version/hash handshake at
  session start is mandatory. Any added/removed/reordered RNG draw changes
  `state_hash`, which now has to agree *across machines*, not just against a
  golden file.
- Only `State` (hence `state_hash`) must agree. Presentation-side RNG (sound
  picks, death-anim, disease flash, interpolation) may diverge per peer
  (rule 6) without desyncing.

Wire model: each tick a peer sends only the seat(s) it OWNS as a compact
`(tick_index, seat_mask, packed-inputs)` frame (a seat = 6 bits); the receiver
merges remote seats into its locally-assembled `TickInputs` before `tick()`.
`Simulation::hash()` is exchanged (every tick, or every N to save bandwidth) as
the desync check — the golden infra already guarantees identical
seed+inputs+build ⇒ identical hash, so the digest *is* the on-wire integrity
check with no new machinery.

## Sequencing (multiplayer.md §3.3)

1. **Input codec + loopback lockstep harness** (this increment): `bomber::net`
   `input_codec` (pack/serialize/deserialize `TickInputs`) + a two-`Simulation`
   loopback test that exchanges inputs only through the codec and asserts
   `hash()` equality every tick — the golden determinism property promoted from
   "same seed+inputs" to "same seed + wire-exchanged inputs". No sockets yet.
2. UDP transport behind `bomber::net` (SDL3_net or raw sockets): sequenced,
   resend-unacked input frames. Still no sim coupling.
3. Input-delay lockstep for real 2-player matches; a `state_hash` mismatch
   surfaces as a visible desync error.
4. Rollback (GGPO-style) on top — the port already has the two hard parts, cheap
   value-type snapshots and a fast pure `tick()`, plus `state_hash` to validate.
5. Lobby/session UI: revive the RE'd host(=1)/guest(=2) roles and the 10-slot
   roster (remote seats map onto the original's input-type-4 slot); retire the
   `NETWORK.BM`-only stub.

## Consequences

- A clean seam: `libs/net` is testable headless (no sockets in increment 1), and
  the loopback harness runs under the same `headless` preset + pre-push gate as
  the sim, so the lockstep invariant is guarded like the goldens are.
- The determinism contract's stakes rise from "hold across a golden file" to
  "hold across machines"; the build-hash handshake (step 2+) is the guard.
- No change to `libs/sim`, `State`, or any golden hash — this ADR adds a new
  peer library and a new test suite, nothing in the hashed path moves.
