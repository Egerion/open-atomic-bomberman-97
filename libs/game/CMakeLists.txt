# bomber::game — the SDL3 presentation layer: asset store (textures), the
# renderer, audio, input mapping, and the application shell. Reads the sim
# state and events; never mutates them.

add_library(bomber_game_core STATIC
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
