#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace bomber::assets::res {

// Parsers for the game's commented "id,value" text resources:
//   DATA/RES/VALUELST.RES  — gameplay tuning values (id -> integer)
//   DATA/RES/SOUNDLST.RES  — sound events (id -> RSS base name)

// bugprone-exception-escape (NOLINT below) — same std::map-default-ctor false
// positive documented on bomber::assets::res::Messages (messages.hpp); this
// struct's std::map members can't avoid it without a container-type change.
struct ValueList {  // NOLINT(bugprone-exception-escape)
    // Single-value view: id -> FIRST column. This is what the sim/tuning path
    // reads (match_factory feeds every entry to Tuning::apply); its contents are
    // deliberately unchanged by the multi-column support below.
    std::map<int, std::int64_t> values;
    // Multi-column view: id -> ALL columns of that row, in file order. Some
    // VALUELST rows carry several numbers (e.g. `700,332,140,38,0` = the main
    // menu cursor's X, Y, Y-step, W — see the file's own legend). The original
    // getvalue() (sub_412135) FLATTENS these into consecutive slots, so
    // getvalue(700/701/702) are exactly columns[700][0/1/2]. Presentation code
    // (the front-end menu cursor) reads this; it never feeds Tuning::apply, so
    // it has no bearing on the sim or the golden hashes.
    std::map<int, std::vector<std::int64_t>> columns;
    std::vector<std::string> warnings;  // lines that looked like data but didn't parse

    std::int64_t at_or(int id, std::int64_t fallback) const {
        auto it = values.find(id);
        return it == values.end() ? fallback : it->second;
    }

    // One column of a multi-value row, or `fallback` if the row/column is
    // absent. `column(700, 1)` == the original's getvalue(701).
    std::int64_t column_or(int id, std::size_t index, std::int64_t fallback) const {
        auto it = columns.find(id);
        if (it == columns.end() || index >= it->second.size()) return fallback;
        return it->second[index];
    }
};

// bugprone-exception-escape (NOLINT below) — same std::map-default-ctor false
// positive documented on bomber::assets::res::Messages (messages.hpp).
struct SoundList {  // NOLINT(bugprone-exception-escape)
    std::map<int, std::string> names;  // id -> base name (as written, e.g. "bmdrop2")
    std::vector<std::string> warnings;
};

ValueList load_values(const std::filesystem::path& path);
SoundList load_sounds(const std::filesystem::path& path);

}  // namespace bomber::assets::res
