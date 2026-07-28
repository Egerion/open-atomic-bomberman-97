# Engine-base (libs/platform) suites. FramePacer is the SDL-FREE half of the
# front-end's frame pacing — the decision, split out of the SDL loop precisely
# so it can be pinned here — so this adds the platform include dir directly
# rather than linking bomber::platform (which pulls in SDL3, and which the
# headless preset does not even configure). Same trick as tests/game and
# tests/audio use for their SDL-free pieces.
add_executable(bomber_frame_pacer_tests test_frame_pacer.cpp)
target_link_libraries(bomber_frame_pacer_tests PRIVATE doctest::doctest bomber::warnings)
target_include_directories(bomber_frame_pacer_tests
  PRIVATE ${CMAKE_SOURCE_DIR}/libs/platform/include)
add_test(NAME frame_pacer COMMAND bomber_frame_pacer_tests)
