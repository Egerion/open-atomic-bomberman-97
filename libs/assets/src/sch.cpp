#include "bomber/assets/sch.hpp"

#include <cstddef>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "text_util.hpp"

namespace bomber::assets::sch {
namespace {

using text::trim;

std::vector<std::string> split(const std::string& s, char sep, int max_parts = -1) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (true) {
        if (max_parts > 0 && static_cast<int>(out.size()) == max_parts - 1) {
            out.push_back(s.substr(start));
            break;
        }
        auto p = s.find(sep, start);
        if (p == std::string::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, p - start));
        start = p + 1;
    }
    return out;
}

// Where a directive came from — every numeric field's error text needs both.
struct LineCtx {
    const std::filesystem::path& path;
    int lineno = 0;
};

// std::stoi throws std::invalid_argument / std::out_of_range on a
// non-numeric or oversized field — both are std::logic_error, which
// violates this module's contract. Every sibling text parser (and this
// parser's own -R/-S/-P checks below) reports malformed input as
// std::runtime_error, and that is what callers catch. Re-tag each numeric
// field to a path/line-tagged std::runtime_error; valid fields are
// forwarded unchanged, so well-formed files parse byte-identically.
int to_int(const LineCtx& ctx, const std::string& field) {
    try {
        return std::stoi(field);
    } catch (const std::exception&) {
        throw std::runtime_error("scheme: bad numeric field '" + field + "' on line " +
                                 std::to_string(ctx.lineno) + ": " + ctx.path.string());
    }
}

void parse_row(const std::string& rest, const LineCtx& ctx, Scheme& sch) {
    const auto parts = split(rest, ',', 2);
    if (parts.size() != 2) throw std::runtime_error("bad -R line: " + ctx.path.string());
    sch.rows.push_back(trim(parts[1]));
}

void parse_spawn(const std::string& rest, const LineCtx& ctx, Scheme& sch) {
    const auto parts = split(rest, ',');
    if (parts.size() < 3) throw std::runtime_error("bad -S line: " + ctx.path.string());
    Spawn sp;
    sp.player = to_int(ctx, parts[0]);
    sp.x = to_int(ctx, parts[1]);
    sp.y = to_int(ctx, parts[2]);
    // 4th field = TEAM, stored as a boolean exactly like sub_403EEE's own
    // `sub_4516C1(fields[3]) != 0` — and only when the row actually carries it
    // (the original's `j == 4` arm; a three-field row leaves the slot's record
    // alone).
    if (parts.size() > 3) {
        sp.team = to_int(ctx, parts[3]) != 0 ? 1 : 0;
        sp.has_team = true;
    }
    sch.spawns.push_back(sp);
}

void parse_powerup(const std::string& rest, const LineCtx& ctx, Scheme& sch) {
    const auto parts = split(rest, ',', 6);
    if (parts.size() < 5) throw std::runtime_error("bad -P line: " + ctx.path.string());
    PowerupRule pr;
    pr.id = to_int(ctx, parts[0]);
    pr.born_with = to_int(ctx, parts[1]);
    pr.has_override = to_int(ctx, parts[2]);
    pr.override_value = to_int(ctx, parts[3]);
    pr.forbidden = to_int(ctx, parts[4]);
    if (parts.size() > 5) pr.comment = trim(parts[5]);
    sch.powerups.push_back(pr);
}

}  // namespace

Scheme load(const std::filesystem::path& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open scheme: " + path.string());

    Scheme sch;
    LineCtx ctx{path, 0};
    std::string raw;
    while (std::getline(f, raw)) {
        ++ctx.lineno;
        const std::string line = trim(raw);
        if (line.empty() || line[0] == ';') continue;
        if (line[0] != '-' || line.size() < 2) continue;

        const std::string rest = line.size() > 3 ? line.substr(3) : std::string();  // after "-X,"
        switch (line[1]) {
            case 'V': sch.version = to_int(ctx, rest); break;
            case 'N': sch.name = trim(rest); break;
            case 'B': sch.brick_density = to_int(ctx, rest); break;
            case 'R': parse_row(rest, ctx, sch); break;
            case 'S': parse_spawn(rest, ctx, sch); break;
            case 'P': parse_powerup(rest, ctx, sch); break;
            default: break;  // unknown directive: ignore, format is versioned
        }
    }

    if (sch.rows.empty()) throw std::runtime_error("scheme has no rows: " + path.string());
    for (const auto& row : sch.rows)
        if (row.size() != sch.rows[0].size())
            throw std::runtime_error("scheme rows differ in width: " + path.string());
    return sch;
}

// Serializer for the format load() parses (sub_403C16 @0x403C16,
// docs/re/results-and-options.md §5). The exe's own field grammar (verified
// against load()'s reader above, which is the port's only ground truth for
// the shipped byte layout since we do not commit .SCH samples):
//   -V,<version>
//   -N,<name>
//   -B,<density>
//   -R,<row-number>,<row text>       (one per row, row-number = its index)
//   -S,<player>,<x>,<y>,<team>       (one per spawn; 4th field optional on
//                                     read, always written — facts.md "The
//                                     .SCH -S row's 4th field is the per-slot
//                                     TEAM")
//   -P,<id>,<born_with>,<has_override>,<override_value>,<forbidden>,<comment>
// (13 rows, one per powerup kind; comment = getstring(800+id), §5's "trailing
// comment text is getstring(800+i)" — the SAME 800-block strings the
// roulette result screen uses, docs/re/goldman-roulette.md §7). We do not
// have MESSAGES.TXT text committed, so the comment is written verbatim from
// whatever PowerupRule::comment already holds (round-tripped, never
// invented) — an empty comment writes a bare "-P,..." line with no trailing
// text, which load()'s parts.size()>5 check tolerates on read-back.
// A couple of leading "; comment" lines are emitted for human-readability —
// load() skips any line starting with ';', so their exact wording is not
// part of the round-trip contract.
std::string to_text(const Scheme& scheme) {
    std::ostringstream out;
    out << "; Atomic Bomberman scheme file\n";
    out << "; written by Open Bomberman's scheme editor (docs/re/results-and-options.md #5)\n";
    out << "-V," << scheme.version << "\n";
    out << "-N," << scheme.name << "\n";
    out << "-B," << scheme.brick_density << "\n";
    for (int y = 0; y < scheme.height(); ++y) out << "-R," << y << "," << scheme.rows[y] << "\n";
    for (const auto& sp : scheme.spawns)
        out << "-S," << sp.player << "," << sp.x << "," << sp.y << "," << sp.team << "\n";
    for (const auto& pr : scheme.powerups) {
        out << "-P," << pr.id << "," << pr.born_with << "," << pr.has_override << ","
            << pr.override_value << "," << pr.forbidden;
        if (!pr.comment.empty()) out << "," << pr.comment;
        out << "\n";
    }
    return out.str();
}

void write(const Scheme& scheme, const std::filesystem::path& path) {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot write scheme: " + path.string());
    const std::string text = to_text(scheme);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!f) throw std::runtime_error("failed writing scheme: " + path.string());
}

}  // namespace bomber::assets::sch
