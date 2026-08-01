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
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "bomber/assets/install.hpp"
#include "bomber/render/sprites.hpp"

namespace fs = std::filesystem;
using bomber::game::AniTextures;

namespace {

constexpr int kWindowWidth = 960;
constexpr int kWindowHeight = 720;
// The viewer draws at a fixed 3x so the original's small cells are legible; the
// selftest renders at the same zoom and centre so its reference shots match
// what the interactive window shows.
constexpr float kZoom = 3.0f;
constexpr float kCentreX = kWindowWidth / 2.0f;
constexpr float kCentreY = kWindowHeight / 2.0f;

std::string to_upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// Returns the load error, or an empty string on success. A bool paired with an
// out-parameter said the same thing twice, and every caller had to declare the
// string before it knew whether it would be used.
std::string load_ani(SDL_Renderer* ren, const fs::path& path, AniTextures& out) {
    try {
        out.load(ren, path);
    } catch (const std::exception& e) {
        return e.what();
    }
    return {};
}

// Which step of which animation to draw. A negative or out-of-range `seq` falls
// back to walking the raw frame list, which is how an ANI carrying no sequences
// at all stays browsable.
struct StepRef {
    const AniTextures& ani;
    int seq = -1;
    std::size_t step = 0;
};

// Where the frame lands: the point its hotspot is anchored to, and the zoom.
struct Placement {
    float cx = kCentreX;
    float cy = kCentreY;
    float scale = kZoom;
};

void render_step(SDL_Renderer* ren, const StepRef& ref, const Placement& at) {
    int frame = -1, dx = 0, dy = 0;
    const auto& data = ref.ani.data();
    if (ref.seq >= 0 && ref.seq < static_cast<int>(data.sequences.size())) {
        const auto& steps = data.sequences[ref.seq].steps;
        if (!steps.empty()) {
            const auto& st = steps[ref.step % steps.size()];
            frame = st.frame;
            dx = st.dx;
            dy = st.dy;
        }
    } else if (!data.frames.empty()) {
        frame = static_cast<int>(ref.step % data.frames.size());
    }
    if (frame < 0 || !ref.ani.texture(static_cast<std::size_t>(frame))) return;
    const auto& f = data.frames[static_cast<std::size_t>(frame)];
    SDL_FRect dst;
    dst.w = f.image.width * at.scale;
    dst.h = f.image.height * at.scale;
    dst.x = at.cx + (dx - f.hotspot_x) * at.scale;
    dst.y = at.cy + (dy - f.hotspot_y) * at.scale;
    SDL_RenderTexture(ren, ref.ani.texture(static_cast<std::size_t>(frame)), nullptr, &dst);
}

void clear_frame(SDL_Renderer* ren) {
    SDL_SetRenderDrawColor(ren, 40, 44, 52, 255);
    SDL_RenderClear(ren);
}

// The ANIs the selftest saves a reference shot of. Every file is still loaded
// and rendered; these three are the ones a human eyeballs afterwards.
constexpr std::array<std::string_view, 3> kShotNames{"WALK.ANI", "CLASSICS.ANI", "FLAME.ANI"};

struct TextureCheck {
    std::size_t uploaded = 0;
    int errors = 0;
};

// Every frame that decoded to pixels must also have produced a texture; a null
// one is a silent GPU upload failure that the interactive viewer would show as
// a missing sprite rather than as an error.
TextureCheck check_textures(const AniTextures& ani, const fs::path& path) {
    TextureCheck out;
    const auto& data = ani.data();
    for (std::size_t i = 0; i < data.frames.size(); ++i) {
        if (!data.frames[i].image.empty() && !ani.texture(i)) {
            std::printf("  ERROR %s: frame %zu texture upload failed: %s\n",
                        path.filename().string().c_str(), i, SDL_GetError());
            ++out.errors;
        } else if (ani.texture(i)) {
            ++out.uploaded;
        }
    }
    return out;
}

// Draw the first step of every sequence. Nothing is read back — the point is to
// exercise the whole decode-to-blit path and let SDL report a failure.
void render_every_sequence(SDL_Renderer* ren, const AniTextures& ani) {
    const std::size_t count = ani.data().sequences.size();
    for (int s = 0; s < static_cast<int>(count); ++s) {
        clear_frame(ren);
        render_step(ren, StepRef{ani, s, 0}, Placement{});
    }
}

// "WALK.ANI" under <dir> becomes "<dir>/viewer_WALK.bmp".
fs::path reference_shot_path(const fs::path& shot_dir, const std::string& fname) {
    std::string base = fname;
    if (auto p = base.find_last_of('.'); p != std::string::npos) base.erase(p);
    return shot_dir / ("viewer_" + base + ".bmp");
}

// Re-renders the first sequence's first step before reading pixels back: the
// sweep above leaves whichever sequence happened to come last on the target, so
// the shot would otherwise depend on the file's sequence count.
int save_reference_shot(SDL_Renderer* ren, const AniTextures& ani, const fs::path& out) {
    clear_frame(ren);
    render_step(ren, StepRef{ani, ani.data().sequences.empty() ? -1 : 0, 0}, Placement{});
    SDL_Surface* shot = SDL_RenderReadPixels(ren, nullptr);
    if (!shot) {
        std::printf("  ERROR SDL_RenderReadPixels: %s\n", SDL_GetError());
        return 1;
    }
    fs::create_directories(out.parent_path());
    int errors = 0;
    if (!SDL_SaveBMP(shot, out.string().c_str())) {
        std::printf("  ERROR screenshot %s: %s\n", out.string().c_str(), SDL_GetError());
        ++errors;
    } else {
        std::printf("  screenshot: %s\n", out.string().c_str());
    }
    SDL_DestroySurface(shot);
    return errors;
}

struct SelftestTotals {
    std::size_t frames = 0;
    std::size_t textures = 0;
    std::size_t seqs = 0;
    int errors = 0;
};

int run_selftest(SDL_Renderer* ren, const std::vector<fs::path>& files, const fs::path* shot_dir) {
    SelftestTotals total;
    for (const auto& path : files) {
        AniTextures ani;
        if (const std::string err = load_ani(ren, path, ani); !err.empty()) {
            std::printf("  ERROR %s: %s\n", path.filename().string().c_str(), err.c_str());
            ++total.errors;
            continue;
        }
        total.frames += ani.data().frames.size();
        total.seqs += ani.data().sequences.size();
        const TextureCheck check = check_textures(ani, path);
        total.textures += check.uploaded;
        total.errors += check.errors;
        render_every_sequence(ren, ani);

        const std::string fname = to_upper(path.filename().string());
        const bool wanted =
            std::find(kShotNames.begin(), kShotNames.end(), fname) != kShotNames.end();
        if (shot_dir && wanted)
            total.errors += save_reference_shot(ren, ani, reference_shot_path(*shot_dir, fname));
    }

    std::printf("selftest: %zu files, %zu frames, %zu textures, %zu sequences — %s (%d errors)\n",
                files.size(), total.frames, total.textures, total.seqs,
                total.errors ? "FAILED" : "OK", total.errors);
    return total.errors ? 1 : 0;
}

// The command line: a directory to browse, and optionally the selftest plus the
// directory its reference shots go to.
struct ViewerArgs {
    fs::path dir;
    fs::path shot_dir;
    bool selftest = false;
    bool have_shot_dir = false;
};

ViewerArgs parse_args(int argc, char** argv) {
    ViewerArgs args;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--selftest") {
            args.selftest = true;
        } else if (args.dir.empty()) {
            args.dir = a;
        } else {
            args.shot_dir = a;
            args.have_shot_dir = true;
        }
    }
    return args;
}

std::vector<fs::path> collect_ani_files(const fs::path& dir) {
    std::vector<fs::path> files;
    // A path the user typed is the ordinary case here, not the exceptional one:
    // `directory_iterator` on anything that is not a directory throws, and
    // "you gave me a path that isn't there" deserves the usage message below
    // rather than an exception. main() still catches, for everything else.
    if (!fs::is_directory(dir)) return files;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (e.is_regular_file() && to_upper(e.path().extension().string()) == ".ANI")
            files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

// The interactive browser's cursor: which file, which sequence, which step, and
// the playback state the keys drive.
struct Browser {
    int file_idx = 0;
    int seq_idx = 0;
    std::size_t step_idx = 0;
    bool paused = false;
    int step_ms = 66;
};

// Wrap `idx` by ±1 within a non-empty range, without going negative on the way.
int wrap(int idx, int delta, std::size_t count) {
    const int n = static_cast<int>(count);
    return (idx + delta + n) % n;
}

// What the cursor may currently wrap around: how many files are on disk, and
// how many sequences the loaded ANI has. Both change as the browser moves, so
// they are read fresh per key press rather than stored on the cursor.
struct BrowserBounds {
    std::size_t files = 0;
    std::size_t seqs = 0;
};

enum class KeyAction : std::uint8_t { None, Reload, Quit };

// One key press against the cursor. Flat by design: a switch over an enum is
// the shape a reader walks fastest, and the browser has no key that means
// anything different depending on what came before it.
KeyAction apply_key(SDL_Keycode key, Browser& cur, BrowserBounds bounds) {
    switch (key) {
        case SDLK_ESCAPE: return KeyAction::Quit;
        case SDLK_SPACE: cur.paused = !cur.paused; return KeyAction::None;
        case SDLK_UP: cur.file_idx = wrap(cur.file_idx, -1, bounds.files); return KeyAction::Reload;
        case SDLK_DOWN:
            cur.file_idx = wrap(cur.file_idx, 1, bounds.files);
            return KeyAction::Reload;
        case SDLK_LEFT:
            if (bounds.seqs != 0) {
                cur.seq_idx = wrap(cur.seq_idx, -1, bounds.seqs);
                cur.step_idx = 0;
            }
            return KeyAction::None;
        case SDLK_RIGHT:
            if (bounds.seqs != 0) {
                cur.seq_idx = wrap(cur.seq_idx, 1, bounds.seqs);
                cur.step_idx = 0;
            }
            return KeyAction::None;
        case SDLK_PLUS:
        case SDLK_EQUALS: cur.step_ms = std::max(16, cur.step_ms - 10); return KeyAction::None;
        case SDLK_MINUS: cur.step_ms = std::min(500, cur.step_ms + 10); return KeyAction::None;
        default: return KeyAction::None;
    }
}

int run_interactive(SDL_Renderer* ren, SDL_Window* win, const std::vector<fs::path>& files) {
    Browser cur;
    AniTextures ani;

    auto reload = [&]() {
        ani.reset();
        cur.seq_idx = 0;
        cur.step_idx = 0;
        if (const std::string err = load_ani(ren, files[cur.file_idx], ani); !err.empty())
            std::fprintf(stderr, "load failed: %s\n", err.c_str());
        const std::string title =
            "Open Bomberman viewer — " + files[cur.file_idx].filename().string();
        SDL_SetWindowTitle(win, title.c_str());
    };
    reload();

    std::uint64_t last_step = SDL_GetTicks();
    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) running = false;
            if (ev.type != SDL_EVENT_KEY_DOWN) continue;
            const KeyAction act = apply_key(
                ev.key.key, cur, BrowserBounds{files.size(), ani.data().sequences.size()});
            if (act == KeyAction::Quit) running = false;
            if (act == KeyAction::Reload) reload();
        }

        const std::uint64_t now = SDL_GetTicks();
        if (!cur.paused && now - last_step >= static_cast<std::uint64_t>(cur.step_ms)) {
            last_step = now;
            ++cur.step_idx;
        }

        clear_frame(ren);
        render_step(ren,
                    StepRef{ani, ani.data().sequences.empty() ? -1 : cur.seq_idx, cur.step_idx},
                    Placement{});
        SDL_RenderPresent(ren);
        SDL_Delay(5);
    }

    ani.reset();
    return 0;
}

// Everything that can throw, so `main` below stays a handler and nothing escapes
// it. `abtool`'s main is the model and states the reason: a bare main must not
// let an exception out (bugprone-exception-escape), and the asset loaders throw
// freely on a corrupt install — this viewer reads the same 1997 files.
int run_viewer(int argc, char** argv) {
    ViewerArgs args = parse_args(argc, argv);
    if (args.dir.empty()) args.dir = bomber::assets::default_game_dir();
    if (args.dir.empty()) {
        std::fprintf(stderr,
                     "usage: bomber_viewer <path to DATA/ANI or game dir> [--selftest [shot_dir]]\n"
                     "(or set BOMBER_GAME_DIR, or put the game path in gamedir.txt)\n");
        return 2;
    }
    // Accept either the animation directory itself or the install root above it.
    if (fs::is_directory(args.dir / "DATA" / "ANI")) args.dir = args.dir / "DATA" / "ANI";

    const std::vector<fs::path> files = collect_ani_files(args.dir);
    if (files.empty()) {
        std::fprintf(stderr, "no .ANI files in %s\n", args.dir.string().c_str());
        return 1;
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* win = nullptr;
    SDL_Renderer* ren = nullptr;
    if (!SDL_CreateWindowAndRenderer("Open Bomberman — asset viewer", kWindowWidth, kWindowHeight,
                                     0, &win, &ren)) {
        std::fprintf(stderr, "SDL_CreateWindowAndRenderer: %s\n", SDL_GetError());
        return 1;
    }

    const int rc = args.selftest
                       ? run_selftest(ren, files, args.have_shot_dir ? &args.shot_dir : nullptr)
                       : run_interactive(ren, win, files);

    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return rc;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run_viewer(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "error: unknown exception\n");
        return 1;
    }
}
