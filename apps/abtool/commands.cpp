#include "commands.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

#include "bomber/assets/ani.hpp"
#include "bomber/assets/pcx.hpp"
#include "bomber/assets/reslist.hpp"
#include "bomber/assets/rss.hpp"
#include "bomber/assets/sch.hpp"
#include "bomber/match/match_factory.hpp"
#include "bomber/sim/simulation.hpp"
#include "writers.hpp"

namespace bomber::tools {

namespace fs = std::filesystem;
using namespace bomber::assets;

namespace {

std::string to_upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::vector<fs::path> files_with_ext(const fs::path& dir, std::string_view ext_upper) {
    std::vector<fs::path> out;
    if (!fs::is_directory(dir)) return out;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (!e.is_regular_file()) continue;
        if (to_upper(e.path().extension().string()) == ext_upper) out.push_back(e.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

// One row per grid line, NUL-terminated so a row prints as a C string.
using AsciiRow = std::array<char, sim::kGridWidth + 1>;
using AsciiArena = std::array<AsciiRow, sim::kGridHeight>;

// The glyph for one tile. The order is this view's precedence, not the sim's:
// terrain first, then the two transient flame layers, then a powerup revealed
// on the floor — so a flame crossing a powerup reads as flame.
char tile_glyph(const sim::State& s, int x, int y) {
    if (s.cells[y][x] == sim::Cell::Solid) return '#';
    if (s.cells[y][x] == sim::Cell::Brick) return ':';
    if (s.burning[y][x] > 0) return '%';
    if (s.flame[y][x] > 0) return '*';
    if (s.floor[y][x] != sim::PowerupType::None) return 'p';
    return '.';
}

AsciiArena arena_ascii(const sim::State& s) {
    AsciiArena grid{};
    for (int y = 0; y < sim::kGridHeight; ++y) {
        for (int x = 0; x < sim::kGridWidth; ++x) grid[y][x] = tile_glyph(s, x, y);
        grid[y][sim::kGridWidth] = 0;
    }
    // Actors overwrite terrain, and players overwrite bombs — otherwise a
    // player standing on the bomb they just dropped would vanish from the view.
    for (const auto& b : s.bombs)
        if (b.active) grid[b.tile_y()][b.tile_x()] = 'Q';
    for (int i = 0; i < sim::kMaxPlayers; ++i) {
        const auto& p = s.players[i];
        if (p.present && p.alive) grid[p.tile_y()][p.tile_x()] = static_cast<char>('0' + i);
    }
    return grid;
}

void print_sim(const sim::State& s) {
    const AsciiArena grid = arena_ascii(s);
    std::printf("tick %llu  alive %d  bombs %zu\n", static_cast<unsigned long long>(s.tick),
                sim::alive_count(s), s.bombs.size());
    for (const AsciiRow& row : grid) std::printf("  %s\n", row.data());
}

// Every survey section reports a failure the same way and contributes to one
// exit code. Returning a count keeps the sections independent of each other
// instead of threading a shared counter through a captured lambda.
int survey_error(const std::string& what) {
    std::printf("  ERROR: %s\n", what.c_str());
    return 1;
}

// Frame-level tallies for one ANI: the two CIMG compression types the format
// uses, plus frames that carry a name but decoded to no pixels.
struct AniTally {
    std::size_t type4 = 0;
    std::size_t type11 = 0;
    int errors = 0;
};

AniTally tally_frames(const ani::AniFile& a, const std::string& filename) {
    AniTally t;
    for (const auto& f : a.frames) {
        if (f.cimg_type == 4)
            ++t.type4;
        else if (f.cimg_type == 11)
            ++t.type11;
        if (!f.name.empty() && f.image.empty())
            t.errors += survey_error(filename + ": frame '" + f.name + "' has no pixels");
    }
    return t;
}

int survey_ani(const fs::path& game) {
    const fs::path dir = game / "DATA" / "ANI";
    std::printf("== ANI (%s)\n", dir.string().c_str());
    int errors = 0;
    std::size_t frames = 0, seqs = 0, type4 = 0, type11 = 0;
    const auto anis = files_with_ext(dir, ".ANI");
    for (const auto& p : anis) {
        try {
            const auto a = ani::load(p);
            frames += a.frames.size();
            seqs += a.sequences.size();
            for (const auto& w : a.warnings)
                std::printf("  warn: %s: %s\n", p.filename().string().c_str(), w.c_str());
            const AniTally t = tally_frames(a, p.filename().string());
            type4 += t.type4;
            type11 += t.type11;
            errors += t.errors;
        } catch (const std::exception& e) {
            errors += survey_error(e.what());
        }
    }
    std::printf("  %zu files, %zu frames (%zu type-4, %zu type-11), %zu sequences\n", anis.size(),
                frames, type4, type11, seqs);
    return errors;
}

int survey_schemes(const fs::path& game) {
    std::printf("== SCHEMES\n");
    int errors = 0;
    const auto schemes = files_with_ext(game / "DATA" / "SCHEMES", ".SCH");
    for (const auto& p : schemes) {
        try {
            const auto s = sch::load(p);
            if (s.width() != 15 || s.height() != 11)
                errors +=
                    survey_error(p.filename().string() + ": unexpected grid " +
                                 std::to_string(s.width()) + "x" + std::to_string(s.height()));
        } catch (const std::exception& e) {
            errors += survey_error(e.what());
        }
    }
    std::printf("  %zu schemes\n", schemes.size());
    return errors;
}

int survey_values(const fs::path& game) {
    try {
        const auto vl = res::load_values(game / "DATA" / "RES" / "VALUELST.RES");
        std::printf("  VALUELST: %zu values, %zu warnings\n", vl.values.size(), vl.warnings.size());
        for (const auto& w : vl.warnings) std::printf("    warn: %s\n", w.c_str());
    } catch (const std::exception& e) {
        return survey_error(e.what());
    }
    return 0;
}

// SOUNDLST names clips that must exist as RSS files next door; a name with no
// file is a warning rather than an error, since the original ships some.
int survey_soundlst(const fs::path& game) {
    try {
        const auto sl = res::load_sounds(game / "DATA" / "RES" / "SOUNDLST.RES");
        std::size_t resolved = 0, missing = 0;
        for (const auto& [id, name] : sl.names) {
            if (fs::exists(game / "DATA" / "SOUND" / (to_upper(name) + ".RSS"))) {
                ++resolved;
            } else {
                ++missing;
                std::printf("  warn: sound %d '%s' has no RSS file\n", id, name.c_str());
            }
        }
        std::printf(
            "  SOUNDLST: %zu entries, %zu resolve to RSS files, %zu missing, %zu warnings\n",
            sl.names.size(), resolved, missing, sl.warnings.size());
    } catch (const std::exception& e) {
        return survey_error(e.what());
    }
    return 0;
}

int survey_pcx(const fs::path& game) {
    int errors = 0;
    const auto pcxs = files_with_ext(game / "DATA" / "RES", ".PCX");
    std::size_t pcx_ok = 0;
    for (const auto& p : pcxs) {
        try {
            // Decoding is the whole check — a PCX that parses is a PCX that works.
            [[maybe_unused]] const auto img = pcx::load(p);
            ++pcx_ok;
        } catch (const std::exception& e) {
            errors += survey_error(e.what());
        }
    }
    std::printf("  PCX: %zu/%zu decode\n", pcx_ok, pcxs.size());
    return errors;
}

void survey_sound(const fs::path& game) {
    std::printf("== SOUND\n");
    const auto rsss = files_with_ext(game / "DATA" / "SOUND", ".RSS");
    double total_sec = 0;
    for (const auto& p : rsss) total_sec += rss::load(p).seconds();
    std::printf("  %zu RSS files, %.1f minutes of audio\n", rsss.size(), total_sec / 60.0);
}

// Frames are named by their ANI entry, falling back to the index for the
// unnamed ones. Those names are the artist's source files and still carry that
// extension ("WLKE0000.TGA"), which is stripped so the dump does not land as
// "WLKE0000.TGA.bmp".
void dump_ani_frames(const ani::AniFile& a, const fs::path& out_dir) {
    fs::create_directories(out_dir);
    for (std::size_t i = 0; i < a.frames.size(); ++i) {
        const auto& f = a.frames[i];
        if (f.image.empty()) continue;
        std::string base = f.name.empty() ? ("frame" + std::to_string(i)) : f.name;
        if (auto p = base.find_last_of('.'); p != std::string::npos) base.erase(p);
        write_bmp(out_dir / (base + ".bmp"), f.image);
    }
    std::printf("frames dumped to %s\n", out_dir.string().c_str());
}

// The scripted drive for `simrun`: both players walk a four-phase box, 25 ticks
// a side, and drop a bomb on their own period. Enough to exercise movement,
// bombs and flames without an input device — this is a smoke test, not a replay.
sim::TickInputs scripted_inputs(int t) {
    const int phase = (t / 25) % 4;
    sim::TickInputs in;
    auto& p0 = in.players[0];
    p0.right = phase == 0;
    p0.down = phase == 1;
    p0.left = phase == 2;
    p0.up = phase == 3;
    p0.action1 = (t % 50) == 24;
    auto& p1 = in.players[1];
    p1.left = phase == 0;
    p1.up = phase == 1;
    p1.right = phase == 2;
    p1.down = phase == 3;
    p1.action1 = (t % 60) == 30;
    return in;
}

}  // namespace

int cmd_survey(const fs::path& game) {
    int errors = survey_ani(game);
    errors += survey_schemes(game);
    std::printf("== RES\n");
    errors += survey_values(game);
    errors += survey_soundlst(game);
    errors += survey_pcx(game);
    survey_sound(game);

    std::printf("\n%s (%d errors)\n", errors ? "SURVEY FAILED" : "SURVEY OK", errors);
    return errors ? 1 : 0;
}

int cmd_ani(const fs::path& file, const fs::path* out_dir) {
    const auto a = ani::load(file);
    std::printf("%s: cell %dx%d, %zu frames, %zu sequences\n", file.filename().string().c_str(),
                a.cell_width, a.cell_height, a.frames.size(), a.sequences.size());
    for (std::size_t i = 0; i < a.frames.size(); ++i) {
        const auto& f = a.frames[i];
        std::printf("  frame %3zu %-14s %3dx%-3d hot(%d,%d) type%u\n", i, f.name.c_str(),
                    f.image.width, f.image.height, f.hotspot_x, f.hotspot_y, f.cimg_type);
    }
    for (const auto& s : a.sequences) {
        std::printf("  seq '%s' (%zu steps):", s.name.c_str(), s.steps.size());
        // dx/dy = the per-STAT FRAM-leaf offset_x/offset_y (docs/formats/ani.md
        // "Rendering a step"): NOT applied by the standard blit, but relevant for
        // spot-checking the rare paths (e.g. sub_41DB41) that do use it.
        for (const auto& st : s.steps) std::printf(" %d(%d,%d)", st.frame, st.dx, st.dy);
        std::printf("\n");
    }
    if (out_dir) dump_ani_frames(a, *out_dir);
    return 0;
}

int cmd_sch(const fs::path& file) {
    const auto s = sch::load(file);
    std::printf("%s  v%d  density %d%%  %dx%d\n", s.name.c_str(), s.version, s.brick_density,
                s.width(), s.height());
    for (const auto& row : s.rows) std::printf("  %s\n", row.c_str());
    for (const auto& sp : s.spawns)
        std::printf("  spawn p%d at (%d,%d) team=%d\n", sp.player, sp.x, sp.y, sp.team);
    return 0;
}

int cmd_simrun(const fs::path& scheme_path, const fs::path* game_dir, int ticks) {
    const auto scheme = sch::load(scheme_path);
    res::ValueList values;
    if (game_dir) values = res::load_values(*game_dir / "DATA" / "RES" / "VALUELST.RES");
    sim::Simulation simulation(
        match::build_match_config(scheme, 2, 42, game_dir ? &values : nullptr));
    const sim::State& s = simulation.state();
    std::printf("scheme '%s'  speed %d  fuse %d\n", scheme.name.c_str(), s.tuning.start_speed,
                s.tuning.fuse_frames);
    print_sim(s);

    for (int t = 0; t < ticks; ++t) {
        simulation.tick(scripted_inputs(t));
        if ((t + 1) % 40 == 0) print_sim(s);
        if (sim::alive_count(s) <= 1) {
            std::printf("match over at tick %llu\n", static_cast<unsigned long long>(s.tick));
            print_sim(s);
            break;
        }
    }
    std::printf("final state hash: %016llx\n", static_cast<unsigned long long>(simulation.hash()));
    return 0;
}

int cmd_pcx(const fs::path& file, const fs::path* out_bmp) {
    const auto img = pcx::load(file);
    std::printf("%s: %dx%d\n", file.string().c_str(), img.width, img.height);
    if (out_bmp) write_bmp(*out_bmp, img);
    return 0;
}

int cmd_rss(const fs::path& file, const fs::path* out_wav) {
    const auto snd = rss::load(file);
    std::printf("%s: %.2fs, %zu samples\n", file.string().c_str(), snd.seconds(),
                snd.samples.size());
    if (out_wav) write_wav(*out_wav, snd);
    return 0;
}

}  // namespace bomber::tools
