#include "core/mod_manager.hpp"
#include "core/mod_info.hpp"
#include "core/paths.hpp"
#include "core/steamcmd.hpp"
#include "core/thread.hpp"
#include "core/text.hpp"
#include "core/updater.hpp"
#include "core/url.hpp"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <ctime>
#include <cstdlib>
#include <utility>

namespace pz {
namespace {

// std::stoull pulls the locale/iostream conversion facets into the MinGW
// static runtime. installed.tsv only contains decimal integers, so parse them
// with the C helper and reject malformed lines without exceptions.
bool parse_stored_number(const std::string& text, std::uint64_t& value) {
    if (text.empty()) return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
    if (errno == ERANGE || end == text.c_str() || *end != '\0') return false;
    value = static_cast<std::uint64_t>(parsed);
    return true;
}

// Moves a downloaded mod into place. The download cache is not kept, so the
// same files are not stored twice. A rename is atomic on the same filesystem;
// a copy is only a fallback for a cross-device cache.
bool move_tree(const fs::path& src, const fs::path& dst) {
    fs::create_directories(dst.parent_path());
    pz::error_code ec;
    fs::rename(src, dst, ec);
    if (!ec) return true;

    ec.clear();
    fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) return false;
    fs::remove_all(src, ec);
    return true;
}

std::string stamp() {
    return std::to_string(static_cast<unsigned long long>(std::time(nullptr)));
}

// Download caches are only needed while an install is running. A crash or a
// killed steamcmd can leave one behind forever; prune it after a day. Backups
// are crash-recovery copies, so keep them for a week before reclaiming them.
void prune_stale_children(const fs::path& dir, fs::file_time_type max_age_seconds) {
    pz::error_code ec;
    fs::directory_iterator entries(dir, ec);
    if (ec) return;
    const auto cutoff = fs::file_time_now() - max_age_seconds;
    for (const auto& entry : entries) {
        pz::error_code item_ec;
        const auto modified = fs::last_write_time(entry.path(), item_ec);
        if (!item_ec && modified < cutoff) fs::remove_all(entry.path(), item_ec);
    }
    fs::remove(dir, ec);   // succeeds only when the directory is empty
}

void prune_stale_state(const fs::path& data_dir) {
    prune_stale_children(data_dir / "steam_cache", 24 * 60 * 60);
    prune_stale_children(data_dir / "backups", 7 * 24 * 60 * 60);
}

std::string read_game_version(const fs::path& zomboid_dir) {
    std::string version;
    for_each_line(zomboid_dir / "version.txt", [&](const std::string& line) {
        if (!version.empty()) return;
        std::size_t begin = 0, end = 0;
        while (begin < line.size() && std::isspace(static_cast<unsigned char>(line[begin])))
            ++begin;
        end = begin;
        while (end < line.size() && !std::isspace(static_cast<unsigned char>(line[end])))
            ++end;
        version = line.substr(begin, end - begin);
    });
    return version;
}

constexpr std::size_t kRemoteCheckWorkers = 4;
constexpr std::uint64_t kUpdateCheckIntervalSeconds = 24 * 60 * 60;

std::uint64_t now_seconds() {
    return static_cast<std::uint64_t>(std::time(nullptr));
}

std::string folded(std::string value) {
    for (char& c : value)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

void add_unique(std::vector<std::string>& values, const std::string& value) {
    if (!value.empty() && std::find(values.begin(), values.end(), value) == values.end())
        values.push_back(value);
}

void add_unique(std::vector<std::uint64_t>& values, std::uint64_t value) {
    if (value != 0 && std::find(values.begin(), values.end(), value) == values.end())
        values.push_back(value);
}

long long numeric_version(const std::string& value) {
    long long number = 0;
    bool any = false;
    for (unsigned char c : value) {
        if (std::isdigit(c)) {
            any = true;
            number = number * 10 + (c - '0');
        } else if (any) {
            break;
        }
    }
    return any ? number : 0;
}

long long trailing_number(const std::string& value) {
    std::size_t end = value.size();
    while (end > 0 && !std::isdigit(static_cast<unsigned char>(value[end - 1]))) --end;
    const std::size_t begin = [&]() {
        std::size_t i = end;
        while (i > 0 && std::isdigit(static_cast<unsigned char>(value[i - 1]))) --i;
        return i;
    }();
    if (begin == end) return 0;
    return std::strtoll(value.substr(begin, end - begin).c_str(), nullptr, 10);
}

bool same_mods(const std::vector<InstalledMod>& a,
               const std::vector<InstalledMod>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].workshop_id != b[i].workshop_id ||
            a[i].folder != b[i].folder ||
            a[i].name != b[i].name ||
            a[i].time_updated != b[i].time_updated ||
            a[i].owned != b[i].owned) return false;
    }
    return true;
}

// The Steam metadata calls are short-lived curl processes. A small fixed pool
// keeps a large collection check quick without spawning one process per item,
// while downloads remain serialized by the caller.
std::vector<std::uint64_t> fetch_remote_times(const std::vector<std::uint64_t>& ids) {
    std::vector<std::uint64_t> times(ids.size(), 0);
    if (ids.empty()) return times;

    const std::size_t workers = std::min(ids.size(), kRemoteCheckWorkers);
    std::atomic<std::size_t> next{0};
    std::vector<thread> pool;
    pool.reserve(workers);
    bool spawn_failed = false;
    for (std::size_t i = 0; i < workers; ++i) {
        thread worker([&]() {
            for (;;) {
                const std::size_t index = next.fetch_add(1);
                if (index >= ids.size()) break;
                times[index] = fetch_remote_time_updated(ids[index]);
            }
        });
        if (!worker.joinable()) {
            // If a thread cannot be created, finish the remaining lookups here
            // after joining the workers that did start.
            spawn_failed = true;
            break;
        }
        pool.push_back(std::move(worker));
    }
    for (auto& thread : pool)
        if (thread.joinable()) thread.join();
    if (spawn_failed) {
        for (std::size_t i = 0; i < ids.size(); ++i)
            if (times[i] == 0) times[i] = fetch_remote_time_updated(ids[i]);
    }
    return times;
}

// Steam answers "Access Denied" for items anonymous steamcmd cannot see at all,
// and steamcmd still exits 0, so the reason has to be named here.
std::string unavailable_item_message(std::uint64_t id, std::uint32_t result) {
    std::string msg = "Workshop item " + std::to_string(id) + " is unavailable";
    if (result == 9) msg += " (removed, private, or not a public Workshop item)";
    else if (result == 15) msg += " (Steam denied access: private or removed)";
    else msg += " (Steam result " + std::to_string(result) + ")";
    return msg;
}
}  // namespace

ModManager::ModManager(Config cfg)
    : cfg_(std::move(cfg)), game_version_(read_game_version(cfg_.zomboid_dir)) {
    fs::create_directories(cfg_.user_mods_dir);
    fs::create_directories(cfg_.data_dir);
    prune_stale_state(cfg_.data_dir);
    load_db();
    load_update_state();
    load_app_update_state();
}

Result ModManager::set_zomboid_dir(const fs::path& dir) {
    if (dir.empty()) return {false, "Zomboid folder is empty"};
    const fs::path normalized = dir.lexically_normal();
    if (!normalized.is_absolute())
        return {false, "Zomboid folder must be an absolute path: " + dir.string()};
    if (normalized == normalized.root_path())
        return {false, "Refusing to use a filesystem root as the Zomboid folder"};
    if (normalized == cfg_.zomboid_dir) return {true, "Zomboid folder unchanged"};

    pz::error_code ec;
    fs::create_directories(normalized / "mods", ec);
    if (ec)
        return {false, "Could not create " + (normalized / "mods").string() +
                           ": " + ec.message()};
    ec.clear();
    const fs::path new_data = normalized / "pzmodmgr";
    fs::create_directories(new_data, ec);
    if (ec)
        return {false, "Could not create " + new_data.string() + ": " + ec.message()};
    prune_stale_state(new_data);

    const fs::path old_builtin = builtin_steamcmd_path(cfg_);
    cfg_.zomboid_dir = normalized;
    cfg_.user_mods_dir = normalized / "mods";
    cfg_.data_dir = new_data;
    if (cfg_.steamcmd_exe.empty() || cfg_.steamcmd_exe == old_builtin)
        cfg_.steamcmd_exe = builtin_steamcmd_path(cfg_);

    game_version_ = read_game_version(cfg_.zomboid_dir);
    db_.clear();
    updates_.clear();
    app_update_available_ = false;
    load_db();
    load_update_state();
    load_app_update_state();
    return {true, "Zomboid folder set to " + normalized.string()};
}

Result ModManager::add(const std::string& input, const LogFn& log) {
    auto id = parse_workshop_id(input);
    if (!id) return {false, "No Workshop ID found in: " + input};

    // Collections and single items share the same URL/numeric-id input.
    auto children = fetch_collection_items(*id);
    if (!children.empty()) return install_collection(*id, children, log);

    if (is_installed(*id)) return {false, "Already installed. Use: update"};
    return install(*id, fetch_remote_time_updated(*id), log, nullptr, true);
}

Result ModManager::remove(std::uint64_t id, const LogFn& log) {
    if (!is_installed(id)) return {false, "Not installed: " + std::to_string(id)};

    // Read the Mod IDs while the folders still exist. Removing them from every
    // activation list first prevents a save or server from loading a mod whose
    // files are already gone; if a list cannot be written, keep the mod intact.
    const std::vector<std::string> removed_ids = mod_ids_for(id);
    int disabled_targets = 0;
    auto disabled = disable_mod_ids(removed_ids, disabled_targets, log);
    if (!disabled.ok) return disabled;
    if (removed_ids.empty())
        log("warning: no Mod ID found for " + std::to_string(id) +
            "; mod lists were not changed");

    int removed = 0, kept = 0;
    for (const auto& m : db_) {
        if (m.workshop_id != id) continue;
        if (m.owned) {
            pz::error_code ec;
            fs::remove_all(cfg_.user_mods_dir / m.folder, ec);
            log("removed " + m.folder);
            ++removed;
        } else {
            // The folder existed before it was tracked; deleting it would
            // destroy a mod the manager never installed.
            log("stopped tracking " + m.folder + " (left in place)");
            ++kept;
        }
    }
    updates_.erase(id);
    db_.erase(std::remove_if(db_.begin(), db_.end(),
              [&](const InstalledMod& m) { return m.workshop_id == id; }), db_.end());
    save_db();
    save_update_state();
    std::string msg = removed != 0 ? "Removed " + std::to_string(id)
                                   : "Stopped tracking " + std::to_string(id);
    if (kept != 0) msg += " (left " + std::to_string(kept) + " existing folder(s) in place)";
    if (disabled_targets != 0)
        msg += " (disabled in " + std::to_string(disabled_targets) + " mod list(s))";
    return {true, msg};
}

Result ModManager::disable_mod_ids(const std::vector<std::string>& mod_ids,
                                   int& disabled_targets, const LogFn& log) {
    disabled_targets = 0;
    if (mod_ids.empty()) return {true, {}};

    auto targets = activation_targets();
    for (auto& target : targets) {
        std::vector<std::string> affected;
        for (const auto& id : mod_ids) {
            if (std::find(target.mod_ids.begin(), target.mod_ids.end(), id) !=
                target.mod_ids.end())
                affected.push_back(id);
        }
        if (affected.empty()) continue;

        if (!write_activation_target(target, affected, {}, false))
            return {false, "Could not disable " + std::to_string(affected.size()) +
                           " Mod ID(s) in " + target.path.string()};
        ++disabled_targets;
        log("disabled " + std::to_string(affected.size()) + " Mod ID(s) in " +
            target.label);
    }
    return {true, {}};
}

Result ModManager::update_one(std::uint64_t id, const LogFn& log) {
    if (!is_installed(id)) return {false, "Not installed: " + std::to_string(id)};
    auto remote = fetch_remote_time_updated(id);
    if (remote == 0) return {false, "Update check failed: " + std::to_string(id)};
    if (remote <= recorded_time(id)) {
        updates_[id] = false;
        save_update_state();
        return {true, "Up to date: " + std::to_string(id)};
    }

    bool changed = false;
    auto r = install(id, remote, log, &changed);
    if (!r.ok) return r;
    updates_[id] = false;
    save_update_state();
    if (!changed) return {true, r.message};
    return {true, "Updated " + std::to_string(id) + ": " + r.message};
}

Result ModManager::update_all(const LogFn& log) {
    const auto ids = unique_installed_ids();
    const auto remote_times = fetch_remote_times(ids);
    int updated = 0;
    int skipped = 0;
    int unchanged = 0;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const auto id = ids[i];
        const auto remote = remote_times[i];
        if (remote == 0) {
            log("update check failed: " + std::to_string(id) + " (skipped)");
            ++skipped;
            continue;
        }
        if (remote <= recorded_time(id)) {
            updates_[id] = false;
            log("up to date: " + std::to_string(id));
            continue;
        }
        bool changed = false;
        auto r = install(id, remote, log, &changed);
        if (!r.ok) {
            log("failed: " + std::to_string(id) + " " + r.message);
            continue;
        }
        updates_[id] = false;
        if (!changed) {
            log("up to date: " + std::to_string(id));
            ++unchanged;
        } else {
            log("updated: " + std::to_string(id) + " " + r.message);
            ++updated;
        }
    }
    if (!ids.empty()) record_update_check();
    save_update_state();
    std::string msg = "Updated " + std::to_string(updated) + " mod item(s)";
    if (unchanged != 0) msg += ", " + std::to_string(unchanged) + " unchanged";
    if (skipped != 0) msg += ", skipped " + std::to_string(skipped) + " (update check failed)";
    return {true, msg};
}

Result ModManager::check_updates(const LogFn& log) {
    const auto ids = unique_installed_ids();
    const auto remote_times = fetch_remote_times(ids);
    int checked = 0, available = 0, failed = 0;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const auto id = ids[i];
        const auto remote = remote_times[i];
        if (remote == 0) {
            ++failed;
            log("update check failed: " + std::to_string(id));
            continue;
        }
        ++checked;
        const bool has_update = remote > recorded_time(id);
        updates_[id] = has_update;
        if (has_update) {
            ++available;
            log("update available: " + std::to_string(id));
        } else {
            log("up to date: " + std::to_string(id));
        }
    }
    if (!ids.empty()) record_update_check();
    save_update_state();
    std::string msg = "Checked " + std::to_string(checked) + " mod(s), " +
                      std::to_string(available) + " update(s) available";
    if (failed != 0) msg += ", " + std::to_string(failed) + " check(s) failed";
    return {true, msg};
}

Result ModManager::check_app_release(const LogFn& log) {
    const AppUpdate update = pz::check_app_update();
    // Record the attempt even when the network failed: the UI asks once a day,
    // not on every launch, so an offline start does not turn into a retry loop.
    record_app_update_check();
    if (!update.ok) {
        log("app update check failed: " + update.message);
        return {false, update.message};
    }

    app_update_available_ = update.available;
    if (!write_text_atomic(app_update_path(), update.latest_version + "\n"))
        log("warning: could not save the app update state");
    log(update.message);
    return {true, update.message};
}

bool ModManager::update_available(std::uint64_t workshop_id) const {
    auto it = updates_.find(workshop_id);
    return it != updates_.end() && it->second;
}

bool ModManager::update_check_due() const {
    if (app_update_check_due()) return true;
    if (db_.empty()) return false;
    const std::uint64_t now = now_seconds();
    const std::uint64_t last = read_last_check();
    if (last == 0) return true;
    // A check timestamp in the future means the clock moved backwards. Do not
    // turn that into a check on every reopen until the clock catches up.
    if (last > now) return false;
    return now - last >= kUpdateCheckIntervalSeconds;
}

bool ModManager::app_update_check_due() const {
    const std::uint64_t now = now_seconds();
    const std::uint64_t last = read_last_app_check();
    if (last == 0) return true;
    if (last > now) return false;
    return now - last >= kUpdateCheckIntervalSeconds;
}

bool ModManager::reload_installed() {
    const std::string old_version = game_version_;
    std::vector<InstalledMod> previous = std::move(db_);
    db_.clear();
    load_db();
    game_version_ = read_game_version(cfg_.zomboid_dir);

    pz::flat_set<std::uint64_t> present;
    for (const auto& mod : db_) present.insert(mod.workshop_id);
    bool updates_changed = false;
    for (auto it = updates_.begin(); it != updates_.end();) {
        if (present.count(it->first) == 0) {
            it = updates_.erase(it);
            updates_changed = true;
        } else {
            ++it;
        }
    }
    if (updates_changed) save_update_state();
    return !same_mods(previous, db_) || old_version != game_version_;
}

Result ModManager::install_collection(std::uint64_t collection_id,
                                      const std::vector<std::uint64_t>& children,
                                      const LogFn& log) {
    log("collection " + std::to_string(collection_id) + ": " +
        std::to_string(children.size()) + " item(s)");

    const auto remote_times = fetch_remote_times(children);
    int installed = 0, updated = 0, skipped = 0, failed = 0;
    for (std::size_t i = 0; i < children.size(); ++i) {
        const auto child = children[i];
        const bool present = is_installed(child);
        const auto remote = remote_times[i];
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

        bool changed = false;
        auto r = install(child, remote, log, &changed, true);
        if (!r.ok) {
            log("failed: " + std::to_string(child) + " " + r.message);
            ++failed;
        } else if (!changed) {
            if (present) ++skipped;
            else ++installed;
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

Result ModManager::install(std::uint64_t id, std::uint64_t remote_time, const LogFn& log,
                           bool* changed, bool enable_new) {
    if (changed) *changed = false;
    // Ask Steam whether the item is reachable before spending a steamcmd run on
    // it: a private or removed item only ever produces "Access Denied", and
    // steamcmd exits 0 while doing so.
    const std::uint32_t availability = fetch_published_file_result(id);
    if (availability != 0 && availability != 1)
        return {false, unavailable_item_message(id, availability)};

    // Each download gets its own cache root. The shared appworkshop ACF can
    // retain a failed or stale item and make later downloads fail with
    // steamcmd's generic "Failure"; a fresh per-item root avoids that state.
    const fs::path cache_dir = cfg_.data_dir / "steam_cache" / std::to_string(id);
    pz::error_code cleanup_ec;
    fs::remove_all(cache_dir, cleanup_ec);
    auto dl = steamcmd_download_workshop_item(cfg_, id, cache_dir, log);
    if (!dl.ok) {
        fs::remove_all(cache_dir, cleanup_ec);
        return {false, dl.detail};
    }
    auto cleanup_cache = [&]() {
        pz::error_code ec;
        fs::remove_all(cache_dir, ec);
    };

    auto roots = discover_mod_roots(dl.content_dir);
    if (roots.empty()) {
        cleanup_cache();
        return {false, "No mod.info in workshop item " + std::to_string(id)};
    }

    // A Workshop item can contain several variants. Prefer the ones whose
    // versionMin/versionMax includes the installed Project Zomboid build. Older
    // builds are still installable: fall back to the downloaded variants and
    // leave a clear activity message instead of blocking the user.
    std::vector<fs::path> compatible;
    std::vector<fs::path> incompatible;
    for (const auto& root : roots) {
        auto info = read_mod_info(root, game_version_);
        if (!info) continue;
        if (supports_game_version(*info, game_version_)) {
            compatible.push_back(root);
            continue;
        }
        incompatible.push_back(root);
        const std::string name = info->name.empty() ? info->id : info->name;
        const std::string label = name.empty() ? root.filename().string() : name;
        log("warning: " + label + " targets " + version_range(*info) +
            ", installed " + game_version_ + "; installing anyway");
    }
    if (compatible.empty()) {
        if (incompatible.empty()) {
            cleanup_cache();
            return {false, "No readable mod.info in workshop item " + std::to_string(id)};
        }
        roots.swap(incompatible);
    } else {
        roots.swap(compatible);
    }

    struct Moved {
        fs::path source;
        fs::path dest;
        fs::path backup;
    };
    std::vector<Moved> moved;
    auto rollback = [&]() {
        for (auto it = moved.rbegin(); it != moved.rend(); ++it) {
            pz::error_code ec;
            if (fs::exists(it->dest)) {
                move_tree(it->dest, it->source);
                if (!it->backup.empty()) fs::rename(it->backup, it->dest, ec);
            }
        }
    };

    std::vector<InstalledMod> fresh;
    int kept = 0;
    bool replaced_any = false;
    bool adopted_external = false;
    for (const auto& root : roots) {
        auto info = read_mod_info(root);
        if (!info) continue;

        std::string folder = (root == dl.content_dir)
            ? (info->id.empty() ? std::to_string(id) : info->id)
            : root.filename().string();

        fs::path dest = cfg_.user_mods_dir / folder;
        bool tracked = false;
        bool was_owned = false;
        for (const auto& m : db_) {
            if (m.workshop_id == id && m.folder == folder) {
                tracked = true;
                was_owned = m.owned;
                break;
            }
        }

        // A folder that already holds the same mod version is either our own
        // previous copy or a manual install the user wants tracked. Keep its
        // files instead of replacing them with identical content.
        bool same_version = false;
        if (fs::exists(dest)) {
            auto existing = read_mod_info(dest, game_version_);
            same_version = existing && !existing->mod_version.empty() &&
                           !existing->id.empty() && existing->id == info->id &&
                           existing->mod_version == info->mod_version;
        }
        if (same_version) {
            log("kept existing " + folder + " (version " + info->mod_version + ")");
            if (!tracked) adopted_external = true;
            fresh.push_back({id, folder, info->name.empty() ? folder : info->name,
                             remote_time, was_owned});
            ++kept;
            continue;
        }

        fs::path backup;
        if (fs::exists(dest)) {   // never silently destroy a manual/local mod
            backup = cfg_.data_dir / "backups" / (folder + "-" + stamp());
            pz::error_code ec;
            fs::create_directories(backup.parent_path());
            fs::rename(dest, backup, ec);
            if (ec) {
                rollback();
                return {false, "Could not move aside " + folder + ": " + ec.message()};
            }
            log("replacing existing " + folder);
        }

        if (!move_tree(root, dest)) {
            pz::error_code ec;
            if (!backup.empty() && fs::exists(backup)) {
                fs::remove_all(dest, ec);
                fs::rename(backup, dest, ec);
            }
            rollback();
            return {false, "Could not move downloaded mod into " + dest.string()};
        }
        moved.push_back({root, dest, backup});
        replaced_any = true;
        log("installed " + folder + " (moved from download cache)");
        fresh.push_back({id, folder, info->name.empty() ? folder : info->name,
                         remote_time, true});
    }
    if (fresh.empty()) {
        cleanup_cache();
        return {false, "mod.info unreadable in item " + std::to_string(id)};
    }

    // The roots were moved out; drop the per-item download cache.
    cleanup_cache();

    // A Workshop item can change which variants it ships (for example an old
    // and a new version folder with different Mod IDs). Drop folders this
    // manager installed for the item that the current download no longer
    // contains. Folders it only adopted are left alone.
    pz::flat_set<std::string> current;
    for (const auto& m : fresh) current.insert(m.folder);
    for (const auto& m : db_) {
        if (m.workshop_id != id || !m.owned || current.count(m.folder) != 0) continue;
        pz::error_code ec;
        fs::remove_all(cfg_.user_mods_dir / m.folder, ec);
        if (ec)
            log("could not remove previous " + m.folder + ": " + ec.message());
        else
            log("removed previous " + m.folder);
    }

    db_.erase(std::remove_if(db_.begin(), db_.end(),
              [&](const InstalledMod& m) { return m.workshop_id == id; }), db_.end());
    db_.insert(db_.end(), fresh.begin(), fresh.end());
    updates_[id] = false;
    save_db();
    save_update_state();

    // The old copy exists only to roll back a failed move. Delete it after a
    // successful install so a replaced mod never permanently uses twice the
    // disk space. A crash leaves the copy in place for manual recovery.
    for (const auto& m : moved) {
        if (m.backup.empty()) continue;
        pz::error_code ec;
        fs::remove_all(m.backup, ec);
    }
    pz::error_code dir_ec;
    fs::remove(cfg_.data_dir / "backups", dir_ec);  // succeeds only when empty

    // Opt-in: make a freshly installed item active in the main Mods list. An
    // update never re-enables something the user turned off.
    if (enable_new && cfg_.auto_enable) {
        auto enabled = set_mods_enabled(ActivationKind::Default, {}, {id}, true, log);
        if (!enabled.ok) log("could not enable automatically: " + enabled.message);
    }

    if (changed) *changed = replaced_any;
    if (!replaced_any) {
        if (adopted_external)
            return {true, "Tracking existing mod(s) without changing files"};
        return {true, "Already up to date (same mod version)"};
    }
    std::string msg = "Installed " + std::to_string(moved.size()) + " mod(s)";
    if (kept != 0) msg += ", kept " + std::to_string(kept) + " existing";
    return {true, msg};
}

std::vector<MissingRequirement> ModManager::missing_requirements() const {
    pz::flat_set<std::string> installed_ids;
    pz::error_code ec;
    fs::directory_iterator mods(cfg_.user_mods_dir, ec);
    if (!ec) {
        for (const auto& entry : mods) {
            if (!entry.is_directory(ec)) continue;
            auto info = read_mod_info(entry.path(), game_version_);
            if (info && !info->id.empty()) installed_ids.insert(folded(info->id));
        }
    }

    std::vector<MissingRequirement> missing;
    pz::flat_set<std::string> reported;
    pz::flat_set<std::string> seen_folders;
    for (const auto& mod : db_) {
        if (!seen_folders.insert(mod.folder).second) continue;
        auto info = read_mod_info(cfg_.user_mods_dir / mod.folder, game_version_);
        if (!info) continue;
        const std::string self = folded(info->id);
        const std::string owner = info->name.empty() ? mod.folder : info->name;
        for (const auto& required : info->required_ids) {
            if (required.empty()) continue;
            const std::string key = folded(mod.folder) + "\n" + folded(required);
            if (!reported.insert(key).second) continue;
            if (!self.empty() && folded(required) == self) continue;
            if (installed_ids.count(folded(required)) != 0) continue;
            missing.push_back({owner, mod.folder, required});
        }
    }
    return missing;
}

bool ModManager::is_installed(std::uint64_t id) const {
    return std::any_of(db_.begin(), db_.end(),
        [id](const InstalledMod& m) { return m.workshop_id == id; });
}

std::vector<std::uint64_t> ModManager::unique_installed_ids() const {
    pz::flat_set<std::uint64_t> seen;
    std::vector<std::uint64_t> ids;
    for (const auto& m : db_)
        if (seen.insert(m.workshop_id).second) ids.push_back(m.workshop_id);
    return ids;
}

std::uint64_t ModManager::recorded_time(std::uint64_t id) const {
    std::uint64_t t = 0;
    for (const auto& m : db_)
        if (m.workshop_id == id) t = std::max(t, m.time_updated);
    return t;
}

std::vector<ActivationTarget> ModManager::activation_targets() const {
    return discover_activation_targets(cfg_.zomboid_dir, game_version_);
}

std::vector<InstalledMod> ModManager::list_all() const {
    std::vector<InstalledMod> mods = db_;
    pz::flat_set<std::string> known;
    for (const auto& mod : db_) known.insert(mod.folder);

    pz::error_code ec;
    fs::directory_iterator entries(cfg_.user_mods_dir, ec);
    if (ec) return mods;
    for (const auto& entry : entries) {
        if (!entry.is_directory(ec)) continue;
        const std::string folder = entry.path().filename().string();
        if (folder.empty() || folder.front() == '.' || known.count(folder) != 0) continue;
        auto info = read_mod_info(entry.path(), game_version_);
        if (!info) continue;
        mods.push_back({0, folder, info->name.empty() ? folder : info->name, 0, false});
    }
    return mods;
}

std::string ModManager::preferred_mod_id_for(std::uint64_t workshop_id) const {
    struct Choice {
        std::string id;
        int compatible = 0;
        long long version_min = 0;
        long long folder_version = 0;
        long long id_version = 0;
        std::string folder;
    };
    std::vector<Choice> choices;
    for (const auto& mod : db_) {
        if (mod.workshop_id != workshop_id) continue;
        auto info = read_mod_info(cfg_.user_mods_dir / mod.folder, game_version_);
        if (!info || info->id.empty()) continue;
        Choice choice;
        choice.id = info->id;
        choice.compatible = supports_game_version(*info, game_version_) ? 1 : 0;
        choice.version_min = numeric_version(info->version_min);
        choice.folder_version = trailing_number(mod.folder);
        choice.id_version = trailing_number(info->id);
        choice.folder = mod.folder;
        choices.push_back(std::move(choice));
    }
    if (choices.empty()) return {};
    const auto& best = *std::max_element(choices.begin(), choices.end(),
        [](const Choice& a, const Choice& b) {
            if (a.compatible != b.compatible) return a.compatible < b.compatible;
            if (a.version_min != b.version_min) return a.version_min < b.version_min;
            if (a.folder_version != b.folder_version)
                return a.folder_version < b.folder_version;
            if (a.id_version != b.id_version) return a.id_version < b.id_version;
            return a.folder < b.folder;
        });
    return best.id;
}

std::vector<std::string> ModManager::mod_ids_for(std::uint64_t workshop_id) const {
    std::vector<std::string> ids;
    for (const auto& m : db_) {
        if (m.workshop_id != workshop_id) continue;
        auto info = read_mod_info(cfg_.user_mods_dir / m.folder, game_version_);
        if (!info || info->id.empty()) continue;
        if (std::find(ids.begin(), ids.end(), info->id) == ids.end())
            ids.push_back(info->id);
    }
    return ids;
}

std::vector<std::string> ModManager::unmapped_mod_ids(const ActivationTarget& target) const {
    std::vector<std::string> missing;
    for (const auto& mod_id : target.mod_ids) {
        bool found = false;
        for (const auto& mod : db_) {
            auto info = read_mod_info(cfg_.user_mods_dir / mod.folder, game_version_);
            if (info && info->id == mod_id) {
                found = true;
                break;
            }
        }
        if (!found) add_unique(missing, mod_id);
    }
    return missing;
}

std::vector<std::string> ModManager::enabled_mod_names(const ActivationTarget& target) const {
    std::vector<std::string> names;
    for (const auto& mod_id : target.mod_ids) {
        std::string name = mod_id;
        bool named = false;
        for (const auto& mod : db_) {
            auto info = read_mod_info(cfg_.user_mods_dir / mod.folder, game_version_);
            if (!info || info->id != mod_id) continue;
            if (!info->name.empty()) {
                name = info->name;
                named = true;
            }
            break;
        }
        if (named) name += " [" + mod_id + "]";
        add_unique(names, name);
    }
    return names;
}

std::vector<std::uint64_t> ModManager::workshop_ids_for(const ActivationTarget& target) const {
    std::vector<std::uint64_t> ids;
    for (const auto& m : db_) {
        auto info = read_mod_info(cfg_.user_mods_dir / m.folder, game_version_);
        if (!info || info->id.empty()) continue;
        if (std::find(target.mod_ids.begin(), target.mod_ids.end(), info->id) ==
            target.mod_ids.end()) continue;
        add_unique(ids, m.workshop_id);
    }
    // A server's WorkshopItems= can contain more than the enabled Mods= list,
    // so prefer the enabled IDs mapped through installed.tsv. When none of the
    // enabled Mod IDs is installed here, the raw WorkshopItems= list is the
    // only information available and is still useful for sharing.
    if (target.kind == ActivationKind::Server && ids.empty()) return target.workshop_ids;
    return ids;
}

Result ModManager::set_mods_enabled(ActivationKind kind, const std::string& name,
                                    const std::vector<std::uint64_t>& workshop_ids,
                                    bool enabled, const LogFn& log) {
    return set_selection_enabled(kind, name, workshop_ids, {}, enabled, log);
}

Result ModManager::set_selection_enabled(ActivationKind kind, const std::string& name,
                                         const std::vector<std::uint64_t>& workshop_ids,
                                         const std::vector<std::string>& folders,
                                         bool enabled, const LogFn& log) {
    auto targets = activation_targets();
    auto target = std::find_if(targets.begin(), targets.end(),
        [&](const ActivationTarget& t) {
            if (t.kind != kind) return false;
            return kind == ActivationKind::Default || t.name == name;
        });
    if (target == targets.end())
        return {false, "Unknown activation target: " + name};

    std::vector<std::string> add_ids;
    std::vector<std::string> remove_ids;
    std::vector<std::uint64_t> workshops;
    for (auto id : workshop_ids) {
        add_unique(workshops, id);
        const auto known = mod_ids_for(id);
        if (enabled) {
            // Several folders are usually game-version variants. Enable only
            // the newest compatible one and remove its siblings from the list.
            const std::string preferred = preferred_mod_id_for(id);
            add_unique(add_ids, preferred);
            for (const auto& mod_id : known)
                if (mod_id != preferred) add_unique(remove_ids, mod_id);
        } else {
            for (const auto& mod_id : known) add_unique(remove_ids, mod_id);
        }
    }
    for (const auto& folder : folders) {
        auto info = read_mod_info(cfg_.user_mods_dir / folder, game_version_);
        if (!info || info->id.empty()) continue;
        add_unique(enabled ? add_ids : remove_ids, info->id);
    }

    if (enabled) {
        // Enabling a mod must also enable the requirements it declares. Walk
        // installed mod.info files transitively; requirements that are missing
        // stay warnings (the dependency notice already reports them) and never
        // make the enable fail.
        struct InstalledInfo {
            std::string id;
            std::vector<std::string> requires;
        };
        std::vector<InstalledInfo> installed;
        for (const auto& mod : list_all()) {
            auto info = read_mod_info(cfg_.user_mods_dir / mod.folder, game_version_);
            if (info && !info->id.empty())
                installed.push_back({info->id, info->required_ids});
        }
        auto find_installed = [&](const std::string& id) -> const InstalledInfo* {
            const std::string key = folded(id);
            for (const auto& entry : installed)
                if (folded(entry.id) == key) return &entry;
            return nullptr;
        };

        std::vector<std::string> queue = add_ids;
        pz::flat_set<std::string> visited;
        for (const auto& id : queue) visited.insert(folded(id));
        pz::flat_set<std::string> warned;
        for (std::size_t i = 0; i < queue.size(); ++i) {
            const std::string owner = queue[i];
            const InstalledInfo* entry = find_installed(owner);
            if (!entry) continue;
            for (const auto& required : entry->requires) {
                if (required.empty()) continue;
                const std::string key = folded(required);
                const InstalledInfo* requirement = find_installed(required);
                if (!requirement) {
                    if (warned.insert(key).second)
                        log("required mod is not installed: " + required +
                            " (needed by " + owner + ")");
                    continue;
                }
                if (!visited.insert(key).second) continue;
                if (std::find(add_ids.begin(), add_ids.end(), requirement->id) ==
                    add_ids.end()) {
                    add_ids.push_back(requirement->id);
                    log("auto-enabled required mod " + requirement->id +
                        " (needed by " + owner + ")");
                }
                queue.push_back(requirement->id);
            }
        }

        remove_ids.erase(std::remove_if(remove_ids.begin(), remove_ids.end(),
            [&](const std::string& id) {
                return std::find(add_ids.begin(), add_ids.end(), id) != add_ids.end();
            }), remove_ids.end());
        if (add_ids.empty()) {
            std::string detail = workshop_ids.empty() && !folders.empty()
                ? "the selected folder has no Mod ID"
                : "no installed Mod ID was found for the selection";
            return {false, "Could not enable: " + detail};
        }
    } else if (remove_ids.empty()) {
        return {false, "Could not disable: no Mod ID was found for the selection"};
    }

    const auto& write_ids = enabled ? add_ids : remove_ids;
    auto write_to = [&](ActivationTarget& destination) {
        if (!remove_ids.empty() &&
            !write_activation_target(destination, remove_ids, {}, false))
            return false;
        return write_activation_target(destination, write_ids, workshops, enabled);
    };

    std::string where = target->label;
    if (enabled && target->kind == ActivationKind::Save) {
        // A save's mod list is additive to the main list. Writing Default too
        // means the mod is available outside that one save as well.
        auto defaults = std::find_if(targets.begin(), targets.end(),
            [](const ActivationTarget& t) {
                return t.kind == ActivationKind::Default;
            });
        if (defaults == targets.end())
            return {false, "Could not find mods/default.txt"};
        if (!write_to(*defaults))
            return {false, "Could not write " + defaults->path.string()};
        where += " + Default";
        log("enabled in Default because save targets also use mods/default.txt");
    }

    if (!write_to(*target))
        return {false, "Could not write " + target->path.string()};

    return {true, std::string(enabled ? "Enabled " : "Disabled ") +
                      std::to_string(write_ids.size()) + " Mod ID(s) in " +
                      where};
}

std::uint64_t ModManager::read_last_check() const {
    std::uint64_t value = 0;
    for_each_line(last_check_path(), [&](const std::string& line) {
        if (value == 0) value = std::strtoull(line.c_str(), nullptr, 10);
    });
    return value;
}

void ModManager::record_update_check() const {
    write_text_atomic(last_check_path(), std::to_string(now_seconds()) + "\n");
}

std::uint64_t ModManager::read_last_app_check() const {
    std::uint64_t value = 0;
    for_each_line(last_app_check_path(), [&](const std::string& line) {
        if (value == 0) value = std::strtoull(line.c_str(), nullptr, 10);
    });
    return value;
}

void ModManager::record_app_update_check() const {
    write_text_atomic(last_app_check_path(), std::to_string(now_seconds()) + "\n");
}

void ModManager::load_app_update_state() {
    std::string latest;
    for_each_line(app_update_path(), [&](const std::string& line) {
        if (!latest.empty()) return;
        latest = split_first_word(line).first;
    });
    if (!latest.empty()) app_update_available_ = app_version_newer(latest, app_version());
}

void ModManager::load_db() {
    for_each_line(db_path(), [&](const std::string& line) {
        const auto parts = split_fields(line, '\t');
        if (parts.size() < 4) return;
        std::uint64_t workshop_id = 0, time_updated = 0;
        if (!parse_stored_number(parts[0], workshop_id) ||
            !parse_stored_number(parts[3], time_updated))
            return;   // skip corrupt line
        const bool is_owned = !(parts.size() >= 5 && parts[4] == "0");
        db_.push_back({workshop_id, parts[1], parts[2], time_updated, is_owned});
    });
}

void ModManager::load_update_state() {
    updates_.clear();
    for_each_line(updates_path(), [&](const std::string& line) {
        const auto parts = split_fields(line, '\t');
        if (parts.empty()) return;
        std::uint64_t workshop_id = 0;
        if (!parse_stored_number(parts[0], workshop_id)) return;
        if (parts.size() >= 2 && parts[1] == "0") return;
        if (is_installed(workshop_id)) updates_[workshop_id] = true;
    });
}

void ModManager::save_db() const {
    std::string text;
    for (const auto& m : db_) {
        text += std::to_string(m.workshop_id);
        text += '\t';
        text += m.folder;
        text += '\t';
        text += m.name;
        text += '\t';
        text += std::to_string(m.time_updated);
        text += '\t';
        text += m.owned ? '1' : '0';
        text += '\n';
    }
    write_text_atomic(db_path(), text);
}

void ModManager::save_update_state() const {
    std::string text;
    for (const auto& entry : updates_) {
        if (!entry.second) continue;
        text += std::to_string(entry.first);
        text += "\t1\n";
    }
    write_text_atomic(updates_path(), text);
}
}  // namespace pz
