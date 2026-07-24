// Checks for the `.RMP` player-colour remap parser. Faithful to the original's
// load + backfill (sub_414A65, decompile 17498-99): a 256-byte index->index
// table with 0-entries backfilled to identity, plus 3 tail bytes = R,G,B percent.
// See docs/re/player-colour.md. SYNTHETIC buffers only — no original bytes.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <fstream>
#include <vector>

#include "bomber/assets/rmp.hpp"

using namespace bomber::assets::res;

namespace {

// Writes a synthetic 259-byte .RMP to a temp path and returns it. The table is
// built by the caller; the 3 tail percent bytes follow.
std::filesystem::path write_rmp(const std::array<std::uint8_t, 256>& table,
                                std::array<std::uint8_t, 3> tail, const char* name) {
    auto p = std::filesystem::temp_directory_path() / name;
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(table.data()), 256);
    f.write(reinterpret_cast<const char*>(tail.data()), 3);
    f.close();
    return p;
}

}  // namespace

TEST_CASE("zero entries backfill to identity; non-zero entries stay") {
    std::array<std::uint8_t, 256> table{};  // all zero
    // A synthetic colour band [100..102] remapped to [200..202]; the rest 0.
    table[100] = 200;
    table[101] = 201;
    table[102] = 202;
    auto path = write_rmp(table, {100, 0, 10}, "obm_test_band.rmp");

    RemapTable t = load_rmp(path);

    // Backfill: every 0 entry becomes its own index (identity).
    CHECK(t.map[0] == 0);
    CHECK(t.map[50] == 50);
    CHECK(t.map[255] == 255);
    // The band's non-zero entries are preserved verbatim.
    CHECK(t.map[100] == 200);
    CHECK(t.map[101] == 201);
    CHECK(t.map[102] == 202);
    // Index 200 itself was 0 in the file -> backfilled to identity, not touched
    // by the band above it.
    CHECK(t.map[200] == 200);

    std::filesystem::remove(path);
}

TEST_CASE("a non-zero entry that equals its own index survives") {
    std::array<std::uint8_t, 256> table{};
    table[7] = 7;  // explicitly identity in the file (not a 0 hole)
    auto path = write_rmp(table, {0, 0, 0}, "obm_test_identity.rmp");

    RemapTable t = load_rmp(path);
    CHECK(t.map[7] == 7);
    std::filesystem::remove(path);
}

TEST_CASE("tail bytes are read as R,G,B percent in order") {
    std::array<std::uint8_t, 256> table{};
    // Red-like tail 100,0,10 (mirrors 2.RMP's documented `64 00 0a`).
    auto path = write_rmp(table, {100, 0, 10}, "obm_test_tail.rmp");

    RemapTable t = load_rmp(path);
    CHECK(t.rgb[0] == 100);
    CHECK(t.rgb[1] == 0);
    CHECK(t.rgb[2] == 10);
    std::filesystem::remove(path);
}

TEST_CASE("a full-band table remaps every index and preserves the tail") {
    // Every index maps to (i+10) mod 256, mimicking a dense remap. No 0 holes
    // except index 246 (-> 0 would backfill, so use 246->0 stays 0? no: value 0
    // backfills to 246). Keep it simple: i -> (i+10)&0xFF but force a 0 hole.
    std::array<std::uint8_t, 256> table{};
    for (int i = 0; i < 256; ++i)
        table[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((i + 10) & 0xFF);
    table[42] = 0;  // a deliberate hole -> must backfill to 42
    auto path = write_rmp(table, {50, 0, 100}, "obm_test_full.rmp");

    RemapTable t = load_rmp(path);
    CHECK(t.map[0] == 10);
    CHECK(t.map[100] == 110);
    CHECK(t.map[250] == 4);   // (250+10)&0xFF = 4
    CHECK(t.map[42] == 42);   // the 0 hole backfilled to identity
    CHECK(t.rgb[0] == 50);
    CHECK(t.rgb[2] == 100);
    std::filesystem::remove(path);
}

TEST_CASE("a short file throws (untrusted input)") {
    auto p = std::filesystem::temp_directory_path() / "obm_test_short.rmp";
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    std::vector<std::uint8_t> partial(100, 0);  // < 259 bytes
    f.write(reinterpret_cast<const char*>(partial.data()),
            static_cast<std::streamsize>(partial.size()));
    f.close();

    CHECK_THROWS(load_rmp(p));
    std::filesystem::remove(p);
}

TEST_CASE("missing the 3 tail bytes throws") {
    // Exactly 256 bytes: the table reads fine but the R,G,B tail runs past EOF.
    std::array<std::uint8_t, 256> table{};
    auto p = std::filesystem::temp_directory_path() / "obm_test_notail.rmp";
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(table.data()), 256);
    f.close();

    CHECK_THROWS(load_rmp(p));
    std::filesystem::remove(p);
}
