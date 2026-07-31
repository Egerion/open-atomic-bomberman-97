# bomber::editor — the scheme (.SCH) editor.
#
# The editing surface itself (editor_screen), the loop that owns it plus its
# load/save/help dialogs (editor_screen_runner), and the small per-session state
# they share. The RULES about what a legal grid is are NOT here: they live in
# bomber::game_util's editor_grid, which is SDL-free and pinned by the headless
# `editor_grid` suite. This package is the SDL surface over that model.
#
# It sits BELOW bomber::frontend, which is the direction that surprises people:
# the front end opens the editor (from the main menu and from the options
# screen), so the front end depends on it and not the other way round. The
# editor reaches only for generic chrome — dialog_chrome, the help browser, the
# filename prompt — all of which live in bomber::ui for exactly this reason.
add_library(bomber_editor STATIC
  src/editor_screen.cpp
  src/editor_screen_runner.cpp
)
add_library(bomber::editor ALIAS bomber_editor)
target_include_directories(bomber_editor PUBLIC include)
target_compile_features(bomber_editor PUBLIC cxx_std_20)
target_link_libraries(bomber_editor
  PUBLIC bomber::ui bomber::render bomber::game_util bomber::assets bomber::audio SDL3::SDL3
  PRIVATE bomber::warnings
)
