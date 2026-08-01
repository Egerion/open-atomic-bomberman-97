#include "bomber/render/sequences.hpp"

#include <string>

namespace bomber::game {

namespace {

// Direction suffixes as the player ANIs spell them, indexed by our Direction
// enum; the stage actors use compass words indexed by godir instead.
constexpr const char* kDirs[4] = {"north", "south", "west", "east"};
constexpr const char* kGodirCompass[4] = {"north", "east", "south", "west"};

// The multi-file pose families (BWALK1..4, PUNBOMB1..4, PUP1..4) spread ONE
// DIRECTION PER FILE, and the file-number -> direction mapping is not a fixed
// convention, so which file owns a name is unknown until it resolves: probe
// every file and keep the first non-empty match.
template <typename Pick>
Anim resolve_probed(int files, const std::string& name, const Pick& pick) {
    for (int f = 0; f < files; ++f) {
        Anim out = resolve_sequence(pick(f), name);
        if (!out.steps.empty()) return out;
    }
    return {};
}

// One player slot and one of its four facings.
struct Facing {
    int player = 0;
    int dir = 0;
};

void resolve_bombs(SequenceSet& q, const AssetStore& a, int p) {
    q.bomb[p] = resolve_sequence(a.bombs(p), "bomb regular green");
    if (q.bomb[p].steps.empty()) q.bomb[p] = resolve_sequence(a.bombs(-1), "bomb regular green");
    q.bomb_dud[p] = resolve_sequence(a.duds(p), "bomb regular green dud");
    // Armed trigger (remote) bomb: TRIGANIM.ANI is the file MASTER.ALI actually
    // loads; TRIGBOMB.ANI's same-named 7-step sequence is dead art (see
    // AssetStore::trigbomb_). Empty -> the bomb draw falls back to the pulse.
    q.bomb_trigger[p] = resolve_sequence(a.trigbomb(p), "bomb trigger green");
    // Jelly wobble: sub_42331C composes "bomb %s green" from the kind table
    // off_45BE44 {"regular","trigger","jelly"}, so jelly has its OWN sequence;
    // we previously drew jelly bombs with the regular pulse.
    q.bomb_jelly[p] = resolve_sequence(a.bombs(p), "bomb jelly green");
    if (q.bomb_jelly[p].steps.empty())
        q.bomb_jelly[p] = resolve_sequence(a.bombs(-1), "bomb jelly green");
}

void resolve_flames(SequenceSet& q, const AssetStore& a, int p) {
    FlameSet& f = q.flames[p];
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
    if (f.center.steps.empty()) f.center = resolve_sequence(a.flame(-1), "flame center green");
}

void resolve_facing(SequenceSet& q, const AssetStore& a, Facing at) {
    const int p = at.player;
    const int d = at.dir;
    const std::string dir = kDirs[d];
    q.stand[p][d] = resolve_sequence(a.stand(p), "stand " + dir);
    q.walk[p][d] = resolve_sequence(a.walk(p), "walk " + dir);
    q.kick[p][d] = resolve_sequence(a.kick(p), "kick " + dir);  // no "green" suffix
    q.walkbomb[p][d] = resolve_probed(AssetStore::bwalk_files(), "walkbomb " + dir,
                                      [&](int f) -> const AniTextures& { return a.bwalk(f, p); });
    q.standbomb[p][d] = resolve_probed(AssetStore::bwalk_files(), "standbomb " + dir,
                                       [&](int f) -> const AniTextures& { return a.bwalk(f, p); });
    q.punch[p][d] = resolve_probed(AssetStore::punch_files(), "punch " + dir,
                                   [&](int f) -> const AniTextures& { return a.punch(f, p); });
    q.pickup[p][d] = resolve_probed(AssetStore::pickup_files(), "pickup " + dir,
                                    [&](int f) -> const AniTextures& { return a.pickup(f, p); });
}

void resolve_player(SequenceSet& q, const AssetStore& a, int p) {
    // Warp/teleport pose: WALK.ANI "spin" (sub_41F29B warp states 6/7, the
    // strcpy'd literal @0x45a213). Empty -> the warp draw falls back to
    // walk/stand so the player never blanks out mid-warp.
    q.spin[p] = resolve_sequence(a.walk(p), "spin");
    for (int d = 0; d < 4; ++d) resolve_facing(q, a, {p, d});
    // The 13 "cornerhead N" fidgets are spread unevenly across CORNER0..7.ANI.
    for (int i = 0; i < kCornerheadVariants; ++i)
        q.cornerhead[p][i] =
            resolve_probed(AssetStore::corner_files(), "cornerhead " + std::to_string(i),
                           [&](int f) -> const AniTextures& { return a.corner(f, p); });
}

void resolve_powerups(SequenceSet& q, const AssetStore& a) {
    // POWERS.ANI "power <name>", indexed by sim::PowerupType. Names verified
    // against the file; SuperDisease is "power disease3" and Spooger "power
    // spooge" (the file's own spellings). A missing sequence leaves the entry
    // empty -> draw_powerups keeps the static POW*.PCX fallback for that kind.
    static constexpr const char* kPowerNames[sim::kPowerupKinds] = {
        "power bomb",       // ExtraBomb
        "power flame",      // Flame
        "power disease",    // Disease
        "power kicker",     // Kick
        "power skate",      // Skate
        "power punch",      // Punch
        "power grab",       // Grab
        "power spooge",     // Spooger
        "power goldflame",  // Goldflame
        "power trigger",    // Trigger
        "power jelly",      // Jelly
        "power disease3",   // SuperDisease
        "power random",     // Random
    };
    for (int k = 0; k < sim::kPowerupKinds; ++k)
        q.powerup_anim[k] = resolve_sequence(a.powers(), kPowerNames[k]);
    // The Goldman wheel's clogs icon lives in the same file but is not a
    // sim::PowerupType (docs/re/goldman-roulette.md §9.4), so it gets its own
    // slot rather than an extra powerup_anim entry.
    q.clogs_anim = resolve_sequence(a.powers(), "power clog");
}

void resolve_stage_actors(SequenceSet& q, const AssetStore& a) {
    // docs/re/stage-actors.md. The original spellings (sub_4056CA) are "extra
    // conveyor <dir>", "extra arrow <dir>", "extra warp 1" (the parsed arg,
    // always 1 in the shipped files) and "extra trampoline" (no direction).
    // Conveyors live in CONVEYOR.ANI, the rest in EXTRAS.ANI.
    for (int g = 0; g < 4; ++g) {
        const std::string compass = kGodirCompass[g];
        q.conveyor[g] = resolve_sequence(a.conveyor(), "extra conveyor " + compass);
        q.dirarrow[g] = resolve_sequence(a.extras(), "extra arrow " + compass);
        // ALIENS1.ANI, godir-indexed like the stage actors. Missing entries
        // leave the renderer's plain marker fallback in place.
        q.ghost[g] = resolve_sequence(a.aliens1(), "ghost " + compass);
        q.rover[g] = resolve_sequence(a.aliens1(), "rover " + compass);
    }
    q.warphole = resolve_sequence(a.extras(), "extra warp 1");
    q.trampoline = resolve_sequence(a.extras(), "extra trampoline");
}

}  // namespace

void SequenceSet::resolve_stage(const AssetStore& a, int stage) {
    const std::string n = std::to_string(stage);
    brick = resolve_sequence(a.tiles(), "tile " + n + " brick");
    solid = resolve_sequence(a.tiles(), "tile " + n + " solid");
    burn = resolve_sequence(a.xbrick(), "flame brick " + n);
}

void SequenceSet::resolve(const AssetStore& a) {
    digits = resolve_sequence(a.kfont(), "numeric font");
    infinity = resolve_sequence(a.kfont(), "infinity");
    hurry = resolve_sequence(a.hurry(), "hurry");
    // The in-round player row's dead-slot marker: sub_420F07 resolves the
    // literal sequence name "xxx" (aXxx) and blits it over a round-eliminated
    // player's score entry. MISC.ANI carries it verbatim (frame 10, XXXX.TGA,
    // 77x20) — CONFIRMED present in the shipped install, not synthesized text.
    eliminated_marker = resolve_sequence(a.misc(), "xxx");
    shadow = resolve_sequence(a.shadow(), "shadow");
    // Every original player sequence name ends in "green" — the engine recolors
    // the green master sprites per player, and so does AssetStore.
    for (int p = 0; p < kLocalPlayers; ++p) {
        resolve_bombs(*this, a, p);
        resolve_flames(*this, a, p);
        resolve_player(*this, a, p);
    }
    resolve_powerups(*this, a);
    resolve_stage_actors(*this, a);
}

}  // namespace bomber::game
