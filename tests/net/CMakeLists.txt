# bomber::net suites (ADR-0010): the per-tick input codec and the loopback
# lockstep harness (two Simulations seeded from one config, inputs exchanged
# only through the wire codec, asserted hash-identical every tick). Uses
# bomber_add_test (it drives a Simulation and shares helpers.hpp) plus the net
# target under test.
bomber_add_test(bomber_net_tests net_loopback test_loopback_lockstep.cpp)
target_link_libraries(bomber_net_tests PRIVATE bomber::net)

# LockstepSession: input-delay lockstep + hash-based desync detection over a
# latent LoopbackLink (two peers stay in perfect sync; a forced divergence is
# caught).
bomber_add_test(bomber_net_session_tests net_session test_lockstep_session.cpp)
target_link_libraries(bomber_net_session_tests PRIVATE bomber::net)

# UdpTransport: a localhost datagram round-trip and the full lockstep stack over
# two real UDP sockets (soft-skips if the sandbox forbids sockets).
bomber_add_test(bomber_net_udp_tests net_udp test_udp_transport.cpp)
target_link_libraries(bomber_net_udp_tests PRIVATE bomber::net)

# RollbackSession: GGPO-style predict + snapshot + re-sim; confirmed states stay
# in agreement under latency, and a real divergence is still caught.
bomber_add_test(bomber_net_rollback_tests net_rollback test_rollback_session.cpp)
target_link_libraries(bomber_net_rollback_tests PRIVATE bomber::net)

# Multi-round rotation: the derived per-round seed + tick base an online
# best-of-N match agrees on with no extra traffic (round_rotation.hpp), and the
# RollbackSession tick base that keeps a straggler from the previous round out of
# the next one's tick space.
bomber_add_test(bomber_net_round_rotation_tests net_round_rotation test_round_rotation.cpp)
target_link_libraries(bomber_net_round_rotation_tests PRIVATE bomber::net)

# Peer drop -> AI handoff (ADR-0011 Risks): a silent peer's seat is handed to the
# deterministic AISystem at one agreed tick on every peer (surviving rollback),
# or — with Options row 12 off — ends the match loudly instead of hanging.
bomber_add_test(bomber_net_peer_drop_tests net_peer_drop test_peer_drop.cpp)
target_link_libraries(bomber_net_peer_drop_tests PRIVATE bomber::net)

# SeedHandshake: the pre-match seed exchange (ADR-0010 §3.3 step 5) over both a
# headless LoopbackLink and real UDP on localhost (soft-skips without sockets).
bomber_add_test(bomber_net_handshake_tests net_handshake test_handshake.cpp)
target_link_libraries(bomber_net_handshake_tests PRIVATE bomber::net)

# build_hash: the cross-build compatibility digest (ADR-0011) two online peers
# compare before a match — deterministic, stable, and non-degenerate.
bomber_add_test(bomber_net_build_hash_tests net_build_hash test_build_hash.cpp)
target_link_libraries(bomber_net_build_hash_tests PRIVATE bomber::net)

# Rendezvous: two peers hole-punch a direct UDP path over localhost, then carry a
# datagram over it (ADR-0011 §3). Soft-skips without sockets.
bomber_add_test(bomber_net_rendezvous_tests net_rendezvous test_rendezvous.cpp)
target_link_libraries(bomber_net_rendezvous_tests PRIVATE bomber::net)

# MatchConfig wire codec: the full-field round-trip that guards the determinism
# boundary (a forgotten field = a desync), plus the malformed-input rejections.
bomber_add_test(bomber_net_match_config_tests net_match_config test_match_config_codec.cpp)
target_link_libraries(bomber_net_match_config_tests PRIVATE bomber::net)

# SetupSession: the host-authoritative setup sync (docs/re/network-screens.md
# §7) over a LoopbackLink — live preview, chunked final config, ack, loss
# tolerance, and the never-partially-applied guarantee.
bomber_add_test(bomber_net_setup_tests net_setup test_setup_session.cpp)
target_link_libraries(bomber_net_setup_tests PRIVATE bomber::net)

# StarHubTransport: the >2-seat host-relay star — fan-out, guest->guest
# reflection, and the stranger-drop guard.
bomber_add_test(bomber_net_star_tests net_star test_star_hub.cpp)
target_link_libraries(bomber_net_star_tests PRIVATE bomber::net)

# RelayedTransport: the TURN-like fallback's client half — the frozen §6 header
# layout, a round-trip through a fake forwarder, and the drop paths.
bomber_add_test(bomber_net_relay_tests net_relay test_relayed_transport.cpp)
target_link_libraries(bomber_net_relay_tests PRIVATE bomber::net)

# Lobby control-plane protocol: the C++ encoders/parser vs the FROZEN
# services/matchmaker/PROTOCOL.md. Needs nlohmann/json, so it is built only under
# BOMBER_ENABLE_LOBBY (the GUI presets) — pure JSON, no sockets.
if(BOMBER_ENABLE_LOBBY)
  bomber_add_test(bomber_net_lobby_protocol_tests net_lobby_protocol test_lobby_protocol.cpp)
  target_link_libraries(bomber_net_lobby_protocol_tests PRIVATE bomber::net)

  # StunClient: reflexive-address discovery against a fake echo server built from
  # a second UdpTransport (no Go binary needed). Soft-skips without sockets.
  bomber_add_test(bomber_net_stun_tests net_stun test_stun_client.cpp)
  target_link_libraries(bomber_net_stun_tests PRIVATE bomber::net)

  # LobbyFlow: the pre-match state machine driven by synthetic server frames,
  # ending with two flows punching each other for real over localhost.
  bomber_add_test(bomber_net_lobby_flow_tests net_lobby_flow test_lobby_flow.cpp)
  target_link_libraries(bomber_net_lobby_flow_tests PRIVATE bomber::net)

  # LIVE interop against the real Go matchmaker — the only check that proves both
  # independently-written sides of PROTOCOL.md agree on the wire. OPT-IN: skips
  # unless BOMBER_MATCHMAKER_URL points at a running server, so the pre-push gate
  # is unaffected.
  bomber_add_test(bomber_net_lobby_live_tests net_lobby_live test_lobby_live.cpp)
  target_link_libraries(bomber_net_lobby_live_tests PRIVATE bomber::net)
endif()
