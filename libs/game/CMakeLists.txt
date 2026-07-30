# bomber::game — the SDL3 presentation layer: asset store (textures), the
# renderer, audio, input mapping, and the application shell. Reads the sim
# state and events; never mutates them.

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
  src/editor_grid.cpp
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
  PUBLIC bomber::assets bomber::sim bomber::match bomber::audio SDL3::SDL3
  # bomber::net: the netplay engine (libs/net) for GameApp::run_netplay +
  # MatchRunner's lockstep drive. SDL-free, already headless-built; PRIVATE
  # because no public bomber::game header exposes a net type by value (the
  # MatchRunnerState seam holds only a forward-declared LockstepSession*). On a
  # static lib the dep still propagates to apps/game's link line (incl. ws2_32).
  PRIVATE bomber::warnings bomber::platform bomber::net
)
