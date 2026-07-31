# bomber::input — keyboard, gamepad and the DOS-scancode bridge.
#
# The one package whose PUBLIC HEADERS are deliberately SDL-free while its
# sources are not. input.hpp stores bindings as plain `int` and dos_scancode.hpp
# is a pure table, precisely so the headless suites can pin the binding model
# (tests/game's `frontend` suite drives cycle_slot_input_type and the DOS
# translation without linking SDL3); only the three .cpp files, which read
# SDL_GetKeyboardState and name SDL_SCANCODE_* constants, need the library.
#
# That split is why this target is SDL-gated but its include/ directory is
# reachable on its own: a headless consumer adds the include path and gets the
# header-only half, and there is nothing to link because there is nothing
# SDL-free to compile. Do not add an SDL-free .cpp here — put it in
# bomber::game_util, where the pre-push gate will actually build it.
add_library(bomber_input STATIC
  src/input.cpp
  src/dos_scancode.cpp
  src/gamepad.cpp
)
add_library(bomber::input ALIAS bomber_input)
target_include_directories(bomber_input PUBLIC include)
target_compile_features(bomber_input PUBLIC cxx_std_20)
target_link_libraries(bomber_input
  PUBLIC bomber::sim SDL3::SDL3
  PRIVATE bomber::warnings
)
