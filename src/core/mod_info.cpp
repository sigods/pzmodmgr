#include "core/mod_info.hpp"
#include "core/text.hpp"
#include <algorithm>
#include <cctype>
#include <climits>
#include <vector>

namespace pz {
namespace {

std::string trim_copy(std::string value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::vector<std::string> parse_required_ids(const std::string& value) {
    std::vector<std::string> ids;
    std::size_t start = 0;
    for (;;) {
        const std::size_t separator = value.find_first_of(",;", start);
        std::string id = trim_copy(value.substr(
            start, separator == std::string::npos ? separator : separator - start));
        while (!id.empty() && (id.front() == '\\' || id.front() == '/')) id.erase(0, 1);
        if (id.size() >= 2 && id.front() == '"' && id.back() == '"')
            id = id.substr(1, id.size() - 2);
        if (!id.empty() && std::find(ids.begin(), ids.end(), id) == ids.end())
            ids.push_back(id);
        if (separator == std::string::npos) break;
        start = separator + 1;
    }
    return ids;
}

// B42 mods may keep their metadata in a version subfolder, for example
// mods/HorseMod/42/mod.info. Prefer the highest numeric version.
int version_rank(const std::string& name) {
    if (name.empty()) return -1;
    int value = 0;
    for (char c : name) {
        if (c < '0' || c > '9') return -1;
        const int digit = c - '0';
        if (value > (INT_MAX - digit) / 10) return -1;
        value = value * 10 + digit;
    }
    return value;
}

fs::path resolve_icon(const fs::path& root, const fs::path& info_dir, const std::string& value) {
    auto regular = [](const fs::path& p) {
        pz::error_code ec;
        return fs::is_regular_file(p, ec);
    };

    if (!value.empty()) {
        const fs::path rel(value);
        for (const auto& base : {info_dir, root, root / "common"}) {
            const fs::path candidate = base / rel;
            if (regular(candidate)) return candidate;
        }
    }

    // Only the canonical icon.png is an implicit icon. Do not glob icon*:
    // files such as icon_old.png or icon_alt.png are not the active icon.
    for (const auto& base : {info_dir, root, root / "common"}) {
        const fs::path icon = base / "icon.png";
        if (regular(icon)) return icon;
    }

    // Some Workshop mods only ship a preview/poster image. Keep the explicit
    // icon= path above, but let these common names act as a fallback.
    for (const auto& base : {info_dir, root, root / "common"}) {
        const fs::path poster = base / "poster.png";
        if (regular(poster)) return poster;
    }
    for (const auto& base : {info_dir, root, root / "common"}) {
        const fs::path preview = base / "preview.png";
        if (regular(preview)) return preview;
    }
    return {};
}

// Reads just versionMin/versionMax, without resolving icons. Used while
// choosing a version folder and to gate downloads against the game build.
bool info_supports_game(const fs::path& info_dir, const std::string& game_version) {
    if (game_version.empty()) return true;
    ModInfo bounds;
    for_each_line(info_dir / "mod.info", [&](const std::string& line) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) return;
        const auto k = line.substr(0, eq);
        if (k == "versionMin") bounds.version_min = line.substr(eq + 1);
        else if (k == "versionMax") bounds.version_max = line.substr(eq + 1);
    });
    return supports_game_version(bounds, game_version);
}

// Returns the directory that directly contains mod.info. For a B42 mod this
// is the version folder (for example HorseMod/42), not the mod folder itself.
// When a game version is given, choose the newest version folder that supports
// it instead of blindly choosing the highest folder number.
fs::path mod_info_dir(const fs::path& dir, const std::string& game_version = {}) {
    pz::error_code ec;
    if (fs::is_regular_file(dir / "mod.info", ec)) return dir;

    fs::directory_iterator it(dir, ec);
    if (ec) return {};

    // Version folders are few, so choose the best candidate in one pass rather
    // than instantiating std::sort and its heap/insertion machinery.
    fs::path best_any;
    fs::path best_compatible;
    int best_any_rank = -1, best_compatible_rank = -1;
    std::string best_any_name, best_compatible_name;
    for (const auto& e : it) {
        if (!e.is_directory(ec)) continue;
        if (!fs::is_regular_file(e.path() / "mod.info", ec)) continue;
        const std::string name = e.path().filename().string();
        const int rank = version_rank(name);
        if (rank > best_any_rank || (rank == best_any_rank && name > best_any_name)) {
            best_any = e.path();
            best_any_rank = rank;
            best_any_name = name;
        }
        if (!info_supports_game(e.path(), game_version)) continue;
        if (rank > best_compatible_rank ||
            (rank == best_compatible_rank && name > best_compatible_name)) {
            best_compatible = e.path();
            best_compatible_rank = rank;
            best_compatible_name = name;
        }
    }
    if (!best_compatible.empty()) return best_compatible;
    return best_any;
}

}  // namespace

std::vector<fs::path> discover_mod_roots(const fs::path& content) {
    std::vector<fs::path> roots;
    if (!mod_info_dir(content).empty()) {
        roots.push_back(content);
        return roots;
    }
    for (const auto& base : {content / "mods", content / "Contents" / "mods"}) {
        pz::error_code ec;
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
    ModInfo m;
    if (!for_each_line(info_dir / "mod.info", [&](const std::string& line) {
            auto eq = line.find('=');
            if (eq == std::string::npos) return;
            auto k = line.substr(0, eq);
            auto v = line.substr(eq + 1);
            if (k == "id") m.id = v;
            else if (k == "name") m.name = v;
            else if (k == "author") m.author = v;
            else if (k == "modversion") m.mod_version = v;
            else if (k == "versionMin") m.version_min = v;
            else if (k == "versionMax") m.version_max = v;
            else if (k == "require" || k == "requires") {
                for (const auto& required : parse_required_ids(v)) {
                    if (std::find(m.required_ids.begin(), m.required_ids.end(), required) ==
                        m.required_ids.end())
                        m.required_ids.push_back(required);
                }
            } else if (k == "icon") m.icon = v;
        })) return std::nullopt;
    if (!m.icon.empty()) {
        const std::string value = m.icon.string();
        m.icon = resolve_icon(dir, info_dir, value);
    } else {
        m.icon = resolve_icon(dir, info_dir, {});
    }
    return m;
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

std::string version_range(const ModInfo& info) {
    if (info.version_min.empty() && info.version_max.empty()) return {};
    if (info.version_max.empty()) return "PZ " + info.version_min + "+";
    if (info.version_min.empty()) return "PZ up to " + info.version_max;
    return "PZ " + info.version_min + "-" + info.version_max;
}
}  // namespace pz
