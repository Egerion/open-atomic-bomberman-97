# bomber::match suites (ADR-0008): header-only glue (scheme + VALUELST ->
# MatchConfig, stage rotation, team-colour). ctest NAMEs unchanged.

# team_render_colour (the Team Play red/white sprite-colour rule) lives in
# bomber::match (header-only, SDL-free) so the renderer's mapping can be pinned
# without a libs/game/SDL dependency. The suite also drives a Simulation, so it
# uses bomber_add_test (links bomber::sim + shares helpers.hpp) plus match.
bomber_add_test(bomber_team_tests team test_team.cpp)
target_link_libraries(bomber_team_tests PRIVATE bomber::match)

# Per-match brick fill + scheme->MatchConfig (bomber::match, header-only).
add_executable(bomber_match_tests test_match.cpp)
target_link_libraries(bomber_match_tests PRIVATE bomber::match doctest::doctest bomber::warnings)
add_test(NAME match COMMAND bomber_match_tests)

# LevelRegistry (bomber::match, header-only): pins the built-in rotation +
# asset base names byte-identical and the one-edit custom-level extensibility.
add_executable(bomber_level_registry_tests test_level_registry.cpp)
target_link_libraries(bomber_level_registry_tests PRIVATE bomber::match doctest::doctest bomber::warnings)
add_test(NAME level_registry COMMAND bomber_level_registry_tests)
