#include "core/mod_info.hpp"
#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <system_error>
#include <vector>

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

fs::path resolve_icon(const fs::path& root, const fs::path& info_dir, const std::string& value) {
    std::error_code ec;
    auto regular = [&](const fs::path& p) {
        return fs::is_regular_file(p, ec);
    };

    if (!value.empty()) {
        const fs::path rel(value);
        for (const auto& base : {info_dir, root, root / "common"}) {
            const fs::path candidate = base / rel;
            if (regular(candidate)) return candidate;
        }
    }

    std::vector<fs::path> icons;
    for (const auto& base : {info_dir, root, root / "common"}) {
        const fs::path poster = base / "poster.png";
        if (regular(poster)) icons.push_back(poster);
        fs::directory_iterator it(base, ec);
        if (ec) continue;
        for (const auto& e : it) {
            const std::string name = e.path().filename().string();
            if (name.size() >= 4 && name.compare(0, 4, "icon") == 0 &&
                e.path().extension() == ".png" && regular(e.path()))
                icons.push_back(e.path());
        }
    }
    if (icons.empty()) return {};
    std::sort(icons.begin(), icons.end());
    return icons.front();
}

// Reads just versionMin/versionMax, without resolving icons. Used while
// choosing a version folder and to gate downloads against the game build.
bool info_supports_game(const fs::path& info_dir, const std::string& game_version) {
    if (game_version.empty()) return true;
    std::ifstream in(info_dir / "mod.info");
    ModInfo bounds;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const auto k = line.substr(0, eq);
        if (k == "versionMin") bounds.version_min = line.substr(eq + 1);
        else if (k == "versionMax") bounds.version_max = line.substr(eq + 1);
    }
    return supports_game_version(bounds, game_version);
}

// Returns the directory that directly contains mod.info. For a B42 mod this
// is the version folder (for example HorseMod/42), not the mod folder itself.
// When a game version is given, choose the newest version folder that supports
// it instead of blindly choosing the highest folder number.
fs::path mod_info_dir(const fs::path& dir, const std::string& game_version = {}) {
    std::error_code ec;
    if (fs::is_regular_file(dir / "mod.info", ec)) return dir;

    fs::directory_iterator it(dir, ec);
    if (ec) return {};
    std::vector<std::pair<int, fs::path>> candidates;
    for (const auto& e : it) {
        if (!e.is_directory(ec)) continue;
        if (!fs::is_regular_file(e.path() / "mod.info", ec)) continue;
        candidates.emplace_back(version_rank(e.path().filename().string()), e.path());
    }
    std::sort(candidates.begin(), candidates.end(),
        [](const auto& a, const auto& b) {
            if (a.first != b.first) return a.first > b.first;
            return a.second.filename() > b.second.filename();
        });
    if (candidates.empty()) return {};

    for (const auto& candidate : candidates)
        if (info_supports_game(candidate.second, game_version)) return candidate.second;
    return candidates.front().second;
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
    return read_mod_info(dir, {});
}

std::optional<ModInfo> read_mod_info(const fs::path& dir, const std::string& game_version) {
    const fs::path info_dir = mod_info_dir(dir, game_version);
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
        else if (k == "author") m.author = v;
        else if (k == "modversion") m.mod_version = v;
        else if (k == "versionMin") m.version_min = v;
        else if (k == "versionMax") m.version_max = v;
        else if (k == "icon") m.icon = v;
    }
    if (!m.icon.empty()) {
        const std::string value = m.icon.string();
        m.icon = resolve_icon(dir, info_dir, value);
    } else {
        m.icon = resolve_icon(dir, info_dir, {});
    }
    return m;
}

int compare_versions(const std::string& a, const std::string& b) {
    std::size_t ai = 0, bi = 0;
    while (ai < a.size() && !std::isdigit(static_cast<unsigned char>(a[ai]))) ++ai;
    while (bi < b.size() && !std::isdigit(static_cast<unsigned char>(b[bi]))) ++bi;
    for (;;) {
        unsigned long av = 0, bv = 0;
        bool a_part = false, b_part = false;
        while (ai < a.size() && std::isdigit(static_cast<unsigned char>(a[ai]))) {
            a_part = true;
            av = av * 10 + static_cast<unsigned long>(a[ai++] - '0');
        }
        while (bi < b.size() && std::isdigit(static_cast<unsigned char>(b[bi]))) {
            b_part = true;
            bv = bv * 10 + static_cast<unsigned long>(b[bi++] - '0');
        }
        if (av != bv) return av < bv ? -1 : 1;
        if (!a_part && !b_part) return 0;
        while (ai < a.size() && a[ai] == '.') ++ai;
        while (bi < b.size() && b[bi] == '.') ++bi;
        if (ai >= a.size() && bi >= b.size()) return 0;
    }
}

namespace {
int major_version(const std::string& value) {
    std::size_t i = 0;
    while (i < value.size() && !std::isdigit(static_cast<unsigned char>(value[i]))) ++i;
    int major = 0;
    bool found = false;
    while (i < value.size() && std::isdigit(static_cast<unsigned char>(value[i]))) {
        found = true;
        major = major * 10 + (value[i++] - '0');
    }
    return found ? major : -1;
}
}  // namespace

bool supports_game_version(const ModInfo& info, const std::string& game_version) {
    if (game_version.empty()) return true;
    const int game = major_version(game_version);
    if (game < 0) return true;
    const int minimum = major_version(info.version_min);
    const int maximum = major_version(info.version_max);
    if (minimum < 0 && maximum < 0) return true;

    // Project Zomboid mods break across major builds (41 -> 42 -> 43), but a
    // 42.22 mod normally still runs on 42.21. Treat the bounds as tested major
    // builds rather than exact patch versions.
    if (minimum >= 0 && maximum >= 0)
        return game >= minimum && game <= maximum;
    if (minimum >= 0) return game == minimum;
    return game == maximum;
}
}  // namespace pz
