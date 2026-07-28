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

# SoundDirector's event -> SOUNDLST-group mapping. Also SDL-free: the director
# talks to SoundSink, so the suite substitutes a recorder for AudioEngine and
# compiles sound_director.cpp directly, same trick as the bank suite above. It
# pins the cues the original does NOT have (head hit, spooger string, throw,
# a punch that launches nothing) so a removed one cannot quietly come back.
add_executable(bomber_sound_director_tests test_sound_director.cpp
  ${CMAKE_SOURCE_DIR}/libs/audio/src/sound_director.cpp)
target_link_libraries(bomber_sound_director_tests
  PRIVATE bomber::sim doctest::doctest bomber::warnings)
target_include_directories(bomber_sound_director_tests
  PRIVATE ${CMAKE_SOURCE_DIR}/libs/audio/include)
add_test(NAME sound_director COMMAND bomber_sound_director_tests)
