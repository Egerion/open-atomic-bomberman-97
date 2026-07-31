#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace bomber::assets::res {

// The `.CAM` campaign/stage-definition format (sub_401085, docs/re/campaign.md):
// plain text, one stage per line, each line the marker "-C" followed by the 9
// comma-separated fields below in order.
//
// A line NOT starting with "-C" is skipped silently — that covers ';' comments
// and the format's own self-documenting header. Only a line that does start with
// the marker and then fails to yield exactly 9 fields is a warning.
struct CampaignStage {
    std::string name;
    int level_no = 0;
    std::string scheme;  // resolved to <scheme>.SCH by the caller
    int rovers = 0;
    int rover_speed = 0;
    int ghosts = 0;
    int ghost_speed = 0;
    int ai_count = 0;
    int ai_difficulty = 0;  // 0-100, unused at present per the format's own comment
};

struct Campaign {
    std::vector<CampaignStage> stages;
    std::vector<std::string> warnings;

    bool empty() const { return stages.empty(); }
};

Campaign parse_campaign(std::string_view text);
// Throws std::runtime_error if the file cannot be read.
Campaign load_campaign(const std::filesystem::path& path);

}  // namespace bomber::assets::res
