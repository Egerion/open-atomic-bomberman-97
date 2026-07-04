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
bomber_add_test(bomber_golden_tests   golden       test_golden.cpp)

# ANI pacing helper lives in the game lib (header-only, SDL-free): frame =
# counter % statecnt, mirroring sub_41DAA7. Own target so it needn't link sim.
add_executable(bomber_anim_tests test_anim.cpp)
target_link_libraries(bomber_anim_tests PRIVATE doctest::doctest bomber::warnings)
target_include_directories(bomber_anim_tests PRIVATE ${CMAKE_SOURCE_DIR}/libs/game/include)
add_test(NAME anim COMMAND bomber_anim_tests)
