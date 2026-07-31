# bomber::game — the APPLICATION SHELL, and only that.
#
# GameApp: SDL/window/renderer lifetime, the install resolve, the options.ini
# round trip, the shared services every screen borrows through a ScreenContext,
# and the AppState loop that hands control to one screen at a time. It owns
# nothing that draws; it owns who draws next.
#
# This directory used to be the whole presentation layer — 112 files, 73 public
# headers in one include/bomber/game/. It is now the top of an eight-package
# stack: game_util (SDL-free models) -> input/render -> ui -> netui -> editor ->
# frontend -> netplay -> here. Each of those is its own target with its own
# public headers, and the arrows point one way only, so a change inside one of
# them cannot silently reach sideways into another.
#
# The remaining work here is GameApp itself, which is still a single ~740-line
# class. Splitting it is the next step, not this one.
if(NOT BOMBER_BUILD_VIEWER)
  return()
endif()

add_library(bomber_game_core STATIC
  src/game_app.cpp
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
