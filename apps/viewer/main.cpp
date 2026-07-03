// bomber_viewer — SDL3 viewer for original ANI animations.
//
// Interactive:  bomber_viewer <path to DATA/ANI or game dir>
//   Up/Down: file · Left/Right: sequence · Space: pause · +/-: speed · Esc: quit
//
// Selftest:     bomber_viewer <path> --selftest [screenshot_dir]
//   Loads every ANI, uploads every frame as a texture, renders every sequence's
//   first step, optionally saves reference screenshots. Exit 0 = all good.
//   Works headless (SDL_VIDEODRIVER=dummy) — used by CI/sandbox runs.

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

#include "bomber/assets/install.hpp"
#include "bomber/game/sprites.hpp"

namespace fs = std::filesystem;
using bomber::game::AniTextures;

namespace {

std::string to_upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

bool load_ani(SDL_Renderer* ren, const fs::path& path, AniTextures& out, std::string& err) {
    try {
        out.load(ren, path);
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
    return true;
}

void render_step(SDL_Renderer* ren, const AniTextures& ani, int seq_idx, std::size_t step_idx,
                 float cx, float cy, float scale) {
    int frame = -1, dx = 0, dy = 0;
    const auto& data = ani.data();
    if (seq_idx >= 0 && seq_idx < static_cast<int>(data.sequences.size())) {
        const auto& steps = data.sequences[seq_idx].steps;
        if (!steps.empty()) {
            const auto& st = steps[step_idx % steps.size()];
            frame = st.frame;
            dx = st.dx;
            dy = st.dy;
        }
    } else if (!data.frames.empty()) {
        frame = static_cast<int>(step_idx % data.frames.size());
    }
    if (frame < 0 || !ani.texture(static_cast<std::size_t>(frame))) return;
    const auto& f = data.frames[static_cast<std::size_t>(frame)];
    SDL_FRect dst;
    dst.w = f.image.width * scale;
    dst.h = f.image.height * scale;
    dst.x = cx + (dx - f.hotspot_x) * scale;
    dst.y = cy + (dy - f.hotspot_y) * scale;
    SDL_RenderTexture(ren, ani.texture(static_cast<std::size_t>(frame)), nullptr, &dst);
}

int run_selftest(SDL_Renderer* ren, const std::vector<fs::path>& files, const fs::path* shot_dir) {
    int errors = 0;
    std::size_t total_frames = 0, total_textures = 0, total_seqs = 0;
    const std::vector<std::string> shot_names = {"WALK.ANI", "CLASSICS.ANI", "FLAME.ANI"};

    for (const auto& path : files) {
        AniTextures ani;
        std::string err;
        if (!load_ani(ren, path, ani, err)) {
            std::printf("  ERROR %s: %s\n", path.filename().string().c_str(), err.c_str());
            ++errors;
            continue;
        }
        const auto& data = ani.data();
        total_frames += data.frames.size();
        total_seqs += data.sequences.size();
        for (std::size_t i = 0; i < data.frames.size(); ++i) {
            if (!data.frames[i].image.empty() && !ani.texture(i)) {
                std::printf("  ERROR %s: frame %zu texture upload failed: %s\n",
                            path.filename().string().c_str(), i, SDL_GetError());
                ++errors;
            } else if (ani.texture(i)) {
                ++total_textures;
            }
        }

        // Render the first step of every sequence (exercises the full path).
        for (int s = 0; s < static_cast<int>(data.sequences.size()); ++s) {
            SDL_SetRenderDrawColor(ren, 40, 44, 52, 255);
            SDL_RenderClear(ren);
            render_step(ren, ani, s, 0, 480, 360, 3.0f);
        }

        std::string fname = to_upper(path.filename().string());
        if (shot_dir && std::find(shot_names.begin(), shot_names.end(), fname) != shot_names.end()) {
            SDL_SetRenderDrawColor(ren, 40, 44, 52, 255);
            SDL_RenderClear(ren);
            render_step(ren, ani, data.sequences.empty() ? -1 : 0, 0, 480, 360, 3.0f);
            SDL_Surface* shot = SDL_RenderReadPixels(ren, nullptr);
            if (shot) {
                fs::create_directories(*shot_dir);
                std::string base = fname;
                if (auto p = base.find_last_of('.'); p != std::string::npos) base.erase(p);
                fs::path out = *shot_dir / ("viewer_" + base + ".bmp");
                if (!SDL_SaveBMP(shot, out.string().c_str())) {
                    std::printf("  ERROR screenshot %s: %s\n", out.string().c_str(),
                                SDL_GetError());
                    ++errors;
                } else {
                    std::printf("  screenshot: %s\n", out.string().c_str());
                }
                SDL_DestroySurface(shot);
            } else {
                std::printf("  ERROR SDL_RenderReadPixels: %s\n", SDL_GetError());
                ++errors;
            }
        }
    }

    std::printf("selftest: %zu files, %zu frames, %zu textures, %zu sequences — %s (%d errors)\n",
                files.size(), total_frames, total_textures, total_seqs,
                errors ? "FAILED" : "OK", errors);
    return errors ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    fs::path dir;
    bool selftest = false;
    fs::path shot_dir;
    bool have_shot_dir = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--selftest") selftest = true;
        else if (dir.empty()) dir = a;
        else { shot_dir = a; have_shot_dir = true; }
    }
    if (dir.empty()) dir = bomber::assets::default_game_dir();
    if (dir.empty()) {
        std::fprintf(stderr,
                     "usage: bomber_viewer <path to DATA/ANI or game dir> [--selftest [shot_dir]]\n"
                     "(or set BOMBER_GAME_DIR, or put the game path in gamedir.txt)\n");
        return 2;
    }
    if (fs::is_directory(dir / "DATA" / "ANI")) dir = dir / "DATA" / "ANI";

    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (e.is_regular_file() && to_upper(e.path().extension().string()) == ".ANI")
            files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        std::fprintf(stderr, "no .ANI files in %s\n", dir.string().c_str());
        return 1;
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* win = nullptr;
    SDL_Renderer* ren = nullptr;
    if (!SDL_CreateWindowAndRenderer("Open Bomberman — asset viewer", 960, 720, 0, &win, &ren)) {
        std::fprintf(stderr, "SDL_CreateWindowAndRenderer: %s\n", SDL_GetError());
        return 1;
    }

    if (selftest) {
        int rc = run_selftest(ren, files, have_shot_dir ? &shot_dir : nullptr);
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);
        SDL_Quit();
        return rc;
    }

    int file_idx = 0, seq_idx = 0;
    std::size_t step_idx = 0;
    bool paused = false;
    int step_ms = 66;
    AniTextures ani;
    std::string err;

    auto reload = [&]() {
        ani.reset();
        seq_idx = 0;
        step_idx = 0;
        err.clear();
        if (!load_ani(ren, files[file_idx], ani, err))
            std::fprintf(stderr, "load failed: %s\n", err.c_str());
        std::string title = "Open Bomberman viewer — " + files[file_idx].filename().string();
        SDL_SetWindowTitle(win, title.c_str());
    };
    reload();

    std::uint64_t last_step = SDL_GetTicks();
    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) running = false;
            if (ev.type == SDL_EVENT_KEY_DOWN) {
                switch (ev.key.key) {
                    case SDLK_ESCAPE: running = false; break;
                    case SDLK_SPACE: paused = !paused; break;
                    case SDLK_UP:
                        file_idx = (file_idx + static_cast<int>(files.size()) - 1) %
                                   static_cast<int>(files.size());
                        reload();
                        break;
                    case SDLK_DOWN:
                        file_idx = (file_idx + 1) % static_cast<int>(files.size());
                        reload();
                        break;
                    case SDLK_LEFT:
                        if (!ani.data().sequences.empty()) {
                            seq_idx = (seq_idx + static_cast<int>(ani.data().sequences.size()) -
                                       1) %
                                      static_cast<int>(ani.data().sequences.size());
                            step_idx = 0;
                        }
                        break;
                    case SDLK_RIGHT:
                        if (!ani.data().sequences.empty()) {
                            seq_idx = (seq_idx + 1) %
                                      static_cast<int>(ani.data().sequences.size());
                            step_idx = 0;
                        }
                        break;
                    case SDLK_PLUS:
                    case SDLK_EQUALS: step_ms = std::max(16, step_ms - 10); break;
                    case SDLK_MINUS: step_ms = std::min(500, step_ms + 10); break;
                    default: break;
                }
            }
        }

        std::uint64_t now = SDL_GetTicks();
        if (!paused && now - last_step >= static_cast<std::uint64_t>(step_ms)) {
            last_step = now;
            ++step_idx;
        }

        SDL_SetRenderDrawColor(ren, 40, 44, 52, 255);
        SDL_RenderClear(ren);
        render_step(ren, ani, ani.data().sequences.empty() ? -1 : seq_idx, step_idx, 960 / 2.0f,
                    720 / 2.0f, 3.0f);
        SDL_RenderPresent(ren);
        SDL_Delay(5);
    }

    ani.reset();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
