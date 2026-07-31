#include "bomber/assets/messages.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "text_util.hpp"

namespace bomber::assets::res {
namespace {

using text::trim;

void parse_stream(std::istream& in, Messages& m) {
    std::string raw;
    int lineno = 0;
    while (std::getline(in, raw)) {
        ++lineno;
        // Skip blanks and full-line ';' comments (leading whitespace allowed).
        const auto b = raw.find_first_not_of(text::kTrimmed);
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
            std::string val = trim(raw.substr(comma + 1));
            // MESSAGES.TXT quotes SOME payloads and not others (e.g.
            // `50,"Available players:"` vs `10,Are you sure...`); the
            // original's string loader strips ONE surrounding pair of double
            // quotes so `"%u %s to win match"` renders unquoted. Without this
            // every getstring-fed UI label showed literal quotes (caught
            // 2026-07-12 on the live setup screen).
            if (val.size() >= 2 && val.front() == '"' && val.back() == '"')
                val = val.substr(1, val.size() - 2);
            m.strings[id] = std::move(val);
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
