#pragma once
#include "core/types.hpp"
namespace pz {
Config default_config();
// Reads config_file (or data_dir/config.ini when config_file is unset) and
// applies zomboid_dir before rebasing the built-in steamcmd path.
void load_config(Config& c);
// Writes config_file (or data_dir/config.ini when config_file is unset).
// Returns false when the file could not be replaced.
bool save_config(const Config& c);

// Writes text to a temporary file in the destination directory and atomically
// replaces the destination. Used for installed.tsv and config.ini so a crash or
// a concurrent reader never sees a truncated file.
bool write_text_atomic(const fs::path& file, const std::string& text);

// Finds a system steamcmd (PATH, common install dirs, or PZMODMGR_STEAMCMD).
// Returns an empty path when nothing is found.
fs::path find_steamcmd();

// The app-managed steamcmd under <Zomboid>/pzmodmgr/steamcmd. It is
// downloaded on first use, so the file may not exist yet.
fs::path builtin_steamcmd_path(const Config& c);
}  // namespace pz
