# bomber::game — the SDL3 presentation layer: asset store (textures), the
# renderer, audio, input mapping, and the application shell. Reads the sim
# state and events; never mutates them.
#
# TWO targets, split on SDL exactly like libs/audio (ADR-0008 stage 2, and
# CLAUDE.md calls that split "the model — the seam fell where the dependency
# did"):
#
#   bomber::game_util  the SDL-FREE pieces. Built by EVERY preset including
#                      headless, so they sit inside the pre-push gate, and
#                      tests/game LINKS them rather than recompiling their
#                      sources into its own binaries.
#   bomber::game       everything that includes SDL3. Gated below on
#                      BOMBER_BUILD_VIEWER.
#
# Only genuinely SDL-free sources belong in the first list. Adding one that
# includes SDL breaks the headless preset immediately, which is the intended
# feedback.
add_library(bomber_game_util STATIC
  # The package's diagnostic seam (log.hpp, docs/coding-standards.md §11).
  src/log.cpp
  # The scheme editor's pure grid model — SDL-free despite living under src/.
  src/editor_grid.cpp
)
add_library(bomber::game_util ALIAS bomber_game_util)
target_include_directories(bomber_game_util PUBLIC include)
target_compile_features(bomber_game_util PUBLIC cxx_std_20)
target_link_libraries(bomber_game_util
  PUBLIC bomber::assets bomber::sim
  PRIVATE bomber::warnings
)

# Everything past this point needs SDL3.
if(NOT BOMBER_BUILD_VIEWER)
  return()
endif()

# The credits addendum's author photograph, compiled in rather than shipped
# beside the exe — the whole point of the single-file build is that the user
# drops ONE file into their install (see cmake/EmbedBinary.cmake). It is a
# 24-bit PCX so it needs no image-decoding dependency: libs/assets' own parser
# already reads the 3-plane variant DATA_HD uses.
include(${CMAKE_SOURCE_DIR}/cmake/EmbedBinary.cmake)
bomber_embed_binary(
  ${CMAKE_SOURCE_DIR}/assets/me/me.pcx
  ${CMAKE_CURRENT_BINARY_DIR}/generated/author_photo_data.cpp
  kAuthorPhotoPcx)

add_library(bomber_game_core STATIC
  ${CMAKE_CURRENT_BINARY_DIR}/generated/author_photo_data.cpp
  src/sprites.cpp
  src/asset_store.cpp
  src/sequences.cpp
  src/renderer.cpp
  src/input.cpp
  src/dos_scancode.cpp
  src/gamepad.cpp
  src/screen.cpp
  src/bmscreen.cpp
  src/dialog_chrome.cpp
  # The lobby chat overlay (PORT-ONLY, PROTOCOL.md §7). Built unconditionally:
  # without BOMBER_ENABLE_LOBBY its definitions compile to inert no-ops, so
  # callers link the same symbols on either kind of build.
  src/chat_overlay.cpp
  src/options_screen.cpp
  src/keyremap_screen.cpp
  src/goldman_screen.cpp
  src/editor_screen.cpp
  src/campaign_screen.cpp
  src/screens/asset_screen.cpp
  src/screens/boot_screen.cpp
  src/screens/help_screens.cpp
  src/screens/debug_info_screen.cpp
  src/screens/video_settings_screen.cpp
  src/screens/scheme_filename_prompt.cpp
  src/screens/options_screens.cpp
  src/screens/editor_screen_runner.cpp
  src/screens/menu_screen.cpp
  src/screens/netplay_connect_screen.cpp
  src/screens/lobby_screen.cpp
  # The F3 in-match netplay diagnostic panel + the netdiag.log session record
  # (net_stats.hpp does the measuring; this only draws and writes).
  src/screens/net_overlay.cpp
  src/screens/net_setup_link.cpp
  # The netplay orchestration, extracted out of GameApp (ADR-0009): the entry
  # points + the setup stage + the session loop, and the best-of-N match under
  # them. Built unconditionally like the rest — only the online-lobby LEAF
  # inside the runner is BOMBER_HAS_LOBBY-guarded.
  src/screens/netplay_runner.cpp
  src/screens/netplay_match.cpp
  src/screens/map_select_screen.cpp
  src/screens/campaign_screens.cpp
  src/screens/setup_screen.cpp
  src/screens/results_screens.cpp
  src/screens/match_runner.cpp
  src/game_app.cpp
)
add_library(bomber::game ALIAS bomber_game_core)

target_include_directories(bomber_game_core PUBLIC include)
target_compile_features(bomber_game_core PUBLIC cxx_std_20)
target_link_libraries(bomber_game_core
  PUBLIC bomber::assets bomber::sim bomber::match bomber::audio bomber::game_util SDL3::SDL3
  # bomber::net: the netplay engine (libs/net) for GameApp::run_netplay +
  # MatchRunner's lockstep drive. SDL-free, already headless-built.
  #
  # PRIVATE is NOT the "no public header knows about net" claim it used to say —
  # two public headers do: net_tally.hpp includes bomber/net/rollback_session.hpp
  # and takes a net::RollbackSession&, net_setup_roster.hpp includes
  # bomber/net/protocol.hpp. Both are header-only, SDL-free seams kept public on
  # purpose so tests/game and tests/net can drive the real wiring, and both name
  # net types only BY REFERENCE, so nothing net-shaped crosses an ABI here. What
  # PRIVATE does cost is the include PATH: a consumer that links bomber::game and
  # includes either header gets no bomber::net interface directories from us. It
  # holds today only because this is a STATIC library (the link dep still reaches
  # apps/game's and apps/viewer's lines, incl. ws2_32) and because the two suites
  # that include these headers — tests/game's net_setup_roster, tests/net's
  # kill_tally — reach them through a raw include dir and link bomber::net
  # themselves. Make this PUBLIC (or move the headers into src/) if either
  # stops being true.
  PRIVATE bomber::warnings bomber::platform bomber::net
)
