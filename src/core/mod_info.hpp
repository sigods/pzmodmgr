#pragma once
#include "core/types.hpp"
#include <optional>
#include <string>
#include <vector>
namespace pz {
struct ModInfo {
    std::string id;
    std::string name;
    std::string author;
    std::string mod_version;   // modversion= from mod.info
    std::string version_min;   // minimum supported Project Zomboid build
    std::string version_max;   // maximum supported build, empty = open ended
    std::vector<std::string> required_ids; // require= entries, in mod.info order
    fs::path icon;             // resolved icon PNG referenced by the newest mod.info
};
std::optional<ModInfo> read_mod_info(const fs::path& dir);
std::optional<ModInfo> read_mod_info(const fs::path& dir, const std::string& game_version);

// Formats versionMin/versionMax as "PZ 41.78-42.1", "PZ 41.78+", or
// "PZ up to 42.1". Returns empty when neither bound is set.
std::string version_range(const ModInfo& info);

// True when the mod's versionMin/versionMax range includes the game build.
// An empty game version or empty bounds means the mod is accepted.
bool supports_game_version(const ModInfo& info, const std::string& game_version);

// Returns the folders inside a workshop item that hold mod.info:
//   <item>/mod.info, <item>/mods/<X>/mod.info, <item>/Contents/mods/<X>/mod.info
std::vector<fs::path> discover_mod_roots(const fs::path& content);
}  // namespace pz
