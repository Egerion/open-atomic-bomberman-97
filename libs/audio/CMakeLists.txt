# bomber::audio — the audio module (ADR-0008 stage 2), extracted from
# libs/game: AudioEngine (SDL PCM stream pool, music/SFX) + SoundDirector (sim
# events -> SOUNDLST id ranges). Depends on assets (RSS/SOUNDLST) + sim (event
# types) + SDL3; nothing above it. (Namespace stays bomber::game for now — a
# later mechanical rename to bomber::audio, D1.)
add_library(bomber_audio STATIC
  src/audio_engine.cpp
  src/sound_bank.cpp    # SDL-free sound SELECTION engine (sub_427961 & friends)
  src/sound_director.cpp
)
add_library(bomber::audio ALIAS bomber_audio)

target_include_directories(bomber_audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(bomber_audio PUBLIC cxx_std_20)
target_link_libraries(bomber_audio
  PUBLIC bomber::assets bomber::sim SDL3::SDL3
  PRIVATE bomber::warnings)
