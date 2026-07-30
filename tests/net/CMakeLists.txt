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

# Rollback PACING on independent wall clocks — the standing-lag regression. Every
# other session suite pumps both peers inside one synchronous step(), which cannot
# express a per-machine wall-clock skew, a per-direction delay, or the frame loop
# the pump is nested inside; a live match pinned at the prediction cap for a whole
# round was invisible to all of them.
bomber_add_test(bomber_net_pacing_tests net_pacing test_rollback_pacing.cpp)
target_link_libraries(bomber_net_pacing_tests PRIVATE bomber::net)

# The per-match KILL TALLY under rollback: an accumulator in the presentation
# layer fed by State::events, which replays. Two peers with different prediction
# histories reached different totals — and so different win-by-kills verdicts —
# from a simulation that never desynced, and no hash-based guard could see it
# because events are deliberately outside state_hash. Drives both peers over the
# shared ms_clock_harness rig and calls the REAL libs/game wiring
# (net_tally.hpp — header-only and SDL-free, hence the game include dir), so the
# suite fails if either half of the fix is reverted.
bomber_add_test(bomber_net_kill_tally_tests net_kill_tally test_kill_tally.cpp)
target_link_libraries(bomber_net_kill_tally_tests PRIVATE bomber::net)
target_include_directories(bomber_net_kill_tally_tests PRIVATE ${CMAKE_SOURCE_DIR}/libs/game/include)

# In-match netplay DIAGNOSTICS (net_stats.hpp): the ack-RTT derived from the
# confirmed frontier a peer's InputRange already carries (no new wire message,
# so no wire-version bump), the rollback/stall/lag counters, the loss ESTIMATE,
# and the one-line session-end log record. Both peers are driven over a
# LoopbackLink, and the clocked run is asserted hash-identical to the unclocked
# one — instrumentation must not perturb what it measures.
bomber_add_test(bomber_net_stats_tests net_stats test_net_stats.cpp)
target_link_libraries(bomber_net_stats_tests PRIVATE bomber::net)

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

bomber_add_test(bomber_net_host_migration_tests net_host_migration test_host_migration.cpp)
target_link_libraries(bomber_net_host_migration_tests PRIVATE bomber::net)

# Match shell (wire v8): the host-authoritative round ABANDON (Esc -> one agreed
# end tick -> draw, instead of a local freeze that dropped the connection) and
# the post-match REMATCH rendezvous that hands the same transport back to the
# setup screens. Both peers are driven over a LoopbackLink — a one-sided test is
# what let the previous connection bug through.
bomber_add_test(bomber_net_match_shell_tests net_match_shell test_match_shell.cpp)
target_link_libraries(bomber_net_match_shell_tests PRIVATE bomber::net)

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

# LinkProbe: the mutual path verification between the punch (or a relay
# allocation) and Ready — both directions proven, a dead path expiring, and the
# punch echo that stops a winner starving a peer that is still punching.
bomber_add_test(bomber_net_link_probe_tests net_link_probe test_link_probe.cpp)
target_link_libraries(bomber_net_link_probe_tests PRIVATE bomber::net)

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

  # TLS (SECURITY.md S1): the wss:// handshake and, just as importantly, the
  # rejection of a certificate that does not verify. The downgrade guard runs
  # offline on every gate; the two network cases are opt-in (BOMBER_TLS_LIVE=1).
  bomber_add_test(bomber_net_lobby_tls_tests net_lobby_tls test_lobby_tls.cpp)
  target_link_libraries(bomber_net_lobby_tls_tests PRIVATE bomber::net)

  # LIVE interop against the real Go matchmaker — the only check that proves both
  # independently-written sides of PROTOCOL.md agree on the wire. OPT-IN: skips
  # unless BOMBER_MATCHMAKER_URL points at a running server, so the pre-push gate
  # is unaffected.
  bomber_add_test(bomber_net_lobby_live_tests net_lobby_live test_lobby_live.cpp)
  target_link_libraries(bomber_net_lobby_live_tests PRIVATE bomber::net)
endif()
