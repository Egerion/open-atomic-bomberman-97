# abtool is SDL-free: it links only the headless libraries.
add_executable(abtool main.cpp commands.cpp)
target_link_libraries(abtool PRIVATE bomber::assets bomber::sim bomber::match bomber::warnings)
target_include_directories(abtool PRIVATE .)
