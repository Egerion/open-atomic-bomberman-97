# bomber::assets suites (ADR-0008): parsers for the original 1997 file formats.
# Each links bomber::assets (its PUBLIC include dir provides the headers); none
# need the sim helpers. ctest NAMEs unchanged from the flat layout.

# .BM text-screen parser.
add_executable(bomber_bmtext_tests test_bmtext.cpp)
target_link_libraries(bomber_bmtext_tests PRIVATE bomber::assets doctest::doctest bomber::warnings)
add_test(NAME bmtext COMMAND bomber_bmtext_tests)

# .RMP player-colour remap parser.
add_executable(bomber_rmp_tests test_rmp.cpp)
target_link_libraries(bomber_rmp_tests PRIVATE bomber::assets doctest::doctest bomber::warnings)
add_test(NAME rmp COMMAND bomber_rmp_tests)

# COLOR.PAL in-match palette snap (synthetic file, no install dependency) —
# pins the RGB555->LUT->master*4 mechanics + entry-0.
add_executable(bomber_colorpal_tests test_colorpal.cpp)
target_link_libraries(bomber_colorpal_tests PRIVATE bomber::assets doctest::doctest bomber::warnings)
add_test(NAME colorpal COMMAND bomber_colorpal_tests)

# options.ini parser.
add_executable(bomber_options_tests test_options.cpp)
target_link_libraries(bomber_options_tests PRIVATE bomber::assets doctest::doctest bomber::warnings)
add_test(NAME options COMMAND bomber_options_tests)

# FON bitmap-font parser.
add_executable(bomber_bmfont_tests test_bmfont.cpp)
target_link_libraries(bomber_bmfont_tests PRIVATE bomber::assets doctest::doctest bomber::warnings)
add_test(NAME bmfont COMMAND bomber_bmfont_tests)

# VALUELST multi-column resource-list parser (getvalue/sub_412135 parity).
add_executable(bomber_valuelst_tests test_valuelst.cpp)
target_link_libraries(bomber_valuelst_tests PRIVATE bomber::assets doctest::doctest bomber::warnings)
add_test(NAME valuelst COMMAND bomber_valuelst_tests)

# In-match message table parser.
add_executable(bomber_messages_tests test_messages.cpp)
target_link_libraries(bomber_messages_tests PRIVATE bomber::assets doctest::doctest bomber::warnings)
add_test(NAME messages COMMAND bomber_messages_tests)

# .CAM campaign/stage-sequence parser.
add_executable(bomber_campaign_tests test_campaign.cpp)
target_link_libraries(bomber_campaign_tests PRIVATE bomber::assets doctest::doctest bomber::warnings)
add_test(NAME campaign COMMAND bomber_campaign_tests)

# .SCH scheme writer round-trip.
add_executable(bomber_sch_write_tests test_sch_write.cpp)
target_link_libraries(bomber_sch_write_tests PRIVATE bomber::assets doctest::doctest bomber::warnings)
add_test(NAME sch_write COMMAND bomber_sch_write_tests)

# Install locator: the probe order that decides whether a fresh copy of the
# game starts on a machine that is not the developer's.
add_executable(bomber_install_locate_tests test_install_locate.cpp)
target_link_libraries(bomber_install_locate_tests PRIVATE bomber::assets doctest::doctest
                                                          bomber::warnings)
add_test(NAME install_locate COMMAND bomber_install_locate_tests)

# .SCH loader malformed-input contract (bad numeric field -> std::runtime_error).
add_executable(bomber_sch_malformed_tests test_sch_malformed.cpp)
target_link_libraries(bomber_sch_malformed_tests PRIVATE bomber::assets doctest::doctest bomber::warnings)
add_test(NAME sch_malformed COMMAND bomber_sch_malformed_tests)

# CHFILEANI container + CIMG/RLE decoder. Synthetic files only; covers the
# valid decode of both pixel formats AND the malformed-input contract, which
# for this parser includes header fields sized to exhaust the allocator.
#
# TIMEOUT for the same reason `extra` below has one, and it is not belt-and-
# braces: measured 2026-08-01, removing the allocator bound does NOT change the
# exception type on a box whose page file can absorb a 17 GB reserve. The two
# SECURITY cases still throw std::runtime_error and still PASS -- at 11.7 s and
# 11.1 s against 0.004 s healthy. Whole suite: 0.07 s intact, 26.7 s regressed.
# 10 s sits ~140x above the healthy run and well below either regression.
add_executable(bomber_ani_tests test_ani.cpp)
target_link_libraries(bomber_ani_tests PRIVATE bomber::assets doctest::doctest bomber::warnings)
add_test(NAME ani COMMAND bomber_ani_tests)
set_tests_properties(ani PROPERTIES TIMEOUT 10)

# PCX decoder: 8-bit indexed (the original assets) and 24-bit (DATA_HD), plus
# the same untrusted-header contract as the .ANI suite above -- including the
# same timing-not-type regression shape: 0.03 s intact, 17.5 s with
# check_decodable disabled (8.1 s + 7.0 s in the two SECURITY cases), both of
# which stay green without the timeout.
add_executable(bomber_pcx_tests test_pcx.cpp)
target_link_libraries(bomber_pcx_tests PRIVATE bomber::assets doctest::doctest bomber::warnings)
add_test(NAME pcx COMMAND bomber_pcx_tests)
set_tests_properties(pcx PROPERTIES TIMEOUT 10)

# EXTRA<N>.RES stage-actor placement. The coordinate-normalisation cases in
# here regress as a HANG rather than a failure (a file-driven loop count, and
# a zero-extent board that never terminates at all), so the suite is capped:
# the timeout is what turns either into a red test instead of a stuck gate.
# The whole suite runs in milliseconds when the closed form is intact.
add_executable(bomber_extra_tests test_extra.cpp)
target_link_libraries(bomber_extra_tests PRIVATE bomber::assets doctest::doctest bomber::warnings)
add_test(NAME extra COMMAND bomber_extra_tests)
set_tests_properties(extra PROPERTIES TIMEOUT 30)
