# Sim test suites (doctest). One executable per suite keeps ctest output
# granular and the suites parallelizable.

include(FetchContent)
# doctest's own CMakeLists declares an old minimum; keep CMake >= 4 happy.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_Declare(doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG v2.4.11
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(doctest)

function(bomber_add_test name ctest_name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE bomber::sim doctest::doctest bomber::warnings)
  target_include_directories(${name} PRIVATE .)
  add_test(NAME ${ctest_name} COMMAND ${name})
endfunction()

bomber_add_test(bomber_tests          determinism  test_determinism.cpp)
bomber_add_test(bomber_sim_tests      sim_rules    test_sim.cpp)
bomber_add_test(bomber_move_tests     movement     test_move.cpp)
bomber_add_test(bomber_disease_tests  diseases     test_disease.cpp)
bomber_add_test(bomber_spooge_tests   spooge       test_spooge.cpp)
bomber_add_test(bomber_jelly_tests    jelly        test_jelly.cpp)
bomber_add_test(bomber_random_tests   random_power test_random.cpp)
bomber_add_test(bomber_dud_tests      duds         test_dud.cpp)
bomber_add_test(bomber_trigger_tests  trigger_allowance test_trigger_allowance.cpp)
bomber_add_test(bomber_goldflame_tests goldflame   test_goldflame.cpp)
bomber_add_test(bomber_kicknuance_tests kick_nuances test_kick_nuances.cpp)
bomber_add_test(bomber_punchthrow_tests punch_throw  test_punch_throw.cpp)
bomber_add_test(bomber_conveyor_tests   conveyor     test_conveyor.cpp)
bomber_add_test(bomber_trampoline_tests trampoline   test_trampoline.cpp)
bomber_add_test(bomber_stageactors_tests stage_actors test_stage_actors.cpp)
bomber_add_test(bomber_golden_tests   golden       test_golden.cpp)
bomber_add_test(bomber_stompdisease_tests stomped_diseases test_stomped_diseases.cpp)
bomber_add_test(bomber_ai_tests       ai           test_ai.cpp)
bomber_add_test(bomber_chainslot_tests chain_slot   test_chain_slot.cpp)
bomber_add_test(bomber_headhit_tests  head_hit_bounds test_head_hit_bounds.cpp)
bomber_add_test(bomber_team_tests     team         test_team.cpp)
# team_render_colour (the Team Play red/white sprite-colour rule) lives in
# bomber::match (header-only, SDL-free) so the renderer's mapping can be
# pinned without a libs/game/SDL dependency.
target_link_libraries(bomber_team_tests PRIVATE bomber::match)
bomber_add_test(bomber_rover_tests    rovers       test_rovers.cpp)
bomber_add_test(bomber_regen_tests    tile_regen   test_regen.cpp)
bomber_add_test(bomber_ice_tests      ice_delay    test_ice.cpp)

# ANI pacing helper lives in the game lib (header-only, SDL-free): frame =
# counter % statecnt, mirroring sub_41DAA7. Own target so it needn't link sim.
add_executable(bomber_anim_tests test_anim.cpp)
target_link_libraries(bomber_anim_tests PRIVATE doctest::doctest bomber::warnings)
target_include_directories(bomber_anim_tests PRIVATE ${CMAKE_SOURCE_DIR}/libs/game/include)
add_test(NAME anim COMMAND bomber_anim_tests)

# Front-end flow core is SDL-free (app_flow.hpp, input.hpp's slot-type cycle):
# pure next(state,input) plus cycle_slot_input_type. Own target linking only
# the game include dir, like bomber_anim_tests; input.hpp pulls in
# bomber/sim/types.hpp (PlayerInput/TickInputs), so link bomber::sim too — it's
# a header-only pull here, not a build of the sim library into this SDL-free
# binary.
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

# .BM text-screen parser lives in bomber::assets (SDL-free). Own target so it
# links the assets lib rather than sim, like the anim/frontend suites.
add_executable(bomber_bmtext_tests test_bmtext.cpp)
target_link_libraries(bomber_bmtext_tests PRIVATE bomber::assets doctest::doctest bomber::warnings)
add_test(NAME bmtext COMMAND bomber_bmtext_tests)

# .RMP player-colour remap parser (bomber::assets, SDL-free), like bmtext.
add_executable(bomber_rmp_tests test_rmp.cpp)
target_link_libraries(bomber_rmp_tests PRIVATE bomber::assets doctest::doctest bomber::warnings)
add_test(NAME rmp COMMAND bomber_rmp_tests)

bomber_add_test(bomber_diarrhea_tests diarrhea_throw test_diarrhea_throw.cpp)
bomber_add_test(bomber_placement_diag_tests placement_diag test_placement_diag.cpp)
bomber_add_test(bomber_goldman_award_tests goldman_award test_goldman_award.cpp)

# Per-match brick fill lives in bomber::match (header-only); link the match lib.
add_executable(bomber_match_tests test_match.cpp)
target_link_libraries(bomber_match_tests PRIVATE bomber::match doctest::doctest bomber::warnings)
target_include_directories(bomber_match_tests PRIVATE .)
add_test(NAME match COMMAND bomber_match_tests)

# options.ini parser lives in bomber::assets (SDL-free), like bmtext.
add_executable(bomber_options_tests test_options.cpp)
target_link_libraries(bomber_options_tests PRIVATE bomber::assets doctest::doctest bomber::warnings)
add_test(NAME options COMMAND bomber_options_tests)

# FON bitmap-font parser + VALUELST column access (both SDL-free, bomber::assets).
add_executable(bomber_bmfont_tests test_bmfont.cpp)
target_link_libraries(bomber_bmfont_tests PRIVATE bomber::assets doctest::doctest bomber::warnings)
add_test(NAME bmfont COMMAND bomber_bmfont_tests)

add_executable(bomber_valuelst_tests test_valuelst.cpp)
target_link_libraries(bomber_valuelst_tests PRIVATE bomber::assets doctest::doctest bomber::warnings)
add_test(NAME valuelst COMMAND bomber_valuelst_tests)

add_executable(bomber_messages_tests test_messages.cpp)
target_link_libraries(bomber_messages_tests PRIVATE bomber::assets doctest::doctest bomber::warnings)
add_test(NAME messages COMMAND bomber_messages_tests)

# .CAM campaign/stage-sequence parser (bomber::assets, SDL-free), like messages.
add_executable(bomber_campaign_tests test_campaign.cpp)
target_link_libraries(bomber_campaign_tests PRIVATE bomber::assets doctest::doctest bomber::warnings)
add_test(NAME campaign COMMAND bomber_campaign_tests)

# .SCH writer round-trip (bomber::assets, SDL-free), like bmtext/rmp.
add_executable(bomber_sch_write_tests test_sch_write.cpp)
target_link_libraries(bomber_sch_write_tests PRIVATE bomber::assets doctest::doctest bomber::warnings)
add_test(NAME sch_write COMMAND bomber_sch_write_tests)

# Scheme editor's pure grid model (editor_grid.hpp/.cpp) is SDL-free despite
# living under libs/game/src — compiled directly into its own test binary
# (like bomber_frontend_tests/bomber_anim_tests) rather than linking the
# whole bomber_game_core (which pulls in SDL3).
add_executable(bomber_editor_grid_tests test_editor_grid.cpp
  ${CMAKE_SOURCE_DIR}/libs/game/src/editor_grid.cpp)
target_link_libraries(bomber_editor_grid_tests PRIVATE bomber::assets bomber::sim doctest::doctest bomber::warnings)
target_include_directories(bomber_editor_grid_tests PRIVATE ${CMAKE_SOURCE_DIR}/libs/game/include)
add_test(NAME editor_grid COMMAND bomber_editor_grid_tests)
