#include "core/activation.hpp"
#include "core/paths.hpp"
#include "core/text.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace pz {
namespace {

std::string trim(std::string value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::string without_mod_prefix(std::string value) {
    value = trim(std::move(value));
    while (!value.empty() && (value.front() == '\\' || value.front() == '/'))
        value.erase(0, 1);
    return trim(std::move(value));
}

bool starts_with_mod(const std::string& line) {
    return line.size() >= 3 && line.compare(0, 3, "mod") == 0 &&
           (line.size() == 3 || std::isspace(static_cast<unsigned char>(line[3])) ||
            line[3] == '=');
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t newline = text.find('\n', start);
        std::string line = text.substr(
            start, newline == std::string::npos ? std::string::npos : newline - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
        if (newline == std::string::npos) break;
        start = newline + 1;
    }
    return lines;
}

bool block_uses_backslash(const std::string& text) {
    for (const auto& line : split_lines(text)) {
        const std::string t = trim(line);
        if (!starts_with_mod(t)) continue;
        const auto eq = t.find('=');
        if (eq == std::string::npos) continue;
        const std::string value = trim(t.substr(eq + 1));
        if (!value.empty() && value.front() == '\\') return true;
    }
    return false;
}

std::string join_lines(const std::vector<std::string>& lines, bool crlf) {
    const std::string eol = crlf ? "\r\n" : "\n";
    std::string out;
    for (const auto& line : lines) {
        out += line;
        out += eol;
    }
    return out;
}

bool uses_crlf(const std::string& text) {
    return text.find("\r\n") != std::string::npos;
}

std::vector<std::string> split_semicolons(const std::string& value) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (;;) {
        const std::size_t pos = value.find(';', start);
        parts.push_back(value.substr(
            start, pos == std::string::npos ? std::string::npos : pos - start));
        if (pos == std::string::npos) break;
        start = pos + 1;
    }
    return parts;
}

std::string ini_value(const std::string& line, const std::string& key, bool* matched) {
    const auto eq = line.find('=');
    if (eq == std::string::npos) return {};
    if (trim(line.substr(0, eq)) != key) return {};
    *matched = true;
    return line.substr(eq + 1);
}

bool digits_only(const std::string& value) {
    return !value.empty() && std::all_of(value.begin(), value.end(),
        [](unsigned char c) { return std::isdigit(c) != 0; });
}

int game_major(const std::string& version) {
    int major = 0;
    bool found = false;
    for (char c : version) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            if (found) break;
            continue;
        }
        found = true;
        major = major * 10 + (c - '0');
    }
    return found ? major : 0;
}

void add_unique(std::vector<std::string>& values, const std::string& value) {
    if (!value.empty() && std::find(values.begin(), values.end(), value) == values.end())
        values.push_back(value);
}

void add_unique(std::vector<std::uint64_t>& values, std::uint64_t value) {
    if (value != 0 && std::find(values.begin(), values.end(), value) == values.end())
        values.push_back(value);
}

std::string display_name(ActivationKind kind, const std::string& name) {
    if (kind == ActivationKind::Default) return "Default (mods/default.txt)";
    if (kind == ActivationKind::Save) return "Save: " + name;
    return "Server: " + name;
}

}  // namespace

std::vector<std::string> parse_block_mod_ids(const std::string& text) {
    std::vector<std::string> ids;
    for (const auto& line : split_lines(text)) {
        const std::string t = trim(line);
        if (!starts_with_mod(t)) continue;
        const auto eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string value = trim(t.substr(eq + 1));
        if (!value.empty() && value.back() == ',') value.pop_back();
        add_unique(ids, without_mod_prefix(value));
    }
    return ids;
}

std::vector<std::string> parse_ini_mod_ids(const std::string& text, bool* backslash) {
    std::vector<std::string> ids;
    if (backslash) *backslash = false;
    for (const auto& line : split_lines(text)) {
        bool matched = false;
        const std::string value = ini_value(line, "Mods", &matched);
        if (!matched) continue;
        for (const auto& part : split_semicolons(value)) {
            const std::string raw = trim(part);
            if (!raw.empty() && raw.front() == '\\' && backslash) *backslash = true;
            add_unique(ids, without_mod_prefix(raw));
        }
    }
    return ids;
}

std::vector<std::uint64_t> parse_ini_workshop_ids(const std::string& text) {
    std::vector<std::uint64_t> ids;
    for (const auto& line : split_lines(text)) {
        bool matched = false;
        const std::string value = ini_value(line, "WorkshopItems", &matched);
        if (!matched) continue;
        for (const auto& part : split_semicolons(value)) {
            const std::string raw = trim(part);
            if (!digits_only(raw)) continue;
            add_unique(ids, std::strtoull(raw.c_str(), nullptr, 10));
        }
    }
    return ids;
}

std::string rewrite_block_mod_ids(const std::string& text,
                                  const std::vector<std::string>& mod_ids,
                                  bool* changed) {
    const bool differs = parse_block_mod_ids(text) != mod_ids || block_uses_backslash(text);
    if (changed) *changed = differs;
    if (!differs) return text;

    const bool crlf = uses_crlf(text);
    std::vector<std::string> lines = split_lines(text);

    int mods_line = -1;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (trim(lines[i]) == "mods") {
            mods_line = static_cast<int>(i);
            break;
        }
    }

    int open = -1;
    int close = -1;
    if (mods_line >= 0) {
        for (std::size_t i = static_cast<std::size_t>(mods_line) + 1; i < lines.size(); ++i) {
            if (trim(lines[i]) == "{") {
                open = static_cast<int>(i);
                break;
            }
        }
    }
    if (open >= 0) {
        int depth = 0;
        for (std::size_t i = static_cast<std::size_t>(open); i < lines.size(); ++i) {
            const std::string t = trim(lines[i]);
            depth += static_cast<int>(std::count(t.begin(), t.end(), '{'));
            depth -= static_cast<int>(std::count(t.begin(), t.end(), '}'));
            if (depth == 0) {
                close = static_cast<int>(i);
                break;
            }
        }
    }

    std::vector<std::string> block;
    block.push_back("mods");
    block.push_back("{");
    for (const auto& id : mod_ids)
        block.push_back("    mod = " + id + ",");
    block.push_back("}");

    if (open >= 0 && close > open) {
        std::vector<std::string> out;
        out.reserve(lines.size() + mod_ids.size());
	out.insert(out.end(), lines.begin(), lines.begin() + mods_line);
        out.insert(out.end(), block.begin(), block.end());
        out.insert(out.end(), lines.begin() + close + 1, lines.end());
        return join_lines(out, crlf);
    }

    if (lines.empty()) {
        std::vector<std::string> fresh{"version = 1,", ""};
        fresh.insert(fresh.end(), block.begin(), block.end());
        return join_lines(fresh, crlf);
    }
    std::vector<std::string> out = lines;
    if (!out.empty() && !trim(out.back()).empty()) out.push_back("");
    out.insert(out.end(), block.begin(), block.end());
    return join_lines(out, crlf);
}

std::string rewrite_ini_mods(const std::string& text,
                             const std::vector<std::string>& mod_ids,
                             bool backslash, bool* changed) {
    bool old_backslash = false;
    if (changed) *changed = parse_ini_mod_ids(text, &old_backslash) != mod_ids;

    std::string value;
    for (const auto& id : mod_ids) {
        if (!value.empty()) value += ';';
        if (backslash) value += '\\';
        value += id;
    }

    const bool crlf = uses_crlf(text);
    std::vector<std::string> lines = split_lines(text);
    bool replaced = false;
    for (auto& line : lines) {
        bool matched = false;
        ini_value(line, "Mods", &matched);
        if (!matched) continue;
        line = "Mods=" + value;
        replaced = true;
        break;
    }
    if (!replaced) {
        if (mod_ids.empty() && trim(text).empty()) return text;
        lines.push_back("Mods=" + value);
    }
    return join_lines(lines, crlf);
}

std::string rewrite_ini_workshop_items(const std::string& text,
                                       const std::vector<std::uint64_t>& ids,
                                       bool* changed) {
    if (changed) *changed = parse_ini_workshop_ids(text) != ids;

    std::string value;
    for (auto id : ids) {
        if (!value.empty()) value += ';';
        value += std::to_string(id);
    }

    const bool crlf = uses_crlf(text);
    std::vector<std::string> lines = split_lines(text);
    bool replaced = false;
    for (auto& line : lines) {
        bool matched = false;
        ini_value(line, "WorkshopItems", &matched);
        if (!matched) continue;
        line = "WorkshopItems=" + value;
        replaced = true;
        break;
    }
    if (!replaced) {
        if (ids.empty() && trim(text).empty()) return text;
        lines.push_back("WorkshopItems=" + value);
    }
    return join_lines(lines, crlf);
}

bool read_activation_target(ActivationTarget& target) {
    std::string text;
    if (!read_text_file(target.path, text)) {
        target.exists = false;
        target.mod_ids.clear();
        target.workshop_ids.clear();
        return true;
    }
    target.exists = true;
    if (target.kind == ActivationKind::Server) {
        bool backslash = false;
        target.mod_ids = parse_ini_mod_ids(text, &backslash);
        target.workshop_ids = parse_ini_workshop_ids(text);
        if (!target.mod_ids.empty()) target.backslash = backslash;
    } else {
        target.mod_ids = parse_block_mod_ids(text);
        // default.txt and save mods.txt always use plain Mod IDs. The B42
        // backslash form belongs to a server's INI Mods= line, not these
        // blocks; a backslash here makes the game discard the entry.
        target.backslash = false;
        // Repair files written by older pzmodmgr builds that used the server
        // syntax here. This is a one-time rewrite; plain files are untouched.
        if (block_uses_backslash(text)) {
            bool changed = false;
            const std::string out = rewrite_block_mod_ids(text, target.mod_ids, &changed);
            if (changed) write_text_atomic(target.path, out);
        }
    }
    return true;
}

std::vector<ActivationTarget> discover_activation_targets(const fs::path& zomboid_dir,
                                                          const std::string& game_version) {
    std::vector<ActivationTarget> targets;

    ActivationTarget defaults;
    defaults.kind = ActivationKind::Default;
    defaults.label = display_name(defaults.kind, {});
    defaults.path = zomboid_dir / "mods" / "default.txt";
    // default.txt uses the block form with plain Mod IDs on both builds.
    read_activation_target(defaults);
    targets.push_back(defaults);

    std::vector<ActivationTarget> saves;
    pz::error_code ec;
    fs::directory_iterator modes(zomboid_dir / "Saves", ec);
    if (!ec) {
        for (const auto& mode : modes) {
            if (!mode.is_directory(ec)) continue;
            fs::directory_iterator games(mode.path(), ec);
            if (ec) continue;
            for (const auto& game : games) {
                if (!game.is_directory(ec)) continue;
                ActivationTarget target;
                target.kind = ActivationKind::Save;
                target.name = mode.path().filename().string() + "/" +
                              game.path().filename().string();
                target.label = display_name(target.kind, target.name);
                target.path = game.path() / "mods.txt";
                pz::error_code thumb_ec;
                const fs::path thumb = game.path() / "thumb.png";
                if (fs::is_regular_file(thumb, thumb_ec)) target.thumbnail = thumb;
                read_activation_target(target);
                saves.push_back(std::move(target));
            }
        }
    }
    std::sort(saves.begin(), saves.end(), [](const ActivationTarget& a, const ActivationTarget& b) {
        return a.name < b.name;
    });
    targets.insert(targets.end(), saves.begin(), saves.end());

    std::vector<ActivationTarget> servers;
    fs::directory_iterator ini_files(zomboid_dir / "Server", ec);
    if (!ec) {
        for (const auto& entry : ini_files) {
            if (!entry.is_regular_file(ec)) continue;
            if (entry.path().extension() != ".ini") continue;
            ActivationTarget target;
            target.kind = ActivationKind::Server;
            target.name = entry.path().stem().string();
            target.label = display_name(target.kind, target.name);
            target.path = entry.path();
            target.backslash = game_major(game_version) >= 42;
            read_activation_target(target);
            servers.push_back(std::move(target));
        }
    }
    std::sort(servers.begin(), servers.end(), [](const ActivationTarget& a, const ActivationTarget& b) {
        return a.name < b.name;
    });
    targets.insert(targets.end(), servers.begin(), servers.end());
    return targets;
}

bool write_activation_target(ActivationTarget& target,
                             const std::vector<std::string>& mod_ids,
                             const std::vector<std::uint64_t>& workshop_ids,
                             bool enabled) {
    std::string text;
    const bool readable = read_text_file(target.path, text);
    if (!readable && !enabled) return true;   // nothing to disable

    if (target.kind == ActivationKind::Server) {
        bool detected_backslash = false;
        std::vector<std::string> ids = parse_ini_mod_ids(text, &detected_backslash);
        const bool backslash =
            ids.empty() ? target.backslash : detected_backslash;
        std::vector<std::uint64_t> workshops = parse_ini_workshop_ids(text);
        if (enabled) {
            for (const auto& id : mod_ids) add_unique(ids, id);
        } else {
            for (const auto& id : mod_ids)
                ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
        }
        bool mods_changed = false;
        std::string out = rewrite_ini_mods(text, ids, backslash, &mods_changed);
        bool workshops_changed = false;
        if (enabled && !workshop_ids.empty()) {
            for (auto id : workshop_ids) add_unique(workshops, id);
            workshops_changed = parse_ini_workshop_ids(out) != workshops;
            if (workshops_changed) out = rewrite_ini_workshop_items(out, workshops);
        }
        if ((mods_changed || workshops_changed) && !write_text_atomic(target.path, out))
            return false;
        target.mod_ids = std::move(ids);
        target.workshop_ids = std::move(workshops);
        target.backslash = backslash;
        target.exists = true;
        return true;
    }

    std::vector<std::string> ids = parse_block_mod_ids(text);
    if (enabled) {
        for (const auto& id : mod_ids) add_unique(ids, id);
    } else {
        for (const auto& id : mod_ids)
            ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
    }
    bool changed = false;
    const std::string out = rewrite_block_mod_ids(text, ids, &changed);
    if (changed && !write_text_atomic(target.path, out)) return false;
    target.mod_ids = std::move(ids);
    target.backslash = false;
    target.exists = true;
    return true;
}

}  // namespace pz
