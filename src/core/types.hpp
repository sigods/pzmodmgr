#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace pz {
namespace fs = std::filesystem;

inline constexpr std::uint32_t kProjectZomboidAppId = 108600;

using LogFn = std::function<void(const std::string&)>;

struct Config {
    fs::path steamcmd_exe;        // built-in path, or a user-set override
    std::string steam_user;       // account that owns PZ; password is cached by steamcmd
    bool dark_mode = false;       // UI theme preference
    bool auto_enable = false;     // add newly installed mods to mods/default.txt
    fs::path zomboid_dir;         // ~/Zomboid or %USERPROFILE%\Zomboid (GOG == Steam)
    fs::path user_mods_dir;       // zomboid_dir/mods
    fs::path data_dir;            // zomboid_dir/pzmodmgr  (state, cache, temporary swap copy)
    // Where settings live. Installed builds use config.ini next to the
    // executable; source/portable builds without it use the Zomboid folder.
    fs::path config_file;
};

struct InstalledMod {
    std::uint64_t workshop_id = 0;
    std::string folder;           // folder name inside user_mods_dir
    std::string name;
    std::uint64_t time_updated = 0; // Steam epoch seconds, 0 = unknown
    // False when the manager adopted a folder it did not install. Removing the
    // record must then leave the folder alone.
    bool owned = true;
};

struct Result {
    bool ok = false;
    std::string message;
};

}  // namespace pz
