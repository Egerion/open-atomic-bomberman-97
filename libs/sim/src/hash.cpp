// FNV-1a digest of the gameplay state. Every field that influences gameplay
// MUST be mixed in here (determinism contract rule 4).
//
// THE EXCLUSION REGISTER. Everything in State/Player/Bomb/Brain/Rover is hashed
// EXCEPT the five below, and this list is exhaustive on purpose: until the
// 2026-08-01 sweep the packed words documented their UNUSED BITS to the bit
// while nine genuinely missing fields went unmentioned, so a reader could not
// tell a deliberate omission from a forgotten one. If you add a field, either
// mix it or add it here with a reason.
//   State::events     derived per-tick output: rebuilt every tick, never read
//                     back by the sim (rule 4 names it explicitly).
//   State::sub_trace  the same, for the renderer's intra-tick motion replay.
//   State::tuning     static per-match config. Two peers holding different
//                     tuning diverge in gameplay within a few ticks and the
//                     hash catches it there; SetupSession puts the host's copy
//                     on the wire, and build_hash guards the code that reads it.
//   State::forbidden  static per-match config, as tuning.
//   AISystem::danger_/obstacle_  not State at all — per-tick caches rebuilt from
//                     hashed inputs and owned by the system.
// Two fields are hashed that a reader might expect here, and are NOT excluded:
// Bomb::fly_arc and Rover::anim_step drive only draw height and draw frame, but
// both are persistent struct state rather than per-tick outputs, so rule 4
// covers them and nobody has to relitigate it per field.
//
// The exact byte layout is part of the golden-hash contract
// (tests/sim/test_golden.cpp), so the comments below record what each PACKED
// word's bit ranges mean and which ranges are deliberately left unused. Growing
// the layout is a deliberate act: it shifts every pinned constant even when no
// gameplay moved, and the goldens are recaptured in the same commit.
//
// tests/sim/test_hash_coverage.cpp is the mechanical check on all of the above:
// it perturbs every field of every struct and fails on any that leaves the
// digest unmoved, and it pins the exclusions above by asserting they DON'T.

#include "bomber/sim/simulation.hpp"

#include "grid.hpp"

namespace bomber::sim {
namespace {

// The FNV-1a accumulator as a callable, so a per-struct mixer can be lifted out
// of state_hash without either copying the fold or changing a single call site:
// `mix(word)` reads the same in here as it did as a lambda. The BYTE STREAM is
// the golden-hash contract, so a mixer that moves out must fold exactly the
// same words in exactly the same order.
struct Fnv {
    std::uint64_t h = 1469598103934665603ULL;
    void operator()(std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (i * 8)) & 0xFF;
            h *= 1099511628211ULL;
        }
    }
};

// One bomb. Lifted out of state_hash rather than nested inside its loop because
// the airborne block below is a second level of nesting, and the complexity
// ratchet (scripts/complexity.sh) charges for depth: state_hash is baselined at
// 26 and may not get worse.
void mix_bomb(Fnv& mix, const Bomb& b) {
    mix(static_cast<std::uint64_t>(b.id));  // the pending-chain queue's key
    mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(b.x)) |
        (static_cast<std::uint64_t>(static_cast<std::uint32_t>(b.y)) << 32));
    // Bits: 0 fuse, 32 flame, 40 moving, 41 flying, 42 jelly, 43 stop_pending
    // (the sub_4247C5/+57 kick-stop flag, which decides where a sliding bomb
    // halts), 44 colour (a slot < kMaxPlayers, 4 bits; equal to `owner` except
    // on a chain-transferred bomb's final tick), 48 owner, 56 trigger, 57
    // active, 58 dir (6 bits).
    //
    // jelly, trigger, dir and active were absent until the 2026-08-01 rule-4
    // sweep. jelly decides whether a kicked bomb reverses off an obstacle and
    // whether a flight boundary DRAWS the veer roll — a divergence there
    // desynchronises the RNG stream itself, not just the board. trigger decides
    // eligibility for detonate_triggered; dir is the slide and flight heading.
    // Bit 42 held a bomb warp latch removed 2026-07-10 (bombs never warp:
    // sub_4230A5 blocks entry to a warphole tile outright, facts.md
    // "Bomb/warphole reconciliation") and was left free to keep the other shifts
    // stable, so spending it on jelly costs nothing. fly_ticks MOVED to the
    // flight word below, where it and fly_total get 32 bits each: the 6-bit mask
    // it had here overflows silently once VALUELST id 301 (punched_bomb_speed)
    // is tuned down far enough to make a leg 64+ ticks.
    // One field per line: this IS the bit table the comment above describes, and
    // packing two shifts onto a line (which the shorter indent now leaves room
    // for) makes it unreviewable against that map.
    // clang-format off
    mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(b.fuse)) |
        (static_cast<std::uint64_t>(b.flame) << 32) |
        (static_cast<std::uint64_t>(b.moving) << 40) |
        (static_cast<std::uint64_t>(b.flying) << 41) |
        (static_cast<std::uint64_t>(b.jelly) << 42) |
        (static_cast<std::uint64_t>(b.stop_pending) << 43) |
        (static_cast<std::uint64_t>(b.colour & 0xF) << 44) |
        (static_cast<std::uint64_t>(b.owner) << 48) |
        (static_cast<std::uint64_t>(b.trigger) << 56) |
        (static_cast<std::uint64_t>(b.active) << 57) |
        (static_cast<std::uint64_t>(static_cast<std::uint8_t>(b.dir) & 0x3F) << 58));
    // clang-format on
    // fuse_init (+74) feeds the throw restart and the trigger-eviction relight
    // (facts.md "Core-feel audit" §2/§5).
    mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(b.dud_left)) |
        (static_cast<std::uint64_t>(static_cast<std::uint32_t>(b.fuse_init)) << 32));
    // created_tick (+64) gates same-tick trigger detonation (bombs.md #4).
    mix(b.created_tick);
    // The airborne leg, gated on `flying` exactly as the carried-bomb payload is
    // gated on `carrying`: BombSystem::fly and the renderer are the only readers
    // and both test `flying` first, so on a grounded bomb these are a stale leg
    // nothing consults and the next launch() rewrites all six together. fly_arc
    // drives only the draw height, and is hashed on the same ground as
    // Rover::anim_step (rover.hpp) — persistent struct state, not a per-tick
    // derived output, so rule 4 covers it and no reader has to relitigate
    // whether it counts as gameplay.
    if (!b.flying) return;
    mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(b.fly_ticks)) |
        (static_cast<std::uint64_t>(static_cast<std::uint32_t>(b.fly_total)) << 32));
    mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(b.from_x)) |
        (static_cast<std::uint64_t>(static_cast<std::uint32_t>(b.from_y)) << 32));
    mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(b.to_x)) |
        (static_cast<std::uint64_t>(static_cast<std::uint32_t>(b.to_y)) << 32));
    mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(b.fly_arc)));
}

}  // namespace

std::uint64_t state_hash(const State& s) {
    Fnv mix;
    mix(s.tick);
    mix(s.rng);
    mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(s.ticks_left)));
    mix(static_cast<std::uint64_t>(s.hurry) |
        (static_cast<std::uint64_t>(static_cast<std::uint32_t>(s.enclose_index)) << 8) |
        (static_cast<std::uint64_t>(static_cast<std::uint32_t>(s.enclose_timer)) << 40));
    // The spiral's ARMED flag as much as its cadence: EnclosureSystem::update
    // arms once on the first closing tick and gates the whole drop branch on
    // `enclose_interval > 0`, so two peers can disagree about whether the walls
    // are closing at all. A non-zero interval is IMPLIED by the non-zero
    // enclose_timer hashed above and NOTHING ENFORCES THAT, which is what kept
    // the gap latent rather than absent. Its own word, not spare bits in the
    // word above: the field is an int32 and a masked pack would trade this
    // hazard for the silent-truncation one.
    mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(s.enclose_interval)));
    mix(s.dud_gate);
    // Bomb-id allocator: grows by one per bomb ever created, and the pending-
    // chain queue below re-finds bombs by that id (facts.md "Chain-reaction
    // timing"), so it is gameplay state even though it drives no arithmetic.
    mix(static_cast<std::uint64_t>(s.next_bomb_id));
    // Per-level tile regeneration countdown (facts.md "Per-level tile
    // regeneration"). Moves only on Haunted House.
    mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(s.regen_timer)));

    // The field, row-major. Bits: 0 cells, 8 hidden, 16 floor, 24 flame, 32
    // burning, 40 flame_owner, 48 flame_kind (the arm-piece shape, derived at
    // ignition with no RNG draw — facts.md "Flame arm-shape selection"), 56
    // flame_colour (the igniting bomb's creation-time colour, which differs from
    // flame_owner because a chain hit rewrites only the latter — facts.md
    // "Bomb/flame colour is not the owner"). The last two read 0 wherever
    // flame == 0, where nothing reads them.
    grid::for_each_cell([&](int x, int y) {
        mix(static_cast<std::uint64_t>(s.cells[y][x]) |
            (static_cast<std::uint64_t>(s.hidden[y][x]) << 8) |
            (static_cast<std::uint64_t>(s.floor[y][x]) << 16) |
            (static_cast<std::uint64_t>(s.flame[y][x]) << 24) |
            (static_cast<std::uint64_t>(s.burning[y][x]) << 32) |
            (static_cast<std::uint64_t>(s.flame_owner[y][x]) << 40) |
            (static_cast<std::uint64_t>(s.flame_kind[y][x]) << 48) |
            (static_cast<std::uint64_t>(s.flame_colour[y][x]) << 56));
    });

    // Stage-actor layout (docs/re/stage-actors.md): static per match but
    // gameplay-affecting like cells. Bits: 0 actor_type, 8 actor_dir, 16/24 the
    // warphole exit tile.
    grid::for_each_cell([&](int x, int y) {
        mix(static_cast<std::uint64_t>(static_cast<std::uint8_t>(s.actor_type[y][x])) |
            (static_cast<std::uint64_t>(s.actor_dir[y][x]) << 8) |
            (static_cast<std::uint64_t>(s.warp_dest_x[y][x]) << 16) |
            (static_cast<std::uint64_t>(s.warp_dest_y[y][x]) << 24));
    });

    for (const auto& p : s.players) {
        if (!p.present) {
            mix(0xEE);
            continue;
        }
        mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.x)) |
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.y)) << 32));
        // Bits 56..60 hold bombs_placed (small); bit 61 is Player::ai, a gameplay
        // input SOURCE (ADR-0005). Bits 62/63 formerly held the stage-actor
        // re-entry latches (tramp_latch/warp_latch), removed 2026-07-28 —
        // the original has no such player state, its guard against re-entering
        // an exit is geometric (facts.md "Warphole/trampoline entry predicate").
        // Left unused rather than reassigned, so every other shift stays stable.
        mix(static_cast<std::uint64_t>(p.alive) | (static_cast<std::uint64_t>(p.max_bombs) << 8) |
            (static_cast<std::uint64_t>(p.flame) << 16) |
            (static_cast<std::uint64_t>(p.skates) << 24) |
            (static_cast<std::uint64_t>(p.speed) << 32) |
            (static_cast<std::uint64_t>(p.kick) << 48) |
            (static_cast<std::uint64_t>(p.punch) << 49) |
            (static_cast<std::uint64_t>(p.grab) << 50) |
            (static_cast<std::uint64_t>(p.carrying) << 51) |
            (static_cast<std::uint64_t>(p.spooge) << 52) |
            (static_cast<std::uint64_t>(p.goldflame) << 53) |
            (static_cast<std::uint64_t>(p.trigger) << 54) |
            (static_cast<std::uint64_t>(p.jelly) << 55) |
            (static_cast<std::uint64_t>(p.bombs_placed) << 56) |
            (static_cast<std::uint64_t>(p.ai) << 61));
        // Trigger-bomb allowance (+85): its own word so the counter cannot be
        // truncated by the packing above.
        mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.trigger_placed)));
        // Team id (+84): a gameplay input to AI targeting and to round-end.
        mix(static_cast<std::uint64_t>(p.team));
        // Clogs (docs/re/goldman-roulette.md §9): an input to `speed`, like skates.
        mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.clogs)));
        // In-flight trampoline/warp: bounce (<=30), warp (<=18) and the pending
        // warp destination tile captured at step-on (<=14 each) — one byte apiece.
        // docs/re/stage-actors.md §4-5.
        mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.bounce) & 0xFF) |
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.warp) & 0xFF) << 8) |
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.warp_to_x) & 0xFF) << 16) |
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.warp_to_y) & 0xFF) << 24));
        // The carried bomb's payload, hashed while HELD and not only at throw
        // time. Bits: 0 fuse, 32 flame, 48 owner, 56/57 kind, 58 colour (a slot
        // < kMaxPlayers, so 4 bits).
        if (p.carrying)
            mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.carried_fuse)) |
                (static_cast<std::uint64_t>(p.carried_flame) << 32) |
                (static_cast<std::uint64_t>(p.carried_owner) << 48) |
                (static_cast<std::uint64_t>(p.carried_jelly) << 56) |
                (static_cast<std::uint64_t>(p.carried_trigger) << 57) |
                (static_cast<std::uint64_t>(p.carried_colour & 0xF) << 58));
        mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.stun)) |
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.move_budget)) << 32));
        // Grab pickup-pause (+78 == 4): a SEPARATE counter from p.stun above
        // (facts.md "Player state machine (+78) — COMPLETE"), so its own word.
        mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.pickup_pause)));
        // Action-key edge latches (+54/+55). The bomb-action tail's drop and
        // punch edges read them, so two sims agreeing on everything else but
        // these disagree on the NEXT tick's placement.
        mix(static_cast<std::uint64_t>(p.prev_action1 ? 1u : 0u) |
            (static_cast<std::uint64_t>(p.prev_action2 ? 1u : 0u) << 1));
        // Facing (+46) is gameplay state, not a draw pose: it decides WHERE a
        // bomb goes — the kick direction, the punch's target tile and launch
        // direction, the throw's launch direction. Two sims agreeing on every
        // position and counter but differing here send the next punched bomb to
        // different tiles, and the per-tick hash exchange used to be blind to it,
        // so the desync detector stayed quiet through exactly the divergence it
        // exists to catch.
        mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.facing)));
        std::uint32_t dbits = 0;
        for (int k = 0; k < kDiseaseKinds; ++k)
            if (p.disease[k]) dbits |= (1u << k);
        mix(static_cast<std::uint64_t>(dbits) |
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.disease_timer)) << 16) |
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.disease_fresh)) << 40));
        // Ice / input-lag ring buffer (facts.md "Ice / input-lag"): it determines
        // a future tick's effective movement direction on Hockey Rink. Packed 8
        // bytes per word.
        for (int base = 0; base < Player::kIceHistoryLen; base += 8) {
            std::uint64_t w = 0;
            for (int k = 0; k < 8 && base + k < Player::kIceHistoryLen; ++k)
                w |= (static_cast<std::uint64_t>(static_cast<std::uint8_t>(p.ice_history[base + k]))
                      << (8 * k));
            mix(w);
        }
    }

    // Computer-AI brains (ADR-0005 §3 / docs/re/ai.md §1.1), in one block
    // parallel to `players`: every gameplay field of every Brain.
    for (const auto& br : s.brains) {
        mix(static_cast<std::uint64_t>(br.personality) |
            (static_cast<std::uint64_t>(br.state_flag) << 8) |
            (static_cast<std::uint64_t>(static_cast<std::uint8_t>(br.wander_dir)) << 16) |
            (static_cast<std::uint64_t>(br.has_path_target) << 24) |
            (static_cast<std::uint64_t>(static_cast<std::uint16_t>(br.path_target_x)) << 32) |
            (static_cast<std::uint64_t>(static_cast<std::uint16_t>(br.path_target_y)) << 48));
        mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(br.path_target_cost)));
        mix(static_cast<std::uint64_t>(br.pow_seek.active) |  // the +24/+28/+32/+36 sub-struct
            (static_cast<std::uint64_t>(static_cast<std::uint16_t>(br.pow_seek.tile_x)) << 8) |
            (static_cast<std::uint64_t>(static_cast<std::uint16_t>(br.pow_seek.tile_y)) << 24) |
            (static_cast<std::uint64_t>(static_cast<std::uint8_t>(br.pow_seek.step_dir)) << 40) |
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(br.pow_seek.timer) & 0xFFFF)
             << 48));
        mix(static_cast<std::uint64_t>(br.enemy_seek.active) |  // the +10/+12/+16/+20 sub-struct
            (static_cast<std::uint64_t>(static_cast<std::uint8_t>(br.enemy_seek.target_slot))
             << 8) |
            (static_cast<std::uint64_t>(static_cast<std::uint8_t>(br.enemy_seek.step_dir)) << 16) |
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(br.enemy_seek.timer)) << 32));
    }

    for (const auto& b : s.bombs) mix_bomb(mix, b);

    // Pending chain-detonation queue (facts.md "Chain-reaction timing",
    // sub_423209's dword_4621F8/FC/462200): it determines which bombs forcibly
    // detonate at the top of next tick.
    mix(static_cast<std::uint64_t>(s.pending_chain.size()));
    for (const auto& pc : s.pending_chain) {
        mix(static_cast<std::uint64_t>(pc.bomb_id) |
            (static_cast<std::uint64_t>(static_cast<std::uint8_t>(pc.skip_dir)) << 32));
    }

    // Campaign rover/ghost hazards (docs/re/campaign.md "Rover/ghost/AI roster",
    // "Per-tick mover").
    mix(static_cast<std::uint64_t>(s.rovers.size()));
    for (const auto& r : s.rovers) {
        mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(r.x)) |
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(r.y)) << 32));
        mix(static_cast<std::uint64_t>(r.alive) |
            (static_cast<std::uint64_t>(static_cast<std::uint8_t>(r.kind)) << 8) |
            (static_cast<std::uint64_t>(r.dir) << 16) |
            (static_cast<std::uint64_t>(r.anim_step) << 48));
        // speed moved down beside move_budget, where both get a clean 32 bits.
        // At `<< 24` in the word above its uint32 spanned bits 24-55 and OVERLAID
        // anim_step's 48-63, so the two ORed together and distinct (speed,
        // anim_step) pairs could collide into one word. Every field WAS mixed —
        // this is the other half of rule 4, that mixing a field and DISCRIMINATING
        // it are not the same thing. Latent only because a .CAM speed never
        // reaches 2^24; nothing enforces that either.
        mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(r.move_budget)) |
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(r.speed)) << 32));
    }

    // Campaign hazard-active flag + grace timer (docs/re/campaign.md "Round
    // pacing" clause 3), and at bits 40-55 the round-start input freeze
    // (dword_4621E0, facts.md "Round-start input freeze"), which gates input and
    // AI acquisition.
    mix(static_cast<std::uint64_t>(s.campaign_hazards_active) |
        (static_cast<std::uint64_t>(static_cast<std::uint32_t>(s.hazard_clear_timer)) << 8) |
        (static_cast<std::uint64_t>(static_cast<std::uint32_t>(s.input_freeze) & 0xFFFF) << 40));
    return mix.h;
}

}  // namespace bomber::sim
