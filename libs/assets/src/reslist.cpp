#include "bomber/assets/reslist.hpp"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "text_util.hpp"

namespace bomber::assets::res {
namespace {

std::string strip(const std::string& raw) {
    // ';' starts a comment; text::trim then takes the surrounding blanks and
    // the DOS EOF marker VALUELST.RES ends with.
    const auto semi = raw.find(';');
    return text::trim(semi == std::string::npos ? std::string_view(raw)
                                                : std::string_view(raw).substr(0, semi));
}

// Within a row the fields are separated by spaces and tabs only — a '\r' or a
// 0x1a can never survive strip() above, so this deliberately uses the NARROWER
// set rather than text::kTrimmed.
constexpr std::string_view kFieldBlanks = " \t";

// Split a comma-separated value list ("332,140, 38,  0") into trimmed tokens.
std::vector<std::string> split_values(const std::string& rest) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= rest.size()) {
        const std::size_t comma = rest.find(',', start);
        const std::size_t len = comma == std::string::npos ? std::string::npos : comma - start;
        const std::string tok = rest.substr(start, len);
        const auto b = tok.find_first_not_of(kFieldBlanks);
        const auto e = tok.find_last_not_of(kFieldBlanks);
        out.push_back(b == std::string::npos ? std::string() : tok.substr(b, e - b + 1));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

// The FIRST comma-separated token of a row: the single value the sim's tuning
// path and the sound table read. Deliberately not split_values(rest).front():
// this leaves an all-blank field ALONE where split_values would collapse it to
// "", and load_sounds distinguishes the two.
std::string first_token(std::string rest) {
    if (const auto b = rest.find_first_not_of(kFieldBlanks); b != std::string::npos)
        rest = rest.substr(b);
    if (const auto p = rest.find(','); p != std::string::npos) rest.erase(p);
    if (const auto e = rest.find_last_not_of(kFieldBlanks); e != std::string::npos)
        rest.erase(e + 1);
    return rest;
}

// One "<id>,<rest>" line. `first` is the leading token; `rest` is everything
// after the id's comma, so a caller that wants the whole multi-column row does
// not have to re-read the file to get it — two independent scans used to.
struct Entry {
    int id = 0;
    std::string first;
    std::string rest;
    int lineno = 0;
};

template <typename Fn>
void parse_lines(const std::filesystem::path& path, std::vector<std::string>& warnings,
                 Fn&& on_entry) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open: " + path.string());
    std::string raw;
    int lineno = 0;
    while (std::getline(f, raw)) {
        ++lineno;
        const std::string line = strip(raw);
        if (line.empty()) continue;
        const auto comma = line.find(',');
        if (comma == std::string::npos) {
            warnings.push_back(path.filename().string() + ":" + std::to_string(lineno) + ": " +
                               line);
            continue;
        }
        try {
            Entry e;
            e.lineno = lineno;
            e.id = std::stoi(line.substr(0, comma));
            e.rest = line.substr(comma + 1);
            e.first = first_token(e.rest);
            on_entry(e);
        } catch (const std::exception&) {
            warnings.push_back(path.filename().string() + ":" + std::to_string(lineno) + ": " +
                               line);
        }
    }
}

// Every numeric column of a row, stopping at the first field that is not a
// number (a trailing legend word ends the run, it does not invalidate the row).
std::vector<std::int64_t> numeric_columns(const std::string& rest) {
    std::vector<std::int64_t> cols;
    for (const std::string& tok : split_values(rest)) {
        if (tok.empty()) continue;
        try {
            cols.push_back(std::stoll(tok));
        } catch (const std::exception&) {
            break;
        }
    }
    return cols;
}

}  // namespace

ValueList load_values(const std::filesystem::path& path) {
    ValueList vl;
    // The single-value `values` map keeps EXACTLY the first-column semantics the
    // sim relies on. In the same pass we also record every column into `columns`
    // (for the presentation-side getvalue(id+n) rows like the menu cursor).
    // Parsing both here means neither view can drift from the other — this used
    // to be two independent scans of the same file, each carrying its own copy
    // of the strip / find-comma / parse-id preamble.
    parse_lines(path, vl.warnings, [&](const Entry& e) {
        try {
            vl.values[e.id] = std::stoll(e.first);  // unchanged first-column value
        } catch (const std::exception&) {
            vl.warnings.push_back(path.filename().string() + ":" + std::to_string(e.lineno) +
                                  ": non-numeric value '" + e.first + "'");
        }
        if (std::vector<std::int64_t> cols = numeric_columns(e.rest); !cols.empty())
            vl.columns[e.id] = std::move(cols);
    });
    return vl;
}

SoundList load_sounds(const std::filesystem::path& path) {
    SoundList sl;
    parse_lines(path, sl.warnings, [&](const Entry& e) {
        if (!e.first.empty()) sl.names[e.id] = e.first;
    });
    return sl;
}

}  // namespace bomber::assets::res
