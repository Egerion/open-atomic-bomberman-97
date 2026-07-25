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
