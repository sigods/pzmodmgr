#pragma once
#include "core/mod_manager.hpp"
namespace pz {
// Implemented per platform in ui/win32 or ui/x11.
int run_ui(ModManager& mm);
}  // namespace pz
