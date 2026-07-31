# bomber::platform — the engine-base layer (ADR-0008). Game-agnostic
# infrastructure the front-end sits on, so screens never re-implement timing.
#
# What is actually here today is frame pacing, and only that: FramePacer (the
# SDL-free decision, pinned by tests/platform) and FrameClock (the SDL-backed
# half). ADR-0008 also scopes a window wrapper, an input pump and a resource
# cache to this layer; none of them have landed, and screens still drain their
# own input. Stated as pending rather than listed as present — a build file that
# advertises components that do not exist sends a reader looking for them.
#
# Header-only while that holds; grows a .cpp as those classes land.
add_library(bomber_platform INTERFACE)
add_library(bomber::platform ALIAS bomber_platform)

target_include_directories(bomber_platform INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(bomber_platform INTERFACE cxx_std_20)
target_link_libraries(bomber_platform INTERFACE SDL3::SDL3)
