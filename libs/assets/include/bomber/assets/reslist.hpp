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

struct ValueList {
    std::map<int, std::int64_t> values;
    std::vector<std::string> warnings;  // lines that looked like data but didn't parse

    std::int64_t at_or(int id, std::int64_t fallback) const {
        auto it = values.find(id);
        return it == values.end() ? fallback : it->second;
    }
};

struct SoundList {
    std::map<int, std::string> names;  // id -> base name (as written, e.g. "bmdrop2")
    std::vector<std::string> warnings;
};

ValueList load_values(const std::filesystem::path& path);
SoundList load_sounds(const std::filesystem::path& path);

}  // namespace bomber::assets::res
