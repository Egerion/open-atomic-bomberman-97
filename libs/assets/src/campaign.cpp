#include "bomber/assets/campaign.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace bomber::assets::res {
namespace {

// Trim leading/trailing ASCII whitespace, trailing CR, and the DOS EOF marker
// (0x1a) — same convention as messages.cpp / reslist.cpp.
std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\x1a");
    if (b == std::string::npos) return {};
    auto e = s.find_last_not_of(" \t\r\x1a");
    return s.substr(b, e - b + 1);
}

// Best-effort field->int (sub_401085 parses each numeric field with atoi-like
// leniency): a field that doesn't start with a digit/sign parses to 0 rather
// than throwing — untrusted 1997 text, lenient like the rest of this parser.
int to_int(const std::string& field) { return std::atoi(field.c_str()); }

void parse_stream(std::istream& in, Campaign& c) {
    std::string raw;
    int lineno = 0;
    while (std::getline(in, raw)) {
        ++lineno;
        auto b = raw.find_first_not_of(" \t\r\x1a");
        if (b == std::string::npos || raw[b] == ';') continue;  // blank / full-line comment

        // The marker check (pseudo.c 4374): byte 0 == '-', byte 1 case-
        // insensitively 'C'. Anything else (including the format's own
        // "; Field descriptions:" header block) is simply not a stage line.
        if (raw.size() < b + 2 || raw[b] != '-' ||
            std::toupper(static_cast<unsigned char>(raw[b + 1])) != 'C')
            continue;

        // Fields start after "-C,": split on ',' into exactly 9 trimmed
        // fields. A line starting with the marker but not yielding 9 fields
        // is malformed -> warned, not silently dropped.
        std::size_t pos = raw.find(',', b);
        if (pos == std::string::npos) {
            c.warnings.push_back("line " + std::to_string(lineno));
            continue;
        }
        ++pos;  // past the comma following "-C"

        std::vector<std::string> fields;
        fields.reserve(9);
        while (fields.size() < 9) {
            std::size_t comma = raw.find(',', pos);
            std::string field = (comma == std::string::npos) ? raw.substr(pos) : raw.substr(pos, comma - pos);
            fields.push_back(trim(field));
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        if (fields.size() != 9) {
            c.warnings.push_back("line " + std::to_string(lineno));
            continue;
        }

        CampaignStage stage;
        stage.name = fields[0];
        stage.level_no = to_int(fields[1]);
        stage.scheme = fields[2];
        stage.rovers = to_int(fields[3]);
        stage.rover_speed = to_int(fields[4]);
        stage.ghosts = to_int(fields[5]);
        stage.ghost_speed = to_int(fields[6]);
        stage.ai_count = to_int(fields[7]);
        stage.ai_difficulty = to_int(fields[8]);
        c.stages.push_back(std::move(stage));
    }
}

}  // namespace

Campaign parse_campaign(std::string_view text) {
    Campaign c;
    std::istringstream in{std::string(text)};
    parse_stream(in, c);
    return c;
}

Campaign load_campaign(const std::filesystem::path& path) {
    Campaign c;
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open: " + path.string());
    parse_stream(f, c);
    return c;
}

}  // namespace bomber::assets::res
