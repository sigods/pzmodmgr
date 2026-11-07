#include "core/paths.hpp"
#include "core/text.hpp"
#include <cstdlib>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace pz {
namespace {
fs::path home_dir() {
#ifdef _WIN32
    const char* v = std::getenv("USERPROFILE");
#else
    const char* v = std::getenv("HOME");
#endif
    return v ? fs::path(v) : fs::current_path();
}

bool is_executable(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return false;
#ifndef _WIN32
    const auto perms = fs::status(p, ec).permissions();
    const auto exec = fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec;
    return (perms & exec) != fs::perms::none;
#else
    return true;
#endif
}

fs::path executable_dir() {
#ifdef _WIN32
    std::vector<wchar_t> buffer(1024);
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buffer.data(),
                                           static_cast<DWORD>(buffer.size()));
        if (n == 0) return {};
        if (n < buffer.size() - 1)
            return fs::path(std::wstring(buffer.data(), n)).parent_path();
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__linux__)
    std::vector<char> buffer(1024);
    for (;;) {
        const ssize_t n = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (n < 0) return {};
        if (static_cast<std::size_t>(n) < buffer.size())
            return fs::path(std::string(buffer.data(), static_cast<std::size_t>(n)))
                .parent_path();
        buffer.resize(buffer.size() * 2);
    }
#else
    return {};
#endif
}

std::vector<fs::path> path_dirs() {
    std::vector<fs::path> dirs;
    const char* env = std::getenv("PATH");
    if (!env) return dirs;
    const char sep =
#ifdef _WIN32
        ';';
#else
        ':';
#endif
    std::string value(env);
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t end = value.find(sep, start);
        const std::size_t stop = end == std::string::npos ? value.size() : end;
        if (stop > start) dirs.emplace_back(value.substr(start, stop - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return dirs;
}
}  // namespace

fs::path find_steamcmd() {
    for (const char* var : {"PZMODMGR_STEAMCMD", "STEAMCMD"}) {
        const char* value = std::getenv(var);
        if (value && *value && is_executable(value)) return fs::path(value);
    }

    for (const auto& dir : path_dirs()) {
#ifdef _WIN32
        const fs::path candidate = dir / "steamcmd.exe";
        if (is_executable(candidate)) return candidate;
#else
        for (const char* name : {"steamcmd", "steamcmd.sh"}) {
            const fs::path candidate = dir / name;
            if (is_executable(candidate)) return candidate;
        }
#endif
    }

#ifdef _WIN32
    for (const char* var : {"ProgramFiles", "ProgramFiles(x86)", "LOCALAPPDATA", "USERPROFILE"}) {
        const char* value = std::getenv(var);
        if (!value) continue;
        for (const auto& rel : {fs::path("steamcmd/steamcmd.exe"),
                                fs::path("Steam/steamcmd.exe"),
                                fs::path("steamcmd.exe")}) {
            const fs::path candidate = fs::path(value) / rel;
            if (is_executable(candidate)) return candidate;
        }
    }
#else
    const fs::path common[] = {
        "/usr/bin/steamcmd", "/usr/games/steamcmd", "/usr/local/bin/steamcmd",
        "/opt/steamcmd/steamcmd.sh", "/snap/bin/steamcmd"};
    for (const auto& candidate : common)
        if (is_executable(candidate)) return candidate;
    for (const auto& rel : {fs::path("steamcmd/steamcmd.sh"),
                            fs::path("Steam/steamcmd.sh")}) {
        const fs::path candidate = home_dir() / rel;
        if (is_executable(candidate)) return candidate;
    }
#endif
    return {};
}
fs::path builtin_steamcmd_path(const Config& c) {
#ifdef _WIN32
    return c.data_dir / "steamcmd" / "steamcmd.exe";
#else
    return c.data_dir / "steamcmd" / "steamcmd.sh";
#endif
}

Config default_config() {
    Config c;
    c.zomboid_dir   = home_dir() / "Zomboid";
    c.user_mods_dir = c.zomboid_dir / "mods";
    c.data_dir      = c.zomboid_dir / "pzmodmgr";
    // An installed build gets its config from the installer next to the
    // executable, so no bootstrap files are created in the default Zomboid
    // folder. Source/portable builds without that file keep the old fallback.
    std::error_code ec;
    const fs::path exe_dir = executable_dir();
    const fs::path installed_config =
        exe_dir.empty() ? fs::path{} : exe_dir / "config.ini";
    if (!installed_config.empty() && fs::is_regular_file(installed_config, ec))
        c.config_file = installed_config;
    else
        c.config_file = c.data_dir / "config.ini";
    // Prefer the app-managed copy; it is downloaded on first use. A configured
    // path, a previously bootstrapped copy, or a system install overrides it.
    c.steamcmd_exe  = builtin_steamcmd_path(c);
    return c;
}

void load_config(Config& c) {
    const fs::path file = c.config_file.empty() ? c.data_dir / "config.ini"
                                                : c.config_file;
    std::string steamcmd_exe, steam_user, zomboid_dir;
    bool dark_mode = false, has_dark_mode = false;
    bool auto_enable = false, has_auto_enable = false;
    for_each_line(file, [&](const std::string& line) {
        auto eq = line.find('=');
        if (eq == std::string::npos) return;
        auto k = line.substr(0, eq);
        auto v = line.substr(eq + 1);
        if (k == "steamcmd_exe") steamcmd_exe = v;
        else if (k == "steam_user") steam_user = v;
        else if (k == "zomboid_dir") zomboid_dir = v;
        else if (k == "dark_mode") {
            dark_mode = v == "1" || v == "on" || v == "true" ||
                        v == "yes" || v == "dark";
            has_dark_mode = true;
        } else if (k == "auto_enable") {
            auto_enable = v == "1" || v == "on" || v == "true" || v == "yes";
            has_auto_enable = true;
        }
    });

    if (!zomboid_dir.empty()) {
        const fs::path dir(zomboid_dir);
        if (dir.is_absolute()) {
            const fs::path old_builtin = builtin_steamcmd_path(c);
            c.zomboid_dir = dir;
            c.user_mods_dir = dir / "mods";
            c.data_dir = dir / "pzmodmgr";
            if (c.steamcmd_exe.empty() || c.steamcmd_exe == old_builtin)
                c.steamcmd_exe = builtin_steamcmd_path(c);
        }
    }
    if (!steamcmd_exe.empty()) c.steamcmd_exe = steamcmd_exe;
    c.steam_user = steam_user;
    if (has_dark_mode) c.dark_mode = dark_mode;
    if (has_auto_enable) c.auto_enable = auto_enable;
}

bool save_config(const Config& c) {
    const fs::path file = c.config_file.empty() ? c.data_dir / "config.ini"
                                                : c.config_file;
    std::string text;
    text += "steamcmd_exe=" + c.steamcmd_exe.string() + "\n";
    text += "steam_user=" + c.steam_user + "\n";
    text += "zomboid_dir=" + c.zomboid_dir.string() + "\n";
    text += std::string("dark_mode=") + (c.dark_mode ? "1" : "0") + "\n";
    text += std::string("auto_enable=") + (c.auto_enable ? "1" : "0") + "\n";
    return write_text_atomic(file, text);
}

bool write_text_atomic(const fs::path& file, const std::string& text) {
    std::error_code ec;
    if (!file.parent_path().empty()) fs::create_directories(file.parent_path(), ec);
    if (ec) return false;

#ifdef _WIN32
    const unsigned long pid = GetCurrentProcessId();
#else
    const unsigned long pid = static_cast<unsigned long>(::getpid());
#endif
    fs::path temp = file;
    temp += ".tmp." + std::to_string(pid);

    {
        std::FILE* out = open_text_write(temp);
        if (!out) return false;
        const bool wrote = std::fwrite(text.data(), 1, text.size(), out) == text.size();
        const bool flushed = std::fflush(out) == 0;
        const bool closed = std::fclose(out) == 0;
        if (!wrote || !flushed || !closed) {
            fs::remove(temp, ec);
            return false;
        }
    }

#ifdef _WIN32
    if (!MoveFileExW(temp.wstring().c_str(), file.wstring().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        fs::remove(temp, ec);
        return false;
    }
#else
    fs::rename(temp, file, ec);
    if (ec) {
        fs::remove(temp, ec);
        return false;
    }
#endif
    return true;
}
}  // namespace pz
