#include "bomber/net/match_config_codec.hpp"

#include <utility>

namespace bomber::net {

namespace {

// The two halves of ONE visitor (see the header): Writer appends, Reader
// consumes, and both expose the identical call interface so visit_config()
// below is the single place a field is named. That symmetry is deliberate — the
// classic serialization bug is a field added to the encoder and forgotten in the
// decoder, which here is unrepresentable.
//
// Cfg is the config type the visitor walks: `const sim::MatchConfig` when
// writing (so every member binds to Writer's const& parameters) and plain
// `sim::MatchConfig` when reading.

class Writer {
public:
    void byte(const std::uint8_t& v) { out_.push_back(v); }
    void flag(const bool& v) { out_.push_back(v ? 1U : 0U); }
    void u16(const std::uint16_t& v) {
        for (int i = 0; i < 2; ++i)
            out_.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFU));
    }
    void u32(const std::uint32_t& v) {
        for (int i = 0; i < 4; ++i)
            out_.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFU));
    }
    void i32(const std::int32_t& v) { u32(static_cast<std::uint32_t>(v)); }
    void cell(const sim::Cell& v) { out_.push_back(static_cast<std::uint8_t>(v)); }
    void actor(const sim::ActorType& v) { out_.push_back(static_cast<std::uint8_t>(v)); }

    // Spawns beyond sim::kMaxPlayers are dropped: setup.cpp indexes this vector
    // by PLAYER NUMBER, so nothing past slot 9 can ever reach the sim, and a
    // hand-edited .SCH with a `-S,50,...` row must not be able to inflate the
    // datagram. Loss-free for gameplay, and it keeps the blob bounded.
    void spawns(const std::vector<sim::SpawnPoint>& v) {
        const std::size_t n = v.size() < static_cast<std::size_t>(sim::kMaxPlayers)
                                  ? v.size()
                                  : static_cast<std::size_t>(sim::kMaxPlayers);
        out_.push_back(static_cast<std::uint8_t>(n));
        for (std::size_t i = 0; i < n; ++i) {
            i32(v[i].x);
            i32(v[i].y);
        }
    }

    std::vector<std::uint8_t> take() { return std::move(out_); }

private:
    std::vector<std::uint8_t> out_;
};

class Reader {
public:
    Reader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    void byte(std::uint8_t& v) {
        if (!want(1)) return;
        v = data_[off_++];
    }
    void flag(bool& v) {
        if (!want(1)) return;
        v = data_[off_++] != 0;
    }
    void u16(std::uint16_t& v) {
        if (!want(2)) return;
        v = static_cast<std::uint16_t>(static_cast<unsigned>(data_[off_]) |
                                       (static_cast<unsigned>(data_[off_ + 1]) << 8));
        off_ += 2;
    }
    void u32(std::uint32_t& v) {
        if (!want(4)) return;
        std::uint32_t u = 0;
        for (int i = 0; i < 4; ++i) u |= static_cast<std::uint32_t>(data_[off_ + i]) << (8 * i);
        off_ += 4;
        v = u;
    }
    void i32(std::int32_t& v) {
        std::uint32_t u = 0;
        u32(u);
        v = static_cast<std::int32_t>(u);
    }
    void cell(sim::Cell& v) {
        if (!want(1)) return;
        const std::uint8_t b = data_[off_++];
        if (b > static_cast<std::uint8_t>(sim::Cell::Solid)) {
            ok_ = false;  // untrusted: only Blank/Brick/Solid exist
            return;
        }
        v = static_cast<sim::Cell>(b);
    }
    void actor(sim::ActorType& v) {
        if (!want(1)) return;
        const std::uint8_t b = data_[off_++];
        // The four real actor kinds mirror the original's actor+4 type field
        // (0..3); 255 is our "no actor" sentinel. Nothing else is legal.
        if (b > static_cast<std::uint8_t>(sim::ActorType::Trampoline) &&
            b != static_cast<std::uint8_t>(sim::ActorType::None)) {
            ok_ = false;
            return;
        }
        v = static_cast<sim::ActorType>(b);
    }
    void spawns(std::vector<sim::SpawnPoint>& v) {
        std::uint8_t n = 0;
        byte(n);
        if (!ok_) return;
        if (n > sim::kMaxPlayers) {
            ok_ = false;  // the writer never emits more; a bigger count is hostile
            return;
        }
        v.assign(n, sim::SpawnPoint{});
        for (std::uint8_t i = 0; i < n; ++i) {
            i32(v[i].x);
            i32(v[i].y);
        }
    }

    bool ok() const { return ok_; }
    bool at_end() const { return off_ == size_; }

private:
    // Every read funnels through here, so no path can walk off the buffer: once
    // ok_ is false every subsequent field is a no-op and the caller discards the
    // half-filled scratch config.
    bool want(std::size_t n) {
        if (!ok_ || size_ - off_ < n) {
            ok_ = false;
            return false;
        }
        return true;
    }

    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t off_ = 0;
    bool ok_ = true;
};

// Tuning's fields in DECLARATION order (tuning.hpp): 157 int32 then the four
// bool flags. Split out from visit_config only for readability — it is the same
// single-definition rule.
template <class Ar, class Tun>
void visit_tuning(Ar& ar, Tun& t) {
    ar.i32(t.input_freeze_ticks);
    ar.i32(t.fuse_frames);
    ar.i32(t.start_speed);
    ar.i32(t.skate_speed_bonus);
    ar.i32(t.clogs_speed_penalty);
    ar.i32(t.kicked_bomb_speed);
    ar.i32(t.punched_bomb_speed);
    ar.i32(t.game_seconds);
    ar.i32(t.taunt_chance);
    ar.i32(t.hurry_seconds);
    ar.i32(t.overpowered_relocate_seconds);
    ar.i32(t.enclosement_depth);
    ar.i32(t.wall_detonates);
    for (auto& v : t.start_with) ar.i32(v);
    for (auto& v : t.limits) ar.i32(v);
    for (auto& v : t.spawn_counts) ar.i32(v);
    for (auto& row : t.color_rgb)
        for (auto& v : row) ar.i32(v);
    for (auto& v : t.level_enabled) ar.i32(v);
    ar.i32(t.level_index);
    for (auto& v : t.regen_seconds) ar.i32(v);
    ar.i32(t.regen_clear_radius);
    for (auto& v : t.ice_delay_ms) ar.i32(v);
    ar.i32(t.pickup_pause);
    ar.i32(t.powers_lost_min);
    ar.i32(t.powers_lost_rand);
    ar.i32(t.head_stun_frames);
    ar.i32(t.punch_arc_first);
    ar.i32(t.punch_arc_hop);
    ar.i32(t.taunt_many_bombs);
    ar.i32(t.taunt_many_chance);
    ar.i32(t.jelly_turn_chance);
    ar.i32(t.dud_gate_base);
    ar.i32(t.dud_gate_rand);
    ar.i32(t.dud_chance);
    ar.i32(t.dud_frames);
    ar.i32(t.flame_frames);
    ar.i32(t.brick_burn_frames);
    ar.i32(t.conveyor_speed_count);
    for (auto& v : t.conveyor_speeds) ar.i32(v);
    ar.i32(t.conveyor_speed_index);
    ar.i32(t.trampoline_bounce_frames);
    ar.i32(t.ai_personalities);
    ar.i32(t.fire_god_lookahead);
    ar.i32(t.ai_blast_chance);
    ar.i32(t.ai_powerup_range);
    for (auto& v : t.disease_frames) ar.i32(v);
    ar.i32(t.disease_freshness);
    ar.i32(t.disease_cure_chance);
    ar.flag(t.diseases_time_limited);
    ar.flag(t.diseases_multiply);
    ar.flag(t.diseases_curable);
    ar.flag(t.diseases_destroyable);
    ar.i32(t.rover_turn_chance);
    ar.i32(t.campaign_ai_kill_score);
    ar.i32(t.rover_kill_score);
    ar.i32(t.ghost_kill_score);
}

template <class Ar, class Cfg>
void visit_config(Ar& ar, Cfg& cfg) {
    for (auto& row : cfg.cells)
        for (auto& c : row) ar.cell(c);
    for (auto& row : cfg.actor_type)
        for (auto& a : row) ar.actor(a);
    for (auto& row : cfg.actor_dir)
        for (auto& d : row) ar.byte(d);
    for (auto& row : cfg.warp_dest_x)
        for (auto& d : row) ar.byte(d);
    for (auto& row : cfg.warp_dest_y)
        for (auto& d : row) ar.byte(d);
    ar.spawns(cfg.spawns);
    ar.i32(cfg.player_count);
    for (auto& v : cfg.ai) ar.flag(v);
    for (auto& v : cfg.active) ar.flag(v);
    for (auto& v : cfg.team) ar.byte(v);
    ar.u32(cfg.seed);
    visit_tuning(ar, cfg.tuning);
    for (auto& v : cfg.spawn_override) ar.i32(v);
    for (auto& v : cfg.forbidden) ar.flag(v);
    for (auto& slot : cfg.born_with_extra)
        for (auto& v : slot) ar.flag(v);
    for (auto& v : cfg.born_with_clogs) ar.i32(v);
    ar.i32(cfg.campaign_rovers);
    ar.i32(cfg.campaign_rover_speed);
    ar.i32(cfg.campaign_ghosts);
    ar.i32(cfg.campaign_ghost_speed);
}

}  // namespace

std::vector<std::uint8_t> encode_match_config(const sim::MatchConfig& cfg) {
    Writer w;
    w.u16(kMatchConfigLayout);
    visit_config(w, cfg);
    return w.take();
}

bool decode_match_config(const std::uint8_t* data, std::size_t size, sim::MatchConfig* out) {
    if (data == nullptr || out == nullptr) return false;
    if (size > kMaxMatchConfigBytes) return false;  // bound before any work

    Reader r(data, size);
    std::uint16_t layout = 0;
    r.u16(layout);
    if (!r.ok() || layout != kMatchConfigLayout) return false;

    sim::MatchConfig scratch;  // never *out until the whole blob validates
    visit_config(r, scratch);
    // Exact-length framing, like input_codec's deserialize(): trailing bytes mean
    // the sender disagrees with us about the layout, which is a desync waiting to
    // happen, not something to shrug off.
    if (!r.ok() || !r.at_end()) return false;

    *out = std::move(scratch);
    return true;
}

}  // namespace bomber::net
