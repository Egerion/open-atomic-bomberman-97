# bomber::netplay — the ONLINE session, from the lobby to the last round.
#
# The lobby screen (codes, seats, chat), the direct connect/host screen, the
# best-of-N online match, and the runner that sequences all of it: entry points,
# the setup stage where the host's MatchConfig goes over the wire, then the
# session loop. Extracted out of GameApp by ADR-0009 and kept together here
# because they are one flow, not four screens that happen to mention sockets.
#
# It sits at the TOP of the presentation stack, above bomber::frontend, because
# that is what it does: it drives the ordinary setup and map-select screens and
# then hands MatchRunner a rollback session instead of local input. Nothing in
# the front end knows this package exists. The widgets the front end DOES embed
# while online — chat, the setup link, the F3 panel — are in bomber::netui,
# below it, which is what keeps this arrow pointing one way.
#
# Only the online-lobby LEAF inside the runner is BOMBER_HAS_LOBBY-guarded;
# everything else compiles on either kind of build.
add_library(bomber_netplay STATIC
  src/lobby_screen.cpp
  src/netplay_connect_screen.cpp
  src/netplay_runner.cpp
  src/netplay_match.cpp
)
add_library(bomber::netplay ALIAS bomber_netplay)
target_include_directories(bomber_netplay PUBLIC include)
target_compile_features(bomber_netplay PUBLIC cxx_std_20)
target_link_libraries(bomber_netplay
  PUBLIC bomber::frontend bomber::netui bomber::ui bomber::render bomber::input
         bomber::game_util bomber::net bomber::match bomber::audio SDL3::SDL3
  PRIVATE bomber::warnings bomber::platform
)
