# bomber::platform — the engine-base layer (ADR-0008): SDL-backed window,
# frame clock/pacing, input pump, resource cache. Game-agnostic infrastructure
# the front-end sits on, so screens never re-implement the main loop, timing, or
# input drain. Header-only for now (FrameClock); grows a .cpp as classes land.
add_library(bomber_platform INTERFACE)
add_library(bomber::platform ALIAS bomber_platform)

target_include_directories(bomber_platform INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(bomber_platform INTERFACE cxx_std_20)
target_link_libraries(bomber_platform INTERFACE SDL3::SDL3)
