#pragma once
#include "core/mod_manager.hpp"
namespace pz {
// Commands: add <url|id> | remove <id> | update [id] | list | config <key> <value>
// A bare Workshop URL or numeric id is treated as `add`.
Result run_command(ModManager& mm, const std::string& line, const LogFn& log);
}  // namespace pz
