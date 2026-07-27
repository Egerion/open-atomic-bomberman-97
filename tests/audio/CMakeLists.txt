# Audio-module suites. The sound SELECTION engine (sound_bank.cpp) is SDL-free
# by design — the mixing half lives in AudioEngine — so it compiles straight
# into its own test binary rather than linking bomber_audio (which pulls in
# SDL3), keeping the suite buildable under the headless preset.
add_executable(bomber_sound_bank_tests test_sound_bank.cpp
  ${CMAKE_SOURCE_DIR}/libs/audio/src/sound_bank.cpp)
target_link_libraries(bomber_sound_bank_tests
  PRIVATE bomber::assets doctest::doctest bomber::warnings)
target_include_directories(bomber_sound_bank_tests
  PRIVATE ${CMAKE_SOURCE_DIR}/libs/audio/include)
add_test(NAME sound_bank COMMAND bomber_sound_bank_tests)
