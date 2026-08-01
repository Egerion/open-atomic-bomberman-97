# Audio-module suites. They link bomber::audio_core — the SDL-FREE half of the
# module (sound_bank.cpp + sound_director.cpp; AudioEngine, the mixing half, is
# the only file that includes SDL). That target is built by every preset,
# `headless` included, so these suites exercise the SHIPPED objects.
#
# They used to compile sound_bank.cpp/sound_director.cpp straight into each test
# binary instead, because libs/audio was added only under BOMBER_BUILD_VIEWER and
# so did not exist headlessly at all. The trick kept the suites green under the
# pre-push gate while the library itself sat outside it — a gate that skips
# shipped code. libs/audio now declares its SDL-free half unconditionally, so the
# workaround is unnecessary and suite and library can no longer drift apart
# (same source, but a separate TU built with different flags).

# The sound SELECTION engine (sub_427961 & friends).
#
# TWO ctest entries over one binary, split on the `install:` case-name prefix.
# The synthetic cases are what gate the cull table and run everywhere; exactly
# one case needs an original install and returns early without it, and doctest
# has no runtime skip — so a single entry reported PASSED for a run that was
# partly a skip. Filtering separates the two facts: `sound_bank` is a real pass,
# `sound_bank_install` reports SKIP off the marker the case already printed and
# nothing consumed.
add_executable(bomber_sound_bank_tests test_sound_bank.cpp)
target_link_libraries(bomber_sound_bank_tests
  PRIVATE bomber::audio_core doctest::doctest bomber::warnings)
add_test(NAME sound_bank COMMAND bomber_sound_bank_tests --test-case-exclude=install:*)
add_test(NAME sound_bank_install COMMAND bomber_sound_bank_tests --test-case=install:*)
set_tests_properties(sound_bank_install PROPERTIES
  SKIP_REGULAR_EXPRESSION "SOUND_BANK_SKIP")

# SoundDirector's event -> SOUNDLST-group mapping. The director talks to
# SoundSink, so the suite substitutes a recorder for AudioEngine. It pins the
# cues the original does NOT have (head hit, spooger string, throw, a punch that
# launches nothing) so a removed one cannot quietly come back.
add_executable(bomber_sound_director_tests test_sound_director.cpp)
target_link_libraries(bomber_sound_director_tests
  PRIVATE bomber::audio_core doctest::doctest bomber::warnings)
# death_anim.hpp moved to the shared SDL-free floor; reached by include dir for
# the same reason the tests/game suites reach game_util headers that way (its
# target PUBLIC-links bomber::net, which this suite has no business linking).
target_include_directories(bomber_sound_director_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/libs/game_util/include)
add_test(NAME sound_director COMMAND bomber_sound_director_tests)

# The round-init music decision (round_music.hpp). Header-only and SDL-free, so
# the suite needs no sources beyond itself; it links audio_core purely for the
# include path. It pins the guard the LOCAL and the NETPLAY round loops now
# share — they used to keep separate copies and the netplay one had dropped the
# "Disable music during gameplay" option.
add_executable(bomber_round_music_tests test_round_music.cpp)
target_link_libraries(bomber_round_music_tests
  PRIVATE bomber::audio_core doctest::doctest bomber::warnings)
add_test(NAME round_music COMMAND bomber_round_music_tests)
