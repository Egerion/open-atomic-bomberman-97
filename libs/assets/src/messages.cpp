#include "bomber/assets/messages.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace bomber::assets::res {
namespace {

// Trim leading/trailing ASCII whitespace, trailing CR, and the DOS EOF marker
// (0x1a) — mirroring how the id,x resource files are read (see reslist.cpp).
std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\x1a");
    if (b == std::string::npos) return {};
    auto e = s.find_last_not_of(" \t\r\x1a");
    return s.substr(b, e - b + 1);
}

void parse_stream(std::istream& in, Messages& m) {
    std::string raw;
    int lineno = 0;
    while (std::getline(in, raw)) {
        ++lineno;
        // Skip blanks and full-line ';' comments (leading whitespace allowed).
        auto b = raw.find_first_not_of(" \t\r\x1a");
        if (b == std::string::npos || raw[b] == ';') continue;
        auto comma = raw.find(',', b);
        if (comma == std::string::npos) {
            m.warnings.push_back("line " + std::to_string(lineno));
            continue;
        }
        try {
            // std::stoi skips the id token's surrounding whitespace and stops at
            // the comma; anything else is a malformed id -> warned below.
            int id = std::stoi(raw.substr(b, comma - b));
            // Everything after the FIRST comma is the message text, kept as-is
            // apart from surrounding whitespace (it may contain commas and %
            // specifiers). No ';'-truncation — the text is the payload.
            m.strings[id] = trim(raw.substr(comma + 1));
        } catch (const std::exception&) {
            m.warnings.push_back("line " + std::to_string(lineno));
        }
    }
}

}  // namespace

Messages parse_messages(std::string_view text) {
    Messages m;
    std::istringstream in{std::string(text)};
    parse_stream(in, m);
    return m;
}

Messages load_messages(const std::filesystem::path& path) {
    Messages m;
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open: " + path.string());
    parse_stream(f, m);
    return m;
}

}  // namespace bomber::assets::res
