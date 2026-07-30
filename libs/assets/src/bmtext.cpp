#include "bomber/assets/bmtext.hpp"

#include <stdexcept>

#include "bomber/assets/binary_reader.hpp"

namespace bomber::assets::bmtext {
namespace {

constexpr char kEof = '\x1a';  // DOS EOF (^Z): sub_41302D stops at the stream's EOF flag.
constexpr std::string_view kTag = "<IMG";

// Split one already-newline-free line into text/image segments. The line has
// ALREADY been tab-expanded (expand_tabs below) — the original does the same,
// in that order.
BmLine parse_line(std::string_view line) {
    BmLine segs;
    std::string text;

    for (std::size_t i = 0; i < line.size();) {
        if (line.compare(i, kTag.size(), kTag) == 0) {
            // Flush the text accumulated before the tag (if any).
            if (!text.empty()) {
                segs.push_back(BmSegment::text(std::move(text)));
                text.clear();
            }
            std::size_t name_start = i + kTag.size();  // skip "<IMG": the original advances 4.
            std::size_t close = line.find('>', name_start);
            if (close == std::string_view::npos)
                throw std::runtime_error("bmtext: unterminated <IMG tag");
            segs.push_back(BmSegment::image(std::string(line.substr(name_start, close - name_start))));
            i = close + 1;
            continue;
        }
        text.push_back(line[i]);
        ++i;
    }
    if (!text.empty()) segs.push_back(BmSegment::text(std::move(text)));
    return segs;
}

}  // namespace

std::string expand_tabs(std::string_view line) {
    std::string out;
    int col = 0;
    for (char ch : line) {
        if (ch == '\t') {
            // At least one space, then on to the next 4-column stop:
            // `do { *dst++ = ' '; ++col; } while (col & 3);`
            do {
                out.push_back(' ');
                ++col;
            } while (col & 3);
        } else {
            out.push_back(ch);
        }
        // The shared per-source-character increment (see the header): after a
        // tab it makes `col` one MORE than the number of characters emitted,
        // which is what the shipped column art is drawn against.
        ++col;
        if (col >= 255) break;  // the original's 256-byte scratch buffer
    }
    return out;
}

BmDocument parse(std::string_view data) {
    // sub_41302D reads in text mode: everything from the first 0x1A is EOF.
    if (auto eof = data.find(kEof); eof != std::string_view::npos) data = data.substr(0, eof);

    BmDocument doc;
    // One record per fgets-style line. A trailing newline does NOT synthesize an
    // extra empty line, and an empty screen yields no lines (both match the
    // original's read loop, which never emits a record for the final newline).
    std::size_t start = 0;
    while (start < data.size()) {
        std::size_t nl = data.find('\n', start);
        std::size_t end = (nl == std::string_view::npos) ? data.size() : nl;
        std::string_view line = data.substr(start, end - start);
        // fgets keeps the CR of a CRLF pair only until the '\n' cut; strip it so
        // segments never carry a trailing carriage return.
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        // Tabs first, over the whole raw line; tag splitting second — the
        // original's order, and the reason a tag's characters count toward a
        // later tab's column (expand_tabs' header comment).
        const std::string expanded = expand_tabs(line);
        doc.lines.push_back(parse_line(expanded));
        if (nl == std::string_view::npos) break;
        start = nl + 1;
    }
    return doc;
}

BmDocument load(const std::filesystem::path& path) {
    auto buf = read_file(path);
    return parse(std::string_view(reinterpret_cast<const char*>(buf.data()), buf.size()));
}

}  // namespace bomber::assets::bmtext
