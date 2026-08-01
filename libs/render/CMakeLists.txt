# bomber::render — how a pixel gets on the screen, and nothing else.
#
# The SDL RAII shims (sdl.hpp), texture upload and recolouring (AssetStore),
# ANI sprite banks and sequence resolution (sprites/sequences), and the world
# Renderer that draws a sim::State. It READS the sim's state and events and
# never mutates them.
#
# The boundary that keeps this package readable on its own: nothing here knows
# what a screen, a dialog or a menu is. Every rule it applies that can be stated
# without SDL — the alpha bleed, the transparent-blit key, the scaling filter,
# the animation pacing, the carry pose — lives in bomber::game_util and is
# pinned by a headless test; this package is the SDL translation of those rules.
# A drawing routine that starts needing a ScreenContext belongs in bomber::ui,
# not here, and the dependency arrow (ui -> render) says why.
add_library(bomber_render STATIC
  # The credits addendum's author photograph, compiled in rather than shipped
  # beside the exe — the whole point of the single-file build is that the user
  # drops ONE file into their install (see cmake/EmbedBinary.cmake). It is a
  # 24-bit PCX so it needs no image-decoding dependency: libs/assets' own parser
  # already reads the 3-plane variant DATA_HD uses. AssetStore is the consumer.
  ${CMAKE_CURRENT_BINARY_DIR}/generated/author_photo_data.cpp
  src/sprites.cpp
  src/asset_store.cpp
  src/sequences.cpp
  src/renderer.cpp
)
include(${CMAKE_SOURCE_DIR}/cmake/EmbedBinary.cmake)
bomber_embed_binary(
  ${CMAKE_SOURCE_DIR}/assets/me/me.pcx
  ${CMAKE_CURRENT_BINARY_DIR}/generated/author_photo_data.cpp
  kAuthorPhotoPcx)

add_library(bomber::render ALIAS bomber_render)
target_include_directories(bomber_render PUBLIC include)
target_compile_features(bomber_render PUBLIC cxx_std_20)
# No bomber::audio edge: renderer.cpp's one reason for it — the shared
# death-animation table — lives in bomber::game_util now (death_anim.hpp), and
# the consumers that DO speak audio types (bomber::ui via bmscreen.hpp, and
# everything above it) all link bomber::audio themselves.
target_link_libraries(bomber_render
  PUBLIC bomber::assets bomber::sim bomber::match bomber::game_util SDL3::SDL3
  PRIVATE bomber::warnings
)
