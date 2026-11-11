#pragma once
#include "core/mod_manager.hpp"
namespace pz {
// Commands: add <url|id> | remove <id> | update [id] | check | list |
// lock|unlock <id|name>... | targets | target-lock <index> on|off |
// enable|disable <target index> <id>... | share <target index> | self-update |
// config <key> <value>. A bare Workshop URL or numeric id is treated as `add`.
// lock/unlock manage the explicit per-item lock; target-lock controls whether
// enabled mods in one Default, save, or server target are update-locked.
Result run_command(ModManager& mm, const std::string& line, const LogFn& log);
}  // namespace pz
