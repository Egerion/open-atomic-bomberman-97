#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace bomber::assets::bmtext {

// Parser for the front-end `*.BM` text screens (credits, manual, options help,
// low/high-memory notes). Plain ASCII, one screen row per line, with a single
// inline markup form `<IMGname>` for an embedded image. See docs/formats/bm.md;
// the original renderer is sub_41302D (BM95.EXE, imagebase 0x400000).

// One run within a line: either literal text or an inline image reference.
struct BmSegment {
    enum class Kind : std::uint8_t { Text, Image };
    Kind kind = Kind::Text;
    // Text: the run's characters, with tabs already expanded to 4-column stops.
    // Image: the base name between `<IMG` and `>` (no extension), used verbatim
    //        to look up the asset (e.g. "CREDBAR" -> CREDBAR.PCX / CREDBAR.PLT).
    std::string value;

    bool is_text() const { return kind == Kind::Text; }
    bool is_image() const { return kind == Kind::Image; }

    static BmSegment text(std::string s) { return {Kind::Text, std::move(s)}; }
    static BmSegment image(std::string s) { return {Kind::Image, std::move(s)}; }
};

// One screen row: an ordered list of text/image segments, laid out left to
// right. An empty line is a row with no segments (blank line on screen).
using BmLine = std::vector<BmSegment>;

// A full screen: rows top to bottom, in file order.
struct BmDocument {
    std::vector<BmLine> lines;
};

// Tab expansion, exactly as sub_41302D's pass-2 scratch-buffer loop does it.
//
// NOT the textbook "advance to the next multiple of 4". The original shares ONE
// counter between the tab stop and its 255-character buffer limit and bumps it
// once per SOURCE character on top of the once-per-emitted-space the tab branch
// already did, so every tab leaves the counter a column AHEAD of the text
// written and each later tab on the line resolves against that inflated column.
//
// The drift is the original's and the shipped files were authored against it: in
// CREDITS.BM the textbook rule tears the middle "(for ...)" annotation 37 px out
// of the column the file is drawing.
//
// Expansion runs over the WHOLE raw line, `<IMGname>` tags included, BEFORE any
// tag scanning — so a tag's own characters count toward a later tab's column.
std::string expand_tabs(std::string_view line);

// CRLF endings and a trailing DOS EOF (0x1A) are handled as sub_41302D handles
// them. Throws std::runtime_error on an unterminated `<IMG` tag.
BmDocument parse(std::string_view data);
BmDocument load(const std::filesystem::path& path);

}  // namespace bomber::assets::bmtext
