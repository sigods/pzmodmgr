#pragma once
#include "core/types.hpp"
#include <cstdint>
#include <vector>

namespace pz {
struct ModIcon {
    int size = 0;
    std::vector<std::uint8_t> rgba;   // size * size * 4, top-down
};

// Decodes a mod icon and center-crops it into a square thumbnail.
bool load_mod_icon(const fs::path& path, int size, ModIcon& out);
}  // namespace pz
