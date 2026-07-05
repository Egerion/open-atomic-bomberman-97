#pragma once

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
    enum class Kind { Text, Image };
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

// Parse an in-memory `.BM` screen. `data` may contain CRLF endings and a
// trailing DOS EOF (0x1A); both are handled the way sub_41302D handles them.
// Throws std::runtime_error on a malformed tag (an unterminated `<IMG`).
BmDocument parse(std::string_view data);

// Convenience loader: read the file at `path` and parse it. Reuses the assets
// read_file helper (throws std::runtime_error when the file cannot be opened).
BmDocument load(const std::filesystem::path& path);

}  // namespace bomber::assets::bmtext
