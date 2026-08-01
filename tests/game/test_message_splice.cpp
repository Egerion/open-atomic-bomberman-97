// THE MESSAGES.TXT SPLICES (hud_format.hpp's fmt_u / fmt_s / fmt_us).
//
// These three exist for one reason: the format string is the PLAYER'S OWN FILE.
// DATA/MESSAGES.TXT ships with the game and nothing stops a player editing it,
// so a row that says "%s" where the caller has an int — or "%z", or a bare
// trailing "%" — reaches a splice that must not read off the end and must not
// hand a wrong-typed argument to a real printf. The header says so; nothing
// pinned it. The clock half of hud_format.hpp (format_clock / clock_warning) is
// covered in test_frontend.cpp; this suite is the substitution half, which was
// untested despite being reachable from the setup, results, debug and keyremap
// screens and from game_app itself.
//
// The contract, from the header: substitute the FIRST specifier of the expected
// kind, leave any other specifier LITERAL rather than guessing, and never throw
// or over-read. "Leave it literal" is the whole design — a garbled row should
// render garbled, not crash the screen that draws it.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>

#include "bomber/game_util/hud_format.hpp"

using bomber::game::fmt_s;
using bomber::game::fmt_seq;
using bomber::game::fmt_u;
using bomber::game::fmt_us;
using bomber::game::splice_int;

TEST_CASE("fmt_u substitutes the first integer specifier, in all three spellings") {
    CHECK(fmt_u("Round %u", 3) == "Round 3");
    CHECK(fmt_u("Round %d", 3) == "Round 3");
    CHECK(fmt_u("Round %i", 3) == "Round 3");
    CHECK(fmt_u("%u wins", 12) == "12 wins");
    CHECK(fmt_u("[%u]", 0) == "[0]");
    CHECK(fmt_u("%d", -5) == "-5");
}

TEST_CASE("fmt_u substitutes ONLY the first specifier") {
    // A row with two numbers is not this function's job (fmt_us is), and
    // silently filling both with the same value would be worse than leaving the
    // second visible.
    CHECK(fmt_u("%u of %u", 3) == "3 of %u");
}

TEST_CASE("fmt_u walks flags and width, and drops them when it substitutes") {
    // The scan skips everything up to the conversion character, so a width or
    // zero-pad flag is consumed with the specifier rather than left stranded in
    // the output — but it is NOT honoured. format_clock is the function that
    // implements "%02u"; this one deliberately does not, and a caller that needs
    // padding must not reach for it.
    CHECK(fmt_u("%02u", 5) == "5");
    CHECK(fmt_u("%3d", 7) == "7");
    CHECK(fmt_u("[%-4i]", 9) == "[9]");
}

TEST_CASE("fmt_u leaves a WRONG-KIND specifier completely alone") {
    // The crash this file exists to prevent: handing an int where the user's row
    // asks for a string. The scan stops at 's' and refuses rather than
    // substituting, so the row renders with its "%s" visible.
    CHECK(fmt_u("Player %s", 4) == "Player %s");
    // A literal percent is a terminator too — "100%%" must survive untouched.
    CHECK(fmt_u("100%% done", 4) == "100%% done");
}

TEST_CASE("fmt_u survives hostile and degenerate format strings") {
    // Each of these is a row a hand-edited MESSAGES.TXT can contain, and none
    // may read past the end.
    CHECK(fmt_u("", 1).empty());
    CHECK(fmt_u("no specifier here", 1) == "no specifier here");
    CHECK(fmt_u("trailing percent %", 1) == "trailing percent %");  // '%' is the last byte
    CHECK(fmt_u("%", 1) == "%");                                    // ...and the only one
    CHECK(fmt_u("%z", 1) == "%z");                                // unknown conversion: left alone
    CHECK(fmt_u("%qqqqqqqqqqqqqqqq", 1) == "%qqqqqqqqqqqqqqqq");  // scan runs off, returns
}

TEST_CASE("fmt_s is the mirror image: %s only, never a number") {
    CHECK(fmt_s("Player %s", "Ege") == "Player Ege");
    CHECK(fmt_s("%s!", "Ege") == "Ege!");
    CHECK(fmt_s("%-10s|", "Ege") == "Ege|");  // width consumed, not honoured
    CHECK(fmt_s("Round %u", "Ege") == "Round %u");
    CHECK(fmt_s("%d", "Ege") == "%d");
    CHECK(fmt_s("%s and %s", "Ege") == "Ege and %s");  // first only
    CHECK(fmt_s("", "Ege").empty());
    CHECK(fmt_s("%", "Ege") == "%");
    // Substituting an empty string is a substitution, not a no-op.
    CHECK(fmt_s("[%s]", "") == "[]");
}

TEST_CASE("fmt_us fills a two-specifier row, numeric first") {
    CHECK(fmt_us("Player %u: %s", 2, "Ege") == "Player 2: Ege");
    CHECK(fmt_us("%u/%s", 7, "x") == "7/x");
}

TEST_CASE("fmt_us on a REORDERED row degrades gracefully - and loses the number") {
    // The header promises graceful degradation for a MESSAGES.TXT whose author
    // swapped the two specifiers. Worth pinning EXACTLY what that means, because
    // "graceful" is not "correct": fmt_u runs first, finds '%s' at the front,
    // refuses (right call - it will not print an int through %s), and returns the
    // row untouched; fmt_s then fills the '%s'. So the string lands and the
    // NUMBER IS SILENTLY DROPPED, leaving a literal "%u" on screen. That is the
    // intended trade - no crash, visibly wrong row - and if someone later
    // "fixes" fmt_u to substitute through an %s, this case is what should stop
    // them.
    CHECK(fmt_us("%s (%u)", 5, "Ege") == "Ege (%u)");
}

TEST_CASE("fmt_us degenerate rows: a missing half is left literal, not invented") {
    CHECK(fmt_us("only %u", 5, "Ege") == "only 5");
    CHECK(fmt_us("only %s", 5, "Ege") == "only Ege");
    CHECK(fmt_us("neither", 5, "Ege") == "neither");
    CHECK(fmt_us("", 5, "Ege").empty());
}

// --- the SEQUENTIAL variants, moved here from their two screen-local copies ---

TEST_CASE("splice_int fills one integer specifier per call, left to right") {
    // The score-strip row (getstring 37) and the results row (getstring 31),
    // exactly as MatchRunner::draw_player_score and the scoreboard fill them.
    std::string hud = "S:%d K:%d";
    splice_int(hud, 3);
    CHECK(hud == "S:3 K:%d");
    splice_int(hud, 7);
    CHECK(hud == "S:3 K:7");

    std::string row = fmt_u("Player %u score: %u (kills: %d)", 2);
    splice_int(row, 4);
    splice_int(row, 9);
    CHECK(row == "Player 2 score: 4 (kills: 9)");
}

TEST_CASE("splice_int never over-reads a degenerate row") {
    std::string none = "no specifier";
    splice_int(none, 1);
    CHECK(none == "no specifier");
    std::string trailing = "dangling %";
    splice_int(trailing, 1);
    CHECK(trailing == "dangling %");
    std::string empty;
    splice_int(empty, 1);
    CHECK(empty.empty());
}

TEST_CASE("splice_int scans THROUGH a wrong-kind specifier - pinned, not endorsed") {
    // Unlike fmt_u, the scan does not stop at 's' or '%': it walks from the
    // first '%' to the first u/d/i and replaces the whole span. On the shipped
    // rows (integers only) the two behave identically; on a hand-edited "%s"
    // row this one eats up to the next integer conversion character. That is
    // what BOTH original copies (match_runner's splice_int, results_screens'
    // splice_next) always did — this case pins the quirk so a future "fix"
    // is a decision, not an accident.
    std::string s = "Player %s okay";  // no u/d/i anywhere after the '%'...
    splice_int(s, 4);
    CHECK(s == "Player %s okay");  // ...so the scan runs off the end and refuses
    std::string t = "%s placed";   // here the 'd' inside "placed" is the stop
    splice_int(t, 4);
    CHECK(t == "4");  // everything from '%' through that 'd' became the number
}

TEST_CASE("fmt_seq fills each bare specifier in order and stops at what it cannot fill") {
    // The four-field modem row (getstring 264) as OptionsScreen builds it.
    CHECK(fmt_seq("Modem:  P:%u  I:%u  B:%u  #:%s", {"2", "3", "19200", "555-1212"}) ==
          "Modem:  P:2  I:3  B:19200  #:555-1212");
    // %% is a literal, never an argument slot.
    CHECK(fmt_seq("100%% at %u", {"5"}) == "100%% at 5");
    // A width flag is not a bare specifier: the walk stops rather than guesses.
    CHECK(fmt_seq("%02u then %u", {"1", "2"}) == "%02u then %u");
    // More arguments than slots: the excess is dropped, the row stays intact.
    CHECK(fmt_seq("just %u", {"1", "2", "3"}) == "just 1");
    // Fewer arguments than slots: the rest stay literal.
    CHECK(fmt_seq("%u and %u", {"1"}) == "1 and %u");
    CHECK(fmt_seq("", {"1"}).empty());
}
