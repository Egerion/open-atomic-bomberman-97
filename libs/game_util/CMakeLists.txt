# bomber::game_util — the SDL-FREE floor of the presentation stack.
#
# Everything here is a PURE MODEL: the front-end state machine, the pixel and
# pacing rules the renderer applies, the match/results bookkeeping, the editor's
# grid, and the log seam. No SDL, so EVERY preset builds it — including
# `headless`, which is what scripts/test.sh runs in the pre-push hook. That is
# the whole point of the target: CLAUDE.md's "Build & test" lists three separate
# occasions on which code that only compiled under the SDL presets rotted
# unnoticed, and the fix each time was to move the SDL-free half below the SDL
# line rather than to remember harder.
#
# The rule for adding a file is mechanical: if it includes SDL3 (directly or
# through bomber/render/sdl.hpp) it does not belong, and the headless preset will
# say so on the next build.
#
# Nothing here may include another libs/* PRESENTATION header — this is the
# bottom of that graph.
add_library(bomber_game_util STATIC
  # The package's diagnostic seam (log.hpp, docs/coding-standards.md §11).
  src/log.cpp
  # The scheme editor's pure grid model — libs/editor renders it, but every rule
  # about what a legal grid IS lives here.
  src/editor_grid.cpp
)
add_library(bomber::game_util ALIAS bomber_game_util)
target_include_directories(bomber_game_util PUBLIC include)
target_compile_features(bomber_game_util PUBLIC cxx_std_20)
# All three are PUBLIC because PUBLIC HEADERS include them, which is a usage
# requirement CMake can only express per target:
#   bomber::sim     editor_grid/results/match_outcome/goldman_wheel/carry_pose
#   bomber::assets  editor_grid (sch), match_outcome (reslist), credits_addendum
#   bomber::net     net_tally.hpp ALONE — it takes a net::RollbackSession&
# The net edge is therefore the one worth watching: it is a single 20-line header
# that puts an IXWebSocket/mbedTLS-linked target in every consumer's link line,
# and it is why tests/game's suites reach these headers through a raw include dir
# instead of linking this target. Moving net_tally.hpp anywhere else would take
# it OUT of the headless gate, which is worse; see the note in that header.
target_link_libraries(bomber_game_util
  PUBLIC bomber::assets bomber::sim bomber::net
  PRIVATE bomber::warnings
)
