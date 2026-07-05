# bomber::assets — loaders for the original game's file formats (ANI, PCX,
# SCH, RES lists, RSS) plus the install locator. SDL-free by design so the
# headless tools can link it.

add_library(bomber_assets STATIC
  src/ani.cpp
  src/pcx.cpp
  src/sch.cpp
  src/extra.cpp
  src/reslist.cpp
  src/rss.cpp
  src/bmtext.cpp
  src/bmfont.cpp
  src/install.cpp
)
add_library(bomber::assets ALIAS bomber_assets)

target_include_directories(bomber_assets PUBLIC include)
target_compile_features(bomber_assets PUBLIC cxx_std_20)
target_link_libraries(bomber_assets PRIVATE bomber::warnings)
