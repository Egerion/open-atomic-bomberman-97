# bomber::sim — the deterministic gameplay core. Depends on nothing but the
# standard library; MUST stay free of SDL, I/O, floats, and wall-clock time.

add_library(bomber_sim STATIC
  src/setup.cpp
  src/hash.cpp
  src/simulation.cpp
  src/systems/movement.cpp
  src/systems/flames.cpp
  src/systems/bombs.cpp
  src/systems/powerups.cpp
  src/systems/diseases.cpp
  src/systems/enclosure.cpp
  src/systems/stage_actors.cpp
)
add_library(bomber::sim ALIAS bomber_sim)

target_include_directories(bomber_sim
  PUBLIC include
  PRIVATE src
)
target_compile_features(bomber_sim PUBLIC cxx_std_20)
target_link_libraries(bomber_sim PRIVATE bomber::warnings)
