#include "core/mod_info.hpp"
#include <charconv>
#include <fstream>
#include <system_error>

namespace pz {
namespace {

// B42 mods may keep their metadata in a version subfolder, for example
// mods/HorseMod/42/mod.info. Prefer the highest numeric version.
int version_rank(const std::string& name) {
    if (name.empty()) return -1;
    int value = 0;
    auto r = std::from_chars(name.data(), name.data() + name.size(), value);
    if (r.ec != std::errc{} || r.ptr != name.data() + name.size()) return -1;
    return value;
}

// Returns the directory that directly contains mod.info. For a B42 mod this
// is the version folder (for example HorseMod/42), not the mod folder itself.
fs::path mod_info_dir(const fs::path& dir) {
    std::error_code ec;
    if (fs::is_regular_file(dir / "mod.info", ec)) return dir;

    fs::directory_iterator it(dir, ec);
    if (ec) return {};
    fs::path best;
    int best_rank = -1;
    for (const auto& e : it) {
        if (!e.is_directory(ec)) continue;
        if (!fs::is_regular_file(e.path() / "mod.info", ec)) continue;
        const int rank = version_rank(e.path().filename().string());
        const bool better =
            best.empty() || rank > best_rank ||
            (rank == -1 && best_rank == -1 && e.path().filename() > best.filename());
        if (better) {
            best = e.path();
            best_rank = rank;
        }
    }
    return best;
}

}  // namespace

std::vector<fs::path> discover_mod_roots(const fs::path& content) {
    std::vector<fs::path> roots;
    if (!mod_info_dir(content).empty()) {
        roots.push_back(content);
        return roots;
    }
    for (const auto& base : {content / "mods", content / "Contents" / "mods"}) {
        std::error_code ec;
        if (!fs::is_directory(base, ec)) continue;
        for (const auto& e : fs::directory_iterator(base)) {
            if (e.is_directory() && !mod_info_dir(e.path()).empty())
                roots.push_back(e.path());
        }
    }
    return roots;
}

std::optional<ModInfo> read_mod_info(const fs::path& dir) {
    const fs::path info_dir = mod_info_dir(dir);
    if (info_dir.empty()) return std::nullopt;
    std::ifstream in(info_dir / "mod.info");
    if (!in) return std::nullopt;
    ModInfo m;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto k = line.substr(0, eq);
        auto v = line.substr(eq + 1);
        if (k == "id") m.id = v;
        else if (k == "name") m.name = v;
    }
    return m;
}
}  // namespace pz
