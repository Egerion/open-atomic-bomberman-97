# bomber::netui — the net widgets that are drawn INSIDE another screen.
#
# The lobby chat overlay, the online setup link (the host's authoritative roster
# preview arriving over the wire while the local setup screen is up) and its
# pure roster mapping, and the F3 in-match diagnostic panel + netdiag.log record.
#
# This is deliberately NOT the same package as bomber::netplay, and the seam is
# a dependency fact rather than a taste: the local setup and map-select screens
# EMBED the chat overlay and the setup link, so those must sit BELOW
# bomber::frontend — while the lobby, connect, match and runner screens DRIVE the
# front-end screens and must sit above it. One "netui" package spanning both
# would have put an arrow in each direction across the same boundary. Split on
# where the arrow actually points, and neither half needs the other.
#
# Nothing here knows about a lobby session or a match loop; a widget takes what
# it draws as an argument. If a file starts owning an event loop that sequences
# screens, it belongs in bomber::netplay.
add_library(bomber_netui STATIC
  # PORT-ONLY, PROTOCOL.md §7. Built unconditionally: without
  # BOMBER_ENABLE_LOBBY its definitions compile to inert no-ops, so callers link
  # the same symbols on either kind of build.
  src/chat_overlay.cpp
  src/net_setup_link.cpp
  # The F3 in-match netplay diagnostic panel + the netdiag.log session record
  # (net_stats.hpp does the measuring; this only draws and writes).
  src/net_overlay.cpp
)
add_library(bomber::netui ALIAS bomber_netui)
target_include_directories(bomber_netui PUBLIC include)
target_compile_features(bomber_netui PUBLIC cxx_std_20)
target_link_libraries(bomber_netui
  PUBLIC bomber::ui bomber::render bomber::input bomber::game_util bomber::net SDL3::SDL3
  PRIVATE bomber::warnings bomber::platform
)
