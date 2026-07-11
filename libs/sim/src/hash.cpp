// FNV-1a digest of the gameplay state. Every field that influences gameplay
// MUST be mixed in here; events are derived per-tick outputs and are excluded.
// The exact byte layout is part of the golden-hash contract
// (tests/test_golden.cpp) — change it only deliberately.

#include "bomber/sim/simulation.hpp"

namespace bomber::sim {

std::uint64_t state_hash(const State& s) {
    std::uint64_t h = 1469598103934665603ULL;
    auto mix = [&h](std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (i * 8)) & 0xFF;
            h *= 1099511628211ULL;
        }
    };
    mix(s.tick);
    mix(s.rng);
    mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(s.ticks_left)));
    mix(static_cast<std::uint64_t>(s.hurry) |
        (static_cast<std::uint64_t>(static_cast<std::uint32_t>(s.enclose_index)) << 8) |
        (static_cast<std::uint64_t>(static_cast<std::uint32_t>(s.enclose_timer)) << 40));
    mix(s.dud_gate);
    // Bomb-id allocator (docs/re/facts.md "Chain-reaction timing"): grows by
    // one per bomb ever created, so it is gameplay state (feeds the pending-
    // chain queue below) even though it never influences arithmetic by
    // itself. A ONE-TIME hash-layout growth; every existing scenario still
    // creates the exact same bombs in the exact same order, so this mixes a
    // deterministic-but-new sequence of values, not a behaviour change.
    mix(static_cast<std::uint64_t>(s.next_bomb_id));
    // Per-level tile regeneration countdown (docs/re/facts.md "Per-level tile
    // regeneration"). A ONE-TIME hash-layout growth (CLAUDE.md determinism
    // contract rule 5; tests/test_golden.cpp recaptured in the same commit).
    // Always 0 on every existing scenario (TileRegenSystem never moves it
    // when tuning.regen_seconds[level] <= 0, true on every level but Haunted
    // House) -> mix(0) for every golden/test scenario, byte-identical
    // gameplay, only the digest layout shifted.
    mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(s.regen_timer)));
    for (int y = 0; y < kGridHeight; ++y) {
        for (int x = 0; x < kGridWidth; ++x) {
            mix(static_cast<std::uint64_t>(s.cells[y][x]) |
                (static_cast<std::uint64_t>(s.hidden[y][x]) << 8) |
                (static_cast<std::uint64_t>(s.floor[y][x]) << 16) |
                (static_cast<std::uint64_t>(s.flame[y][x]) << 24) |
                (static_cast<std::uint64_t>(s.burning[y][x]) << 32) |
                (static_cast<std::uint64_t>(s.flame_owner[y][x]) << 40) |
                // Flame arm-piece kind (docs/re/facts.md "Flame arm-shape
                // selection", 2026-07-10 explosion-draw audit): a NEW hashed
                // field, purely derived from existing bomb/direction/reach
                // data at ignition (no RNG draw), fitting the two spare bytes
                // this packed word already had (bits 48-63 were unused). 0
                // (TipNorth) wherever flame[y][x]==0 (unread there) — a
                // ONE-TIME hash-layout growth like the others in this file:
                // every scenario with active bombs now mixes real, varying,
                // but fully deterministic values here, not a gameplay change
                // (CLAUDE.md determinism contract rule 5; tests/
                // test_golden.cpp recaptured in the same commit).
                (static_cast<std::uint64_t>(s.flame_kind[y][x]) << 48));
        }
    }
    // Stage-actor layout (docs/re/stage-actors.md): static per match but
    // gameplay-affecting like cells, so it must be hashed. Packed one word per
    // tile: low byte = actor_type, next = actor_dir, then the warphole exit
    // tile (warp_dest_x, warp_dest_y). The warp bytes are 0 on non-warp tiles,
    // so a board with no warpholes hashes identically to before this field
    // existed (the golden scenarios place no actors → unchanged).
    for (int y = 0; y < kGridHeight; ++y) {
        for (int x = 0; x < kGridWidth; ++x) {
            mix(static_cast<std::uint64_t>(static_cast<std::uint8_t>(s.actor_type[y][x])) |
                (static_cast<std::uint64_t>(s.actor_dir[y][x]) << 8) |
                (static_cast<std::uint64_t>(s.warp_dest_x[y][x]) << 16) |
                (static_cast<std::uint64_t>(s.warp_dest_y[y][x]) << 24));
        }
    }
    for (const auto& p : s.players) {
        if (!p.present) {
            mix(0xEE);
            continue;
        }
        mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.x)) |
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.y)) << 32));
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
            // Computer-AI flag (ADR-0005): a gameplay input source, so hashed.
            // 0 on every non-AI player → golden scenarios unchanged (bit was
            // previously a constant 0). bits 56..60 hold bombs_placed (small).
            (static_cast<std::uint64_t>(p.ai) << 61) |
            // Stage-actor re-entry latches (#7): gameplay state (they gate re-
            // warp / re-bounce), so hashed. Both 0 on boards with no warpholes/
            // trampolines → golden scenarios unchanged. See stage-actors.md §4-5.
            (static_cast<std::uint64_t>(p.tramp_latch) << 62) |
            (static_cast<std::uint64_t>(p.warp_latch) << 63));
        // Trigger-bomb allowance (player byte +85): its own word so the counter
        // is not truncated. Part of the hashed contract now that #9 caps it.
        mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.trigger_placed)));
        // Team id (player byte +84, docs/re/setup-screens.md; docs/re/ai.md TEAM
        // follow-up): a gameplay input to AI targeting and round-end, so hashed.
        // Own word (not packed into the flags word above, which is full) — a
        // ONE-TIME hash-layout growth. 0 on every existing scenario (default),
        // so this is mix(0) for every golden/test player -> byte-identical
        // gameplay, only the digest layout shifted (CLAUDE.md determinism
        // contract rule 5; tests/test_golden.cpp recaptured in the same commit).
        mix(static_cast<std::uint64_t>(p.team));
        // Clogs count (Goldman wheel booby prize, docs/re/goldman-roulette.md
        // §9): a gameplay input to `speed` (already hashed), so hashed itself
        // like `skates`. Own word — a ONE-TIME hash-layout growth (CLAUDE.md
        // determinism contract rule 5; tests/test_golden.cpp recaptured in
        // the same commit). 0 on every existing scenario (no config sets
        // born_with_clogs), so this is mix(0) for every golden/test player ->
        // byte-identical gameplay, only the digest layout shifted.
        mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.clogs)));
        // Trampoline bounce countdown (Player::bounce, #7), warp countdown
        // (Player::warp) and the pending warp destination tile (warp_to_x/y,
        // captured at step-on): all gate/drive an in-flight warp or bounce, so
        // they are gameplay state and MUST be hashed. Packed into one word —
        // bounce (≤30) / warp (≤18) / dest x,y (≤14) each fit a byte. ALL are 0
        // on boards with no trampolines/warpholes, so this word is mix(0) there,
        // byte-identical to before warp_to_* existed → golden scenarios (no
        // actors) unchanged. See docs/re/stage-actors.md §4-5.
        mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.bounce) & 0xFF) |
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.warp) & 0xFF) << 8) |
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.warp_to_x) & 0xFF) << 16) |
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.warp_to_y) & 0xFF) << 24));
        if (p.carrying)
            mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.carried_fuse)) |
                (static_cast<std::uint64_t>(p.carried_flame) << 32) |
                (static_cast<std::uint64_t>(p.carried_owner) << 48) |
                // Carried bomb kind (set on the thrown bomb in throw_carried) —
                // hashed state while held, not just at throw time.
                (static_cast<std::uint64_t>(p.carried_jelly) << 56) |
                (static_cast<std::uint64_t>(p.carried_trigger) << 57));
        mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.stun)) |
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.move_budget)) << 32));
        // Grab pickup-pause (Player::pickup_pause, player state +78==4):
        // CONFIRMED a separate counter from p.stun above (facts.md "Player
        // state machine (+78) — COMPLETE") — split into its own field
        // 2026-07-11, so it needs its own hash contribution. Own word — a
        // ONE-TIME hash-layout growth (CLAUDE.md determinism contract rule 5;
        // tests/test_golden.cpp recaptured in the same commit). 0 whenever no
        // grab has happened this tick's-worth of history.
        mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.pickup_pause)));
        std::uint32_t dbits = 0;
        for (int k = 0; k < kDiseaseKinds; ++k)
            if (p.disease[k]) dbits |= (1u << k);
        mix(static_cast<std::uint64_t>(dbits) |
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.disease_timer)) << 16) |
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.disease_fresh)) << 40));
        // Ice / input-lag ring buffer (docs/re/facts.md "Ice / input-lag"):
        // live gameplay state (determines a future tick's effective movement
        // direction on Hockey Rink), so hashed — packed 8 bytes/word. A
        // ONE-TIME hash-layout growth (CLAUDE.md determinism contract rule
        // 5; tests/test_golden.cpp recaptured in the same commit). Always
        // all-zero on every existing scenario (MovementSystem::ice_delay
        // never writes it when tuning.ice_delay_ms[level] <= 0, true on
        // every level but Hockey Rink) -> mix(0) x4 for every golden/test
        // player, byte-identical gameplay, only the digest layout shifted.
        for (int base = 0; base < Player::kIceHistoryLen; base += 8) {
            std::uint64_t w = 0;
            for (int k = 0; k < 8 && base + k < Player::kIceHistoryLen; ++k)
                w |= (static_cast<std::uint64_t>(static_cast<std::uint8_t>(p.ice_history[base + k]))
                      << (8 * k));
            mix(w);
        }
    }
    // Computer-AI brains (ADR-0005 §3 / docs/re/ai.md §1.1). Hashed in one
    // clean block, parallel to `players`: every gameplay field of every Brain is
    // mixed. A non-AI/absent player's Brain is zero-initialised, so this is a
    // run of mix(0) words for the golden (no-AI) scenarios — byte-identical to
    // before this block existed apart from those added zero words (the one-time
    // hash-layout growth called out in ADR-0005 §7). The danger/obstacle grids
    // are per-tick scratch (like s.events) and are NEVER hashed.
    for (const auto& br : s.brains) {
        // Scalars + the two flags. personality/state_flag/wander_dir are small;
        // has_path_target packs alongside them.
        mix(static_cast<std::uint64_t>(br.personality) |
            (static_cast<std::uint64_t>(br.state_flag) << 8) |
            (static_cast<std::uint64_t>(static_cast<std::uint8_t>(br.wander_dir)) << 16) |
            (static_cast<std::uint64_t>(br.has_path_target) << 24) |
            (static_cast<std::uint64_t>(static_cast<std::uint16_t>(br.path_target_x)) << 32) |
            (static_cast<std::uint64_t>(static_cast<std::uint16_t>(br.path_target_y)) << 48));
        mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(br.path_target_cost)));
        // Powerup-seek sub-struct (+24/+28/+32/+36).
        mix(static_cast<std::uint64_t>(br.pow_seek.active) |
            (static_cast<std::uint64_t>(static_cast<std::uint16_t>(br.pow_seek.tile_x)) << 8) |
            (static_cast<std::uint64_t>(static_cast<std::uint16_t>(br.pow_seek.tile_y)) << 24) |
            (static_cast<std::uint64_t>(static_cast<std::uint8_t>(br.pow_seek.step_dir)) << 40) |
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(br.pow_seek.timer) & 0xFFFF)
             << 48));
        // Enemy-seek sub-struct (+10/+12/+16/+20).
        mix(static_cast<std::uint64_t>(br.enemy_seek.active) |
            (static_cast<std::uint64_t>(static_cast<std::uint8_t>(br.enemy_seek.target_slot))
             << 8) |
            (static_cast<std::uint64_t>(static_cast<std::uint8_t>(br.enemy_seek.step_dir)) << 16) |
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(br.enemy_seek.timer)) << 32));
    }
    for (const auto& b : s.bombs) {
        // Stable id (docs/re/facts.md "Chain-reaction timing"): own word,
        // needed by the pending-chain queue below to re-find this bomb.
        mix(static_cast<std::uint64_t>(b.id));
        mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(b.x)) |
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(b.y)) << 32));
        mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(b.fuse)) |
            (static_cast<std::uint64_t>(b.flame) << 32) |
            (static_cast<std::uint64_t>(b.moving) << 40) |
            (static_cast<std::uint64_t>(b.flying) << 41) |
            // Bit 42 formerly hashed a bomb warp latch; removed 2026-07-10
            // (facts.md "Bomb/warphole reconciliation") — bombs never warp in
            // the original (sub_4230A5 blocks entry to a warphole tile
            // outright), so the field was dead. Left unused rather than
            // reassigned, to keep every OTHER field's shift stable.
            // Kick+action2 stop flag (sub_4247C5/sub_42331C +57, facts.md
            // "Core-feel audit" §4): gameplay state (it decides where a
            // sliding bomb halts), so hashed.
            (static_cast<std::uint64_t>(b.stop_pending) << 43) |
            (static_cast<std::uint64_t>(b.owner) << 48) |
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(b.fly_ticks) & 0x3F) << 56));
        // fuse_init (creation-time duration, sub_422EDE word +74; facts.md
        // "Core-feel audit" §2/§5): feeds the throw restart and the trigger-
        // eviction relight, so hashed alongside dud_left in the same word.
        mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(b.dud_left)) |
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(b.fuse_init)) << 32));
    }
    // Pending chain-detonation queue (docs/re/facts.md "Chain-reaction
    // timing", sub_423209's dword_4621F8/FC/462200): gameplay state — it
    // determines which bomb(s) forcibly detonate at the top of next tick.
    // Empty on every tick with no in-flight chain reaction/trigger-press/
    // flame-landing, so this is mix(0) for the overwhelming majority of
    // ticks in every scenario.
    mix(static_cast<std::uint64_t>(s.pending_chain.size()));
    for (const auto& pc : s.pending_chain) {
        mix(static_cast<std::uint64_t>(pc.bomb_id) |
            (static_cast<std::uint64_t>(static_cast<std::uint8_t>(pc.skip_dir)) << 32));
    }
    // Campaign rover/ghost hazards (docs/re/campaign.md "Rover/ghost/AI
    // roster", "Per-tick mover"). Empty on every non-campaign match, so this
    // is a ONE-TIME hash-layout growth (mix(0) for the count word, no per-
    // entry words at all) for every existing golden scenario — CLAUDE.md
    // determinism contract rule 5.
    mix(static_cast<std::uint64_t>(s.rovers.size()));
    for (const auto& r : s.rovers) {
        mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(r.x)) |
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(r.y)) << 32));
        mix(static_cast<std::uint64_t>(r.alive) |
            (static_cast<std::uint64_t>(static_cast<std::uint8_t>(r.kind)) << 8) |
            (static_cast<std::uint64_t>(r.dir) << 16) |
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(r.speed)) << 24) |
            (static_cast<std::uint64_t>(r.anim_step) << 48));
        mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(r.move_budget)));
    }
    // Campaign hazard-active flag + grace timer (docs/re/campaign.md "Round
    // pacing" clause 3): both always 0/false on a non-campaign match, so
    // mix(0) for every existing golden scenario.
    mix(static_cast<std::uint64_t>(s.campaign_hazards_active) |
        (static_cast<std::uint64_t>(static_cast<std::uint32_t>(s.hazard_clear_timer)) << 8));
    return h;
}

}  // namespace bomber::sim
