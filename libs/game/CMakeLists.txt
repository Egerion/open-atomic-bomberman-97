# bomber::game — the APPLICATION SHELL, and only that.
#
# GameApp: SDL/window/renderer lifetime, the install resolve, the shared services
# every screen borrows through a ScreenContext, and the AppState loop that hands
# control to one screen at a time. It owns nothing that draws; it owns who draws
# next.
#
# This directory used to be the whole presentation layer — 112 files, 73 public
# headers in one include/bomber/game/. It is now the top of a NINE-package
# stack: game_util (SDL-free models) -> input/render -> ui -> netui -> editor ->
# frontend -> netplay -> here. Each of those is its own target with its own
# public headers, and the arrows point one way only, so a change inside one of
# them cannot silently reach sideways into another.
#
# app_options.hpp is the only PUBLIC header besides game_app.hpp; the four units
# under src/ are private, each taking a by-reference slot bundle the shell builds
# on demand.
if(NOT BOMBER_BUILD_VIEWER)
  return()
endif()

add_library(bomber_game_core STATIC
  src/app_settings.cpp
  src/boot_loading.cpp
  src/capture_runs.cpp
  src/game_app.cpp
  src/global_hotkeys.cpp
  src/window_icon.cpp
)
add_library(bomber::game ALIAS bomber_game_core)

target_include_directories(bomber_game_core PUBLIC include)
target_compile_features(bomber_game_core PUBLIC cxx_std_20)
target_link_libraries(bomber_game_core
  PUBLIC bomber::netplay bomber::frontend bomber::editor bomber::netui bomber::ui
         bomber::render bomber::input bomber::game_util
         bomber::assets bomber::sim bomber::match bomber::audio SDL3::SDL3
  PRIVATE bomber::warnings bomber::platform bomber::net
)
