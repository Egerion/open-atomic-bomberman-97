package main

// Phase 2 — UDP relay fallback (ADR-0011 decision 3, design §4). NOT built yet.
//
// This file is the drop-in seam: the control-plane message AllocateRelay is
// already routed (manager.handleAllocateRelay) and currently answers with an
// Error{code:"not_implemented"} so a client gets a clean signal rather than a
// silent drop. When Phase 2 lands, replace that stub with the handler below and
// stand up the forwarder.
//
// TODO(phase2): AllocateRelay{lobby_id, seat} → RelayAllocated{relay_addr, alloc_id}
//   1. Mint alloc_id (crypto/rand, like newHandle) under the lobby; store
//      {alloc_id → {seatA_addr, seatB_addr}} learned off the first relayed
//      datagram from each side (same peer-learning as UdpTransport::poll()).
//   2. Run a public-IP UDP forwarder co-located with the STUN listener: read
//      [alloc_id][opaque libs/net datagram], look up alloc_id, forward the
//      OPAQUE payload to the far seat's learned address. The relay NEVER decodes
//      an Input/Hash frame — it forwards opaque bytes, so it still "never
//      simulates" (ADR-0011 hard rule).
//   3. Reap allocations with the lobby (evict on empty/timeout).
//
// The client side (RelayedTransport) is a plain Transport swap decided by the
// Rendezvous outcome (design §4); the RollbackSession above it is unchanged.
// Frozen message shapes live in PROTOCOL.md under "Phase 2 TODO".

const relayNotImplemented = "relay allocation is Phase 2 (not implemented)"
