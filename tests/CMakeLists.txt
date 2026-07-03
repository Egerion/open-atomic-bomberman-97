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
bomber_add_test(bomber_golden_tests   golden       test_golden.cpp)
