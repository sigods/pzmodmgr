#include "core/commands.hpp"
#include "core/paths.hpp"
#include "core/text.hpp"
#include "core/updater.hpp"
#include "core/url_c.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace pz {
namespace {

struct BatchCounts {
    int ok = 0;
    int failed = 0;
};

// The parser is C99; this is the only command-layer copy into the vector used
// by the existing batch loop.
std::vector<std::uint64_t> parse_ids(const std::string& input) {
    std::vector<std::uint64_t> ids;
    pz_url_ids parsed{};
    if (!pz_url_parse_workshop_ids(input.data(), input.size(), &parsed)) {
        pz_url_ids_free(&parsed);
        return ids;
    }
    if (parsed.count != 0) ids.assign(parsed.items, parsed.items + parsed.count);
    pz_url_ids_free(&parsed);
    return ids;
}

template <typename Fn>
BatchCounts apply_ids(const std::vector<std::uint64_t>& ids, const LogFn& log,
                      bool log_each, Fn fn) {
    BatchCounts counts;
    for (auto id : ids) {
        auto r = fn(id);
        if (log_each) log(std::to_string(id) + ": " + r.message);
        if (r.ok) ++counts.ok;
        else ++counts.failed;
    }
    return counts;
}

Result add_workshop_ids(ModManager& mm, const std::vector<std::uint64_t>& ids,
                        const LogFn& log) {
    if (ids.size() == 1) return mm.add(std::to_string(ids.front()), log);

    const auto counts = apply_ids(ids, log, true,
        [&](std::uint64_t id) { return mm.add(std::to_string(id), log); });
    std::string msg = "Installed " + std::to_string(counts.ok) + " of " +
                      std::to_string(ids.size()) + " Workshop item(s)";
    if (counts.failed != 0) msg += ", " + std::to_string(counts.failed) + " failed";
    return {counts.failed == 0, msg};
}

Result add_workshop_input(ModManager& mm, const std::string& arg, const LogFn& log) {
    auto ids = parse_ids(arg);
    if (ids.empty()) return {false, "No Workshop ID found in: " + arg};
    return add_workshop_ids(mm, ids, log);
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string decode_folder_token(const std::string& token) {
    if (token.size() < 3 || token[0] != 'f' || token[1] != ':' ||
        (token.size() - 2) % 2 != 0)
        return {};
    std::string value;
    for (std::size_t i = 2; i + 1 < token.size(); i += 2) {
        const int high = hex_digit(token[i]);
        const int low = hex_digit(token[i + 1]);
        if (high < 0 || low < 0) return {};
        value += static_cast<char>((high << 4) | low);
    }
    return value;
}

std::string lowercase(std::string value) {
    for (char& c : value)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

void add_unique(std::vector<std::uint64_t>& values, std::uint64_t value) {
    if (value != 0 && std::find(values.begin(), values.end(), value) == values.end())
        values.push_back(value);
}

// Resolves a paste-ready list of Workshop IDs or installed folder/name tokens.
// A whole name with spaces is tried first, then each whitespace-separated token
// gets an exact match or a unique substring match.
Result resolve_lock_ids(ModManager& mm, const std::string& arg,
                        std::vector<std::uint64_t>& ids) {
    if (arg.empty()) return {false, "expected Workshop IDs or mod names"};
    const auto mods = mm.list_all();

    auto match = [&](const std::string& token, std::vector<std::uint64_t>& out) {
        std::uint64_t id = 0;
        if (pz_url_parse_workshop_id(token.data(), token.size(), &id)) {
            for (const auto& mod : mods) {
                if (mod.workshop_id == id) {
                    add_unique(out, id);
                    return true;
                }
            }
            return false;
        }
        const std::string needle = lowercase(token);
        std::vector<std::uint64_t> exact;
        std::vector<std::uint64_t> partial;
        for (const auto& mod : mods) {
            if (mod.workshop_id == 0) continue;
            const std::string name = lowercase(mod.name);
            const std::string folder = lowercase(mod.folder);
            if (name == needle || folder == needle)
                add_unique(exact, mod.workshop_id);
            else if (name.find(needle) != std::string::npos ||
                     folder.find(needle) != std::string::npos)
                add_unique(partial, mod.workshop_id);
        }
        if (exact.size() == 1) {
            add_unique(out, exact.front());
            return true;
        }
        if (exact.size() > 1) return false;
        if (partial.size() == 1) {
            add_unique(out, partial.front());
            return true;
        }
        return false;
    };

    std::vector<std::uint64_t> whole;
    if (match(arg, whole) && !whole.empty()) {
        ids = std::move(whole);
        return {true, {}};
    }

    std::string remaining = arg;
    while (!remaining.empty()) {
        const auto [token, tail] = split_first_word(remaining);
        remaining = tail;
        std::vector<std::uint64_t> matched;
        if (!match(token, matched) || matched.empty())
            return {false, "unknown or ambiguous mod: " + token};
        for (auto id : matched) add_unique(ids, id);
    }
    if (ids.empty()) return {false, "no installed Workshop item matched: " + arg};
    return {true, {}};
}

}  // namespace

Result run_command(ModManager& mm, const std::string& line, const LogFn& log) {
    const auto [cmd, arg] = split_first_word(line);

    if (cmd == "add") return add_workshop_input(mm, arg, log);
    if (cmd == "remove") {
        auto ids = parse_ids(arg);
        if (ids.empty()) return {false, "expected a Workshop ID"};
        if (ids.size() == 1) return mm.remove(ids.front(), log);

        const auto counts = apply_ids(ids, log, false,
            [&](std::uint64_t id) { return mm.remove(id, log); });
        const int missing = static_cast<int>(ids.size()) - counts.ok;
        std::string msg = "Removed " + std::to_string(counts.ok) + " of " +
                          std::to_string(ids.size()) + " selected mod(s)";
        if (missing != 0) msg += ", " + std::to_string(missing) + " not installed";
        return {missing == 0, msg};
    }
    if (cmd == "check") {
        auto result = mm.check_updates(log);
        auto app = mm.check_app_release(log);
        if (app.ok) result.message += " " + app.message;
        return result;
    }
    if (cmd == "self-update") return stage_app_update(mm.config(), log);
    if (cmd == "targets") {
        auto targets = mm.activation_targets();
        for (std::size_t i = 0; i < targets.size(); ++i) {
            const auto& t = targets[i];
            log(std::to_string(i) + ": " + t.label + " (" +
                std::to_string(t.mod_ids.size()) + " mod ID(s))");
        }
        return {true, std::to_string(targets.size()) +
                      " activation target(s); use enable/disable <index> <Workshop IDs>"};
    }
    if (cmd == "set-enabled") {
        const auto [index_text, rest] = split_first_word(arg);
        const auto [state, tokens] = split_first_word(rest);
        if (index_text.empty() || (state != "on" && state != "off"))
            return {false, "expected: set-enabled <index> on|off [w:ID|f:folder]..."};
        if (index_text.find_first_not_of("0123456789") != std::string::npos)
            return {false, "unknown target index: " + index_text};
        auto targets = mm.activation_targets();
        const unsigned long index = std::strtoul(index_text.c_str(), nullptr, 10);
        if (index >= targets.size())
            return {false, "unknown target index: " + index_text};

        std::vector<std::uint64_t> ids;
        std::vector<std::string> folders;
        std::string remaining = tokens;
        while (!remaining.empty()) {
            const auto [token, tail] = split_first_word(remaining);
            remaining = tail;
            if (token.compare(0, 2, "w:") == 0) {
                const std::string number = token.substr(2);
                if (number.empty() ||
                    number.find_first_not_of("0123456789") != std::string::npos)
                    return {false, "bad Workshop ID token: " + token};
                ids.push_back(std::strtoull(number.c_str(), nullptr, 10));
            } else if (token.compare(0, 2, "f:") == 0) {
                const std::string folder = decode_folder_token(token);
                if (folder.empty()) return {false, "bad folder token: " + token};
                folders.push_back(folder);
            } else {
                return {false, "unknown token: " + token};
            }
        }
        const auto& target = targets[index];
        return mm.set_selection_enabled(target.kind, target.name, ids, folders,
                                        state == "on", log);
    }
    if (cmd == "enable" || cmd == "disable") {
        const auto [index_text, ids_text] = split_first_word(arg);
        auto ids = parse_ids(ids_text);
        if (index_text.empty() || ids.empty())
            return {false, "expected: " + cmd + " <target index> <Workshop ID>..."};
        if (index_text.find_first_not_of("0123456789") != std::string::npos)
            return {false, "unknown target index: " + index_text};
        auto targets = mm.activation_targets();
        const unsigned long index = std::strtoul(index_text.c_str(), nullptr, 10);
        if (index >= targets.size())
            return {false, "unknown target index: " + index_text};
        const auto& target = targets[index];
        return mm.set_mods_enabled(target.kind, target.name, ids, cmd == "enable", log);
    }
    if (cmd == "share") {
        const auto [index_text, ignored] = split_first_word(arg);
        (void)ignored;
        if (index_text.empty() || index_text.find_first_not_of("0123456789") != std::string::npos)
            return {false, "expected: share <target index>"};
        auto targets = mm.activation_targets();
        const unsigned long index = std::strtoul(index_text.c_str(), nullptr, 10);
        if (index >= targets.size())
            return {false, "unknown target index: " + index_text};
        const auto& target = targets[index];
        const auto ids = mm.workshop_ids_for(target);
        std::string list;
        for (auto id : ids) {
            if (!list.empty()) list += ' ';
            list += std::to_string(id);
        }
        log(list.empty() ? std::string("(no Workshop IDs)") : list);
        std::string msg = std::to_string(ids.size()) + " Workshop ID(s) from " +
                          target.label + "; paste into Add";
        if (!list.empty()) msg += ", or pin them with: lock " + list;
        return {true, msg};
    }
    if (cmd == "lock" || cmd == "unlock") {
        std::vector<std::uint64_t> ids;
        auto resolved = resolve_lock_ids(mm, arg, ids);
        if (!resolved.ok) return resolved;
        auto result = mm.set_locked(ids, cmd == "lock", log);
        if (cmd == "unlock") {
            for (auto id : ids) {
                if (mm.server_locked(id))
                    log("still locked by a server target: " + std::to_string(id));
            }
        }
        return result;
    }
    if (cmd == "update") {
        if (arg.empty()) return mm.update_all(log);
        auto ids = parse_ids(arg);
        if (ids.empty()) return {false, "expected a Workshop ID"};
        if (ids.size() == 1) return mm.update_one(ids.front(), log);

        const auto counts = apply_ids(ids, log, true,
            [&](std::uint64_t id) { return mm.update_one(id, log); });
        std::string msg = "Updated " + std::to_string(counts.ok) + " of " +
                          std::to_string(ids.size()) + " selected mod(s)";
        if (counts.failed != 0) msg += ", " + std::to_string(counts.failed) + " failed";
        return {counts.failed == 0, msg};
    }
    if (cmd == "list") {
        auto mods = mm.list();
        for (const auto& m : mods) {
            std::string line = m.name + " [" + m.folder + "] id=" +
                               std::to_string(m.workshop_id);
            if (m.locked) line += " [locked]";
            else if (mm.server_locked(m.workshop_id)) line += " [server lock]";
            log(line);
        }
        return {true, std::to_string(mods.size()) + " mod(s)"};
    }
    if (cmd == "config") {
        const auto [key, value] = split_first_word(arg);
        auto& c = mm.config();
        if (key == "steamcmd_exe") {
            c.steamcmd_exe = value;
        } else if (key == "steam_user") {
            c.steam_user = value;
        } else if (key == "zomboid_dir") {
            const fs::path dir = value.empty() ? default_config().zomboid_dir
                                               : fs::path(value);
            auto r = mm.set_zomboid_dir(dir);
            if (!r.ok) return r;
        } else if (key == "dark_mode") {
            c.dark_mode = value == "1" || value == "on" || value == "true" ||
                          value == "yes" || value == "dark";
        } else if (key == "auto_enable") {
            c.auto_enable = value == "1" || value == "on" || value == "true" ||
                            value == "yes";
        } else {
            return {false, "unknown config key: " + key};
        }
        if (!save_config(mm.config())) return {false, "could not save config"};
        return {true, "saved " + key};
    }

    // A pasted Workshop URL, numeric id, or whitespace-separated list with no
    // command prefix means add.
    auto ids = parse_ids(line);
    if (!ids.empty()) return add_workshop_ids(mm, ids, log);

    return {false, "unknown command: " + cmd};
}
}  // namespace pz
