# SDL-free front-end logic suites (ADR-0008). These exercise header-only pieces
# of libs/game (anim_pace, app_flow, goldman_wheel) and the pure editor grid
# model, so they add the game INCLUDE dir directly rather than linking
# bomber_game_core (which pulls in SDL3) — keeping them buildable under the
# headless preset. ctest NAMEs unchanged.

# ANI pacing helper (anim_pace.hpp): frame = counter % statecnt, mirroring
# sub_41DAA7. Own target so it needn't link sim.
add_executable(bomber_anim_tests test_anim.cpp)
target_link_libraries(bomber_anim_tests PRIVATE doctest::doctest bomber::warnings)
target_include_directories(bomber_anim_tests PRIVATE ${CMAKE_SOURCE_DIR}/libs/game/include)
add_test(NAME anim COMMAND bomber_anim_tests)

# The F9 cadence lever resolved once for all four of its consumers
# (match_cadence.hpp). Header-only and SDL-free, so the rule "netplay is
# fixed-tick 20 Hz, on the presentation side too" is pinned without a window —
# which is the point: nothing in the game notices when one of the four disagrees,
# and for a while three of them did.
add_executable(bomber_match_cadence_tests test_match_cadence.cpp)
target_link_libraries(bomber_match_cadence_tests PRIVATE doctest::doctest bomber::warnings)
target_include_directories(bomber_match_cadence_tests PRIVATE ${CMAKE_SOURCE_DIR}/libs/game/include)
add_test(NAME match_cadence COMMAND bomber_match_cadence_tests)

# The ONLINE Esc rule (net_esc.hpp): one press stops the round (host only), a
# second one LEAVES (everyone). Pure and SDL-free precisely so the case it exists
# for — a peer that has stopped responding — is a headless test rather than
# something only reproducible with two machines and a broken network.
add_executable(bomber_net_esc_tests test_net_esc.cpp)
target_link_libraries(bomber_net_esc_tests PRIVATE doctest::doctest bomber::warnings)
target_include_directories(bomber_net_esc_tests PRIVATE ${CMAKE_SOURCE_DIR}/libs/game/include)
add_test(NAME net_esc COMMAND bomber_net_esc_tests)

# Front-end flow core (app_flow.hpp + input.hpp's slot-type cycle): pure
# next(state,input) plus cycle_slot_input_type, and the best-of-N match decision
# (match_outcome.hpp's round_winner/match_clinch/reset_match_scores). input.hpp
# pulls in bomber/sim/types.hpp (PlayerInput/TickInputs) and match_outcome.hpp
# pulls in assets/reslist.hpp's ValueList, so link bomber::sim + bomber::assets —
# header-only pulls here, not a build of either library into this SDL-free binary.
add_executable(bomber_frontend_tests test_frontend.cpp)
target_link_libraries(bomber_frontend_tests
  PRIVATE bomber::assets bomber::sim doctest::doctest bomber::warnings)
target_include_directories(bomber_frontend_tests PRIVATE ${CMAKE_SOURCE_DIR}/libs/game/include)
add_test(NAME frontend COMMAND bomber_frontend_tests)

# The CAMPAIGN round end (campaign_round_end.hpp): sub_4016DA's pacing verdict
# and the 0x42A63B branch that skips the whole outcome tier. Header-only with no
# dependency at all, so this target needs nothing but the game include dir — and
# it is headless precisely because the alternative is proving a hidden mode's
# screen flow by hand through a five-keypress easter egg.
add_executable(bomber_campaign_round_end_tests test_campaign_round_end.cpp)
target_link_libraries(bomber_campaign_round_end_tests PRIVATE doctest::doctest bomber::warnings)
target_include_directories(bomber_campaign_round_end_tests PRIVATE ${CMAKE_SOURCE_DIR}/libs/game/include)
add_test(NAME campaign_round_end COMMAND bomber_campaign_round_end_tests)

# Goldman wheel PURE math (goldman_wheel.hpp) is SDL-free like app_flow.hpp:
# own target linking only the game include dir + sim (for PowerupType).
add_executable(bomber_goldman_wheel_tests test_goldman_wheel.cpp)
target_link_libraries(bomber_goldman_wheel_tests PRIVATE bomber::sim doctest::doctest bomber::warnings)
target_include_directories(bomber_goldman_wheel_tests PRIVATE ${CMAKE_SOURCE_DIR}/libs/game/include)
add_test(NAME goldman_wheel COMMAND bomber_goldman_wheel_tests)

# The online setup stage's PURE roster/level mapping (net_setup_roster.hpp): the
# local 10-slot roster <-> net::SetupPreviewFrame translation, including the seat
# re-pointing that makes the host's own player read as "someone else's" on the
# guest. SDL-free like the suites above, but it does speak the wire types, so it
# links bomber::net (which the headless preset builds too).
add_executable(bomber_net_setup_roster_tests test_net_setup_roster.cpp)
target_link_libraries(bomber_net_setup_roster_tests
  PRIVATE bomber::net bomber::sim doctest::doctest bomber::warnings)
target_include_directories(bomber_net_setup_roster_tests PRIVATE ${CMAKE_SOURCE_DIR}/libs/game/include)
add_test(NAME net_setup_roster COMMAND bomber_net_setup_roster_tests)

# The generic list dialog's pinned pixel geometry (list_dialog_geometry.hpp,
# sub_42DBCC). Header-only integer math with no SDL and no other dependency, so
# this target needs nothing but the game include dir.
add_executable(bomber_list_dialog_tests test_list_dialog.cpp)
target_link_libraries(bomber_list_dialog_tests PRIVATE doctest::doctest bomber::warnings)
target_include_directories(bomber_list_dialog_tests PRIVATE ${CMAKE_SOURCE_DIR}/libs/game/include)
add_test(NAME list_dialog COMMAND bomber_list_dialog_tests)

# The PORT-ONLY soft-scaling filter's pure decisions (scale_filter.hpp): which
# sampling mode each texture class gets, and the capture-run pin. Header-only and
# SDL-free (the SDL translation lives in sprites.cpp), so like the list-dialog
# suite this needs nothing but the game include dir.
add_executable(bomber_scale_filter_tests test_scale_filter.cpp)
target_link_libraries(bomber_scale_filter_tests PRIVATE doctest::doctest bomber::warnings)
target_include_directories(bomber_scale_filter_tests PRIVATE ${CMAKE_SOURCE_DIR}/libs/game/include)
add_test(NAME scale_filter COMMAND bomber_scale_filter_tests)

# The alpha-dilate pass every texture upload runs (alpha_bleed.hpp): SDL-free
# pixel rules over assets::Image, so this links bomber::assets for that type and
# nothing else.
add_executable(bomber_alpha_bleed_tests test_alpha_bleed.cpp)
target_link_libraries(bomber_alpha_bleed_tests PRIVATE bomber::assets doctest::doctest bomber::warnings)
target_include_directories(bomber_alpha_bleed_tests PRIVATE ${CMAKE_SOURCE_DIR}/libs/game/include)
add_test(NAME alpha_bleed COMMAND bomber_alpha_bleed_tests)

# The `.BM` viewer's transparent-blit key (key_color.hpp): sub_44AED5 skips
# palette index 0. SDL-free pixel rules over assets::Image, same shape as the
# alpha_bleed suite above.
add_executable(bomber_key_color_tests test_key_color.cpp)
target_link_libraries(bomber_key_color_tests PRIVATE bomber::assets doctest::doctest bomber::warnings)
target_include_directories(bomber_key_color_tests PRIVATE ${CMAKE_SOURCE_DIR}/libs/game/include)
add_test(NAME key_color COMMAND bomber_key_color_tests)

# The port's own credits addendum (credits_addendum.hpp): the layout budget and
# the centred photograph's clearance, driven through the same bmtext parser the
# install's .BM files go through. SDL-free; links bomber::assets for the parser.
add_executable(bomber_credits_addendum_tests test_credits_addendum.cpp)
target_link_libraries(bomber_credits_addendum_tests
  PRIVATE bomber::assets doctest::doctest bomber::warnings)
target_include_directories(bomber_credits_addendum_tests PRIVATE ${CMAKE_SOURCE_DIR}/libs/game/include)
add_test(NAME credits_addendum COMMAND bomber_credits_addendum_tests)

# The MESSAGES.TXT substitution half of hud_format.hpp (fmt_u/fmt_s/fmt_us). The
# clock half is covered inside the frontend suite; these three had no coverage at
# all, and they are the ones that parse the PLAYER'S OWN file — a hand-edited row
# reaches them, so "leave a wrong-kind specifier literal rather than guess" is a
# contract and not an implementation detail. Header-only with no dependency, so
# this target needs nothing but the game include dir.
add_executable(bomber_message_splice_tests test_message_splice.cpp)
target_link_libraries(bomber_message_splice_tests PRIVATE doctest::doctest bomber::warnings)
target_include_directories(bomber_message_splice_tests PRIVATE ${CMAKE_SOURCE_DIR}/libs/game/include)
add_test(NAME message_splice COMMAND bomber_message_splice_tests)

# The cross-screen helpers (frontend_util.hpp): the ONE shared presentation LCG
# every pre-match screen steps through pick_glue, and the case-insensitive
# DATA/SCHEMES resolve. Both were untested, and both are what the ADR-0008 screen
# decomposition was most likely to break — a per-screen copy of pick_glue still
# returns plausible backdrop names, so nothing but a test notices. Links
# bomber::assets for ValueList and the .SCH loader/writer the fixtures use.
add_executable(bomber_frontend_util_tests test_frontend_util.cpp)
target_link_libraries(bomber_frontend_util_tests
  PRIVATE bomber::assets doctest::doctest bomber::warnings)
target_include_directories(bomber_frontend_util_tests PRIVATE ${CMAKE_SOURCE_DIR}/libs/game/include)
add_test(NAME frontend_util COMMAND bomber_frontend_util_tests)

# Scheme editor's pure grid model (editor_grid.hpp/.cpp) is SDL-free despite
# living under libs/game/src. It now LINKS bomber::game_util — libs/game's
# SDL-free half — instead of recompiling editor_grid.cpp into this binary, which
# is what it did while the only target offering that source also dragged in SDL3.
# A suite that rebuilds the code under test is testing its own copy of it: the
# same shape libs/audio was fixed out of, where tests/audio recompiled
# sound_bank.cpp and so covered a file the headless build never linked.
add_executable(bomber_editor_grid_tests test_editor_grid.cpp)
target_link_libraries(bomber_editor_grid_tests
  PRIVATE bomber::game_util doctest::doctest bomber::warnings)
add_test(NAME editor_grid COMMAND bomber_editor_grid_tests)

# The package's diagnostic seam (log.hpp/.cpp, docs/coding-standards.md §11).
# Worth a suite precisely because the seam's VALUE is that output can be
# redirected: the 53 call sites it replaced could not be captured or silenced at
# all, and "the sink receives what the call site formatted" is the one property
# every one of them now depends on. Links bomber::game_util like the suite above.
add_executable(bomber_log_tests test_log.cpp)
target_link_libraries(bomber_log_tests PRIVATE bomber::game_util doctest::doctest bomber::warnings)
add_test(NAME log COMMAND bomber_log_tests)
