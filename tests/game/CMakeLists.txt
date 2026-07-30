# SDL-free front-end logic suites (ADR-0008). These exercise header-only pieces
# of libs/game (anim_pace, app_flow, goldman_wheel) and the pure editor grid
# model, so they add the game INCLUDE dir directly rather than linking
# bomber_game_core (which pulls in SDL3) — keeping them buildable under the
# headless preset. ctest NAMEs unchanged.

# ANI pacing helper (anim_pace.hpp): frame = counter % statecnt, mirroring
# sub_41DAA7. Own target so it needn't link sim.
add_executable(bomber_anim_tests test_anim.cpp)
target_link_libraries(bomber_anim_tests PRIVATE doctest::doctest bomber::warnings)
target_include_directories(bomber_anim_tests PRIVATE ${CMAKE_SOURCE_DIR}/libs/game/include)
add_test(NAME anim COMMAND bomber_anim_tests)

# Front-end flow core (app_flow.hpp + input.hpp's slot-type cycle): pure
# next(state,input) plus cycle_slot_input_type, and the best-of-N match decision
# (match_outcome.hpp's round_winner/match_clinch/reset_match_scores). input.hpp
# pulls in bomber/sim/types.hpp (PlayerInput/TickInputs) and match_outcome.hpp
# pulls in assets/reslist.hpp's ValueList, so link bomber::sim + bomber::assets —
# header-only pulls here, not a build of either library into this SDL-free binary.
add_executable(bomber_frontend_tests test_frontend.cpp)
target_link_libraries(bomber_frontend_tests
  PRIVATE bomber::assets bomber::sim doctest::doctest bomber::warnings)
target_include_directories(bomber_frontend_tests PRIVATE ${CMAKE_SOURCE_DIR}/libs/game/include)
add_test(NAME frontend COMMAND bomber_frontend_tests)

# Goldman wheel PURE math (goldman_wheel.hpp) is SDL-free like app_flow.hpp:
# own target linking only the game include dir + sim (for PowerupType).
add_executable(bomber_goldman_wheel_tests test_goldman_wheel.cpp)
target_link_libraries(bomber_goldman_wheel_tests PRIVATE bomber::sim doctest::doctest bomber::warnings)
target_include_directories(bomber_goldman_wheel_tests PRIVATE ${CMAKE_SOURCE_DIR}/libs/game/include)
add_test(NAME goldman_wheel COMMAND bomber_goldman_wheel_tests)

# The online setup stage's PURE roster/level mapping (net_setup_roster.hpp): the
# local 10-slot roster <-> net::SetupPreviewFrame translation, including the seat
# re-pointing that makes the host's own player read as "someone else's" on the
# guest. SDL-free like the suites above, but it does speak the wire types, so it
# links bomber::net (which the headless preset builds too).
add_executable(bomber_net_setup_roster_tests test_net_setup_roster.cpp)
target_link_libraries(bomber_net_setup_roster_tests
  PRIVATE bomber::net bomber::sim doctest::doctest bomber::warnings)
target_include_directories(bomber_net_setup_roster_tests PRIVATE ${CMAKE_SOURCE_DIR}/libs/game/include)
add_test(NAME net_setup_roster COMMAND bomber_net_setup_roster_tests)

# The generic list dialog's pinned pixel geometry (list_dialog_geometry.hpp,
# sub_42DBCC). Header-only integer math with no SDL and no other dependency, so
# this target needs nothing but the game include dir.
add_executable(bomber_list_dialog_tests test_list_dialog.cpp)
target_link_libraries(bomber_list_dialog_tests PRIVATE doctest::doctest bomber::warnings)
target_include_directories(bomber_list_dialog_tests PRIVATE ${CMAKE_SOURCE_DIR}/libs/game/include)
add_test(NAME list_dialog COMMAND bomber_list_dialog_tests)

# The PORT-ONLY soft-scaling filter's pure decisions (scale_filter.hpp): which
# sampling mode each texture class gets, and the capture-run pin. Header-only and
# SDL-free (the SDL translation lives in sprites.cpp), so like the list-dialog
# suite this needs nothing but the game include dir.
add_executable(bomber_scale_filter_tests test_scale_filter.cpp)
target_link_libraries(bomber_scale_filter_tests PRIVATE doctest::doctest bomber::warnings)
target_include_directories(bomber_scale_filter_tests PRIVATE ${CMAKE_SOURCE_DIR}/libs/game/include)
add_test(NAME scale_filter COMMAND bomber_scale_filter_tests)

# The alpha-dilate pass every texture upload runs (alpha_bleed.hpp): SDL-free
# pixel rules over assets::Image, so this links bomber::assets for that type and
# nothing else.
add_executable(bomber_alpha_bleed_tests test_alpha_bleed.cpp)
target_link_libraries(bomber_alpha_bleed_tests PRIVATE bomber::assets doctest::doctest bomber::warnings)
target_include_directories(bomber_alpha_bleed_tests PRIVATE ${CMAKE_SOURCE_DIR}/libs/game/include)
add_test(NAME alpha_bleed COMMAND bomber_alpha_bleed_tests)

# Scheme editor's pure grid model (editor_grid.hpp/.cpp) is SDL-free despite
# living under libs/game/src — compiled directly into its own test binary rather
# than linking bomber_game_core (which pulls in SDL3).
add_executable(bomber_editor_grid_tests test_editor_grid.cpp
  ${CMAKE_SOURCE_DIR}/libs/game/src/editor_grid.cpp)
target_link_libraries(bomber_editor_grid_tests PRIVATE bomber::assets bomber::sim doctest::doctest bomber::warnings)
target_include_directories(bomber_editor_grid_tests PRIVATE ${CMAKE_SOURCE_DIR}/libs/game/include)
add_test(NAME editor_grid COMMAND bomber_editor_grid_tests)
