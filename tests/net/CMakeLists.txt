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
