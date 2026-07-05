#include "bomber/assets/bmtext.hpp"

#include <stdexcept>

#include "bomber/assets/binary_reader.hpp"

namespace bomber::assets::bmtext {
namespace {

constexpr char kEof = '\x1a';  // DOS EOF (^Z): sub_41302D stops at the stream's EOF flag.
constexpr std::string_view kTag = "<IMG";

// Expand tabs to the next 4-column stop, mirroring sub_41302D
// (`while (col & 3) emit ' '`), and append to the current text run.
void append_expanded(std::string& out, char ch, int& col) {
    if (ch == '\t') {
        do {
            out.push_back(' ');
            ++col;
        } while (col & 3);
    } else {
        out.push_back(ch);
        ++col;
    }
}

// Split one already-newline-free line into text/image segments. `col` tracks
// the visual column so tab stops stay aligned across segments within the line.
BmLine parse_line(std::string_view line) {
    BmLine segs;
    std::string text;
    int col = 0;

    for (std::size_t i = 0; i < line.size();) {
        if (line.compare(i, kTag.size(), kTag) == 0) {
            // Flush the text accumulated before the tag (if any).
            if (!text.empty()) {
                segs.push_back(BmSegment::text(std::move(text)));
                text.clear();
            }
            std::size_t name_start = i + kTag.size();  // skip "<IMG", like `v41 += 4`.
            std::size_t close = line.find('>', name_start);
            if (close == std::string_view::npos)
                throw std::runtime_error("bmtext: unterminated <IMG tag");
            segs.push_back(BmSegment::image(std::string(line.substr(name_start, close - name_start))));
            i = close + 1;
            continue;
        }
        append_expanded(text, line[i], col);
        ++i;
    }
    if (!text.empty()) segs.push_back(BmSegment::text(std::move(text)));
    return segs;
}

}  // namespace

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
        doc.lines.push_back(parse_line(line));
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
