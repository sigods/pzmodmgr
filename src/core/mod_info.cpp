#include "core/mod_info.hpp"
#include <fstream>

namespace pz {
std::vector<fs::path> discover_mod_roots(const fs::path& content) {
    std::vector<fs::path> roots;
    if (fs::exists(content / "mod.info")) {
        roots.push_back(content);
        return roots;
    }
    for (const auto& base : {content / "mods", content / "Contents" / "mods"}) {
        std::error_code ec;
        if (!fs::is_directory(base, ec)) continue;
        for (const auto& e : fs::directory_iterator(base)) {
            if (e.is_directory() && fs::exists(e.path() / "mod.info"))
                roots.push_back(e.path());
        }
    }
    return roots;
}

std::optional<ModInfo> read_mod_info(const fs::path& dir) {
    std::ifstream in(dir / "mod.info");
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
