#pragma once
#include "core/types.hpp"
#include <optional>
#include <vector>
namespace pz {
struct ModInfo {
    std::string id;
    std::string name;
};
std::optional<ModInfo> read_mod_info(const fs::path& dir);

// Returns the folders inside a workshop item that hold mod.info:
//   <item>/mod.info, <item>/mods/<X>/mod.info, <item>/Contents/mods/<X>/mod.info
std::vector<fs::path> discover_mod_roots(const fs::path& content);
}  // namespace pz
