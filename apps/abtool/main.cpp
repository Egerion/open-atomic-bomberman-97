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
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "commands.hpp"

namespace {

// Every abtool verb has the same positional shape: the verb, one required
// subject path, and at most two optional trailing operands. Modelling that
// shape once removes the size-check-then-maybe-take-a-pointer dance that was
// repeated verbatim at four of the six call sites, and it owns the paths so
// `option()` can hand back the `const fs::path*` that commands.hpp already uses
// to mean "the caller omitted this".
class CommandLine {
public:
    CommandLine(int argc, char** argv) : args_(argv + 1, argv + argc) {
        if (args_.size() >= 2) subject_ = args_[1];
        if (args_.size() >= 3) option_ = args_[2];
    }

    // A verb only dispatches once its required subject is present; without it
    // the caller falls through to the usage text.
    bool is(std::string_view verb) const { return args_.size() >= 2 && args_[0] == verb; }

    const std::filesystem::path& subject() const { return subject_; }
    const std::filesystem::path* option() const { return args_.size() >= 3 ? &option_ : nullptr; }

    // The third operand, the only numeric one any verb takes (simrun's tick
    // count). Throws like the rest of the parse, into main's handler below.
    int count_or(int fallback) const { return args_.size() >= 4 ? std::stoi(args_[3]) : fallback; }

private:
    std::vector<std::string> args_;
    std::filesystem::path subject_;
    std::filesystem::path option_;
};

}  // namespace

int main(int argc, char** argv) {
    using namespace bomber::tools;

    // Whole body in the try (not just the dispatch): the command line's
    // construction can throw bad_alloc too, and a bare `main` must not let any
    // exception escape (bugprone-exception-escape) — catch (...) as a last
    // resort below the std::exception handler covers non-standard-derived
    // throws as well.
    try {
        const CommandLine cmd(argc, argv);
        if (cmd.is("survey")) return cmd_survey(cmd.subject());
        if (cmd.is("ani")) return cmd_ani(cmd.subject(), cmd.option());
        if (cmd.is("sch")) return cmd_sch(cmd.subject());
        if (cmd.is("simrun")) return cmd_simrun(cmd.subject(), cmd.option(), cmd.count_or(400));
        if (cmd.is("pcx")) return cmd_pcx(cmd.subject(), cmd.option());
        if (cmd.is("rss")) return cmd_rss(cmd.subject(), cmd.option());
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
