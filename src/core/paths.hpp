#pragma once
#include "core/types.hpp"
namespace pz {
Config default_config();
void load_config(Config& c);          // reads data_dir/config.ini
void save_config(const Config& c);

// Finds a system steamcmd (PATH, common install dirs, or PZMODMGR_STEAMCMD).
// Returns an empty path when nothing is found.
fs::path find_steamcmd();

// The app-managed steamcmd under <Zomboid>/pzmodmgr/steamcmd. It is
// downloaded on first use, so the file may not exist yet.
fs::path builtin_steamcmd_path(const Config& c);
}  // namespace pz
