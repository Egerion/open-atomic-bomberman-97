// Checks for the front-end `.BM` text-screen parser. Faithful to sub_41302D
// (BM95.EXE): CRLF/EOF handling, 4-column tab stops, and the single `<IMGname>`
// inline markup. See docs/formats/bm.md.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>

#include "bomber/assets/bmtext.hpp"

using namespace bomber::assets::bmtext;

namespace {

const BmSegment& seg(const BmDocument& doc, std::size_t line, std::size_t i) {
    return doc.lines.at(line).at(i);
}

}  // namespace

TEST_CASE("text and an <IMG> tag split into the right segments") {
    // Mirrors a CREDITS.BM row: leading text, then an inline image.
    BmDocument doc = parse("Executive Producer: <IMGCREDBAR>\r\n");
    REQUIRE(doc.lines.size() == 1);
    REQUIRE(doc.lines[0].size() == 2);

    CHECK(seg(doc, 0, 0).is_text());
    CHECK(seg(doc, 0, 0).value == "Executive Producer: ");
    CHECK(seg(doc, 0, 1).is_image());
    CHECK(seg(doc, 0, 1).value == "CREDBAR");  // base name only, no extension
}

TEST_CASE("multiple images and trailing text on one line") {
    BmDocument doc = parse("<IMGpowbomb> Extra Bomb <IMGpowflame> end");
    REQUIRE(doc.lines.size() == 1);
    const BmLine& l = doc.lines[0];
    REQUIRE(l.size() == 4);
    CHECK(l[0].is_image());
    CHECK(l[0].value == "powbomb");
    CHECK(l[1].is_text());
    CHECK(l[1].value == " Extra Bomb ");
    CHECK(l[2].is_image());
    CHECK(l[2].value == "powflame");
    CHECK(l[3].is_text());
    CHECK(l[3].value == " end");
}

TEST_CASE("tabs expand to 4-column stops") {
    // 'A' at col 0, tab advances to col 4, 'B' follows: "A   B".
    BmDocument doc = parse("A\tB");
    REQUIRE(doc.lines.size() == 1);
    REQUIRE(doc.lines[0].size() == 1);
    CHECK(seg(doc, 0, 0).value == "A   B");
}

TEST_CASE("each tab leaves the column counter one ahead of the text emitted") {
    // sub_41302D's pass-2 expander shares ONE counter between the tab stop and
    // the buffer limit and bumps it once per SOURCE character, so a tab advances
    // it once more than the spaces it wrote. The next tab on the line therefore
    // resolves against an inflated column. This is the original's behaviour, not
    // a transcription slip, and the shipped column art depends on it.
    //
    // "\tJohn Price \t": leading tab -> 4 spaces, counter 5 (not 4). 11 more
    // characters -> counter 16, 15 written. The second tab runs 16 -> 20, i.e.
    // FOUR spaces; the textbook rule (col 15) would emit exactly one.
    CHECK(expand_tabs("\tJohn Price \t(for Sound)") ==
          "    John Price     (for Sound)");
    // One tab, no drift yet: identical either way.
    CHECK(expand_tabs("\tTim Cain") == "    Tim Cain");
    // A trailing tab after an odd run: counter 16 -> 20, four spaces.
    CHECK(expand_tabs("\tKaycee Vardaman\t") == "    Kaycee Vardaman    ");
    // Tabs alone: every one lands on a 4-stop from the inflated counter, so the
    // run grows 4,3,3,3... rather than a flat 4,4,4.
    CHECK(expand_tabs("\t\t\t") == "          ");
}

TEST_CASE("tab expansion counts the <IMG tag's own characters") {
    // The original expands the WHOLE raw line into its scratch buffer before the
    // render pass ever scans for "<IMG", so a tag's characters occupy columns for
    // any LATER tab on the same line.
    BmDocument doc = parse("<IMGa>\tX");
    REQUIRE(doc.lines.size() == 1);
    REQUIRE(doc.lines[0].size() == 2);
    CHECK(doc.lines[0][0].is_image());
    // "<IMGa>" is 6 characters, so the tab runs col 6 -> 8: two spaces.
    CHECK(doc.lines[0][1].value == "  X");
}

TEST_CASE("expansion stops at the original's 256-byte scratch buffer") {
    // `while (col < 255)`: no shipped .BM line expands past 63 columns, so this
    // never fires on real data — it is here so a hostile file cannot grow a row
    // without bound.
    const std::string wide(400, 'x');
    CHECK(expand_tabs(wide).size() == 255);
}

TEST_CASE("angle-bracket prose that is not <IMG stays literal text") {
    // <ESC>, <button> etc. are documented as literal in the shipped files.
    BmDocument doc = parse("press the <ESC> key");
    REQUIRE(doc.lines.size() == 1);
    REQUIRE(doc.lines[0].size() == 1);
    CHECK(seg(doc, 0, 0).is_text());
    CHECK(seg(doc, 0, 0).value == "press the <ESC> key");
}

TEST_CASE("content after the DOS EOF marker is ignored") {
    BmDocument doc = parse("visible line\n\x1agarbage after eof\nmore");
    REQUIRE(doc.lines.size() == 1);
    CHECK(seg(doc, 0, 0).value == "visible line");
}

TEST_CASE("empty and whitespace-only input") {
    // Empty file: no lines, no crash (fgets loop emits no record).
    CHECK(parse("").lines.empty());
    CHECK(parse("\x1aonly eof text").lines.empty());  // EOF marker first

    // Whitespace-only line keeps its spaces as a single text run.
    BmDocument spaces = parse("   \r\n");
    REQUIRE(spaces.lines.size() == 1);
    REQUIRE(spaces.lines[0].size() == 1);
    CHECK(spaces.lines[0][0].value == "   ");

    // A blank line between two content lines is preserved as an empty row.
    BmDocument blanks = parse("a\n\nb");
    REQUIRE(blanks.lines.size() == 3);
    CHECK(blanks.lines[1].empty());

    // A trailing newline does NOT synthesize an extra empty line.
    CHECK(parse("a\nb\n").lines.size() == 2);
}

TEST_CASE("an unterminated <IMG tag throws") {
    // No closing '>' on the line: documented strict rule (docs/formats/bm.md §2).
    CHECK_THROWS_AS(parse("intro <IMGpowbomb still going"), std::runtime_error);
    CHECK_THROWS_AS(parse("<IMG"), std::runtime_error);
}

TEST_CASE("an empty image name is an image segment with an empty value") {
    BmDocument doc = parse("<IMG>");
    REQUIRE(doc.lines.size() == 1);
    REQUIRE(doc.lines[0].size() == 1);
    CHECK(seg(doc, 0, 0).is_image());
    CHECK(seg(doc, 0, 0).value.empty());
}
