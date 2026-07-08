# bomber::game — the SDL3 presentation layer: asset store (textures), the
# renderer, audio, input mapping, and the application shell. Reads the sim
# state and events; never mutates them.

add_library(bomber_game_core STATIC
  src/sprites.cpp
  src/asset_store.cpp
  src/sequences.cpp
  src/renderer.cpp
  src/audio_engine.cpp
  src/sound_director.cpp
  src/input.cpp
  src/screen.cpp
  src/bmscreen.cpp
  src/options_screen.cpp
  src/transition.cpp
  src/game_app.cpp
)
add_library(bomber::game ALIAS bomber_game_core)

target_include_directories(bomber_game_core PUBLIC include)
target_compile_features(bomber_game_core PUBLIC cxx_std_20)
target_link_libraries(bomber_game_core
  PUBLIC bomber::assets bomber::sim bomber::match SDL3::SDL3
  PRIVATE bomber::warnings
)
