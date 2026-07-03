#pragma once

#include <filesystem>

namespace bomber::assets {

// Locates the player's original Atomic Bomberman installation. Probes, in
// order: the BOMBER_GAME_DIR environment variable, a gamedir.txt in the
// working directory, then the standard install locations. Returns an empty
// path when nothing is found.
std::filesystem::path default_game_dir();

}  // namespace bomber::assets
