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
bomber_add_test(bomber_ai_tests       ai           test_ai.cpp)

# ANI pacing helper lives in the game lib (header-only, SDL-free): frame =
# counter % statecnt, mirroring sub_41DAA7. Own target so it needn't link sim.
add_executable(bomber_anim_tests test_anim.cpp)
target_link_libraries(bomber_anim_tests PRIVATE doctest::doctest bomber::warnings)
target_include_directories(bomber_anim_tests PRIVATE ${CMAKE_SOURCE_DIR}/libs/game/include)
add_test(NAME anim COMMAND bomber_anim_tests)

# Front-end flow core is SDL-free (app_flow.hpp): pure next(state,input). Own
# target linking only the game include dir, like bomber_anim_tests.
add_executable(bomber_frontend_tests test_frontend.cpp)
target_link_libraries(bomber_frontend_tests PRIVATE doctest::doctest bomber::warnings)
target_include_directories(bomber_frontend_tests PRIVATE ${CMAKE_SOURCE_DIR}/libs/game/include)
add_test(NAME frontend COMMAND bomber_frontend_tests)

# .BM text-screen parser lives in bomber::assets (SDL-free). Own target so it
# links the assets lib rather than sim, like the anim/frontend suites.
add_executable(bomber_bmtext_tests test_bmtext.cpp)
target_link_libraries(bomber_bmtext_tests PRIVATE bomber::assets doctest::doctest bomber::warnings)
add_test(NAME bmtext COMMAND bomber_bmtext_tests)

bomber_add_test(bomber_diarrhea_tests diarrhea_throw test_diarrhea_throw.cpp)

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
