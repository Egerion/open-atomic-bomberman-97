# bomber::core — SDL-free, dependency-free shared vocabulary (ADR-0008):
# the fixed-point pixel unit + field geometry, slot/rate limits, scoped-enum
# cast helpers, and (later) the shared id enums + input structs. Header-only;
# every other library may depend on it, it depends on nothing.
add_library(bomber_core INTERFACE)
add_library(bomber::core ALIAS bomber_core)

target_include_directories(bomber_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(bomber_core INTERFACE cxx_std_20)
