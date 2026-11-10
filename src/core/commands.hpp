#pragma once
#include "core/mod_manager.hpp"
namespace pz {
// Commands: add <url|id> | remove <id> | update [id] | check | list |
// lock|unlock <id|name>... | targets | enable|disable <target index> <id>... |
// share <target index> | self-update | config <key> <value>. A bare Workshop
// URL or numeric id is treated as `add`. Items enabled in a server target are
// locked automatically; lock/unlock manage the explicit per-item lock.
Result run_command(ModManager& mm, const std::string& line, const LogFn& log);
}  // namespace pz
