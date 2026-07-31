# bomber::input — keyboard, gamepad and the DOS-scancode bridge.
#
# The one package whose PUBLIC HEADERS are deliberately SDL-free while its
# sources are not, so the headless suites can pin the binding model (tests/game's
# `frontend` suite drives cycle_slot_input_type, the attract rolls, the team
# default and the DOS translation without linking SDL3) by adding the include
# path alone. There is nothing to link because there is nothing SDL-free left to
# compile: `demo_inputs` used to be the exception and now lives inline in
# input.hpp, inside the gate.
#
# THE TARGET CANNOT BE MADE SDL-FREE, and the split libs/audio took does not
# apply here. Measured over the three sources: gamepad.cpp makes 14 calls across
# 7 SDL entry points, input.cpp calls SDL_GetKeyboardState, and dos_scancode.cpp
# calls nothing at all — but it names 97 SDL_SCANCODE_* enumerators, and the
# headless preset fetches no SDL3 at all, not even its headers. Freeing it would
# mean hard-coding 97 numeric scancodes to dodge an include, which trades a
# compile error for a silent mistranslation the day SDL renumbers.
#
# Do not add an SDL-free .cpp here — put it in bomber::game_util, where the
# pre-push gate will actually build it.
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
