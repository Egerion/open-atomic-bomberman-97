# Provides the `ixwebsocket` target for the online lobby CONTROL plane
# (ADR-0011 / docs/online-multiplayer-design.md §1): a small WebSocket client
# used only to talk to the matchmaking server before a match. Pulled in via
# FetchContent the same way SDL3 is (cmake/BomberSDL3.cmake), and ONLY when
# BOMBER_ENABLE_LOBBY is ON — the headless CI gate builds libs/net without it, so
# the pure deterministic netcode (codec / sessions / transport) stays
# dependency-light.
include(FetchContent)

# --- TLS (wss://) is DEFERRED for v1 ----------------------------------------
# IXWebSocket v11.4.6's mbedTLS backend (IXSocketMbedTLS.cpp) does not compile
# against ANY single mbedTLS release: it calls psa_crypto_init() (needs 3.6+) yet
# also calls mbedtls_pk_parse_keyfile() with the 2.x 3-arg signature (3.x needs
# 5). So there is no mbedTLS version that satisfies both sites.
#
# v1 therefore runs the lobby over ws:// (plain WebSocket). This leaks nothing
# sensitive — the signaling carries lobby codes, chosen display names, and the
# candidate IP:port pairs the peers exchange with each other anyway; no
# passwords, no game State. It deploys fine behind a raw-TCP endpoint
# (Fly.io/Render). wss:// is a tracked hardening follow-up: pair a newer
# IXWebSocket commit with mbedTLS 3.6, add an OpenSSL backend, or terminate TLS
# at an edge proxy. Flip BOMBER_LOBBY_TLS once that pairing is sorted.
option(BOMBER_LOBBY_TLS "Build the lobby client with wss:// TLS (unstable — see cmake/BomberIXWebSocket.cmake)" OFF)

if(BOMBER_LOBBY_TLS)
  # mbedTLS 2.28's CMakeLists declares a pre-3.5 cmake_minimum_required, which
  # CMake 4.x rejects; this documented escape hatch relaxes the floor only for
  # the fetched subprojects.
  set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
  set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
  set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(mbedtls
    GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls.git
    GIT_TAG v3.6.2
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(mbedtls)
  set(MBEDTLS_INCLUDE_DIRS "${mbedtls_SOURCE_DIR}/include" CACHE PATH "" FORCE)
  set(MBEDTLS_LIBRARY    mbedtls    CACHE FILEPATH "" FORCE)
  set(MBEDX509_LIBRARY   mbedx509   CACHE FILEPATH "" FORCE)
  set(MBEDCRYPTO_LIBRARY mbedcrypto CACHE FILEPATH "" FORCE)
  set(USE_TLS ON CACHE BOOL "" FORCE)
  set(USE_MBED_TLS ON CACHE BOOL "" FORCE)
  set(USE_OPEN_SSL OFF CACHE BOOL "" FORCE)
else()
  set(USE_TLS OFF CACHE BOOL "" FORCE)
endif()

# --- IXWebSocket ------------------------------------------------------------
# USE_WS would build the standalone `ws` CLI tool (extra deps) — off.
# permessage-deflate (USE_ZLIB) needs system zlib and buys nothing for tiny JSON
# lobby frames — off. No install rules.
set(USE_WS OFF CACHE BOOL "" FORCE)
set(USE_ZLIB OFF CACHE BOOL "" FORCE)
set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(ixwebsocket
  GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
  GIT_TAG v11.4.6
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(ixwebsocket)

# --- nlohmann/json (lobby control-plane JSON) -------------------------------
# Header-only; parses/encodes the matchmaker's JSON frames
# (services/matchmaker/PROTOCOL.md). Provides nlohmann_json::nlohmann_json.
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_Declare(nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(nlohmann_json)

# Third-party headers shouldn't face our /W4. Marking the include dirs SYSTEM
# keeps lobby_client.cpp's own diagnostics while silencing the dependency's.
foreach(_dep_target ixwebsocket mbedtls mbedx509 mbedcrypto)
  if(TARGET ${_dep_target})
    get_target_property(_incs ${_dep_target} INTERFACE_INCLUDE_DIRECTORIES)
    if(_incs)
      set_target_properties(${_dep_target} PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_incs}")
    endif()
  endif()
endforeach()
