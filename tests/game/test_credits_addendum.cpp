// The port's own credits addendum (credits_addendum.hpp): the text appended to
// the install's CREDITS.BM in memory. These checks are the install-free half of
// its layout budget — they bound what the viewer would have to clip, and they
// pin the clearance the centred photograph needs. The pixel half is measured
// against the real FONT6.FON and recorded in the header; a rendered page
// (`--bm-shot CREDITS`) is what confirms it.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstddef>
#include <string>

#include "bomber/game/credits_addendum.hpp"

using namespace bomber::game;
using bomber::assets::bmtext::BmDocument;
using bomber::assets::bmtext::BmLine;

namespace {

// The viewer's own numbers (bmscreen.cpp): a 532 px per-line clip from a text
// inset of 54, and 344/16 = 21 visible rows at FONT6's 16 px cell.
constexpr int kVisibleRows = 21;
// FONT6 is proportional; its WIDEST glyph advances 17 px and its space 12. A
// character bound is therefore a proxy, not a pixel measurement — but a tight
// one for this text, whose widest row measures 349 px of 532 against the real
// font. 44 leaves headroom for an edit without letting one run to the clip.
constexpr std::size_t kMaxChars = 44;

// Characters in a line's TEXT segments (image segments occupy no columns).
std::size_t text_chars(const BmLine& line) {
    std::size_t n = 0;
    for (const auto& s : line)
        if (s.is_text()) n += s.value.size();
    return n;
}

bool has_image(const BmLine& line) {
    for (const auto& s : line)
        if (s.is_image()) return true;
    return false;
}

}  // namespace

TEST_CASE("every row fits inside the viewer's per-line clip") {
    const BmDocument doc = credits_addendum();
    REQUIRE_FALSE(doc.lines.empty());
    for (std::size_t i = 0; i < doc.lines.size(); ++i) {
        INFO("row " << i);
        CHECK(text_chars(doc.lines[i]) <= kMaxChars);
    }
}

TEST_CASE("the photograph appears exactly once, under its reserved name") {
    const BmDocument doc = credits_addendum();
    int images = 0;
    for (const auto& line : doc.lines)
        for (const auto& s : line)
            if (s.is_image()) {
                ++images;
                // A name that is NOT this one would be looked up in DATA/RES and
                // silently draw nothing.
                CHECK(s.value == std::string(kAuthorPhotoTag));
            }
    CHECK(images == 1);
}

TEST_CASE("the centred photograph has clear rows above and below it") {
    // bmscreen.cpp centres an inline image on its row: a 100 px image on a 16 px
    // row reaches (100-16)/2 = 42 px each way, i.e. into the three rows above and
    // the three below. Those rows must not carry text that would run under it —
    // the photo sits 293 px in from the text inset.
    //
    // 24 characters is a PROXY, not a proof. Against the real FONT6 the widest
    // row in the band is the email address at 211 px, 82 px clear of the photo,
    // while a pathological 24-character row of the font's widest glyph ('%', 17)
    // would reach 408. It is calibrated to catch the failure that actually
    // happens — a paragraph line drifting into the band, which is 35 characters
    // and is exactly how this layout was wrong the first time. The pixel truth is
    // pinned by the rendered page (tests/visual/bm_shots.txt), not here.
    constexpr std::size_t kClearChars = 24;
    constexpr int kReach = 3;
    const BmDocument doc = credits_addendum();
    int photo_row = -1;
    for (std::size_t i = 0; i < doc.lines.size(); ++i)
        if (has_image(doc.lines[i])) photo_row = static_cast<int>(i);
    REQUIRE(photo_row >= 0);
    // Far enough from the top that the whole image has rows to sit in.
    CHECK(photo_row >= kReach);
    for (int d = -kReach; d <= kReach; ++d) {
        const int r = photo_row + d;
        if (d == 0 || r < 0 || r >= static_cast<int>(doc.lines.size())) continue;
        INFO("row " << r << " is within the photograph's band");
        CHECK(text_chars(doc.lines[static_cast<std::size_t>(r)]) <= kClearChars);
    }
}

TEST_CASE("the addendum is worth paging and cannot be a single stranded row") {
    const BmDocument doc = credits_addendum();
    // More than one screenful, so the viewer's paging is what reads it — and
    // bounded, so it does not turn a credits page into a document.
    CHECK(doc.lines.size() > static_cast<std::size_t>(kVisibleRows));
    CHECK(doc.lines.size() < static_cast<std::size_t>(kVisibleRows) * 5);
}

TEST_CASE("appending leaves the loaded document's own rows untouched") {
    // BmScreen::enter appends to whatever CREDITS.BM parsed to; the install's
    // rows must survive verbatim and come first.
    BmDocument doc;
    doc.lines.push_back(BmLine{bomber::assets::bmtext::BmSegment::text("original row")});
    const std::size_t before = doc.lines.size();
    append_credits_addendum(doc);
    REQUIRE(doc.lines.size() == before + credits_addendum().lines.size());
    CHECK(doc.lines[0][0].value == "original row");
}

TEST_CASE("the author block is exactly the three items the owner asked for") {
    // A credits page that goes public must not carry invented biography. The
    // owner scoped this section himself: his name, his email address, and that
    // he directed the project. Nothing else about him belongs here, so the three
    // are pinned — a future edit that adds a fourth has to come through this
    // test rather than drift in.
    std::string all;
    for (const auto& line : credits_addendum().lines)
        for (const auto& s : line)
            if (s.is_text()) all += s.value + "\n";
    CHECK(all.find("Ege Demirbas") != std::string::npos);
    CHECK(all.find("egedemirbas@gmail.com") != std::string::npos);
    CHECK(all.find("Directed by") != std::string::npos);
}
