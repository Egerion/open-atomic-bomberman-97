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

ValueList load_values(const std::filesystem::path& path) {
    ValueList vl;
    parse_lines(path, vl.warnings, [&](int id, const std::string& value, int lineno) {
        try {
            vl.values[id] = std::stoll(value);
        } catch (const std::exception&) {
            vl.warnings.push_back(path.filename().string() + ":" + std::to_string(lineno) +
                                  ": non-numeric value '" + value + "'");
        }
    });
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
