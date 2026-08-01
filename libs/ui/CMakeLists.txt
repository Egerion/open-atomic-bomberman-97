# bomber::ui — the reusable front-end chrome every screen is built out of.
#
# The generic Screen primitive (sub_42A088's full-screen image + wait policy),
# the ScreenContext bundle screens are handed, the `.BM` text-screen viewer and
# its font textures (sub_41302D), the window/dialog/list chrome (sub_43C734 and
# friends), and the two modals that are genuinely generic — the help browser and
# the filename prompt.
#
# BmScreen lives HERE rather than in bomber::render, which is a deviation worth
# stating: it looks like a renderer, but bmscreen.cpp draws through
# dialog_chrome, and dialog_chrome.hpp in turn includes bmscreen.hpp. Splitting
# the two across the render/ui line would have needed an arrow back UP from
# render into ui. They are one layer and they sit above the renderer.
#
# help_screens and scheme_filename_prompt are here for the same kind of reason.
# Both are consumed by bomber::frontend AND by bomber::editor, and the editor is
# below the front end (the menu and the options screen both open it). Leaving
# either modal in bomber::frontend would have made frontend and editor mutually
# dependent; as generic chrome they are what they always were, and the cycle
# does not exist.
add_library(bomber_ui STATIC
  src/screen.cpp
  src/bmscreen.cpp
  src/dialog_chrome.cpp
  src/help_screens.cpp
  # The ONE glob->sort->measure->layout->mouse file-list modal (sub_41404B +
  # sub_42DBCC). HelpBrowser, SchemeFilePicker and CampaignFilePicker were
  # ~70-line near-clones of it, disagreeing on the pinned sort.
  src/list_picker.cpp
  src/scheme_filename_prompt.cpp
)
add_library(bomber::ui ALIAS bomber_ui)
target_include_directories(bomber_ui PUBLIC include)
target_compile_features(bomber_ui PUBLIC cxx_std_20)
target_link_libraries(bomber_ui
  PUBLIC bomber::render bomber::input bomber::game_util bomber::assets bomber::audio SDL3::SDL3
  PRIVATE bomber::warnings
)
