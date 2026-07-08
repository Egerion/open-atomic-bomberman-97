#include "bomber/assets/rmp.hpp"

#include <cstddef>

#include "bomber/assets/binary_reader.hpp"

namespace bomber::assets::res {

RemapTable load_rmp(const std::filesystem::path& path) {
    auto buf = read_file(path);
    BinaryReader r(buf);

    RemapTable t;
    // 256-byte index->index table, then the 3 tail percent bytes. BinaryReader
    // bounds-checks every read (out_of_range on a short file), which the caller
    // catches and turns into a "no .RMP, use the fallback recolour" decision.
    for (std::size_t i = 0; i < t.map.size(); ++i) {
        std::uint8_t v = r.u8();
        // Backfill sub_414A65 (decompile 17498-99): a 0 entry is "not remapped",
        // so it maps to itself. This makes the table total — identity outside the
        // colour band, the colour ramp inside — so a plain `dst = map[src]` blit
        // leaves every non-band pixel (shadow, casing, transparent) untouched.
        t.map[i] = (v == 0) ? static_cast<std::uint8_t>(i) : v;
    }
    t.rgb[0] = r.u8();
    t.rgb[1] = r.u8();
    t.rgb[2] = r.u8();
    return t;
}

}  // namespace bomber::assets::res
