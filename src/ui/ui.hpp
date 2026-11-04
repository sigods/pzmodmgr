#pragma once
#include "core/mod_manager.hpp"
namespace pz {
// Implemented per platform in ui/win32 or ui/x11.
int run_ui(ModManager& mm);

// Implemented in main.cpp. Called by main() and, on Windows, by the
// GUI-subsystem wWinMain in ui/win32/win32_entry.cpp.
int run_app(int argc, char** argv);
}  // namespace pz
