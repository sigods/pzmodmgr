#pragma once
#include "core/mod_manager.hpp"
#include <algorithm>
#include <cstdint>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

namespace pz {
// Formats a Steam time_updated value for display. Returns empty when unknown.
inline std::string format_update_date(std::uint64_t epoch) {
    if (epoch == 0) return {};
    const std::time_t when = static_cast<std::time_t>(epoch);
    const std::tm* tm = std::localtime(&when);
    if (!tm) return {};
    char text[16]{};
    if (std::strftime(text, sizeof text, "%Y-%m-%d", tm) == 0) return {};
    return text;
}

inline std::string ascii_lowercase(std::string value) {
    for (char& c : value) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return value;
}

struct DependencyNotice {
    std::string signature;
    std::vector<std::string> messages;
    std::string status;
};

inline DependencyNotice dependency_notice(const std::vector<MissingRequirement>& missing) {
    DependencyNotice notice;
    for (const auto& entry : missing) {
        notice.signature += entry.folder + "\t" + entry.required_id + "\n";
        notice.messages.push_back("warning: " + entry.mod_name + " requires " +
                                  entry.required_id + " (not installed)");
    }
    if (missing.empty()) {
        notice.status.clear();
    } else if (missing.size() == 1) {
        notice.status = "Missing required mod: " + missing[0].required_id +
                        " (needed by " + missing[0].mod_name + ")";
    } else {
        notice.status = "Missing " + std::to_string(missing.size()) +
                        " required mods - see Activity";
    }
    return notice;
}

// One row in the mod list. A single Workshop item can ship several mod
// folders (for example an old and a new variant with different Mod IDs), so
// the folders are grouped here and the item is shown, selected, updated, and
// removed as one entry.
struct ModRow {
    std::uint64_t workshop_id = 0;
    std::string name;
    std::vector<std::string> folders;
    std::uint64_t time_updated = 0;
};

inline bool ascii_less(const std::string& a, const std::string& b) {
    const std::size_t common = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < common; ++i) {
        unsigned char ca = static_cast<unsigned char>(a[i]);
        unsigned char cb = static_cast<unsigned char>(b[i]);
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<unsigned char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<unsigned char>(cb - 'A' + 'a');
        if (ca != cb) return ca < cb;
    }
    return a.size() < b.size();
}

// The row list is small (tens of installed items), so a stable insertion sort
// is cheaper in code size than instantiating std::stable_sort's merge path for
// both sort orders.
inline void stable_sort_rows(std::vector<ModRow>& rows, bool by_update) {
    for (std::size_t i = 1; i < rows.size(); ++i) {
        ModRow value = std::move(rows[i]);
        std::size_t j = i;
        while (j > 0) {
            const ModRow& previous = rows[j - 1];
            bool before = false;
            if (by_update && value.time_updated != previous.time_updated)
                before = value.time_updated > previous.time_updated;
            else
                before = ascii_less(value.name, previous.name);
            if (!before) break;
            rows[j] = std::move(rows[j - 1]);
            --j;
        }
        rows[j] = std::move(value);
    }
}

inline std::string join_workshop_ids(const std::vector<std::uint64_t>& ids) {
    std::string out;
    for (auto id : ids) {
        if (!out.empty()) out += ' ';
        out += std::to_string(id);
    }
    return out;
}

// Folder names can contain spaces, so the enable command passes them as
// "f:<hex>". This keeps one command line usable by both UIs.
inline std::string folder_token(const std::string& folder) {
    static const char* digits = "0123456789abcdef";
    std::string out = "f:";
    for (unsigned char c : folder) {
        out += digits[c >> 4];
        out += digits[c & 15];
    }
    return out;
}

// Short label for one activation target, e.g. "Default", "Save: Apocalypse/X",
// or "Server: servertest".
inline std::string activation_name(const ActivationTarget& target) {
    if (target.kind == ActivationKind::Default) return "Default";
    if (target.kind == ActivationKind::Server) return "Server: " + target.name;
    return target.name;
}

// How many of these Mod IDs the target has enabled.
inline int activation_count(const ActivationTarget& target,
                            const std::vector<std::string>& mod_ids) {
    int count = 0;
    for (const auto& id : mod_ids) {
        if (std::find(target.mod_ids.begin(), target.mod_ids.end(), id) !=
            target.mod_ids.end())
            ++count;
    }
    return count;
}

// Targets where at least one of the item's Mod IDs is enabled. Multi-folder
// items often activate only one variant, so "any" is the useful signal.
inline std::string enabled_in(const std::vector<ActivationTarget>& targets,
                              const std::vector<std::string>& mod_ids) {
    std::string out;
    for (const auto& target : targets) {
        if (activation_count(target, mod_ids) == 0) continue;
        if (!out.empty()) out += ", ";
        out += activation_name(target);
    }
    return out;
}

template <typename HasUpdate>
std::vector<ModRow> visible_mods(const std::vector<InstalledMod>& mods,
                                 const std::string& filter, bool only_updates,
                                 int sort_mode, HasUpdate has_update) {
    const std::string needle = ascii_lowercase(filter);
    std::vector<ModRow> visible;
    visible.reserve(mods.size());
    for (const auto& m : mods) {
        if (only_updates && !has_update(m.workshop_id)) continue;
        if (!needle.empty()) {
            const std::string haystack = m.name + " " + m.folder;
            if (ascii_lowercase(haystack).find(needle) == std::string::npos) continue;
        }
        auto existing = m.workshop_id == 0 ? visible.end()
            : std::find_if(visible.begin(), visible.end(), [&](const ModRow& row) {
                  return row.workshop_id == m.workshop_id;
              });
        if (existing != visible.end()) {
            existing->folders.push_back(m.folder);
            existing->time_updated = std::max(existing->time_updated, m.time_updated);
            continue;
        }
        ModRow row;
        row.workshop_id = m.workshop_id;
        row.name = m.name;
        row.folders.push_back(m.folder);
        row.time_updated = m.time_updated;
        visible.push_back(std::move(row));
    }
    stable_sort_rows(visible, sort_mode != 0);
    return visible;
}

inline std::string mod_meta(const ModRow& mod, const ModInfo* info,
                            const std::string& game_version,
                            const std::string& enabled_in_targets = {}) {
    std::string meta;
    auto add = [&](const std::string& value) {
        if (value.empty()) return;
        if (!meta.empty()) meta += "  |  ";
        meta += value;
    };
    if (mod.folders.size() == 1) add(mod.folders.front());
    if (!enabled_in_targets.empty()) add("On: " + enabled_in_targets);
    const std::string updated = format_update_date(mod.time_updated);
    if (!updated.empty()) add("Updated " + updated);
    if (info && !supports_game_version(*info, game_version)) add(version_range(*info));
    return meta;
}

// Implemented per platform in ui/win32 or ui/x11.
int run_ui(ModManager& mm);

// Implemented in main.cpp. Called by main() and, on Windows, by the
// GUI-subsystem wWinMain in ui/win32/win32_entry.cpp.
int run_app(int argc, char** argv);
}  // namespace pz
