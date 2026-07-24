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

# SeedHandshake: the pre-match seed exchange (ADR-0010 §3.3 step 5) over both a
# headless LoopbackLink and real UDP on localhost (soft-skips without sockets).
bomber_add_test(bomber_net_handshake_tests net_handshake test_handshake.cpp)
target_link_libraries(bomber_net_handshake_tests PRIVATE bomber::net)

# build_hash: the cross-build compatibility digest (ADR-0011) two online peers
# compare before a match — deterministic, stable, and non-degenerate.
bomber_add_test(bomber_net_build_hash_tests net_build_hash test_build_hash.cpp)
target_link_libraries(bomber_net_build_hash_tests PRIVATE bomber::net)
