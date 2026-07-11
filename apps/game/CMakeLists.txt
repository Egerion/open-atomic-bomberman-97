add_executable(bomber_game main.cpp)
target_link_libraries(bomber_game PRIVATE bomber::game bomber::warnings)

# Windows: copy SDL3.dll (and any other runtime DLLs) next to the exe,
# otherwise it dies with STATUS_DLL_NOT_FOUND outside the IDE.
if(WIN32)
  add_custom_command(TARGET bomber_game POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:bomber_game> $<TARGET_FILE_DIR:bomber_game>
    COMMAND_EXPAND_LISTS)
endif()

# Visual golden harness (tests/visual/README.md): pins bomber_game's
# RENDERED OUTPUT against a real install, the same pin-and-recapture
# discipline tests/test_golden.cpp gives the sim (CLAUDE.md's Build & test).
# Registered here (not tests/CMakeLists.txt, which is add_subdirectory'd
# before this SDL-gated block runs) since it needs the bomber_game target.
# Runs entirely through GameApp's own install auto-detection
# (BOMBER_GAME_DIR / gamedir.txt / the standard paths,
# libs/assets/src/install.cpp) -- CI has no install there, so the runner
# script detects exit code 2 ("no game_dir resolved") and self-SKIPs via
# SKIP_REGULAR_EXPRESSION rather than failing, the same shape as
# abtool survey / bomber_viewer --selftest being install-conditional.
add_test(NAME visual_golden
  COMMAND ${CMAKE_COMMAND}
          -DBOMBER_GAME_EXE=$<TARGET_FILE:bomber_game>
          -DBOMBER_SHOTS_DIR=${CMAKE_SOURCE_DIR}/tests/visual
          -DBOMBER_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/visual_shots
          -P ${CMAKE_SOURCE_DIR}/tests/visual/run_visual_golden.cmake)
set_tests_properties(visual_golden PROPERTIES SKIP_REGULAR_EXPRESSION "VISUAL_GOLDEN_SKIP")
