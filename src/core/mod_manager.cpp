#include "core/mod_manager.hpp"
#include "core/mod_info.hpp"
#include "core/steamcmd.hpp"
#include "core/url.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>

namespace pz {
namespace {

// Moves a downloaded mod into place. The download cache is not kept, so the
// same files are not stored twice. A rename is atomic on the same filesystem;
// a copy is only a fallback for a cross-device cache.
bool move_tree(const fs::path& src, const fs::path& dst) {
    fs::create_directories(dst.parent_path());
    std::error_code ec;
    fs::rename(src, dst, ec);
    if (!ec) return true;

    ec.clear();
    fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) return false;
    fs::remove_all(src, ec);
    return true;
}

std::string stamp() {
    return std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
}
}  // namespace

ModManager::ModManager(Config cfg) : cfg_(std::move(cfg)) {
    fs::create_directories(cfg_.user_mods_dir);
    fs::create_directories(cfg_.data_dir);
    load_db();
}

Result ModManager::add(const std::string& input, const LogFn& log) {
    auto id = parse_workshop_id(input);
    if (!id) return {false, "No Workshop ID found in: " + input};

    // Collections and single items share the same URL/numeric-id input.
    auto children = fetch_collection_items(*id);
    if (!children.empty()) return install_collection(*id, children, log);

    if (is_installed(*id)) return {false, "Already installed. Use: update"};
    return install(*id, fetch_remote_time_updated(*id), log);
}

Result ModManager::remove(std::uint64_t id, const LogFn& log) {
    bool any = false;
    for (const auto& m : db_) {
        if (m.workshop_id != id) continue;
        std::error_code ec;
        fs::remove_all(cfg_.user_mods_dir / m.folder, ec);   // only folders we recorded
        log("removed " + m.folder);
        any = true;
    }
    if (!any) return {false, "Not installed: " + std::to_string(id)};
    db_.erase(std::remove_if(db_.begin(), db_.end(),
              [&](const InstalledMod& m) { return m.workshop_id == id; }), db_.end());
    save_db();
    return {true, "Removed " + std::to_string(id)};
}

Result ModManager::update_one(std::uint64_t id, const LogFn& log) {
    if (!is_installed(id)) return {false, "Not installed: " + std::to_string(id)};
    auto remote = fetch_remote_time_updated(id);
    if (remote == 0) return {false, "Update check failed: " + std::to_string(id)};
    if (remote <= recorded_time(id)) return {true, "Up to date: " + std::to_string(id)};

    auto r = install(id, remote, log);
    if (!r.ok) return r;
    return {true, "Updated " + std::to_string(id) + ": " + r.message};
}

Result ModManager::update_all(const LogFn& log) {
    std::vector<std::uint64_t> ids;
    for (const auto& m : db_)
        if (std::find(ids.begin(), ids.end(), m.workshop_id) == ids.end()) ids.push_back(m.workshop_id);

    int updated = 0;
    int skipped = 0;
    for (auto id : ids) {
        auto remote = fetch_remote_time_updated(id);
        if (remote == 0) {
            log("update check failed: " + std::to_string(id) + " (skipped)");
            ++skipped;
            continue;
        }
        if (remote <= recorded_time(id)) {
            log("up to date: " + std::to_string(id));
            continue;
        }
        auto r = install(id, remote, log);
        log((r.ok ? "updated: " : "failed: ") + std::to_string(id) + " " + r.message);
        if (r.ok) ++updated;
    }
    std::string msg = "Updated " + std::to_string(updated) + " mod item(s)";
    if (skipped != 0) msg += ", skipped " + std::to_string(skipped) + " (update check failed)";
    return {true, msg};
}

Result ModManager::install_collection(std::uint64_t collection_id,
                                      const std::vector<std::uint64_t>& children,
                                      const LogFn& log) {
    log("collection " + std::to_string(collection_id) + ": " +
        std::to_string(children.size()) + " item(s)");

    int installed = 0, updated = 0, skipped = 0, failed = 0;
    for (auto child : children) {
        const bool present = is_installed(child);
        auto remote = fetch_remote_time_updated(child);
        if (remote == 0) {
            log("update check failed: " + std::to_string(child) + " (skipped)");
            ++skipped;
            continue;
        }
        if (present && remote <= recorded_time(child)) {
            log("up to date: " + std::to_string(child));
            ++skipped;
            continue;
        }

        auto r = install(child, remote, log);
        if (!r.ok) {
            log("failed: " + std::to_string(child) + " " + r.message);
            ++failed;
        } else if (present) {
            ++updated;
        } else {
            ++installed;
        }
    }

    std::string msg = "Collection " + std::to_string(collection_id) + ": installed " +
        std::to_string(installed) + ", updated " + std::to_string(updated) +
        ", skipped " + std::to_string(skipped) + ", failed " + std::to_string(failed);
    return {failed == 0, msg};
}

Result ModManager::install(std::uint64_t id, std::uint64_t remote_time, const LogFn& log) {
    auto dl = steamcmd_download_workshop_item(cfg_, id, cfg_.data_dir / "steam_cache", log);
    if (!dl.ok) return {false, dl.detail};

    auto roots = discover_mod_roots(dl.content_dir);
    if (roots.empty()) return {false, "No mod.info in workshop item " + std::to_string(id)};

    struct Moved {
        fs::path source;
        fs::path dest;
        fs::path backup;
    };
    std::vector<Moved> moved;
    auto rollback = [&]() {
        for (auto it = moved.rbegin(); it != moved.rend(); ++it) {
            std::error_code ec;
            if (fs::exists(it->dest)) {
                move_tree(it->dest, it->source);
                if (!it->backup.empty()) fs::rename(it->backup, it->dest, ec);
            }
        }
    };

    std::vector<InstalledMod> fresh;
    for (const auto& root : roots) {
        auto info = read_mod_info(root);
        if (!info) continue;

        std::string folder = (root == dl.content_dir)
            ? (info->id.empty() ? std::to_string(id) : info->id)
            : root.filename().string();

        fs::path dest = cfg_.user_mods_dir / folder;
        fs::path backup;
        if (fs::exists(dest)) {   // never silently destroy a manual/local mod
            backup = cfg_.data_dir / "backups" / (folder + "-" + stamp());
            std::error_code ec;
            fs::create_directories(backup.parent_path());
            fs::rename(dest, backup, ec);
            if (ec) {
                rollback();
                return {false, "Could not back up " + folder + ": " + ec.message()};
            }
            log("backed up existing " + folder + " -> " + backup.string());
        }

        if (!move_tree(root, dest)) {
            std::error_code ec;
            if (!backup.empty() && fs::exists(backup)) {
                fs::remove_all(dest, ec);
                fs::rename(backup, dest, ec);
            }
            rollback();
            return {false, "Could not move downloaded mod into " + dest.string()};
        }
        moved.push_back({root, dest, backup});
        log("installed " + folder + " (moved from download cache)");
        fresh.push_back({id, folder, info->name.empty() ? folder : info->name, remote_time});
    }
    if (fresh.empty()) return {false, "mod.info unreadable in item " + std::to_string(id)};

    // The roots were moved out; drop the now-empty download cache directory.
    std::error_code cleanup_ec;
    fs::remove_all(dl.content_dir, cleanup_ec);

    db_.erase(std::remove_if(db_.begin(), db_.end(),
              [&](const InstalledMod& m) { return m.workshop_id == id; }), db_.end());
    db_.insert(db_.end(), fresh.begin(), fresh.end());
    save_db();
    return {true, "Installed " + std::to_string(fresh.size()) + " mod(s)"};
}

bool ModManager::is_installed(std::uint64_t id) const {
    return std::any_of(db_.begin(), db_.end(),
        [id](const InstalledMod& m) { return m.workshop_id == id; });
}

std::uint64_t ModManager::recorded_time(std::uint64_t id) const {
    std::uint64_t t = 0;
    for (const auto& m : db_)
        if (m.workshop_id == id) t = std::max(t, m.time_updated);
    return t;
}

void ModManager::load_db() {
    std::ifstream in(db_path());
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string id, folder, name, tu;
        if (std::getline(ss, id, '\t') && std::getline(ss, folder, '\t') &&
            std::getline(ss, name, '\t') && std::getline(ss, tu)) {
            try {
                db_.push_back({std::stoull(id), folder, name, std::stoull(tu)});
            } catch (...) { /* skip corrupt line */ }
        }
    }
}

void ModManager::save_db() const {
    std::ofstream out(db_path(), std::ios::trunc);
    for (const auto& m : db_)
        out << m.workshop_id << '\t' << m.folder << '\t' << m.name << '\t' << m.time_updated << '\n';
}
}  // namespace pz
