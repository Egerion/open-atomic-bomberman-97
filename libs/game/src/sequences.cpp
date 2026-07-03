#include "bomber/game/sequences.hpp"

#include <string>

namespace bomber::game {

void SequenceSet::resolve_stage(const AssetStore& a, int stage) {
    std::string n = std::to_string(stage);
    brick = resolve_sequence(a.tiles(), "tile " + n + " brick");
    solid = resolve_sequence(a.tiles(), "tile " + n + " solid");
    burn = resolve_sequence(a.xbrick(), "flame brick " + n);
}

void SequenceSet::resolve(const AssetStore& a) {
    digits = resolve_sequence(a.kfont(), "numeric font");
    hurry = resolve_sequence(a.hurry(), "hurry");

    // Every original sequence name ends in "green" — the engine recolors the
    // green master sprites per player, and so do we (AssetStore).
    for (int p = 0; p < kLocalPlayers; ++p) {
        bomb[p] = resolve_sequence(a.bombs(p), "bomb regular green");
        if (bomb[p].steps.empty()) bomb[p] = resolve_sequence(a.bombs(-1), "bomb regular green");
        FlameSet& f = flames[p];
        const AniTextures& fa = a.flame(p);
        f.center = resolve_sequence(fa, "flame center green");
        f.mid_h[0] = resolve_sequence(fa, "flame midwest green");
        f.mid_h[1] = resolve_sequence(fa, "flame mideast green");
        f.mid_v[0] = resolve_sequence(fa, "flame midnorth green");
        f.mid_v[1] = resolve_sequence(fa, "flame midsouth green");
        f.tip_n = resolve_sequence(fa, "flame tipnorth green");
        f.tip_s = resolve_sequence(fa, "flame tipsouth green");
        f.tip_w = resolve_sequence(fa, "flame tipwest green");
        f.tip_e = resolve_sequence(fa, "flame tipeast green");
        if (f.center.steps.empty())
            f.center = resolve_sequence(a.flame(-1), "flame center green");
    }

    static constexpr const char* kDirs[4] = {"north", "south", "west", "east"};
    for (int p = 0; p < kLocalPlayers; ++p) {
        for (int d = 0; d < 4; ++d) {
            stand[p][d] = resolve_sequence(a.stand(p), std::string("stand ") + kDirs[d]);
            walk[p][d] = resolve_sequence(a.walk(p), std::string("walk ") + kDirs[d]);
        }
    }
    shadow = resolve_sequence(a.shadow(), "shadow");
}

}  // namespace bomber::game
