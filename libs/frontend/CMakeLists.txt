# bomber::frontend — every screen the player actually sees, and the match loop.
#
# Boot and the logo/title presenter, the main menu, setup, map select, options
# (both the settings screen and the menu that opens it), key remap, the campaign
# cluster, the results tier and the Goldman wheel, the video/debug panels — and
# MatchRunner, the presentation-side driver of the deterministic sim.
#
# Twenty-odd sibling classes sharing ONE idiom: take a ScreenContext (plus a
# MatchBackdrop where the screen composites over a live match frame), own a
# nested SDL event loop, return an AppInput. That shared idiom is what makes the
# package readable as a unit despite being the largest one here.
#
# MatchRunner is in this package rather than in the app shell because it is a
# front-end screen like the others — it IS AppState::Match — and because the
# online match in bomber::netplay DRIVES it. Putting it in bomber::game (the
# shell) would have meant netplay depending on the shell that owns netplay.
#
# GOLDEN-SENSITIVE: match_runner.cpp's start_match/collect_inputs reproduce the
# sim's seeding and per-tick input assembly statement-for-statement.
# tests/test_golden.cpp and libs/net's build_hash both pin what they produce.
add_library(bomber_frontend STATIC
  src/asset_screen.cpp
  src/boot_screen.cpp
  src/menu_screen.cpp
  src/debug_info_screen.cpp
  src/video_settings_screen.cpp
  src/options_screen.cpp
  src/options_screens.cpp
  src/keyremap_screen.cpp
  src/goldman_screen.cpp
  src/campaign_screen.cpp
  src/campaign_screens.cpp
  src/map_select_screen.cpp
  src/setup_screen.cpp
  src/results_screens.cpp
  src/match_runner.cpp
)
add_library(bomber::frontend ALIAS bomber_frontend)
target_include_directories(bomber_frontend PUBLIC include)
target_compile_features(bomber_frontend PUBLIC cxx_std_20)
target_link_libraries(bomber_frontend
  PUBLIC bomber::ui bomber::netui bomber::editor bomber::render bomber::input
         bomber::game_util bomber::assets bomber::match bomber::audio bomber::net
         SDL3::SDL3
  PRIVATE bomber::warnings bomber::platform
)
