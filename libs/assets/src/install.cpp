#include "bomber/assets/install.hpp"

#include <cstdlib>
#include <fstream>
#include <string>

namespace bomber::assets {

namespace fs = std::filesystem;

fs::path default_game_dir() {
    if (const char* env = std::getenv("BOMBER_GAME_DIR"); env && *env && fs::is_directory(env))
        return env;
    if (std::ifstream f("gamedir.txt"); f) {
        std::string line;
        if (std::getline(f, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (!line.empty() && fs::is_directory(line)) return line;
        }
    }
    for (const char* p : {"D:/Program Files (x86)/INTRPLAY/BOMBRMAN",
                          "C:/Program Files (x86)/INTRPLAY/BOMBRMAN", "./BOMBRMAN"}) {
        if (fs::is_directory(p)) return p;
    }
    return {};
}

}  // namespace bomber::assets
