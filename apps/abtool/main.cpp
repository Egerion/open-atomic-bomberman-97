// abtool — headless inspector/extractor for original Atomic Bomberman assets.
// Usage:
//   abtool survey <game_dir>            parse & validate every known asset
//   abtool ani <file.ani> [out_dir]     list frames/sequences, optionally dump BMPs
//   abtool sch <file.sch>               print scheme as ASCII
//   abtool simrun <file.sch> [game_dir] [ticks]   headless sim demo (ASCII)
//   abtool pcx <file.pcx> [out.bmp]
//   abtool rss <file.rss> [out.wav]

#include <cstdio>
#include <exception>
#include <string>
#include <vector>

#include "commands.hpp"

int main(int argc, char** argv) {
    using namespace bomber::tools;
    namespace fs = std::filesystem;

    // Whole body in the try (not just the dispatch): `args`'s construction can
    // throw bad_alloc too, and a bare `main` must not let any exception escape
    // (bugprone-exception-escape) — catch (...) as a last resort below the
    // std::exception handler covers non-standard-derived throws as well.
    try {
        std::vector<std::string> args(argv + 1, argv + argc);
        if (args.size() >= 2 && args[0] == "survey") return cmd_survey(args[1]);
        if (args.size() >= 2 && args[0] == "ani") {
            fs::path out;
            if (args.size() >= 3) out = args[2];
            return cmd_ani(args[1], args.size() >= 3 ? &out : nullptr);
        }
        if (args.size() >= 2 && args[0] == "sch") return cmd_sch(args[1]);
        if (args.size() >= 2 && args[0] == "simrun") {
            fs::path game;
            if (args.size() >= 3) game = args[2];
            int ticks = args.size() >= 4 ? std::stoi(args[3]) : 400;
            return cmd_simrun(args[1], args.size() >= 3 ? &game : nullptr, ticks);
        }
        if (args.size() >= 2 && args[0] == "pcx") {
            fs::path out;
            if (args.size() >= 3) out = args[2];
            return cmd_pcx(args[1], args.size() >= 3 ? &out : nullptr);
        }
        if (args.size() >= 2 && args[0] == "rss") {
            fs::path out;
            if (args.size() >= 3) out = args[2];
            return cmd_rss(args[1], args.size() >= 3 ? &out : nullptr);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "error: unknown exception\n");
        return 1;
    }
    std::fprintf(stderr,
                 "usage:\n"
                 "  abtool survey <game_dir>\n"
                 "  abtool ani <file.ani> [out_dir]\n"
                 "  abtool sch <file.sch>\n"
                 "  abtool simrun <file.sch> [game_dir] [ticks]\n"
                 "  abtool pcx <file.pcx> [out.bmp]\n"
                 "  abtool rss <file.rss> [out.wav]\n");
    return 2;
}
