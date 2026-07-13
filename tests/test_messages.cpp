#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "bomber/assets/messages.hpp"

using bomber::assets::res::parse_messages;

// NB: all sample text here is SYNTHETIC — the real MESSAGES.TXT is the user's
// game data and is neither embedded nor committed. These only exercise the
// "<id>,<text>" grammar.

TEST_CASE("messages: id,text with comments, commas, and % specifiers") {
    auto m = parse_messages(
        "; a full-line comment\n"
        "\n"
        "1,Hello world\n"
        "  2 ,  Player %u wins\n"   // id + text both surrounded by spaces
        "3,a,b,c has commas\n"      // commas inside the text are kept
        ";another comment\n");
    CHECK(m.get_or(1) == "Hello world");
    CHECK(m.get_or(2) == "Player %u wins");        // trimmed, % preserved
    CHECK(m.get_or(3) == "a,b,c has commas");      // interior commas survive
    CHECK(m.get_or(999, "fallback") == "fallback");
    CHECK(m.find(1) != nullptr);
    CHECK(m.find(4) == nullptr);
    CHECK(m.strings.size() == 3);
}

TEST_CASE("messages: one surrounding pair of double-quotes is stripped") {
    // The real MESSAGES.TXT quotes SOME payloads and not others; getstring
    // strips exactly one surrounding pair (so `"%u %s to win match"` renders
    // unquoted) but leaves interior quotes and unquoted text alone.
    auto m = parse_messages(
        "50,\"Available players:\"\n"
        "211,\"%u %s to win match\"\n"
        "10,Are you sure you want to exit?\n"      // unquoted stays as-is
        "77,\"leading only\n"                       // lone quote: not a pair
        "78,say \"hi\" there\n");                   // interior quotes kept
    CHECK(m.get_or(50) == "Available players:");
    CHECK(m.get_or(211) == "%u %s to win match");
    CHECK(m.get_or(10) == "Are you sure you want to exit?");
    CHECK(m.get_or(77) == "\"leading only");
    CHECK(m.get_or(78) == "say \"hi\" there");
}

TEST_CASE("messages: blank/comment lines skipped, malformed lines warned") {
    auto m = parse_messages(";c\n\nnocomma line\n5,ok\n");
    CHECK(m.strings.size() == 1);
    CHECK(m.get_or(5) == "ok");
    CHECK(!m.warnings.empty());  // "nocomma line" has no comma -> warned
}

TEST_CASE("messages: empty input is empty, no crash") {
    auto m = parse_messages("");
    CHECK(m.strings.empty());
    CHECK(m.warnings.empty());
}
