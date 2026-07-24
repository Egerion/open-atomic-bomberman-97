# bomber::net — netplay for the deterministic sim (docs/re/multiplayer.md,
# ADR-0010). Deterministic-lockstep peers exchange only per-tick inputs; this
# target owns the wire codec for them. SDL-free and I/O-free like the sim it
# serves — sockets (a later increment) will live behind this same target but
# never leak into libs/sim (determinism rule 1). Depends only on bomber::sim
# for the PlayerInput/TickInputs value types.

add_library(bomber_net STATIC
  src/input_codec.cpp
  src/protocol.cpp
  src/handshake.cpp
  src/lockstep_session.cpp
  src/rollback_session.cpp
  src/udp_transport.cpp
  src/build_hash.cpp
  src/rendezvous.cpp
)
add_library(bomber::net ALIAS bomber_net)

target_include_directories(bomber_net PUBLIC include)
target_compile_features(bomber_net PUBLIC cxx_std_20)
target_link_libraries(bomber_net PUBLIC bomber::sim PRIVATE bomber::warnings)

# UdpTransport uses raw winsock on Windows (BSD sockets need no extra lib on
# Linux/macOS). PRIVATE on a static lib still propagates to consumers' link line.
if(WIN32)
  target_link_libraries(bomber_net PRIVATE ws2_32)
endif()

# Online lobby control plane (ADR-0011): the WebSocket/TLS LobbyClient. Compiled
# only when BOMBER_ENABLE_LOBBY (the GUI presets); the headless gate skips it so
# the deterministic netcode above needs no TLS backend. ixwebsocket is provided
# by cmake/BomberIXWebSocket.cmake, included at the top level before this dir.
if(BOMBER_ENABLE_LOBBY)
  target_sources(bomber_net PRIVATE src/lobby_client.cpp src/lobby_messages.cpp)
  target_link_libraries(bomber_net PUBLIC ixwebsocket nlohmann_json::nlohmann_json)
  target_compile_definitions(bomber_net PUBLIC BOMBER_HAS_LOBBY=1)
endif()
