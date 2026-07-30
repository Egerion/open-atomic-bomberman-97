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
// The rule is NOT the textbook "advance to the next multiple of 4". The
// original keeps ONE counter for both the tab stop and the 255-character buffer
// limit, and increments it once per SOURCE character on top of the once-per-
// emitted-space the tab branch already did — so every tab leaves the counter one
// column AHEAD of the text actually written, and each further tab on the same
// line resolves against that inflated column. The drift is the original's, and
// the shipped files were authored against it: in `CREDITS.BM` the three
// "(for ...)" annotations land at x = 179 / 189 / 190 px under this rule (the
// third line has no tab before its parenthesis, so it is a fixed anchor), and at
// 191 / 153 / 190 under the textbook rule — i.e. the textbook rule tears the
// middle row 37 px out of the column the file is drawing.
//
// Expansion runs over the WHOLE raw line, `<IMGname>` tags included, BEFORE any
// tag scanning — so a tag's own characters count toward a later tab's column.
// The 255 cap is the original's 256-byte scratch buffer (no shipped `.BM` line
// expands past 63, so it never fires on real data).
std::string expand_tabs(std::string_view line);

// Parse an in-memory `.BM` screen. `data` may contain CRLF endings and a
// trailing DOS EOF (0x1A); both are handled the way sub_41302D handles them.
// Throws std::runtime_error on a malformed tag (an unterminated `<IMG`).
BmDocument parse(std::string_view data);

// Convenience loader: read the file at `path` and parse it. Reuses the assets
// read_file helper (throws std::runtime_error when the file cannot be opened).
BmDocument load(const std::filesystem::path& path);

}  // namespace bomber::assets::bmtext
