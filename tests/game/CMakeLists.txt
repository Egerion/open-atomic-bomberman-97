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
# next(state,input) plus cycle_slot_input_type. input.hpp pulls in
# bomber/sim/types.hpp (PlayerInput/TickInputs), so link bomber::sim — a
# header-only pull here, not a build of the sim library into this SDL-free binary.
add_executable(bomber_frontend_tests test_frontend.cpp)
target_link_libraries(bomber_frontend_tests PRIVATE bomber::sim doctest::doctest bomber::warnings)
target_include_directories(bomber_frontend_tests PRIVATE ${CMAKE_SOURCE_DIR}/libs/game/include)
add_test(NAME frontend COMMAND bomber_frontend_tests)

# Goldman wheel PURE math (goldman_wheel.hpp) is SDL-free like app_flow.hpp:
# own target linking only the game include dir + sim (for PowerupType).
add_executable(bomber_goldman_wheel_tests test_goldman_wheel.cpp)
target_link_libraries(bomber_goldman_wheel_tests PRIVATE bomber::sim doctest::doctest bomber::warnings)
target_include_directories(bomber_goldman_wheel_tests PRIVATE ${CMAKE_SOURCE_DIR}/libs/game/include)
add_test(NAME goldman_wheel COMMAND bomber_goldman_wheel_tests)

# Scheme editor's pure grid model (editor_grid.hpp/.cpp) is SDL-free despite
# living under libs/game/src — compiled directly into its own test binary rather
# than linking bomber_game_core (which pulls in SDL3).
add_executable(bomber_editor_grid_tests test_editor_grid.cpp
  ${CMAKE_SOURCE_DIR}/libs/game/src/editor_grid.cpp)
target_link_libraries(bomber_editor_grid_tests PRIVATE bomber::assets bomber::sim doctest::doctest bomber::warnings)
target_include_directories(bomber_editor_grid_tests PRIVATE ${CMAKE_SOURCE_DIR}/libs/game/include)
add_test(NAME editor_grid COMMAND bomber_editor_grid_tests)
