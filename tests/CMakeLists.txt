# Test suites (doctest), grouped by the module each suite exercises — the
# ADR-0008 package layout mirrored under tests/. One executable per suite keeps
# ctest output granular and the suites parallelizable. Each module subdir
# (sim/ match/ assets/ game/) declares its own suites; every ctest NAME is
# unchanged from the pre-split flat layout, so `ctest -R <name>` keeps working.

include(FetchContent)
# doctest's own CMakeLists declares an old minimum; keep CMake >= 4 happy.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_Declare(doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG v2.4.11
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(doctest)

# Sim-suite helper: one executable per suite, linked against the sim core and
# given tests/common on its include path (the shared helpers.hpp scaffolding).
# Defined here so every module subdir can call it — CMake functions are global
# once defined, and add_subdirectory below runs after this point.
function(bomber_add_test name ctest_name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE bomber::sim doctest::doctest bomber::warnings)
  target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/tests/common)
  add_test(NAME ${ctest_name} COMMAND ${name})
endfunction()

add_subdirectory(sim)
add_subdirectory(match)
add_subdirectory(net)
add_subdirectory(assets)
add_subdirectory(game)

# The renderer "visual_golden" ctest is NOT registered here: it needs the
# bomber_game target (SDL3) and is wired in apps/game/CMakeLists.txt, reading
# tests/visual/ directly. tests/visual/ is left exactly as-is.
