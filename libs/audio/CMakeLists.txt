# bomber::audio — the audio module (ADR-0008 stage 2), extracted from
# libs/game: AudioEngine (SDL PCM stream pool, music/SFX) + SoundDirector (sim
# events -> SOUNDLST id ranges). Depends on assets (RSS/SOUNDLST) + sim (event
# types) + SDL3; nothing above it. (Namespace stays bomber::game for now — a
# later mechanical rename to bomber::audio, D1.)
#
# TWO targets, split on the ONE header that includes SDL3 (audio_engine.hpp):
#
#   bomber::audio_core — sound_bank.cpp + sound_director.cpp. SDL-FREE, so it
#     builds under the `headless` preset and therefore under the pre-push gate.
#   bomber::audio      — audio_engine.cpp on top. Needs SDL3, so it only exists
#     when BOMBER_BUILD_VIEWER is on (the root CMakeLists provides SDL3::SDL3
#     inside that block, and `headless` sets it OFF).
#
# The split is not cosmetic. This whole directory used to be added only inside
# the viewer block, so `headless` — which is what scripts/test.sh builds for the
# pre-push hook — never compiled ANY of it, and the tests/audio suites papered
# over half of that by compiling sound_bank.cpp/sound_director.cpp into their own
# binaries instead of linking a library. That kept the suites alive headlessly
# but left the SHIPPED objects outside the gate, the same shape of hole
# BOMBER_ENABLE_LOBBY=OFF used to leave (see CLAUDE.md "Build & test").
add_library(bomber_audio_core STATIC
  src/sound_bank.cpp     # SDL-free sound SELECTION engine (sub_427961 & friends)
  src/sound_director.cpp # sim Event -> SOUNDLST id ranges; talks to SoundSink
)
add_library(bomber::audio_core ALIAS bomber_audio_core)

target_include_directories(bomber_audio_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# sound_director.cpp reads the shared death-animation table
# (bomber/game_util/death_anim.hpp, a dependency-free header) via a raw include
# dir rather than by linking bomber::game_util — the same precedent tests/game
# sets: that target PUBLIC-links bomber::net, and audio must not carry an
# IXWebSocket/mbedTLS link line for one constexpr header.
target_include_directories(bomber_audio_core PRIVATE ${CMAKE_SOURCE_DIR}/libs/game_util/include)
target_compile_features(bomber_audio_core PUBLIC cxx_std_20)
target_link_libraries(bomber_audio_core
  PUBLIC bomber::assets bomber::sim
  PRIVATE bomber::warnings)

# The SDL half. Guarded rather than unconditional: `headless` configures no SDL3
# at all, so SDL3::SDL3 does not exist there and this target cannot be declared.
# Nothing below libs/game consumes it, so its absence is invisible headlessly.
if(BOMBER_BUILD_VIEWER)
  add_library(bomber_audio STATIC src/audio_engine.cpp)
  add_library(bomber::audio ALIAS bomber_audio)

  target_compile_features(bomber_audio PUBLIC cxx_std_20)
  target_link_libraries(bomber_audio
    PUBLIC bomber::audio_core SDL3::SDL3
    PRIVATE bomber::warnings)
endif()
