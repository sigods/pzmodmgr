#include "core/paths.hpp"
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

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
    // Prefer the app-managed copy; it is downloaded on first use. A configured
    // path, a previously bootstrapped copy, or a system install overrides it.
    c.steamcmd_exe  = builtin_steamcmd_path(c);
    return c;
}

void load_config(Config& c) {
    std::ifstream in(c.data_dir / "config.ini");
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto k = line.substr(0, eq);
        auto v = line.substr(eq + 1);
        if (k == "steamcmd_exe" && !v.empty()) c.steamcmd_exe = v;
        else if (k == "steam_user") c.steam_user = v;
    }
}

void save_config(const Config& c) {
    fs::create_directories(c.data_dir);
    std::ofstream out(c.data_dir / "config.ini");
    out << "steamcmd_exe=" << c.steamcmd_exe.string() << "\n"
        << "steam_user=" << c.steam_user << "\n";
}
}  // namespace pz
