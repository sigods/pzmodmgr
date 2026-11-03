#pragma once
#include "core/types.hpp"
#include <optional>
#include <vector>
namespace pz {
struct ModInfo {
    std::string id;
    std::string name;
    std::string author;
    std::string mod_version;   // modversion= from mod.info
    std::string version_min;   // minimum supported Project Zomboid build
    std::string version_max;   // maximum supported build, empty = open ended
    fs::path icon;             // resolved icon PNG referenced by the newest mod.info
};
std::optional<ModInfo> read_mod_info(const fs::path& dir);
std::optional<ModInfo> read_mod_info(const fs::path& dir, const std::string& game_version);

// Compares dotted numeric versions such as 42.19.0 and 42.21. Returns a
// negative/zero/positive value like strcmp. Missing components count as zero.
int compare_versions(const std::string& a, const std::string& b);

// True when the mod's versionMin/versionMax range includes the game build.
// An empty game version or empty bounds means the mod is accepted.
bool supports_game_version(const ModInfo& info, const std::string& game_version);

// Returns the folders inside a workshop item that hold mod.info:
//   <item>/mod.info, <item>/mods/<X>/mod.info, <item>/Contents/mods/<X>/mod.info
std::vector<fs::path> discover_mod_roots(const fs::path& content);
}  // namespace pz
