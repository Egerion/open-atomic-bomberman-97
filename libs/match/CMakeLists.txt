# bomber::match — header-only glue turning parsed assets (scheme + VALUELST)
# into a sim::MatchConfig. Keeps bomber::sim asset-free and bomber::assets
# sim-free.

add_library(bomber_match INTERFACE)
add_library(bomber::match ALIAS bomber_match)

target_include_directories(bomber_match INTERFACE include)
target_link_libraries(bomber_match INTERFACE bomber::assets bomber::sim)
