# bomber::net — netplay for the deterministic sim (docs/re/multiplayer.md,
# ADR-0010). Deterministic-lockstep peers exchange only per-tick inputs; this
# target owns the wire codec for them. SDL-free and I/O-free like the sim it
# serves — sockets (a later increment) will live behind this same target but
# never leak into libs/sim (determinism rule 1). Depends only on bomber::sim
# for the PlayerInput/TickInputs value types.

add_library(bomber_net STATIC
  src/input_codec.cpp
  src/protocol.cpp
  src/lockstep_session.cpp
)
add_library(bomber::net ALIAS bomber_net)

target_include_directories(bomber_net PUBLIC include)
target_compile_features(bomber_net PUBLIC cxx_std_20)
target_link_libraries(bomber_net PUBLIC bomber::sim PRIVATE bomber::warnings)
