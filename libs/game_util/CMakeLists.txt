# bomber::game_util — the SDL-FREE floor of the presentation stack.
#
# Everything here is a PURE MODEL: the front-end state machine, the pixel and
# pacing rules the renderer applies, the match/results bookkeeping, the editor's
# grid, and the log seam. No SDL, so EVERY preset builds it — including
# `headless`, which is what scripts/test.sh runs in the pre-push hook. That is
# the whole point of the target's existence: CLAUDE.md's "Build & test" lists
# three separate occasions on which code that only compiled under the SDL
# presets rotted unnoticed, and the fix each time was to move the SDL-free half
# below the SDL line rather than to remember harder.
#
# The rule for adding a file here is mechanical: if it includes SDL3 (directly
# or through bomber/render/sdl.hpp) it does not belong, and the headless preset
# will say so on the next build. That is the intended feedback, not an obstacle.
#
# Nothing in this package may include another libs/* presentation header — it is
# the bottom of the graph and depends only on assets/sim/net/core.
add_library(bomber_game_util STATIC
  # The package's diagnostic seam (log.hpp, docs/coding-standards.md §11).
  src/log.cpp
  # The scheme editor's pure grid model — the SDL editor screen in libs/editor
  # renders it, but every rule about what a legal grid IS lives here.
  src/editor_grid.cpp
)
add_library(bomber::game_util ALIAS bomber_game_util)
target_include_directories(bomber_game_util PUBLIC include)
target_compile_features(bomber_game_util PUBLIC cxx_std_20)
# bomber::net is PUBLIC because net_tally.hpp takes a net::RollbackSession& and
# net_setup_roster.hpp's sibling in libs/netui speaks net::protocol — both are
# header-only, SDL-free seams, and both are built by the headless preset, so a
# consumer of this target needs the net include path to compile them.
target_link_libraries(bomber_game_util
  PUBLIC bomber::assets bomber::sim bomber::net
  PRIVATE bomber::warnings
)
