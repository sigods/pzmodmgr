#pragma once
#include "core/fs_native.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace pz {
namespace fs = native_fs;

inline constexpr std::uint32_t kProjectZomboidAppId = 108600;

// A C-compatible callback: one function pointer plus an opaque context. The
// templated constructor is a C++ convenience for existing lambdas; the storage
// itself is what a C99 version of this header will use.
struct LogFn {
    using Callback = void (*)(void* context, const char* line);

    LogFn() = default;
    LogFn(Callback callback, void* context) : callback_(callback), context_(context) {}

    template <typename F>
    LogFn(const F& function)
        : callback_([](void* context, const char* line) {
              (*static_cast<const F*>(context))(std::string(line));
          }),
          context_(const_cast<F*>(&function)) {}

    void operator()(const std::string& line) const {
        if (callback_) callback_(context_, line.c_str());
    }
    void operator()(const char* line) const {
        if (callback_) callback_(context_, line);
    }
    explicit operator bool() const { return callback_ != nullptr; }

private:
    Callback callback_ = nullptr;
    void* context_ = nullptr;
};

struct Config {
    fs::path steamcmd_exe;        // built-in path, or a user-set override
    fs::path steamcmd_root;       // SteamCMD install/download root (read-only for import)
    std::string steam_user;       // account that owns PZ; password is cached by steamcmd
    bool dark_mode = false;       // UI theme preference
    bool auto_enable = false;     // add newly installed mods to mods/default.txt
    // Activation targets whose enabled mods are update-locked. Keys are
    // "default", "save:<name>", or "server:<name>".
    std::vector<std::string> locked_targets;
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
    // Manual update lock. Server-enabled items are locked dynamically too; this
    // flag only remembers a lock the user set for an item outside a server.
    bool locked = false;
    // True when the files were copied in from an existing SteamCMD download
    // root. The source tree is never modified or tracked in place.
    bool imported = false;
};

struct Result {
    bool ok = false;
    std::string message;
};

}  // namespace pz
