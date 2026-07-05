#include "bomber/assets/reslist.hpp"

#include <fstream>
#include <stdexcept>

namespace bomber::assets::res {
namespace {

std::string strip(const std::string& raw) {
    std::string s = raw;
    if (auto p = s.find(';'); p != std::string::npos) s.erase(p);
    // "\x1a" = DOS EOF marker; VALUELST.RES ends with one.
    auto b = s.find_first_not_of(" \t\r\x1a");
    auto e = s.find_last_not_of(" \t\r\x1a");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

template <typename Fn>
void parse_lines(const std::filesystem::path& path, std::vector<std::string>& warnings,
                 Fn&& on_pair) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open: " + path.string());
    std::string raw;
    int lineno = 0;
    while (std::getline(f, raw)) {
        ++lineno;
        std::string line = strip(raw);
        if (line.empty()) continue;
        auto comma = line.find(',');
        if (comma == std::string::npos) {
            warnings.push_back(path.filename().string() + ":" + std::to_string(lineno) + ": " +
                               line);
            continue;
        }
        try {
            int id = std::stoi(line.substr(0, comma));
            std::string rest = line.substr(comma + 1);
            auto b = rest.find_first_not_of(" \t");
            if (b != std::string::npos) rest = rest.substr(b);
            if (auto p = rest.find(','); p != std::string::npos) rest.erase(p);
            auto e = rest.find_last_not_of(" \t");
            if (e != std::string::npos) rest.erase(e + 1);
            on_pair(id, rest, lineno);
        } catch (const std::exception&) {
            warnings.push_back(path.filename().string() + ":" + std::to_string(lineno) + ": " +
                               line);
        }
    }
}

}  // namespace

// Split a comma-separated value list ("332,140, 38,  0") into trimmed tokens.
std::vector<std::string> split_values(const std::string& rest) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= rest.size()) {
        std::size_t comma = rest.find(',', start);
        std::string tok = rest.substr(start, comma == std::string::npos ? std::string::npos
                                                                         : comma - start);
        auto b = tok.find_first_not_of(" \t");
        auto e = tok.find_last_not_of(" \t");
        out.push_back(b == std::string::npos ? std::string() : tok.substr(b, e - b + 1));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

ValueList load_values(const std::filesystem::path& path) {
    ValueList vl;
    // The single-value `values` map keeps EXACTLY the first-column semantics the
    // sim relies on. In the same pass we also record every column into
    // `columns` (for the presentation-side getvalue(id+n) rows like the menu
    // cursor). Parsing both here means neither view drifts from the other.
    parse_lines(path, vl.warnings, [&](int id, const std::string& first, int lineno) {
        try {
            vl.values[id] = std::stoll(first);  // unchanged first-column value
        } catch (const std::exception&) {
            vl.warnings.push_back(path.filename().string() + ":" + std::to_string(lineno) +
                                  ": non-numeric value '" + first + "'");
        }
    });
    // Second, independent scan for the full multi-column rows. parse_lines only
    // hands back the first column, so re-read the raw file to capture the rest.
    // Kept separate so the first-column path above is byte-for-byte untouched.
    {
        std::ifstream f(path);
        std::string raw;
        int lineno = 0;
        while (f && std::getline(f, raw)) {
            ++lineno;
            std::string line = strip(raw);
            if (line.empty()) continue;
            auto comma = line.find(',');
            if (comma == std::string::npos) continue;  // already warned in pass 1
            int id = 0;
            try {
                id = std::stoi(line.substr(0, comma));
            } catch (const std::exception&) {
                continue;  // already warned in pass 1
            }
            std::vector<std::int64_t> cols;
            for (const std::string& tok : split_values(line.substr(comma + 1))) {
                if (tok.empty()) continue;
                try {
                    cols.push_back(std::stoll(tok));
                } catch (const std::exception&) {
                    // A non-numeric column ends the numeric run for this row.
                    break;
                }
            }
            if (!cols.empty()) vl.columns[id] = std::move(cols);
        }
    }
    return vl;
}

SoundList load_sounds(const std::filesystem::path& path) {
    SoundList sl;
    parse_lines(path, sl.warnings, [&](int id, const std::string& name, int) {
        if (!name.empty()) sl.names[id] = name;
    });
    return sl;
}

}  // namespace bomber::assets::res
