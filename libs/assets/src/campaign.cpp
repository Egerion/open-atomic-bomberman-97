#include "bomber/assets/campaign.hpp"

#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "text_util.hpp"

namespace bomber::assets::res {
namespace {

using text::trim;

// Best-effort field->int (sub_401085 parses each numeric field with atoi-like
// leniency): a field that doesn't start with a digit/sign parses to 0 rather
// than throwing — untrusted 1997 text, lenient like the rest of this parser.
int to_int(const std::string& field) { return std::atoi(field.c_str()); }

// The marker check (pseudo.c 4374): byte 0 == '-', byte 1 case-insensitively
// 'C'. Anything else (including the format's own "; Field descriptions:" header
// block) is simply not a stage line.
bool is_stage_line(const std::string& raw, std::size_t b) {
    return raw.size() >= b + 2 && raw[b] == '-' &&
           std::toupper(static_cast<unsigned char>(raw[b + 1])) == 'C';
}

// Fields start after "-C,": split on ',' into exactly 9 trimmed fields. A line
// carrying the marker but not yielding 9 of them is malformed, and the empty
// answer is what makes the caller warn rather than silently drop it.
std::vector<std::string> split_stage_fields(const std::string& raw, std::size_t b) {
    std::size_t pos = raw.find(',', b);
    if (pos == std::string::npos) return {};
    ++pos;  // past the comma following "-C"

    std::vector<std::string> fields;
    fields.reserve(9);
    while (fields.size() < 9) {
        const std::size_t comma = raw.find(',', pos);
        fields.push_back(
            trim(comma == std::string::npos ? raw.substr(pos) : raw.substr(pos, comma - pos)));
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return fields.size() == 9 ? fields : std::vector<std::string>{};
}

CampaignStage to_stage(const std::vector<std::string>& fields) {
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
    return stage;
}

void parse_stream(std::istream& in, Campaign& c) {
    std::string raw;
    int lineno = 0;
    while (std::getline(in, raw)) {
        ++lineno;
        const auto b = raw.find_first_not_of(text::kTrimmed);
        if (b == std::string::npos || raw[b] == ';') continue;  // blank / full-line comment
        if (!is_stage_line(raw, b)) continue;

        const std::vector<std::string> fields = split_stage_fields(raw, b);
        if (fields.empty()) {
            c.warnings.push_back("line " + std::to_string(lineno));
            continue;
        }
        c.stages.push_back(to_stage(fields));
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
