#pragma once

#include <filesystem>

// abtool subcommands. Each returns a process exit code.

namespace bomber::tools {

// Parses & validates every known asset under an original install.
int cmd_survey(const std::filesystem::path& game_dir);

// Lists frames/sequences of one ANI; optionally dumps frames as BMPs.
int cmd_ani(const std::filesystem::path& file, const std::filesystem::path* out_dir);

// Prints a scheme as ASCII.
int cmd_sch(const std::filesystem::path& file);

// Runs a scripted headless match, printing the arena as ASCII + state hash.
int cmd_simrun(const std::filesystem::path& scheme_path,
               const std::filesystem::path* game_dir, int ticks);

// Decodes a PCX (optionally writing a BMP).
int cmd_pcx(const std::filesystem::path& file, const std::filesystem::path* out_bmp);

// Loads an RSS clip (optionally writing a WAV).
int cmd_rss(const std::filesystem::path& file, const std::filesystem::path* out_wav);

}  // namespace bomber::tools
