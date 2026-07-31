#pragma once

#include <SDL3/SDL.h>

#include <memory>

// Minimal RAII wrappers around the SDL3 objects the game owns. Keeps SDL
// lifetime management out of the application logic.

namespace bomber::game {
// sprites.cpp — destroys a texture AND drops it from the scaling-filter registry
// (sprites.hpp "THE SCALING FILTER"). Forward-declared rather than included so
// this RAII header keeps depending on nothing but SDL; every TexturePtr owner in
// the codebase holds a make_texture() texture, so the deleter must go through it
// or set_scale_filter would later stamp a freed pointer.
void destroy_texture(SDL_Texture* tex);
}  // namespace bomber::game

namespace bomber::game::sdl {

struct WindowDeleter {
    void operator()(SDL_Window* w) const {
        if (w) SDL_DestroyWindow(w);
    }
};
struct RendererDeleter {
    void operator()(SDL_Renderer* r) const {
        if (r) SDL_DestroyRenderer(r);
    }
};
struct TextureDeleter {
    void operator()(SDL_Texture* t) const { bomber::game::destroy_texture(t); }
};

using WindowPtr = std::unique_ptr<SDL_Window, WindowDeleter>;
using RendererPtr = std::unique_ptr<SDL_Renderer, RendererDeleter>;
using TexturePtr = std::unique_ptr<SDL_Texture, TextureDeleter>;

// Owns SDL video initialization; SDL_Quit runs on destruction. Gamepad support
// is initialized alongside video (SDL_INIT_GAMEPAD) so GamepadMapper can
// enumerate/open sticks and the setup screen can hotplug-detect them without a
// separate subsystem lifetime to manage.
class VideoSubsystem {
public:
    VideoSubsystem() : ok_(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {}
    ~VideoSubsystem() {
        if (ok_) SDL_Quit();
    }
    VideoSubsystem(const VideoSubsystem&) = delete;
    VideoSubsystem& operator=(const VideoSubsystem&) = delete;

    bool ok() const { return ok_; }

private:
    bool ok_ = false;
};

}  // namespace bomber::game::sdl
