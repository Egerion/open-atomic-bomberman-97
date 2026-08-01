#pragma once

#include <SDL3/SDL.h>

#include <filesystem>

namespace bomber::game {

// The install's own BM95.ICO as an RGBA surface for SDL_SetWindowIcon, matching the
// native's window/taskbar icon (sub_41095A sets it from the EXE's icon resource,
// which BM95.ICO mirrors). Runtime-loaded like every other asset, never committed.
// Returns nullptr on any malformation, leaving the window icon-less; caller owns it.
SDL_Surface* load_window_icon(const std::filesystem::path& ico_path);

}  // namespace bomber::game
