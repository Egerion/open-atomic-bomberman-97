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
        bomb_dud[p] = resolve_sequence(a.duds(p), "bomb regular green dud");
        // Armed trigger (remote) bomb: TRIGBOMB.ANI "bomb trigger green".
        // Empty if the file is missing -> the bomb draw falls back to the pulse.
        bomb_trigger[p] = resolve_sequence(a.trigbomb(p), "bomb trigger green");
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
            // KICK.ANI names its steps "kick <dir>" (no "green"); PUNCH.ANI
            // uses "punch <dir> green" like the other player sprites.
            kick[p][d] = resolve_sequence(a.kick(p), std::string("kick ") + kDirs[d]);
            punch[p][d] = resolve_sequence(a.punch(p), std::string("punch ") + kDirs[d] + " green");
            // The "carrying a bomb" walk/stand poses live one direction per
            // BWALK*.ANI file, so probe every file until one owns the name.
            std::string wname = std::string("walkbomb ") + kDirs[d];
            std::string sname = std::string("standbomb ") + kDirs[d];
            Anim& wout = walkbomb[p][d];
            Anim& sout = standbomb[p][d];
            wout = {};
            sout = {};
            for (int f = 0; f < a.bwalk_files() && wout.steps.empty(); ++f)
                wout = resolve_sequence(a.bwalk(f, p), wname);
            for (int f = 0; f < a.bwalk_files() && sout.steps.empty(); ++f)
                sout = resolve_sequence(a.bwalk(f, p), sname);
        }
        // The 13 "cornerhead N" fidgets are unevenly distributed across the 8
        // CORNER*.ANI files, so probe every file until one owns the name.
        for (int i = 0; i < kCornerheadVariants; ++i) {
            std::string name = "cornerhead " + std::to_string(i);
            Anim& out = cornerhead[p][i];
            out = {};
            for (int f = 0; f < a.corner_files() && out.steps.empty(); ++f)
                out = resolve_sequence(a.corner(f, p), name);
        }
    }
    shadow = resolve_sequence(a.shadow(), "shadow");

    // Animated floor powerups (POWERS.ANI "power <name>"), indexed by
    // sim::PowerupType. Names verified against POWERS.ANI; SuperDisease maps to
    // "power disease3" and Spooger to "power spooge" (the file's own spellings).
    // A missing sequence leaves the entry empty -> draw_powerups keeps the
    // static POW*.PCX fallback for that kind.
    static constexpr const char* kPowerNames[sim::kPowerupKinds] = {
        "power bomb",    // ExtraBomb
        "power flame",   // Flame
        "power disease", // Disease
        "power kicker",  // Kick
        "power skate",   // Skate
        "power punch",   // Punch
        "power grab",    // Grab
        "power spooge",  // Spooger
        "power goldflame", // Goldflame
        "power trigger", // Trigger
        "power jelly",   // Jelly
        "power disease3", // SuperDisease
        "power random",  // Random
    };
    for (int k = 0; k < sim::kPowerupKinds; ++k)
        powerup_anim[k] = resolve_sequence(a.powers(), kPowerNames[k]);

    // Stage-actor floor art (docs/re/stage-actors.md). Conveyor sequence names
    // are compass words indexed by godir (0=north,1=east,2=south,3=west) to
    // match actor_dir; the trampoline is a single direction-independent seq.
    static constexpr const char* kGodirCompass[4] = {"north", "east", "south", "west"};
    for (int g = 0; g < 4; ++g)
        conveyor[g] = resolve_sequence(a.conveyor(),
                                       std::string("extra conveyor ") + kGodirCompass[g]);
    trampoline = resolve_sequence(a.extras(), "extra trampoline");
}

}  // namespace bomber::game
