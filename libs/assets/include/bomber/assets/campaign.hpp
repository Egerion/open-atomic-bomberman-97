#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace bomber::assets::res {

// The `.CAM` campaign/stage-definition format (sub_401085 @0x401085,
// docs/re/campaign.md): plain text, ';' full-line comments, one stage per
// line. Each stage line starts with the literal two-byte marker "-C"
// (case-insensitive 'C', checked byte-for-byte by the original loader), then
// 9 comma-separated fields, matching the file's own self-documenting header:
//
//   0. campaign name
//   1. levelno
//   2. scheme to use
//   3. number of rovers
//   4. rover speed
//   5. number of ghosts
//   6. ghost speed
//   7. number of AIs
//   8. AI difficulty (0-100) (unused at present, per the file's own comment)
//
// Lines that don't start with "-C" (including ';' comments and the format's
// own header commentary) are skipped, not warned — only a line that DOES
// start with the marker but fails to yield exactly 9 fields is malformed.
struct CampaignStage {
    std::string name;     // field 0: campaign/stage display name
    int level_no = 0;     // field 1
    std::string scheme;   // field 2: scheme name (resolved to <scheme>.SCH by the caller)
    int rovers = 0;        // field 3
    int rover_speed = 0;   // field 4
    int ghosts = 0;         // field 5
    int ghost_speed = 0;    // field 6
    int ai_count = 0;       // field 7
    int ai_difficulty = 0;  // field 8 (unused at present, per the format's own comment)
};

struct Campaign {
    std::vector<CampaignStage> stages;
    std::vector<std::string> warnings;  // "-C" lines that didn't parse to exactly 9 fields

    bool empty() const { return stages.empty(); }
};

// Parse `.CAM` text (in-memory; hermetic for tests).
Campaign parse_campaign(std::string_view text);
// Load + parse a `.CAM` file from a path (throws std::runtime_error if
// unreadable). The install's shipped campaign files (CROUTON.CAM,
// GHOSTS.CAM, SIMPLE.CAM) and any user-authored ones are the player's own
// game data — loaded at runtime, never committed (CLAUDE.md).
Campaign load_campaign(const std::filesystem::path& path);

}  // namespace bomber::assets::res
