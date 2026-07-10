#include "commands.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <exception>
#include <string>
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

std::vector<fs::path> files_with_ext(const fs::path& dir, const std::string& ext_upper) {
    std::vector<fs::path> out;
    if (!fs::is_directory(dir)) return out;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (!e.is_regular_file()) continue;
        if (to_upper(e.path().extension().string()) == ext_upper) out.push_back(e.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

void print_sim(const sim::State& s) {
    char grid[sim::kGridHeight][sim::kGridWidth + 1];
    for (int y = 0; y < sim::kGridHeight; ++y) {
        for (int x = 0; x < sim::kGridWidth; ++x) {
            char c = '.';
            if (s.cells[y][x] == sim::Cell::Solid) c = '#';
            else if (s.cells[y][x] == sim::Cell::Brick) c = ':';
            else if (s.burning[y][x] > 0) c = '%';
            else if (s.flame[y][x] > 0) c = '*';
            else if (s.floor[y][x] != sim::PowerupType::None) c = 'p';
            grid[y][x] = c;
        }
        grid[y][sim::kGridWidth] = 0;
    }
    for (const auto& b : s.bombs)
        if (b.active) grid[b.tile_y()][b.tile_x()] = 'Q';
    for (int i = 0; i < sim::kMaxPlayers; ++i) {
        const auto& p = s.players[i];
        if (p.present && p.alive) grid[p.tile_y()][p.tile_x()] = static_cast<char>('0' + i);
    }
    std::printf("tick %llu  alive %d  bombs %zu\n",
                static_cast<unsigned long long>(s.tick), sim::alive_count(s), s.bombs.size());
    for (int y = 0; y < sim::kGridHeight; ++y) std::printf("  %s\n", grid[y]);
}

}  // namespace

int cmd_survey(const fs::path& game) {
    int errors = 0;
    auto report_err = [&errors](const std::string& what) {
        std::printf("  ERROR: %s\n", what.c_str());
        ++errors;
    };

    std::printf("== ANI (%s)\n", (game / "DATA" / "ANI").string().c_str());
    std::size_t frames = 0, seqs = 0, type4 = 0, type11 = 0;
    auto anis = files_with_ext(game / "DATA" / "ANI", ".ANI");
    for (const auto& p : anis) {
        try {
            auto a = ani::load(p);
            frames += a.frames.size();
            seqs += a.sequences.size();
            for (const auto& w : a.warnings)
                std::printf("  warn: %s: %s\n", p.filename().string().c_str(), w.c_str());
            for (const auto& f : a.frames) {
                if (f.cimg_type == 4) ++type4;
                else if (f.cimg_type == 11) ++type11;
                if (!f.name.empty() && f.image.empty())
                    report_err(p.filename().string() + ": frame '" + f.name + "' has no pixels");
            }
        } catch (const std::exception& e) {
            report_err(e.what());
        }
    }
    std::printf("  %zu files, %zu frames (%zu type-4, %zu type-11), %zu sequences\n",
                anis.size(), frames, type4, type11, seqs);

    std::printf("== SCHEMES\n");
    auto schemes = files_with_ext(game / "DATA" / "SCHEMES", ".SCH");
    for (const auto& p : schemes) {
        try {
            auto s = sch::load(p);
            if (s.width() != 15 || s.height() != 11)
                report_err(p.filename().string() + ": unexpected grid " +
                           std::to_string(s.width()) + "x" + std::to_string(s.height()));
        } catch (const std::exception& e) {
            report_err(e.what());
        }
    }
    std::printf("  %zu schemes\n", schemes.size());

    std::printf("== RES\n");
    try {
        auto vl = res::load_values(game / "DATA" / "RES" / "VALUELST.RES");
        std::printf("  VALUELST: %zu values, %zu warnings\n", vl.values.size(),
                    vl.warnings.size());
        for (const auto& w : vl.warnings) std::printf("    warn: %s\n", w.c_str());
    } catch (const std::exception& e) {
        report_err(e.what());
    }
    std::size_t resolved = 0, missing = 0;
    try {
        auto sl = res::load_sounds(game / "DATA" / "RES" / "SOUNDLST.RES");
        for (const auto& [id, name] : sl.names) {
            if (fs::exists(game / "DATA" / "SOUND" / (to_upper(name) + ".RSS"))) {
                ++resolved;
            } else {
                ++missing;
                std::printf("  warn: sound %d '%s' has no RSS file\n", id, name.c_str());
            }
        }
        std::printf("  SOUNDLST: %zu entries, %zu resolve to RSS files, %zu missing, %zu warnings\n",
                    sl.names.size(), resolved, missing, sl.warnings.size());
    } catch (const std::exception& e) {
        report_err(e.what());
    }

    auto pcxs = files_with_ext(game / "DATA" / "RES", ".PCX");
    std::size_t pcx_ok = 0;
    for (const auto& p : pcxs) {
        try {
            auto img = pcx::load(p);
            (void)img;
            ++pcx_ok;
        } catch (const std::exception& e) {
            report_err(e.what());
        }
    }
    std::printf("  PCX: %zu/%zu decode\n", pcx_ok, pcxs.size());

    std::printf("== SOUND\n");
    auto rsss = files_with_ext(game / "DATA" / "SOUND", ".RSS");
    double total_sec = 0;
    for (const auto& p : rsss) total_sec += rss::load(p).seconds();
    std::printf("  %zu RSS files, %.1f minutes of audio\n", rsss.size(), total_sec / 60.0);

    std::printf("\n%s (%d errors)\n", errors ? "SURVEY FAILED" : "SURVEY OK", errors);
    return errors ? 1 : 0;
}

int cmd_ani(const fs::path& file, const fs::path* out_dir) {
    auto a = ani::load(file);
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
    if (out_dir) {
        fs::create_directories(*out_dir);
        for (std::size_t i = 0; i < a.frames.size(); ++i) {
            const auto& f = a.frames[i];
            if (f.image.empty()) continue;
            std::string base = f.name.empty() ? ("frame" + std::to_string(i)) : f.name;
            if (auto p = base.find_last_of('.'); p != std::string::npos) base.erase(p);
            write_bmp(*out_dir / (base + ".bmp"), f.image);
        }
        std::printf("frames dumped to %s\n", out_dir->string().c_str());
    }
    return 0;
}

int cmd_sch(const fs::path& file) {
    auto s = sch::load(file);
    std::printf("%s  v%d  density %d%%  %dx%d\n", s.name.c_str(), s.version, s.brick_density,
                s.width(), s.height());
    for (const auto& row : s.rows) std::printf("  %s\n", row.c_str());
    for (const auto& sp : s.spawns)
        std::printf("  spawn p%d at (%d,%d) extra=%d\n", sp.player, sp.x, sp.y, sp.extra);
    return 0;
}

int cmd_simrun(const fs::path& scheme_path, const fs::path* game_dir, int ticks) {
    auto scheme = sch::load(scheme_path);
    res::ValueList values;
    if (game_dir) values = res::load_values(*game_dir / "DATA" / "RES" / "VALUELST.RES");
    sim::Simulation simulation(
        match::build_match_config(scheme, 2, 42, game_dir ? &values : nullptr));
    const sim::State& s = simulation.state();
    std::printf("scheme '%s'  speed %d  fuse %d\n", scheme.name.c_str(),
                s.tuning.start_speed, s.tuning.fuse_frames);
    print_sim(s);

    sim::TickInputs in;
    for (int t = 0; t < ticks; ++t) {
        int phase = (t / 25) % 4;
        auto& p0 = in.players[0];
        p0 = {};
        p0.right = phase == 0;
        p0.down = phase == 1;
        p0.left = phase == 2;
        p0.up = phase == 3;
        p0.action1 = (t % 50) == 24;
        auto& p1 = in.players[1];
        p1 = {};
        p1.left = phase == 0;
        p1.up = phase == 1;
        p1.right = phase == 2;
        p1.down = phase == 3;
        p1.action1 = (t % 60) == 30;
        simulation.tick(in);
        if ((t + 1) % 40 == 0) print_sim(s);
        if (sim::alive_count(s) <= 1) {
            std::printf("match over at tick %llu\n", static_cast<unsigned long long>(s.tick));
            print_sim(s);
            break;
        }
    }
    std::printf("final state hash: %016llx\n",
                static_cast<unsigned long long>(simulation.hash()));
    return 0;
}

int cmd_pcx(const fs::path& file, const fs::path* out_bmp) {
    auto img = pcx::load(file);
    std::printf("%s: %dx%d\n", file.string().c_str(), img.width, img.height);
    if (out_bmp) write_bmp(*out_bmp, img);
    return 0;
}

int cmd_rss(const fs::path& file, const fs::path* out_wav) {
    auto snd = rss::load(file);
    std::printf("%s: %.2fs, %zu samples\n", file.string().c_str(), snd.seconds(),
                snd.samples.size());
    if (out_wav) write_wav(*out_wav, snd);
    return 0;
}

}  // namespace bomber::tools
