# bomber::net suites (ADR-0010): the per-tick input codec and the loopback
# lockstep harness (two Simulations seeded from one config, inputs exchanged
# only through the wire codec, asserted hash-identical every tick). Uses
# bomber_add_test (it drives a Simulation and shares helpers.hpp) plus the net
# target under test.
bomber_add_test(bomber_net_tests net_loopback test_loopback_lockstep.cpp)
target_link_libraries(bomber_net_tests PRIVATE bomber::net)
