# Provides the `ixwebsocket` target for the online lobby CONTROL plane
# (ADR-0011 / docs/online-multiplayer-design.md §1): a small WebSocket client
# used only to talk to the matchmaking server before a match. Pulled in via
# FetchContent the same way SDL3 is (cmake/BomberSDL3.cmake), and ONLY when
# BOMBER_ENABLE_LOBBY is ON.
include(FetchContent)

# --- TLS (wss://) -----------------------------------------------------------
# ON by default: the control plane carries the lobby `host_token`, the credential
# that authorises StartMatch, so it must not cross the wire readable
# (services/matchmaker/SECURITY.md S1).
#
# The backend is mbedTLS, built from source next to IXWebSocket — the only TLS
# stack that fits this repo's constraints at once: FetchContent-only (no vcpkg,
# no system packages), statically linked so the shipped exe still has no DLLs
# beside it, and cross-platform for the linux/macos presets. OpenSSL has no
# usable FetchContent build (a perl/nasm Configure script, not CMake) and its
# Windows prebuilts are DLLs; IXWebSocket has no Schannel backend to select.
#
# The old "no mbedTLS release compiles" note was wrong, and the bug was OURS.
# IXSocketMbedTLS.cpp picks the mbedtls_pk_parse_keyfile() arity from
# IXWEBSOCKET_USE_MBED_TLS_MIN_VERSION_3, which IXWebSocket's CMake derives from
# `find_path(MBEDTLS_VERSION_GREATER_THAN_3 mbedtls/build_info.h)` — a probe for
# INSTALLED headers. A FetchContent'd mbedTLS lives in the build tree, so the
# probe found nothing, the define stayed off, and 3.6 headers got compiled
# against the 2.x signature; against 2.28 the OTHER call site (psa_crypto_init,
# 3.6+) then failed instead. Seeding that variable below — rather than patching
# a dependency — makes v11.4.6 + mbedTLS 3.6 build as its author intended.
#
# Trust anchors: on Windows IXWebSocket enumerates the CurrentUser\Root
# certificate store through wincrypt and feeds it to mbedTLS, so verification
# follows the machine's own trust decisions. mbedTLS has no system-store hook on
# Linux/macOS, so lobby_client.cpp points caFile at the platform CA bundle
# there. Peer verification and hostname checking are always on — see
# libs/net/src/lobby_client.cpp.
option(BOMBER_LOBBY_TLS "Build the lobby client with wss:// TLS (mbedTLS)" ON)

if(BOMBER_LOBBY_TLS)
  set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
  set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
  set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)   # keeps the exe DLL-free
  set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(mbedtls
    GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls.git
    GIT_TAG v3.6.2
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(mbedtls)

  # Hand IXWebSocket's FindMbedTLS.cmake its answers up front: each of its
  # find_path/find_library calls is a no-op once the matching cache entry
  # exists, so it never searches the system and never fails. The
  # MBEDTLS_VERSION_GREATER_THAN_3 line is the fix described above.
  set(MBEDTLS_INCLUDE_DIRS "${mbedtls_SOURCE_DIR}/include" CACHE PATH "" FORCE)
  set(MBEDTLS_VERSION_GREATER_THAN_3 "${mbedtls_SOURCE_DIR}/include" CACHE PATH "" FORCE)
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
