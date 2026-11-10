#pragma once
#include "core/mod_manager.hpp"
#include <algorithm>
#include <cstdint>
#include <ctime>
#include <string>
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
    bool locked = false;
};

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
            existing->locked = existing->locked || m.locked;
            continue;
        }
        ModRow row;
        row.workshop_id = m.workshop_id;
        row.name = m.name;
        row.folders.push_back(m.folder);
        row.time_updated = m.time_updated;
        row.locked = m.locked;
        visible.push_back(std::move(row));
    }
    if (sort_mode == 0) {
        std::stable_sort(visible.begin(), visible.end(),
            [](const ModRow& a, const ModRow& b) {
                return ascii_lowercase(a.name) < ascii_lowercase(b.name);
            });
    } else {
        std::stable_sort(visible.begin(), visible.end(),
            [](const ModRow& a, const ModRow& b) {
                if (a.time_updated != b.time_updated)
                    return a.time_updated > b.time_updated;
                return ascii_lowercase(a.name) < ascii_lowercase(b.name);
            });
    }
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
    if (mod.locked) add("Locked");
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
