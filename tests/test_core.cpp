// Dependency-free tests for pzcore. Run via: ctest --test-dir build --output-on-failure
#include "core/commands.hpp"
#include "core/mod_info.hpp"
#include "core/mod_manager.hpp"
#include "core/paths.hpp"
#include "core/steamcmd.hpp"
#include "core/updater.hpp"
#include "core/url_c.h"
#include "ui/ui.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

void write_file(const pz::fs::path& p, const std::string& content) {
    pz::fs::create_directories(p.parent_path());
    std::FILE* out = std::fopen(p.c_str(), "wb");
    if (!out) return;
    std::fwrite(content.data(), 1, content.size(), out);
    std::fclose(out);
}

std::string read_file(const pz::fs::path& p) {
    std::FILE* in = std::fopen(p.c_str(), "rb");
    if (!in) return {};
    std::string text;
    char buffer[4096];
    std::size_t got = 0;
    while ((got = std::fread(buffer, 1, sizeof buffer, in)) != 0)
        text.append(buffer, got);
    std::fclose(in);
    return text;
}

pz::fs::path make_temp_root() {
    auto stamp = static_cast<unsigned long long>(std::time(nullptr));
    auto root = pz::fs::temp_directory_path() / ("pzmodmgr-test-" + std::to_string(stamp));
    pz::fs::create_directories(root);
    return root;
}

void make_executable(const pz::fs::path& p) {
    pz::error_code ec;
    pz::fs::permissions(p,
        pz::fs::perms::owner_read | pz::fs::perms::owner_write | pz::fs::perms::owner_exec |
        pz::fs::perms::group_read | pz::fs::perms::group_exec |
        pz::fs::perms::others_read | pz::fs::perms::others_exec, ec);
    check(!ec, "make executable: " + p.string());
}

bool parse_one_workshop_id(const std::string& input, std::uint64_t& id) {
    return pz_url_parse_workshop_id(input.data(), input.size(), &id) != 0;
}

std::vector<std::uint64_t> parse_workshop_ids(const std::string& input) {
    std::vector<std::uint64_t> result;
    pz_url_ids parsed{};
    if (!pz_url_parse_workshop_ids(input.data(), input.size(), &parsed)) {
        pz_url_ids_free(&parsed);
        return result;
    }
    if (parsed.count != 0) result.assign(parsed.items, parsed.items + parsed.count);
    pz_url_ids_free(&parsed);
    return result;
}

void test_parse_workshop_id() {
    std::uint64_t id = 0;
    check(parse_one_workshop_id("123456789", id) && id == 123456789,
          "plain numeric id");
    check(parse_one_workshop_id("  42  ", id) && id == 42,
          "numeric id with whitespace");

    const char* url = "https://steamcommunity.com/sharedfiles/filedetails/?id=123456789&searchtext=x";
    check(parse_one_workshop_id(url, id) && id == 123456789, "workshop url");
    const char* workshop_url =
        "https://steamcommunity.com/workshop/filedetails/?id=3171167894";
    check(parse_one_workshop_id(workshop_url, id) && id == 3171167894,
          "workshop/filedetails url");

    check(!parse_one_workshop_id("", id), "empty input rejected");
    check(!parse_one_workshop_id("no id here", id), "text without id rejected");
    check(!parse_one_workshop_id("id=abc", id), "non-numeric id rejected");
    check(!parse_one_workshop_id("123456789012345678901", id), "overlong id rejected");

    auto multi = parse_workshop_ids(
        "123\t456\nhttps://steamcommunity.com/sharedfiles/filedetails/?id=789 123");
    check(multi.size() == 3 && multi[0] == 123 && multi[1] == 456 && multi[2] == 789,
          "multiple IDs parsed in order and deduplicated");
    check(parse_workshop_ids("   ").empty(), "empty batch has no IDs");
    check(parse_workshop_ids("123 not-an-id").empty(),
          "a bad token rejects the whole batch");
}

void test_steamcmd_detection(const pz::fs::path& root) {
    auto fake = root / "detected-steamcmd";
    write_file(fake, "#!/bin/sh\nexit 0\n");
    make_executable(fake);

    setenv("PZMODMGR_STEAMCMD", fake.c_str(), 1);
    check(pz::find_steamcmd() == fake, "steamcmd auto-detection uses PZMODMGR_STEAMCMD");
    unsetenv("PZMODMGR_STEAMCMD");

    auto cfg = pz::default_config();
    check(cfg.steamcmd_exe == pz::builtin_steamcmd_path(cfg),
          "default steamcmd is the built-in copy");
}

void test_builtin_steamcmd(const pz::fs::path& root) {
    pz::Config cfg;
    cfg.data_dir = root / "builtin-data";
    cfg.steamcmd_exe = pz::builtin_steamcmd_path(cfg);
    write_file(cfg.steamcmd_exe, "#!/bin/sh\nexit 0\n");
    make_executable(cfg.steamcmd_exe);

    auto log = [](const std::string&) {};
    check(pz::ensure_steamcmd(cfg, log) == pz::builtin_steamcmd_path(cfg),
          "built-in steamcmd is used when present");

    cfg.steamcmd_exe.clear();
    check(pz::ensure_steamcmd(cfg, log) == pz::builtin_steamcmd_path(cfg),
          "blank steamcmd setting resolves to the built-in copy");
}

void test_config_paths(const pz::fs::path& root) {
    pz::Config cfg;
    cfg.zomboid_dir = root / "config-default" / "Zomboid";
    cfg.user_mods_dir = cfg.zomboid_dir / "mods";
    cfg.data_dir = cfg.zomboid_dir / "pzmodmgr";
    cfg.config_file = cfg.data_dir / "config.ini";
    cfg.steamcmd_exe = pz::builtin_steamcmd_path(cfg);
    cfg.steam_user = "config-user";

    pz::ModManager mm(cfg);
    auto log = [](const std::string&) {};
    const auto custom = root / "custom-zomboid";
    auto saved = pz::run_command(mm, "config zomboid_dir " + custom.string(), log);
    check(saved.ok, "config zomboid_dir succeeds: " + saved.message);
    check(mm.config().zomboid_dir == custom, "custom Zomboid dir applied");
    check(mm.config().user_mods_dir == custom / "mods", "custom mods dir applied");
    check(mm.config().data_dir == custom / "pzmodmgr", "custom data dir applied");
    check(pz::fs::exists(custom / "mods") && pz::fs::exists(custom / "pzmodmgr"),
          "custom Zomboid directories created");
    check(mm.config().steamcmd_exe == pz::builtin_steamcmd_path(mm.config()),
          "built-in steamcmd follows the custom Zomboid dir");
    check(mm.config().config_file == cfg.config_file,
          "config file stays at its bootstrap path");
    check(read_file(cfg.config_file).find("zomboid_dir=" + custom.string()) !=
              std::string::npos,
          "config command persists the custom Zomboid dir");

    pz::Config loaded;
    loaded.config_file = cfg.config_file;
    loaded.zomboid_dir = cfg.zomboid_dir;
    loaded.user_mods_dir = cfg.user_mods_dir;
    loaded.data_dir = cfg.data_dir;
    loaded.steamcmd_exe = cfg.steamcmd_exe;
    pz::load_config(loaded);
    check(loaded.zomboid_dir == custom, "custom Zomboid dir loaded on restart");
    check(loaded.user_mods_dir == custom / "mods", "custom mods dir loaded on restart");
    check(loaded.data_dir == custom / "pzmodmgr", "custom data dir loaded on restart");
    check(loaded.config_file == cfg.config_file, "bootstrap config path survives load");
    check(loaded.steam_user == cfg.steam_user, "steam user loads with custom path");

    const auto before = mm.config().zomboid_dir;
    auto relative = mm.set_zomboid_dir("relative/Zomboid");
    check(!relative.ok, "relative Zomboid dir rejected");
    check(mm.config().zomboid_dir == before, "rejected relative path changes nothing");
}

void test_atomic_write(const pz::fs::path& root) {
    const pz::fs::path file = root / "atomic" / "config.ini";
    check(pz::write_text_atomic(file, "first\n"), "atomic write creates a file");
    check(read_file(file) == "first\n", "atomic write stores the first value");
    check(pz::write_text_atomic(file, "second\n"), "atomic write replaces a file");
    check(read_file(file) == "second\n", "atomic write stores the replacement");

    bool leftover = false;
    pz::error_code ec;
    for (const auto& entry : pz::fs::directory_iterator(file.parent_path(), ec)) {
        if (entry.path().filename().string().find(".tmp.") != std::string::npos)
            leftover = true;
    }
    check(!ec && !leftover, "atomic write leaves no temporary file behind");
}

void test_cache_cleanup(const pz::fs::path& root) {
    pz::Config cfg;
    cfg.zomboid_dir = root / "cache-clean" / "Zomboid";
    cfg.user_mods_dir = cfg.zomboid_dir / "mods";
    cfg.data_dir = cfg.zomboid_dir / "pzmodmgr";
    cfg.steamcmd_exe = pz::builtin_steamcmd_path(cfg);

    const auto cache = cfg.data_dir / "steam_cache";
    const auto backups = cfg.data_dir / "backups";
    write_file(cache / "stale-download" / "payload", "old");
    write_file(cache / "recent-download" / "payload", "new");
    write_file(backups / "old-backup" / "payload", "old");
    write_file(backups / "recent-backup" / "payload", "new");

    const auto stale = pz::fs::file_time_now() - 24 * 8 * 60 * 60;
    pz::fs::last_write_time(cache / "stale-download", stale);
    pz::fs::last_write_time(backups / "old-backup", stale);

    pz::ModManager mm(cfg);
    check(!pz::fs::exists(cache / "stale-download"),
          "stale download cache is pruned on startup");
    check(pz::fs::exists(cache / "recent-download"),
          "recent download cache is kept");
    check(!pz::fs::exists(backups / "old-backup"),
          "old manager backup is pruned on startup");
    check(pz::fs::exists(backups / "recent-backup"),
          "recent manager backup is kept");
}

void test_update_cache_cleanup(const pz::fs::path& root) {
    pz::Config cfg;
    cfg.zomboid_dir = root / "update-clean" / "Zomboid";
    cfg.user_mods_dir = cfg.zomboid_dir / "mods";
    cfg.data_dir = cfg.zomboid_dir / "pzmodmgr";

    const auto updates = cfg.data_dir / "updates";
    write_file(updates / "0.3.0" / "pending.txt", "pid=0\nversion=0.3.0\n");
    write_file(updates / "0.4.0" / "pending.txt",
               "pid=" + std::to_string(pz::current_process_id()) +
                   "\nversion=0.4.0\n");
    write_file(updates / "recent" / "package.zip", "new");
    write_file(updates / "old" / "package.zip", "old");
    const auto helper_dir = cfg.data_dir / "update_helper";
    write_file(helper_dir / "pzmodmgr", "old helper");
    const auto stale = pz::fs::file_time_now() - 24 * 8 * 60 * 60;
    pz::fs::last_write_time(updates / "0.4.0", stale);
    pz::fs::last_write_time(updates / "old", stale);

    pz::cleanup_update_cache(cfg);
    check(!pz::fs::exists(updates / "0.3.0"),
          "abandoned staged update is pruned");
    check(pz::fs::exists(updates / "0.4.0"),
          "staged update with a live helper is kept");
    check(!pz::fs::exists(updates / "recent"),
          "completed update staging folder is pruned immediately");
    check(!pz::fs::exists(updates / "old"),
          "old update staging folder is pruned");
    check(!pz::fs::exists(helper_dir),
          "leftover update helper is pruned");
}

void test_external_reload(const pz::fs::path& root) {
    pz::Config cfg;
    cfg.zomboid_dir = root / "reload" / "Zomboid";
    cfg.user_mods_dir = cfg.zomboid_dir / "mods";
    cfg.data_dir = cfg.zomboid_dir / "pzmodmgr";
    cfg.steamcmd_exe = pz::builtin_steamcmd_path(cfg);

    pz::ModManager mm(cfg);
    check(!mm.reload_installed(), "reload without a change reports no change");

    write_file(cfg.data_dir / "installed.tsv",
               "424242\tExternalMod\tExternal Mod\t1700000000\n");
    check(mm.reload_installed(), "external installed.tsv change is detected");
    auto mods = mm.list();
    check(mods.size() == 1 && mods[0].workshop_id == 424242 &&
              mods[0].folder == "ExternalMod",
          "external mod appears after reload");
    check(!mm.reload_installed(), "unchanged reload reports no change");
}

void test_native_fs_copy(const pz::fs::path& root) {
    const pz::fs::path source = root / "fs_copy" / "source";
    const pz::fs::path dest = root / "fs_copy" / "dest";
    write_file(source / "a.txt", "alpha");
    write_file(source / "nested" / "b.txt", "beta");
    pz::fs::create_directories(dest);

    pz::error_code ec;
    check(pz::fs::copy_file(source / "a.txt", dest / "a.txt", ec),
          "copy_file creates the destination");
    check(!ec && read_file(dest / "a.txt") == "alpha", "copy_file contents");
    check(!pz::fs::copy_file(source / "a.txt", dest / "a.txt", ec) && ec,
          "copy_file refuses to overwrite by default");
    check(pz::fs::copy_file(source / "a.txt", dest / "a.txt",
                            pz::fs::copy_options::overwrite_existing, ec),
          "copy_file overwrites when requested");

    check(pz::fs::copy(source, dest / "tree",
                       pz::fs::copy_options::recursive |
                           pz::fs::copy_options::overwrite_existing,
                       ec),
          "recursive copy");
    check(!ec && read_file(dest / "tree" / "nested" / "b.txt") == "beta",
          "recursive copy contents");
    check(pz::fs::remove_all(dest / "tree", ec) > 0 && !pz::fs::exists(dest / "tree"),
          "recursive remove");
}

void test_corrupt_db(const pz::fs::path& root) {
    pz::Config cfg;
    cfg.zomboid_dir = root / "corrupt_db" / "Zomboid";
    cfg.user_mods_dir = cfg.zomboid_dir / "mods";
    cfg.data_dir = cfg.zomboid_dir / "pzmodmgr";
    cfg.steamcmd_exe = pz::builtin_steamcmd_path(cfg);

    write_file(cfg.data_dir / "installed.tsv",
               "123\tGood\tGood Mod\t1700000000\n"
               "oops\tBad\tBad Mod\t1700000000\n"
               "124\tBad\tBad Mod\tnope\n"
               "125\tShort\tShort Mod\n"
               "126\tAdopted\tAdopted Mod\t42\t0\n"
               "127\tOverflow\tOverflow\t999999999999999999999999999\n");

    pz::ModManager mm(cfg);
    const auto mods = mm.list();
    check(mods.size() == 2, "corrupt installed.tsv lines are skipped");
    check(mods.size() == 2 && mods[0].workshop_id == 123 &&
              mods[0].time_updated == 1700000000 && mods[0].owned,
          "valid owned record survives corrupt lines");
    check(mods.size() == 2 && mods[1].workshop_id == 126 &&
              mods[1].time_updated == 42 && !mods[1].owned,
          "adopted record and owned flag survive corrupt lines");
}

void test_version_locks(const pz::fs::path& root) {
    pz::Config cfg;
    cfg.zomboid_dir = root / "locks" / "Zomboid";
    cfg.user_mods_dir = cfg.zomboid_dir / "mods";
    cfg.data_dir = cfg.zomboid_dir / "pzmodmgr";
    cfg.steamcmd_exe = pz::builtin_steamcmd_path(cfg);
    write_file(cfg.user_mods_dir / "FixtureLock" / "mod.info",
               "id=FixtureLock\nname=Fixture Lock\nmodversion=1.0\n");
    // The old five-column format must load with no lock.
    write_file(cfg.data_dir / "installed.tsv",
               "777\tFixtureLock\tFixture Lock\t1700000000\t1\n");

    pz::ModManager mm(cfg);
    check(!mm.locked(777), "old installed.tsv rows load unlocked");
    auto logfn = [](const std::string&) {};
    auto lock = mm.set_locked({777}, true, logfn);
    check(lock.ok && mm.locked(777) && mm.manually_locked(777),
          "manual lock applies and persists to the row");
    check(read_file(cfg.data_dir / "installed.tsv").find("\t1\n") != std::string::npos,
          "manual lock saved as the sixth installed.tsv column");
    {
        pz::ModManager restarted(cfg);
        check(restarted.manually_locked(777), "manual lock survives restart");
    }

    auto unlocked = pz::run_command(mm, "unlock 777", logfn);
    check(unlocked.ok && !mm.locked(777), "unlock by Workshop ID clears the lock");
    auto by_name = pz::run_command(mm, "lock Fixture Lock", logfn);
    check(by_name.ok && mm.manually_locked(777), "lock accepts an installed mod name");
    check(pz::run_command(mm, "unlock 777", logfn).ok, "name-lock fixture unlocked");

    // A server target is not locked unless its per-target switch is on.
    write_file(cfg.zomboid_dir / "Server" / "local.ini",
               "Mods=\\FixtureLock\nWorkshopItems=777\n");
    check(!mm.server_locked(777) && !mm.locked(777),
          "server-enabled items are unlocked by default");
    const auto targets = mm.activation_targets();
    auto server = std::find_if(targets.begin(), targets.end(), [](const pz::ActivationTarget& t) {
        return t.kind == pz::ActivationKind::Server && t.name == "local";
    });
    check(server != targets.end(), "server target discovered");
    if (server != targets.end()) {
        auto target_lock = mm.set_target_locked(server->kind, server->name, true, logfn);
        check(target_lock.ok && mm.server_locked(777) && mm.locked(777),
              "turning on a target lock pins its enabled items");
        check(mm.lock_reason(777) == "target-enabled",
              "target lock reason is reported");

        write_file(cfg.data_dir / "updates.tsv", "777\t1\n");
        {
            pz::Config restarted_cfg = cfg;
            pz::load_config(restarted_cfg);
            pz::ModManager restarted(restarted_cfg);
            check(restarted.locked(777), "target lock survives restart");
            check(read_file(cfg.data_dir / "updates.tsv").find("777") ==
                      std::string::npos,
                  "locked items are removed from updates.tsv on load");
        }

        auto skipped = mm.update_one(777, logfn);
        check(skipped.ok &&
                  skipped.message.find("Locked (target-enabled)") != std::string::npos,
              "update_one skips a target-locked item before checking Steam");
        auto all = mm.update_all(logfn);
        check(all.ok && all.message.find("skipped 1 locked") != std::string::npos,
              "update_all reports target-locked items as skipped");

        // Removing the manual flag does not override the target switch.
        auto still_target = pz::run_command(mm, "unlock 777", logfn);
        check(still_target.ok && mm.locked(777) && !mm.manually_locked(777),
              "unlock leaves a target-locked item locked");

        auto target_unlock = mm.set_target_locked(server->kind, server->name, false, logfn);
        check(target_unlock.ok && !mm.locked(777),
              "turning off the target lock releases its items");
    }
    auto cli_lock = pz::run_command(mm, "target-lock 1 on", logfn);
    check(cli_lock.ok && mm.locked(777), "target-lock command enables a target lock");
    auto cli_unlock = pz::run_command(mm, "target-lock 1 off", logfn);
    check(cli_unlock.ok && !mm.locked(777), "target-lock command disables a target lock");
    pz::error_code ec;
    pz::fs::remove(cfg.zomboid_dir / "Server" / "local.ini", ec);
    check(!ec && !mm.locked(777), "removing the server list leaves items unlocked");

    // A shared ID list can be pasted into lock on another instance.
    auto shared = pz::run_command(mm, "lock 777", logfn);
    check(shared.ok && mm.manually_locked(777),
          "shared Workshop IDs can be locked directly");
}

void test_read_mod_info(const pz::fs::path& root) {
    auto dir = root / "read_mod_info";
    write_file(dir / "mod.info",
               "name=My Mod\r\nid=mymod\r\nauthor=Alice\r\nmodversion=1.2.3\r\n");

    auto info = pz::read_mod_info(dir);
    check(info.has_value(), "mod.info readable");
    if (info) {
        check(info->id == "mymod", "mod.info id parsed");
        check(info->name == "My Mod", "mod.info name parsed (CRLF stripped)");
        check(info->author == "Alice", "mod.info author parsed");
        check(info->mod_version == "1.2.3", "mod.info modversion parsed");
    }

    check(!pz::read_mod_info(root / "missing").has_value(), "missing mod.info returns nullopt");

    auto with_icon = root / "read_mod_info_icon";
    write_file(with_icon / "mod.info", "name=Icon Mod\nicon=art/custom.png\n");
    write_file(with_icon / "art" / "custom.png", "not actually decoded here");
    info = pz::read_mod_info(with_icon);
    check(info.has_value() && info->icon == with_icon / "art" / "custom.png",
          "mod.info icon path resolved");

    auto b42_icon = root / "read_mod_info_b42_icon";
    write_file(b42_icon / "42" / "mod.info", "name=B42 Icon\nicon=icon_file.png\n");
    write_file(b42_icon / "common" / "icon_file.png", "not actually decoded here");
    info = pz::read_mod_info(b42_icon);
    check(info.has_value() && info->icon == b42_icon / "common" / "icon_file.png",
          "B42 common icon path resolved");

    auto preview_fallback = root / "read_mod_info_preview";
    write_file(preview_fallback / "mod.info",
               "name=Preview Fallback\nid=previewfallback\n"
               "require=\\damnlib, \\DamnLibGlass\n");
    write_file(preview_fallback / "preview.png", "not actually decoded here");
    info = pz::read_mod_info(preview_fallback);
    check(info.has_value() && info->icon == preview_fallback / "preview.png",
          "preview.png fallback resolved");
    check(info.has_value() && info->required_ids.size() == 2 &&
              info->required_ids[0] == "damnlib" &&
              info->required_ids[1] == "DamnLibGlass",
          "require IDs parsed, trimmed, and de-escaped");

    auto poster_fallback = root / "read_mod_info_poster";
    write_file(poster_fallback / "mod.info", "name=Poster Fallback\nid=posterfallback\n");
    write_file(poster_fallback / "preview.png", "not actually decoded here");
    write_file(poster_fallback / "poster.png", "not actually decoded here");
    info = pz::read_mod_info(poster_fallback);
    check(info.has_value() && info->icon == poster_fallback / "poster.png",
          "poster.png preferred over preview.png");

    auto no_glob_icon = root / "read_mod_info_no_glob_icon";
    write_file(no_glob_icon / "mod.info", "name=No Glob Icon\nid=noglobicon\n");
    write_file(no_glob_icon / "icon_alt.png", "not actually decoded here");
    write_file(no_glob_icon / "preview.png", "not actually decoded here");
    info = pz::read_mod_info(no_glob_icon);
    check(info.has_value() && info->icon == no_glob_icon / "preview.png",
          "icon* files are ignored; preview.png is used instead");

    auto versioned = root / "read_mod_info_versioned";
    write_file(versioned / "41" / "mod.info", "name=Old version\nid=old\n");
    write_file(versioned / "42" / "mod.info", "name=New version\nid=new\n");
    info = pz::read_mod_info(versioned);
    check(info.has_value(), "B42 versioned mod.info readable");
    if (info) {
        check(info->id == "new", "newest B42 version id selected");
        check(info->name == "New version", "newest B42 version name selected");
    }

    auto ranged = root / "read_mod_info_ranged";
    write_file(ranged / "42.20" / "mod.info",
               "name=For 42.20\nid=ranged\nversionMin=42.20\nversionMax=42.21\n");
    write_file(ranged / "42.22" / "mod.info",
               "name=For 42.22\nid=ranged\nversionMin=42.22\n");
    info = pz::read_mod_info(ranged, "42.21.0");
    check(info.has_value() && info->name == "For 42.22",
          "newest same-major version folder selected for the game build");
    if (info) {
        check(pz::supports_game_version(*info, "42.21.0"),
              "a 42.22 mod is allowed on 42.21");
        check(!pz::supports_game_version(*info, "43.0.0"),
              "a 42-only mod is blocked on build 43");
    }

    pz::ModInfo old_build;
    old_build.version_min = "41.78";
    old_build.version_max = "41.78";
    check(!pz::supports_game_version(old_build, "42.21.0"),
          "a 41-only mod is blocked on build 42");

    pz::ModInfo range_info;
    range_info.version_min = "41.78";
    range_info.version_max = "42.1";
    check(pz::version_range(range_info) == "PZ 41.78-42.1", "two-sided version range formatted");
    range_info.version_max.clear();
    check(pz::version_range(range_info) == "PZ 41.78+", "open-ended version range formatted");
    range_info.version_min.clear();
    range_info.version_max = "42.1";
    check(pz::version_range(range_info) == "PZ up to 42.1", "upper-only version range formatted");
    check(pz::version_range(pz::ModInfo{}).empty(), "empty version range stays empty");
}

void test_fixture_mod_info() {
#ifdef PZMODMGR_TEST_FIXTURES
    const pz::fs::path fixtures(PZMODMGR_TEST_FIXTURES);
    const pz::fs::path basic = fixtures / "mod_info" / "basic";
    auto info = pz::read_mod_info(basic);
    check(info.has_value(), "fixture mod.info readable");
    if (info) {
        check(info->id == "fixturemod" && info->name == "Fixture Mod" &&
                  info->author == "Fixture Author" && info->mod_version == "2.0",
              "fixture mod.info fields parsed");
        check(info->required_ids.size() == 2 &&
                  info->required_ids[0] == "damnlib" &&
                  info->required_ids[1] == "damnlibGlass",
              "fixture requirements are trimmed, de-escaped, and deduplicated");
        check(info->icon == basic / "art" / "icon.png", "fixture icon resolved");
    }

    auto single = pz::discover_mod_roots(fixtures / "discover" / "single");
    check(single.size() == 1 && single[0].filename() == "single",
          "fixture single-root discovery");

    auto multi = pz::discover_mod_roots(fixtures / "discover" / "multi");
    bool saw_a = false, saw_b = false;
    for (const auto& root : multi) {
        saw_a = saw_a || root.filename() == "A";
        saw_b = saw_b || root.filename() == "B";
    }
    check(multi.size() == 2 && saw_a && saw_b, "fixture mods/ and Contents/mods/ discovery");

    auto versioned = pz::discover_mod_roots(fixtures / "discover" / "versioned");
    check(versioned.size() == 1 && versioned[0].filename() == "HorseMod",
          "fixture versioned mod root discovery");
#else
    std::printf("SKIP: PZMODMGR_TEST_FIXTURES not defined\n");
#endif
}

void test_missing_requirements(const pz::fs::path& root) {
    pz::Config cfg;
    cfg.zomboid_dir = root / "deps" / "Zomboid";
    cfg.user_mods_dir = cfg.zomboid_dir / "mods";
    cfg.data_dir = cfg.zomboid_dir / "pzmodmgr";
    cfg.steamcmd_exe = pz::builtin_steamcmd_path(cfg);

    write_file(cfg.data_dir / "installed.tsv", "100\tTrailers\tTrailers\t1\t1\n");
    write_file(cfg.user_mods_dir / "Trailers" / "mod.info",
               "name=Trailers!\nid=trailers\n"
               "require=\\damnlib, \\damnlibGlass\n");

    pz::ModManager mm(cfg);
    auto missing = mm.missing_requirements();
    check(missing.size() == 2, "two missing requirements detected");
    check(!missing.empty() && missing[0].mod_name == "Trailers!" &&
              missing[0].required_id == "damnlib",
          "missing requirement keeps owner and Mod ID");

    write_file(cfg.user_mods_dir / "damnlib" / "mod.info",
               "name=Library\nid=DamnLib\n");
    write_file(cfg.user_mods_dir / "damnlibGlass" / "mod.info",
               "name=Glass\nid=damnlibglass\n");
    check(mm.missing_requirements().empty(),
          "present requirements match case-insensitively");
}

void test_discover_mod_roots(const pz::fs::path& root) {
    auto single = root / "single";
    write_file(single / "mod.info", "id=single\nname=Single\n");
    auto roots = pz::discover_mod_roots(single);
    check(roots.size() == 1 && roots[0] == single, "single-root item detected");

    auto multi = root / "multi";
    write_file(multi / "mods" / "A" / "mod.info", "id=a\nname=A\n");
    write_file(multi / "Contents" / "mods" / "B" / "mod.info", "id=b\nname=B\n");
    pz::fs::create_directories(multi / "mods" / "not-a-mod");  // no mod.info
    roots = pz::discover_mod_roots(multi);
    check(roots.size() == 2, "mods/ and Contents/mods/ roots detected");
    bool saw_a = false, saw_b = false;
    for (const auto& r : roots) {
        saw_a = saw_a || r.filename() == "A";
        saw_b = saw_b || r.filename() == "B";
    }
    check(saw_a && saw_b, "both mod folders discovered");

    auto versioned = root / "versioned";
    write_file(versioned / "mods" / "HorseMod" / "42" / "mod.info",
               "id=HorseMod\nname=Horse Mod\n");
    pz::fs::create_directories(versioned / "mods" / "HorseMod" / "common");
    roots = pz::discover_mod_roots(versioned);
    check(roots.size() == 1 && roots[0].filename() == "HorseMod",
          "B42 version folder keeps the mod folder as the install root");

    auto empty = root / "empty";
    pz::fs::create_directories(empty);
    check(pz::discover_mod_roots(empty).empty(), "item without mod.info yields no roots");
}

void test_time_updated_parser() {
    check(pz::parse_published_file_time_updated(
              R"({"response":{"publishedfiledetails":[{"result":1,"time_updated":1700000123}]}})") ==
              1700000123,
          "time_updated parsed");

    check(pz::parse_published_file_time_updated(
              R"({"response":{"publishedfiledetails":[{"result":1,"title":"trick \"time_updated\":999","time_updated" : 42}]}})") ==
              42,
          "time_updated text inside a string is ignored");

    check(pz::parse_published_file_time_updated(
              R"({"response":{"publishedfiledetails":[{"title":"time_updated","time_updated":7}]}})") == 7,
          "string value named time_updated does not stop the scan");

    check(pz::parse_published_file_time_updated(
              R"({"response":{"publishedfiledetails":[{"result":9,"publishedfileid":"1"}]}})") == 0,
          "missing time_updated rejected");
    check(pz::parse_published_file_time_updated(
              R"({"response":{"publishedfiledetails":[{"time_updated":"123"}]}})") == 0,
          "non-numeric time_updated rejected");
    check(pz::parse_published_file_time_updated("not json") == 0, "garbage rejected");
}

void test_ui_helpers() {
    pz::InstalledMod beta;
    beta.workshop_id = 1;
    beta.folder = "Beta";
    beta.name = "Beta";
    pz::InstalledMod alpha;
    alpha.workshop_id = 2;
    alpha.folder = "Alpha";
    alpha.name = "Alpha";
    const std::vector<pz::InstalledMod> mods{beta, alpha};

    auto visible = pz::visible_mods(mods, "alp", false, 0,
        [](std::uint64_t) { return false; });
    check(visible.size() == 1 && visible[0].workshop_id == 2,
          "UI filter matches name and folder case-insensitively");

    visible = pz::visible_mods(mods, "", false, 0,
        [](std::uint64_t) { return false; });
    check(visible.size() == 2 && visible[0].workshop_id == 2,
          "UI list sorts by name");

    pz::InstalledMod newer_z;
    newer_z.workshop_id = 3;
    newer_z.folder = "Zed";
    newer_z.name = "Zed";
    newer_z.time_updated = 20;
    pz::InstalledMod newer_a;
    newer_a.workshop_id = 4;
    newer_a.folder = "Able";
    newer_a.name = "Able";
    newer_a.time_updated = 20;
    visible = pz::visible_mods({beta, newer_z, alpha, newer_a}, "", false, 1,
        [](std::uint64_t) { return false; });
    check(visible.size() == 4 && visible[0].workshop_id == 4 &&
              visible[1].workshop_id == 3 && visible[2].workshop_id == 2 &&
              visible[3].workshop_id == 1,
          "UI list sorts by update time, then name");

    visible = pz::visible_mods(mods, "", true, 1,
        [](std::uint64_t id) { return id == 1; });
    check(visible.size() == 1 && visible[0].workshop_id == 1,
          "UI update filter uses the update callback");

    pz::MissingRequirement missing{"Owner", "Folder", "req"};
    const auto notice = pz::dependency_notice({missing});
    check(notice.messages.size() == 1 && notice.status.find("req") != std::string::npos,
          "UI dependency notice reports missing requirement");

    pz::ModRow mod;
    mod.folders.push_back("Folder");
    pz::ModInfo info;
    info.author = "Author";
    info.mod_version = "1.2";
    info.version_min = "41";
    info.version_max = "42";
    check(pz::mod_meta(mod, &info, "43") == "Folder  |  PZ 41-42",
          "UI metadata keeps folder and incompatible range, not author/version");

    // One Workshop item may ship several folders (an old and a new variant).
    pz::InstalledMod old_variant;
    old_variant.workshop_id = 9;
    old_variant.folder = "OldVariant";
    old_variant.name = "Ladders old";
    pz::InstalledMod new_variant = old_variant;
    new_variant.folder = "NewVariant";
    new_variant.name = "Ladders new";
    new_variant.time_updated = 50;
    visible = pz::visible_mods({old_variant, new_variant}, "", false, 0,
        [](std::uint64_t) { return false; });
    check(visible.size() == 1 && visible[0].folders.size() == 2 &&
              visible[0].time_updated == 50,
          "UI groups folders from one Workshop item into a single row");
    mod.folders.push_back("Other");
    check(pz::mod_meta(mod, nullptr, "43").empty(),
          "UI metadata omits the folder list for a grouped item");
    check(pz::join_workshop_ids({7, 42}) == "7 42",
          "shared Workshop ID list is paste-ready");
}

void test_activation_formats(const pz::fs::path& root) {
    const std::string block =
        "VERSION = 1,\r\n\r\nmods\r\n{\r\n    mod = Alpha,\r\n    mod = \\Beta,\r\n}\r\n\r\nmaps\r\n{\r\n}\r\n";
    auto ids = pz::parse_block_mod_ids(block);
    check(ids.size() == 2 && ids[1] == "Beta",
          "default/save block Mod IDs parsed and legacy prefix stripped");

    bool changed = false;
    const std::string out = pz::rewrite_block_mod_ids(
        block, {"Beta", "Gamma"}, &changed);
    check(changed, "block rewrite reports a change");
    check(out.find("mod = Gamma,") != std::string::npos &&
              out.find("mod = \\Gamma,") == std::string::npos &&
              out.find("mod = Alpha") == std::string::npos,
          "block rewrite adds and removes plain Mod IDs");
    check(out.find("maps") != std::string::npos &&
              out.find("\r\n") != std::string::npos,
          "block rewrite preserves maps and CRLF line endings");
    check(pz::parse_block_mod_ids(out) ==
              (std::vector<std::string>{"Beta", "Gamma"}),
          "rewritten block round-trips");

    const std::string ini =
        "PublicName=X\r\nMods=\\Alpha;\\Beta\r\nWorkshopItems=111;222\r\n"
        "Map=Muldraugh, KY\r\n";
    bool backslash = false;
    auto mods = pz::parse_ini_mod_ids(ini, &backslash);
    check(backslash && mods.size() == 2 && mods[0] == "Alpha",
          "server Mods= parsed and backslash convention detected");
    const std::string ini_out = pz::rewrite_ini_mods(
        ini, {"Beta", "Gamma"}, true, &changed);
    check(ini_out.find("Mods=\\Beta;\\Gamma") != std::string::npos,
          "server Mods= rewritten with the file's convention");
    check(ini_out.find("WorkshopItems=111;222") != std::string::npos &&
              ini_out.find("Map=Muldraugh") != std::string::npos,
          "server rewrite leaves other ini lines alone");
    check(pz::parse_ini_workshop_ids(ini_out) ==
              (std::vector<std::uint64_t>{111, 222}),
          "server WorkshopItems= parsed");

    const pz::fs::path z = root / "ActivationZomboid";
    write_file(z / "mods" / "default.txt",
               "VERSION = 1,\n\nmods\n{\n}\n\nmaps\n{\n}\n");
    write_file(z / "Saves" / "Apocalypse" / "Save1" / "players.db", "");
    write_file(z / "Saves" / "Apocalypse" / "Save1" / "thumb.png", "preview");
    write_file(z / "Server" / "servertest.ini", "Mods=\nWorkshopItems=\n");
    auto targets = pz::discover_activation_targets(z, "42.21.0");
    check(targets.size() == 3, "default, save, and server targets discovered");

    auto find_target = [&](pz::ActivationKind kind, const std::string& name) -> pz::ActivationTarget* {
        for (auto& target : targets)
            if (target.kind == kind && target.name == name) return &target;
        return nullptr;
    };

    auto* defaults = find_target(pz::ActivationKind::Default, "");
    check(defaults != nullptr && pz::write_activation_target(*defaults, {"Foo"}, {}, true),
          "default.txt enable writes");
    check(read_file(defaults->path).find("mod = Foo,") != std::string::npos,
          "default.txt contains a plain enabled Mod ID");

    // A file corrupted by the old backslash writer is repaired on discovery,
    // before the next enable/disable action.
    write_file(defaults->path,
               "VERSION = 1,\n\nmods\n{\n    mod = \\Plain,\n}\n");
    targets = pz::discover_activation_targets(z, "42.21.0");
    defaults = find_target(pz::ActivationKind::Default, "");
    std::string repaired;
    if (defaults) repaired = read_file(defaults->path);
    check(defaults != nullptr && defaults->mod_ids ==
              (std::vector<std::string>{"Plain"}) &&
              repaired.find("mod = Plain,") != std::string::npos &&
              repaired.find("mod = \\Plain,") == std::string::npos,
          "default.txt discovery repairs backslash IDs to plain form");
    check(defaults != nullptr && pz::write_activation_target(*defaults, {"Foo"}, {}, true),
          "default.txt repair enable writes");
    check(read_file(defaults->path).find("mod = Foo,") != std::string::npos,
          "default.txt still accepts a plain Mod ID after repair");

    auto* save = find_target(pz::ActivationKind::Save, "Apocalypse/Save1");
    check(save != nullptr && save->thumbnail ==
              z / "Saves" / "Apocalypse" / "Save1" / "thumb.png",
          "save thumb.png preview discovered");
    check(save != nullptr && pz::write_activation_target(*save, {"Bar"}, {}, true),
          "save mods.txt enable writes");
    check(read_file(save->path).find("mod = Bar,") != std::string::npos,
          "save mods.txt uses plain Mod IDs");

    auto* server = find_target(pz::ActivationKind::Server, "servertest");
    check(server != nullptr && pz::write_activation_target(*server, {"Baz"}, {123}, true),
          "server enable writes Mods= and WorkshopItems=");
    const std::string server_text = read_file(server->path);
    check(server_text.find("Mods=\\Baz") != std::string::npos,
          "B42 server gets the backslash form: " + server_text);
    check(server_text.find("WorkshopItems=123") != std::string::npos,
          "server WorkshopItems= receives the Workshop ID");
    check(pz::write_activation_target(*server, {"Baz"}, {123}, false) &&
              read_file(server->path).find("Baz") == std::string::npos,
          "server disable removes the Mod ID but keeps WorkshopItems");
}

void test_published_result_parser() {
    check(pz::parse_published_file_result(
              R"({"response":{"result":1,"resultcount":1,"publishedfiledetails":[{"publishedfileid":"1","result":1}]}})") == 1,
          "item result parsed, not the outer call result");

    check(pz::parse_published_file_result(
              R"({"response":{"result":1,"resultcount":1,"publishedfiledetails":[{"publishedfileid":"1786325498","result":9}]}})") == 9,
          "removed or private item reports result 9");

    check(pz::parse_published_file_result(
              R"({"response":{"result":1,"publishedfiledetails":[{"title":"result\":7","result":15}]}})") == 15,
          "result text inside a string is ignored");

    check(pz::parse_published_file_result(
              R"({"response":{"result":1,"resultcount":0,"publishedfiledetails":[]}})") == 0,
          "empty publishedfiledetails gives 0");
    check(pz::parse_published_file_result("not json") == 0, "garbage rejected");
}

void test_collection_parser() {
    auto ids = pz::parse_collection_children(
        R"({"response":{"result":1,"resultcount":1,"collectiondetails":[{"publishedfileid":"3417834390","result":1,"children":[{"publishedfileid":"111","sortorder":0,"filetype":0},{"publishedfileid":"222","sortorder":1,"filetype":0}]}]}})");
    check(ids.size() == 2 && ids[0] == 111 && ids[1] == 222, "collection children parsed");
    check(std::find(ids.begin(), ids.end(), 3417834390) == ids.end(), "collection id excluded");

    check(pz::parse_collection_children(
              R"({"response":{"publishedfiledetails":[{"publishedfileid":"123","time_updated":5}]}})").empty(),
          "item response is not a collection");

    auto dup = pz::parse_collection_children(
        R"({"children":[{"publishedfileid":"7"},{"publishedfileid":"7"},{"publishedfileid":"8"}]})");
    check(dup.size() == 2, "duplicate collection children deduped");
}

void test_activation_default_and_requirements(const pz::fs::path& root) {
    const pz::fs::path z = root / "ActivationRequirements";
    pz::Config cfg;
    cfg.zomboid_dir = z;
    cfg.user_mods_dir = z / "mods";
    cfg.data_dir = z / "pzmodmgr";
    cfg.config_file = cfg.data_dir / "config.ini";
    write_file(z / "mods" / "default.txt",
               "VERSION = 1,\n\nmods\n{\n}\n\nmaps\n{\n}\n");
    write_file(z / "Saves" / "Apocalypse" / "Save1" / "players.db", "");
    write_file(cfg.user_mods_dir / "BaseReq" / "mod.info",
               "id=BaseReq\nname=Base Requirement\n");
    write_file(cfg.user_mods_dir / "ExtraReq" / "mod.info",
               "id=ExtraReq\nname=Extra Requirement\nrequire=BaseReq\n");
    write_file(cfg.user_mods_dir / "Owner" / "mod.info",
               "id=OwnerMod\nname=Owner\nrequire=ExtraReq\n");

    pz::ModManager mm(cfg);
    std::vector<std::string> log;
    auto logfn = [&](const std::string& line) { log.push_back(line); };
    auto enable = mm.set_selection_enabled(pz::ActivationKind::Save,
        "Apocalypse/Save1", {}, {"Owner"}, true, logfn);
    check(enable.ok, "save enable succeeds: " + enable.message);

    const std::string defaults = read_file(cfg.user_mods_dir / "default.txt");
    const std::string save = read_file(
        z / "Saves" / "Apocalypse" / "Save1" / "mods.txt");
    for (const char* id : {"OwnerMod", "ExtraReq", "BaseReq"}) {
        const std::string line = std::string("mod = ") + id + ",";
        check(defaults.find(line) != std::string::npos,
              "save enable also writes Default: " + std::string(id));
        check(save.find(line) != std::string::npos,
              "save enable writes recursive requirement: " + std::string(id));
    }
    check(std::find(log.begin(), log.end(),
              "auto-enabled required mod ExtraReq (needed by OwnerMod)") != log.end(),
          "required mod auto-enable is logged");

    auto disable = mm.set_selection_enabled(pz::ActivationKind::Save,
        "Apocalypse/Save1", {}, {"Owner"}, false, logfn);
    check(disable.ok, "save disable succeeds: " + disable.message);
    check(read_file(z / "Saves" / "Apocalypse" / "Save1" / "mods.txt")
                  .find("mod = OwnerMod,") == std::string::npos,
          "save disable removes the selected mod");
    check(read_file(cfg.user_mods_dir / "default.txt")
                  .find("mod = OwnerMod,") != std::string::npos,
          "save disable leaves the global Default entry alone");
}

void test_updater_hash(const pz::fs::path& root) {
    const pz::fs::path file = root / "updater-hash" / "abc.bin";
    write_file(file, "abc");
    std::string hex, detail;
    check(pz::sha256_file(file, hex, detail),
          "SHA-256 file read succeeds: " + detail);
    check(hex == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "SHA-256 known vector matches: " + hex);

    write_file(file, "");
    check(pz::sha256_file(file, hex, detail),
          "empty SHA-256 file read succeeds: " + detail);
    check(hex == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "empty SHA-256 matches: " + hex);

    write_file(file, "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq");
    check(pz::sha256_file(file, hex, detail),
          "multi-block SHA-256 file read succeeds: " + detail);
    check(hex == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
          "multi-block SHA-256 matches: " + hex);
    check(!pz::sha256_file(root / "updater-hash" / "missing.bin", hex, detail),
          "missing file has no SHA-256");
}

void test_updater_parser() {
    const std::string json = R"({
        "tag_name": "v0.3.0",
        "assets": [
            {"name": "PZModManager-0.3.0-win64-setup.exe",
             "browser_download_url": "https://example/setup.exe", "size": 10},
            {"name": "PZModManager-0.3.0-win64.zip",
             "browser_download_url": "https://example/win.zip", "size": 123,
             "digest": "sha256:abc"},
            {"name": "PZModManager-0.3.0-linux-x86_64.tar.gz",
             "browser_download_url": "https://example/linux.tar.gz", "size": 456,
             "digest": "sha256:def"}
        ]
    })";

    check(pz::app_version_newer("v0.3.0", "0.2.1"), "newer version detected");
    check(!pz::app_version_newer("0.2.1", "0.2.1"), "same version is not newer");
    check(!pz::app_version_newer("0.2.0", "0.2.1"), "older version is not newer");

    auto win = pz::parse_latest_app_release(json, "0.2.1", "win");
    check(win.ok && win.available && win.latest_version == "0.3.0",
          "Windows release parsed");
    check(win.asset_name == "PZModManager-0.3.0-win64.zip" &&
              win.asset_url == "https://example/win.zip" &&
              win.asset_size == 123 && win.asset_digest == "sha256:abc",
          "Windows zip asset selected, setup ignored");

    auto linux_release = pz::parse_latest_app_release(json, "0.2.1", "linux");
    check(linux_release.ok && linux_release.available &&
              linux_release.asset_name == "PZModManager-0.3.0-linux-x86_64.tar.gz",
          "Linux tar.gz asset selected");

    auto current = pz::parse_latest_app_release(json, "0.3.0", "win");
    check(current.ok && !current.available, "current release reports up to date");
    check(!pz::parse_latest_app_release("{}", "0.2.1", "win").ok,
          "release without a tag is rejected");
}

void test_install_flow(const pz::fs::path& root) {
#ifdef FAKE_STEAMCMD_PATH
    const pz::fs::path zombie = root / "Zomboid";
    pz::Config cfg;
    cfg.zomboid_dir = zombie;
    cfg.user_mods_dir = zombie / "mods";
    cfg.data_dir = zombie / "pzmodmgr";
    // Copy the fake steamcmd out of the source tree at runtime; source files
    // are not guaranteed to be executable.
    const pz::fs::path fake_steamcmd = root / "fake_steamcmd.sh";
    write_file(fake_steamcmd, read_file(FAKE_STEAMCMD_PATH));
    make_executable(fake_steamcmd);
    cfg.steamcmd_exe = fake_steamcmd;
    cfg.steam_user = "test-user";
    write_file(zombie / "version.txt", "42.21.0 4a0e9546ec\n");

    // Fake curl serves a canned Steam API response from the environment, so
    // the update flow can be tested without network access.
    const pz::fs::path bin = root / "bin";
    const pz::fs::path curl_log = root / "curl.log";
    const pz::fs::path steam_log = root / "steamcmd.log";
    write_file(bin / "curl", R"(#!/bin/sh
if [ -n "${FAKE_CURL_LOG:-}" ]; then
    printf '%s\n' "$*" >> "$FAKE_CURL_LOG"
fi
case "$*" in
    *api.github.com/repos/sigods/pzmodmgr/releases/latest*)
        if [ -n "${FAKE_CURL_APP_JSON:-}" ]; then
            printf '%s\n' "$FAKE_CURL_APP_JSON"
        fi
        ;;
    *collectioncount*)
        if [ -n "${FAKE_CURL_COLLECTION_JSON:-}" ]; then
            printf '%s\n' "$FAKE_CURL_COLLECTION_JSON"
        fi
        ;;
    *)
        if [ -n "${FAKE_CURL_JSON:-}" ]; then
            printf '%s\n' "$FAKE_CURL_JSON"
        fi
        ;;
esac
exit "${FAKE_CURL_EXIT:-0}"
)");

    make_executable(bin / "curl");

    const char* old_path = std::getenv("PATH");
    std::string path = bin.string() + ":" + (old_path ? old_path : "/usr/bin:/bin");
    setenv("PATH", path.c_str(), 1);
    setenv("FAKE_STEAMCMD_LOG", steam_log.c_str(), 1);
    setenv("FAKE_CURL_LOG", curl_log.c_str(), 1);
    setenv("FAKE_CURL_EXIT", "0", 1);
    setenv("FAKE_STEAMCMD_MOD_VERSION", "1.0", 1);

    auto set_json_time = [](std::uint64_t t) {
        std::string json =
            R"({"response":{"publishedfiledetails":[{"publishedfileid":"123456789","result":1,"title":"Fixture","time_created":1600000000,"time_updated":)";
        json += std::to_string(t);
        json += "}]}}";
        setenv("FAKE_CURL_JSON", json.c_str(), 1);
    };
    set_json_time(1700000000);

    // A folder the manager did not install must survive remove().
    write_file(cfg.user_mods_dir / "not-ours" / "mod.info", "id=notours\nname=Not Ours\n");

    pz::ModManager mm(cfg);
    std::vector<std::string> log;
    auto logfn = [&log](const std::string& s) { log.push_back(s); };

    // add() downloads through the fake steamcmd and stores the API timestamp.
    auto r = mm.add("https://steamcommunity.com/sharedfiles/filedetails/?id=123456789", logfn);
    check(r.ok, "add succeeds with fake steamcmd: " + r.message);
    check(pz::fs::exists(cfg.user_mods_dir / "FixtureMod" / "mod.info"), "mod folder installed");
    check(!pz::fs::exists(cfg.data_dir / "steam_cache" / "123456789"),
          "per-download cache removed after install");
    check(pz::fs::exists(cfg.data_dir / "installed.tsv"), "installed.tsv written");
    check(mm.list().size() == 1, "one db record after add");
    check(!mm.list().empty() && mm.list()[0].time_updated == 1700000000,
          "add records remote time_updated");
    check(read_file(curl_log).find("itemcount=1&publishedfileids[0]=123456789") != std::string::npos,
          "Steam API POST body built");
    check(mm.add("123456789", logfn).ok == false, "duplicate add rejected");

    // A manual install with the same mod version is adopted instead of
    // replaced: no backup, no duplicate copy, and remove leaves it in place.
    setenv("FAKE_STEAMCMD_PER_ITEM", "1", 1);
    setenv("FAKE_STEAMCMD_MOD_VERSION", "3.0", 1);
    const auto manual_dir = cfg.user_mods_dir / "FixtureMod888";
    write_file(manual_dir / "mod.info",
               "id=FixtureMod888\nname=Manual Fixture\nmodversion=3.0\n");
    write_file(manual_dir / "manual-marker.txt", "keep\n");
    set_json_time(1700000700);
    log.clear();
    auto adopted = mm.add("888", logfn);
    check(adopted.ok, "external same-version add succeeds: " + adopted.message);
    check(pz::fs::exists(manual_dir / "manual-marker.txt"),
          "adopted folder is left untouched");
    check(!pz::fs::exists(cfg.data_dir / "backups"),
          "adopted folder is not backed up");
    auto adopted_records = mm.list();
    auto adopted_record = std::find_if(adopted_records.begin(), adopted_records.end(),
        [](const pz::InstalledMod& m) { return m.workshop_id == 888; });
    check(adopted_record != adopted_records.end(), "adopted folder is tracked");
    check(adopted_record != adopted_records.end() && !adopted_record->owned,
          "adopted folder is marked as externally installed");
    auto adopted_remove = mm.remove(888, logfn);
    check(adopted_remove.ok, "external record can be removed");
    check(pz::fs::exists(manual_dir / "manual-marker.txt"),
          "removing the record leaves the external folder in place");
    check(mm.list().size() == 1, "external record removed from the database");
    pz::fs::remove_all(manual_dir);
    unsetenv("FAKE_STEAMCMD_PER_ITEM");
    setenv("FAKE_STEAMCMD_MOD_VERSION", "1.0", 1);

    check(mm.update_check_due(), "first update check is due");

    // A check records update availability without downloading.
    set_json_time(1700000500);
    const std::string downloads_before_check = read_file(steam_log);
    log.clear();
    auto check_result = mm.check_updates(logfn);
    check(check_result.ok, "check_updates succeeds: " + check_result.message);
    check(mm.update_available(123456789), "update availability recorded");
    check(read_file(steam_log) == downloads_before_check, "check does not download");
    check(pz::fs::exists(cfg.data_dir / "updates.tsv"),
          "update availability persisted");
    const std::uint64_t check_now = static_cast<std::uint64_t>(std::time(nullptr));
    write_file(cfg.data_dir / "last_app_check", std::to_string(check_now));
    check(!mm.update_check_due(), "recent update check is not due");
    check(pz::fs::exists(cfg.data_dir / "last_check"), "last check time persisted");
    {
        pz::ModManager restarted(cfg);
        check(restarted.update_available(123456789),
              "update availability survives restart without another check");
        check(!restarted.update_check_due(),
              "recent update check stays suppressed after restart");
    }
    // A transient API failure must not erase a previously known update.
    setenv("FAKE_CURL_EXIT", "1", 1);
    log.clear();
    auto failed_check = mm.check_updates(logfn);
    check(failed_check.ok && mm.update_available(123456789),
          "failed check preserves known update availability");
    setenv("FAKE_CURL_EXIT", "0", 1);
    set_json_time(1700000500);

    const auto now = static_cast<std::uint64_t>(std::time(nullptr));
    write_file(cfg.data_dir / "last_check", std::to_string(now - 86401));
    check(mm.update_check_due(), "update check is due after a day");
    write_file(cfg.data_dir / "last_check", std::to_string(now + 3600));
    check(!mm.update_check_due(), "future timestamp does not force repeated checks");
    write_file(cfg.data_dir / "last_check", std::to_string(now - 100));
    check(!mm.update_check_due(), "check less than a day old is not due");

    // Unchanged remote time: update must not call steamcmd or create a backup.
    set_json_time(1700000000);
    const std::string downloads_before = read_file(steam_log);
    log.clear();
    auto u = mm.update_all(logfn);
    check(u.ok, "update_all succeeds: " + u.message);
    check(std::find(log.begin(), log.end(), "up to date: 123456789") != log.end(),
          "unchanged mod is reported up to date");
    check(read_file(steam_log) == downloads_before, "unchanged mod not downloaded again");
    check(!pz::fs::exists(cfg.data_dir / "backups"), "unchanged mod not backed up");

    // New remote time but the same mod version: inspect the download, then keep
    // the installed files and do not create a backup.
    set_json_time(1700000500);
    const std::string downloads_before_same = read_file(steam_log);
    log.clear();
    u = mm.update_all(logfn);
    check(u.ok, "same-version update succeeds: " + u.message);
    check(mm.list().size() == 1 && mm.list()[0].time_updated == 1700000500,
          "same-version update records the new time_updated");
    check(read_file(steam_log).size() > downloads_before_same.size(),
          "same-version update still inspects the download");
    check(!pz::fs::exists(cfg.data_dir / "backups"),
          "same-version update does not keep a backup");
    check(std::find(log.begin(), log.end(), "kept existing FixtureMod (version 1.0)") != log.end(),
          "same-version update reports the kept copy");

    // A real version change replaces the folder. The old copy is only kept
    // until the move succeeds, so it is deleted again immediately.
    setenv("FAKE_STEAMCMD_MOD_VERSION", "2.0", 1);
    set_json_time(1700000600);
    const std::string downloads_before_update = read_file(steam_log);
    u = mm.update_all(logfn);
    check(u.ok, "update_all with new version succeeds: " + u.message);
    check(mm.list().size() == 1 && mm.list()[0].time_updated == 1700000600,
          "version update records new time_updated");
    check(read_file(steam_log).size() > downloads_before_update.size(),
          "changed mod downloaded again");
    check(!pz::fs::exists(cfg.data_dir / "backups"),
          "successful update deletes the temporary old copy");
    check(!mm.update_available(123456789), "successful update clears update availability");
    {
        pz::ModManager restarted(cfg);
        check(!restarted.update_available(123456789),
              "cleared update availability stays cleared after restart");
    }

    // API failure: skip instead of blindly reinstalling.
    const std::string downloads_after_update = read_file(steam_log);
    setenv("FAKE_CURL_EXIT", "1", 1);
    log.clear();
    u = mm.update_all(logfn);
    check(u.ok, "update_all survives API failure: " + u.message);
    check(std::find(log.begin(), log.end(), "update check failed: 123456789 (skipped)") != log.end(),
          "API failure reported and skipped");
    check(read_file(steam_log) == downloads_after_update, "no download when check fails");
    setenv("FAKE_CURL_EXIT", "0", 1);

    // Single-item update, used by the GUI selection actions.
    log.clear();
    auto single = pz::run_command(mm, "update 123456789", logfn);
    check(single.ok && single.message.find("Up to date") != std::string::npos,
          "single update reports up to date: " + single.message);
    set_json_time(1700000900);
    const std::string downloads_before_single = read_file(steam_log);
    log.clear();
    auto single2 = pz::run_command(mm, "update 123456789", logfn);
    check(single2.ok, "single update succeeds: " + single2.message);
    check(single2.message.find("Already up to date") != std::string::npos,
          "single update reports the unchanged version: " + single2.message);
    check(mm.list().size() == 1 && mm.list()[0].time_updated == 1700000900,
          "single update records new time");
    check(read_file(steam_log).size() > downloads_before_single.size(),
          "single update downloaded");

    // Failed steamcmd download surfaces as an error, nothing installed.
    auto bad = mm.add("555", logfn);
    check(!bad.ok && bad.message.find("steamcmd failed") != std::string::npos,
          "failed download reported: " + bad.message);
    check(mm.list().size() == 1, "failed download does not change db");

    // Steam reporting the item as not found (private/removed) is refused before
    // steamcmd runs, because steamcmd would only print "Access Denied" and
    // still exit 0.
    const std::string downloads_before_missing = read_file(steam_log);
    setenv("FAKE_CURL_JSON",
           R"({"response":{"result":1,"resultcount":1,"publishedfiledetails":[{"publishedfileid":"999","result":9}]}})",
           1);
    log.clear();
    auto missing = mm.add("999", logfn);
    check(!missing.ok && missing.message.find("unavailable") != std::string::npos,
          "unavailable item reported before download: " + missing.message);
    check(read_file(steam_log) == downloads_before_missing,
          "unavailable item does not reach steamcmd");
    check(mm.list().size() == 1, "unavailable item does not change db");
    set_json_time(1700000900);

    // A first install whose mod.info targets an older major game build is
    // allowed, with an activity warning instead of a hard failure.
    setenv("FAKE_STEAMCMD_PER_ITEM", "1", 1);
    setenv("FAKE_STEAMCMD_VERSION_MAX", "41.0", 1);
    log.clear();
    auto older_build = mm.add("777", logfn);
    check(older_build.ok, "older-build first install succeeds: " + older_build.message);
    check(pz::fs::exists(cfg.user_mods_dir / "FixtureMod777" / "mod.info"),
          "older-build mod folder installed");
    check(std::find(log.begin(), log.end(),
              "warning: Fixture Mod 777 targets PZ up to 41.0, installed 42.21.0; installing anyway") != log.end(),
          "older-build install leaves an activity warning");
    check(!pz::fs::exists(cfg.data_dir / "steam_cache" / "777"),
          "older-build per-download cache removed");
    unsetenv("FAKE_STEAMCMD_VERSION_MAX");
    unsetenv("FAKE_STEAMCMD_PER_ITEM");
    check(mm.remove(777, logfn).ok, "older-build fixture removed");

    // remove command deletes only the recorded folder.
    auto rem = pz::run_command(mm, "remove 123456789", logfn);
    check(rem.ok, "remove succeeds: " + rem.message);
    check(!pz::fs::exists(cfg.user_mods_dir / "FixtureMod"), "installed folder removed");
    check(pz::fs::exists(cfg.user_mods_dir / "not-ours"), "unmanaged folder preserved");
    check(mm.list().empty(), "db empty after remove");
    check(!mm.remove(123456789, logfn).ok, "second remove rejected");

    // A pasted Workshop URL/id with no "add" prefix must act as add.
    setenv("FAKE_STEAMCMD_PER_ITEM", "1", 1);
    auto pasted = pz::run_command(mm,
        "https://steamcommunity.com/sharedfiles/filedetails/?id=333", logfn);
    check(pasted.ok, "bare workshop URL acts as add: " + pasted.message);
    check(pz::fs::exists(cfg.user_mods_dir / "FixtureMod333" / "mod.info"), "bare URL installed");
    auto pasted_id = pz::run_command(mm, "444", logfn);
    check(pasted_id.ok, "bare workshop id acts as add: " + pasted_id.message);
    check(pz::fs::exists(cfg.user_mods_dir / "FixtureMod444" / "mod.info"), "bare id installed");
    auto batch = pz::run_command(mm, "add 445 446", logfn);
    check(batch.ok, "batch add succeeds: " + batch.message);
    check(batch.message.find("Installed 2 of 2") != std::string::npos,
          "batch add reports every installed item");
    check(pz::fs::exists(cfg.user_mods_dir / "FixtureMod445" / "mod.info") &&
              pz::fs::exists(cfg.user_mods_dir / "FixtureMod446" / "mod.info"),
          "both items in a batch installed");
    check(std::find(log.begin(), log.end(), "446: Installed 1 mod(s)") != log.end(),
          "batch add logs each item");
    check(mm.remove(333, logfn).ok && mm.remove(444, logfn).ok &&
              mm.remove(445, logfn).ok && mm.remove(446, logfn).ok,
          "bare-add and batch mods removed");

    // A collection expands into its children; already-installed children skip.
    setenv("FAKE_CURL_COLLECTION_JSON",
        R"({"response":{"result":1,"resultcount":1,"collectiondetails":[{"publishedfileid":"3417834390","result":1,"children":[{"publishedfileid":"111","sortorder":0,"filetype":0},{"publishedfileid":"222","sortorder":1,"filetype":0}]}]}})", 1);
    log.clear();
    auto coll = pz::run_command(mm,
        "https://steamcommunity.com/sharedfiles/filedetails/?id=3417834390", logfn);
    check(coll.ok, "collection add succeeds: " + coll.message);
    check(mm.list().size() == 2, "collection installs both children");
    check(pz::fs::exists(cfg.user_mods_dir / "FixtureMod111" / "mod.info"), "collection child 111 installed");
    check(pz::fs::exists(cfg.user_mods_dir / "FixtureMod222" / "mod.info"), "collection child 222 installed");
    check(mm.list().size() == 2 && mm.list()[0].name != mm.list()[1].name,
          "distinct items keep distinct names");
    check(read_file(curl_log).find("collectioncount=1&publishedfileids[0]=3417834390") != std::string::npos,
          "collection API POST body built");

    const std::string collection_downloads = read_file(steam_log);
    log.clear();
    auto coll_again = pz::run_command(mm, "3417834390", logfn);
    check(coll_again.ok, "collection re-add succeeds: " + coll_again.message);
    check(std::find(log.begin(), log.end(), "up to date: 111") != log.end(),
          "collection child reported up to date");
    check(read_file(steam_log) == collection_downloads, "collection re-add downloads nothing");

    log.clear();
    auto multi_update = pz::run_command(mm, "update 111 222", logfn);
    check(multi_update.ok, "multi update succeeds: " + multi_update.message);
    check(std::find(log.begin(), log.end(), "111: Up to date: 111") != log.end(),
          "multi update reports each selected item");

    auto multi_remove = pz::run_command(mm, "remove 111 222", logfn);
    check(multi_remove.ok, "multi remove succeeds: " + multi_remove.message);
    check(mm.list().empty(), "multi remove cleared the db");
    unsetenv("FAKE_CURL_COLLECTION_JSON");

    // One Workshop item can ship several mod folders (an old and a new
    // variant). Every folder is installed, the UI groups them into one row,
    // and a variant the item later drops is removed on update.
    setenv("FAKE_STEAMCMD_PER_ITEM", "1", 1);
    setenv("FAKE_STEAMCMD_EXTRA_MOD", "VariantB", 1);
    set_json_time(1700001100);
    log.clear();
    auto variants = mm.add("1010", logfn);
    check(variants.ok, "multi-folder install succeeds: " + variants.message);
    check(pz::fs::exists(cfg.user_mods_dir / "FixtureMod1010" / "mod.info") &&
              pz::fs::exists(cfg.user_mods_dir / "VariantB" / "mod.info"),
          "both variants from one Workshop item installed");
    auto grouped = pz::visible_mods(mm.list(), "", false, 0,
        [](std::uint64_t) { return false; });
    check(grouped.size() == 1 && grouped[0].folders.size() == 2,
          "multi-folder item appears as one UI row");

    // Manual folders are visible and can be enabled by folder name. Enabling a
    // multi-folder item writes only its preferred variant.
    bool saw_manual = false;
    for (const auto& m : mm.list_all())
        if (m.workshop_id == 0 && m.folder == "not-ours") saw_manual = true;
    check(saw_manual, "untracked manual folder appears in the full list");
    log.clear();
    auto manual_enable = mm.set_selection_enabled(
        pz::ActivationKind::Default, {}, {}, {"not-ours"}, true, logfn);
    check(manual_enable.ok, "manual folder can be enabled: " + manual_enable.message);
    check(read_file(cfg.user_mods_dir / "default.txt").find("mod = notours,") !=
              std::string::npos,
          "manual folder Mod ID written to default.txt");

    const std::string preferred = mm.preferred_mod_id_for(1010);
    check(!preferred.empty(), "multi-folder item has a preferred variant");
    auto variant_enable = mm.set_mods_enabled(
        pz::ActivationKind::Default, {}, {1010}, true, logfn);
    check(variant_enable.ok, "variant enable succeeds: " + variant_enable.message);
    const std::string default_text = read_file(cfg.user_mods_dir / "default.txt");
    check(default_text.find("mod = " + preferred + ",") != std::string::npos,
          "preferred variant is enabled");
    for (const auto& id : mm.mod_ids_for(1010)) {
        if (id == preferred) continue;
        check(default_text.find("mod = " + id + ",") == std::string::npos,
              "sibling variant is not enabled");
    }

    unsetenv("FAKE_STEAMCMD_EXTRA_MOD");
    set_json_time(1700001200);
    log.clear();
    auto variant_update = mm.update_one(1010, logfn);
    check(variant_update.ok, "multi-folder update succeeds: " + variant_update.message);
    check(!pz::fs::exists(cfg.user_mods_dir / "VariantB"),
          "variant no longer shipped is removed");
    check(pz::fs::exists(cfg.user_mods_dir / "FixtureMod1010" / "mod.info"),
          "remaining variant kept");
    check(mm.remove(1010, logfn).ok, "multi-folder fixture removed");
    unsetenv("FAKE_STEAMCMD_PER_ITEM");

    // A stale per-item cache from an interrupted download must not poison the
    // next install. A fresh downloader root is created for every item.
    const auto stale_cache = cfg.data_dir / "steam_cache" / "999";
    write_file(stale_cache / "poison-cache", "stale\n");
    setenv("FAKE_STEAMCMD_PER_ITEM", "1", 1);
    set_json_time(1700001000);
    log.clear();
    auto stale_install = mm.add("999", logfn);
    check(stale_install.ok, "install ignores stale per-item cache: " + stale_install.message);
    check(!pz::fs::exists(stale_cache), "stale per-item cache cleared");
    check(mm.remove(999, logfn).ok, "stale-cache fixture removed");
    unsetenv("FAKE_STEAMCMD_PER_ITEM");

    // Activation targets: server Mods= and WorkshopItems= are written together.
    setenv("FAKE_STEAMCMD_PER_ITEM", "1", 1);
    set_json_time(1700001050);
    log.clear();
    auto activation_install = mm.add("999", logfn);
    check(activation_install.ok, "activation fixture installed: " + activation_install.message);
    write_file(cfg.zomboid_dir / "Server" / "local.ini", "Mods=\nWorkshopItems=\n");
    log.clear();
    auto enable_server = mm.set_mods_enabled(pz::ActivationKind::Server, "local", {999},
                                             true, logfn);
    check(enable_server.ok, "server enable succeeds: " + enable_server.message);
    const std::string server_ini = read_file(cfg.zomboid_dir / "Server" / "local.ini");
    check(server_ini.find("Mods=\\FixtureMod999") != std::string::npos,
          "server Mods= enabled with B42 backslash");
    check(server_ini.find("WorkshopItems=999") != std::string::npos,
          "server WorkshopItems= filled for downloads");
    auto server_targets = mm.activation_targets();
    auto local_server = std::find_if(server_targets.begin(), server_targets.end(),
        [](const pz::ActivationTarget& t) {
            return t.kind == pz::ActivationKind::Server && t.name == "local";
        });
    check(local_server != server_targets.end() &&
              mm.workshop_ids_for(*local_server) == (std::vector<std::uint64_t>{999}),
          "server target shares its Workshop IDs");
    auto disable_server = mm.set_mods_enabled(pz::ActivationKind::Server, "local", {999},
                                              false, logfn);
    check(disable_server.ok &&
              read_file(cfg.zomboid_dir / "Server" / "local.ini").find("FixtureMod999") ==
                  std::string::npos,
          "server disable removes the Mod ID");

    // Removing an item also disables it in every list: default.txt, saves, and
    // server INIs. Enable all three targets before remove().
    auto enable_default = mm.set_mods_enabled(pz::ActivationKind::Default, {}, {999},
                                              true, logfn);
    check(enable_default.ok, "default enable for remove succeeds: " + enable_default.message);
    write_file(cfg.zomboid_dir / "Saves" / "Apocalypse" / "Save1" / "players.db", "");
    auto enable_save = mm.set_mods_enabled(pz::ActivationKind::Save, "Apocalypse/Save1",
                                           {999}, true, logfn);
    check(enable_save.ok, "save enable for remove succeeds: " + enable_save.message);
    auto enable_server_again = mm.set_mods_enabled(pz::ActivationKind::Server, "local",
                                                   {999}, true, logfn);
    check(enable_server_again.ok,
          "server re-enable for remove succeeds: " + enable_server_again.message);

    log.clear();
    auto remove_enabled = mm.remove(999, logfn);
    check(remove_enabled.ok, "remove disables activation targets: " + remove_enabled.message);
    check(!pz::fs::exists(cfg.user_mods_dir / "FixtureMod999"),
          "removed mod folder is gone");
    check(read_file(cfg.user_mods_dir / "default.txt").find("mod = FixtureMod999,") ==
              std::string::npos,
          "remove disables the Mod ID in default.txt");
    check(read_file(cfg.zomboid_dir / "Saves" / "Apocalypse" / "Save1" / "mods.txt")
                  .find("mod = FixtureMod999,") == std::string::npos,
          "remove disables the Mod ID in a save's mods.txt");
    check(read_file(cfg.zomboid_dir / "Server" / "local.ini").find("FixtureMod999") ==
              std::string::npos,
          "remove disables the Mod ID in a server INI");
    check(read_file(cfg.zomboid_dir / "Server" / "local.ini").find("WorkshopItems=999") !=
              std::string::npos,
          "remove leaves the server WorkshopItems entry alone");
    check(remove_enabled.message.find("disabled in 3 mod list(s)") != std::string::npos,
          "remove reports how many lists were updated: " + remove_enabled.message);

    // Opt-in auto-enable adds a new install to mods/default.txt.
    mm.config().auto_enable = true;
    setenv("FAKE_STEAMCMD_PER_ITEM", "1", 1);
    set_json_time(1700001300);
    log.clear();
    auto auto_enabled = mm.add("2020", logfn);
    check(auto_enabled.ok, "auto-enable install succeeds: " + auto_enabled.message);
    check(read_file(cfg.user_mods_dir / "default.txt").find("mod = FixtureMod2020,") !=
              std::string::npos,
          "auto-enable writes the plain Mod ID to default.txt");
    check(mm.remove(2020, logfn).ok, "auto-enable fixture removed");
    check(read_file(cfg.user_mods_dir / "default.txt").find("mod = FixtureMod2020,") ==
              std::string::npos,
          "remove disables an auto-enabled Mod ID in default.txt");
    mm.config().auto_enable = false;
    unsetenv("FAKE_STEAMCMD_PER_ITEM");

    // The GUI settings dialog saves through the config command.
    auto cfg_cmd = pz::run_command(mm, "config steam_user ui-user", logfn);
    check(cfg_cmd.ok, "config command succeeds: " + cfg_cmd.message);
    check(read_file(cfg.data_dir / "config.ini").find("steam_user=ui-user") != std::string::npos,
          "config command persists the Steam user");

    // A daily check also checks the latest app release, so the UI can enable
    // its Update app button without waiting for the user to ask.
    setenv("FAKE_CURL_APP_JSON",
           R"({"tag_name":"v99.0.0","assets":[{"name":"PZModManager-99.0.0-linux-x86_64.tar.gz","browser_download_url":"https://example/linux.tar.gz","size":1,"digest":"sha256:abc"}]})",
           1);
    log.clear();
    auto app_check = pz::run_command(mm, "check", logfn);
    check(app_check.ok, "check command with app release succeeds: " + app_check.message);
    check(mm.app_update_available(), "daily check records an available app update");
    check(std::find(log.begin(), log.end(),
              "PZ Mod Manager 99.0.0 is available.") != log.end(),
          "app update availability is logged");
    check(!mm.app_update_check_due(), "app check is recorded for the day");
    check(pz::fs::exists(cfg.data_dir / "app_update"), "app update state persisted");
    {
        pz::ModManager restarted(cfg);
        check(restarted.app_update_available(),
              "app update availability survives a restart");
        check(!restarted.update_check_due(),
              "recent app and mod checks are not repeated after restart");
    }
    const auto app_now = static_cast<std::uint64_t>(std::time(nullptr));
    write_file(cfg.data_dir / "last_app_check", std::to_string(app_now - 86401));
    check(mm.app_update_check_due(), "app check is due after a day");
    write_file(cfg.data_dir / "app_update", std::string(pz::app_version()) + "\n");
    {
        pz::ModManager current(cfg);
        check(!current.app_update_available(),
              "running the cached release clears the app update state");
    }
    unsetenv("FAKE_CURL_APP_JSON");
#else
    std::printf("SKIP: FAKE_STEAMCMD_PATH not defined; install flow not tested\n");
#endif
}

}  // namespace

int main() {
    auto root = make_temp_root();
    test_parse_workshop_id();
    test_steamcmd_detection(root);
    test_builtin_steamcmd(root);
    test_config_paths(root);
    test_atomic_write(root);
    test_cache_cleanup(root);
    test_update_cache_cleanup(root);
    test_external_reload(root);
    test_native_fs_copy(root);
    test_corrupt_db(root);
    test_version_locks(root);
    test_read_mod_info(root);
    test_fixture_mod_info();
    test_missing_requirements(root);
    test_discover_mod_roots(root);
    test_time_updated_parser();
    test_activation_formats(root);
    test_activation_default_and_requirements(root);
    test_updater_hash(root);
    test_updater_parser();
    test_ui_helpers();
    test_published_result_parser();
    test_collection_parser();
    test_install_flow(root);

    pz::error_code ec;
    pz::fs::remove_all(root, ec);

    if (g_failures != 0) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all tests passed\n");
    return 0;
}
