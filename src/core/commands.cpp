#include "core/commands.hpp"
#include "core/paths.hpp"
#include "core/url.hpp"
#include <algorithm>
#include <sstream>
#include <vector>

namespace pz {
namespace {

std::vector<std::uint64_t> parse_ids(const std::string& arg) {
    std::istringstream in(arg);
    std::vector<std::uint64_t> ids;
    std::string token;
    while (in >> token) {
        auto id = parse_workshop_id(token);
        if (!id) return {};
        if (std::find(ids.begin(), ids.end(), *id) == ids.end()) ids.push_back(*id);
    }
    return ids;
}
}  // namespace

Result run_command(ModManager& mm, const std::string& line, const LogFn& log) {
    std::istringstream ss(line);
    std::string cmd;
    ss >> cmd;
    std::string arg;
    std::getline(ss >> std::ws, arg);

    if (cmd == "add") return mm.add(arg, log);
    if (cmd == "remove") {
        auto ids = parse_ids(arg);
        if (ids.empty()) return {false, "expected a Workshop ID"};
        if (ids.size() == 1) return mm.remove(ids[0], log);

        int removed = 0;
        for (auto id : ids)
            if (mm.remove(id, log).ok) ++removed;
        const int missing = static_cast<int>(ids.size()) - removed;
        std::string msg = "Removed " + std::to_string(removed) + " of " +
                          std::to_string(ids.size()) + " selected mod(s)";
        if (missing != 0) msg += ", " + std::to_string(missing) + " not installed";
        return {missing == 0, msg};
    }
    if (cmd == "check") return mm.check_updates(log);
    if (cmd == "update") {
        if (arg.empty()) return mm.update_all(log);
        auto ids = parse_ids(arg);
        if (ids.empty()) return {false, "expected a Workshop ID"};
        if (ids.size() == 1) return mm.update_one(ids[0], log);

        int updated = 0, failed = 0;
        for (auto id : ids) {
            auto r = mm.update_one(id, log);
            log(std::to_string(id) + ": " + r.message);
            if (r.ok) ++updated;
            else ++failed;
        }
        std::string msg = "Updated " + std::to_string(updated) + " of " +
                          std::to_string(ids.size()) + " selected mod(s)";
        if (failed != 0) msg += ", " + std::to_string(failed) + " failed";
        return {failed == 0, msg};
    }
    if (cmd == "list") {
        auto mods = mm.list();
        for (const auto& m : mods)
            log(m.name + " [" + m.folder + "] id=" + std::to_string(m.workshop_id));
        return {true, std::to_string(mods.size()) + " mod(s)"};
    }
    if (cmd == "config") {
        std::istringstream a(arg);
        std::string key, value;
        a >> key;
        std::getline(a >> std::ws, value);
        auto& c = mm.config();
        if (key == "steamcmd_exe") c.steamcmd_exe = value;
        else if (key == "steam_user") c.steam_user = value;
        else return {false, "unknown config key: " + key};
        save_config(c);
        return {true, "saved " + key};
    }

    // A pasted Workshop URL or numeric id with no command prefix means add.
    if (parse_workshop_id(line)) return mm.add(line, log);

    return {false, "unknown command: " + cmd};
}
}  // namespace pz
