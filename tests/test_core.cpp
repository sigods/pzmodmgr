// Dependency-free tests for pzcore. Run via: ctest --test-dir build --output-on-failure
#include "core/commands.hpp"
#include "core/mod_info.hpp"
#include "core/mod_manager.hpp"
#include "core/paths.hpp"
#include "core/steamcmd.hpp"
#include "core/url.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
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
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << content;
}

std::string read_file(const pz::fs::path& p) {
    std::ifstream in(p);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

pz::fs::path make_temp_root() {
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto root = pz::fs::temp_directory_path() / ("pzmodmgr-test-" + std::to_string(stamp));
    pz::fs::create_directories(root);
    return root;
}

void make_executable(const pz::fs::path& p) {
    std::error_code ec;
    pz::fs::permissions(p,
        pz::fs::perms::owner_read | pz::fs::perms::owner_write | pz::fs::perms::owner_exec |
        pz::fs::perms::group_read | pz::fs::perms::group_exec |
        pz::fs::perms::others_read | pz::fs::perms::others_exec, ec);
    check(!ec, "make executable: " + p.string());
}

void test_parse_workshop_id() {
    check(pz::parse_workshop_id("123456789").value_or(0) == 123456789, "plain numeric id");
    check(pz::parse_workshop_id("  42  ").value_or(0) == 42, "numeric id with whitespace");

    const char* url = "https://steamcommunity.com/sharedfiles/filedetails/?id=123456789&searchtext=x";
    check(pz::parse_workshop_id(url).value_or(0) == 123456789, "workshop url");

    check(!pz::parse_workshop_id("").has_value(), "empty input rejected");
    check(!pz::parse_workshop_id("no id here").has_value(), "text without id rejected");
    check(!pz::parse_workshop_id("id=abc").has_value(), "non-numeric id rejected");
    check(!pz::parse_workshop_id("123456789012345678901").has_value(), "overlong id rejected");
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

    check(pz::compare_versions("42.21.0", "42.21") == 0,
          "missing version components compare as zero");
    check(pz::compare_versions("42.20", "42.21.0") < 0, "older build compares lower");
    check(pz::compare_versions("v42.22.1", "42.21.9") > 0, "v-prefixed build compares higher");
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
    check(!pz::fs::exists(cfg.data_dir / "steam_cache" / "steamapps" / "workshop" /
                          "content" / "108600" / "123456789"),
          "download cache moved into the mods folder");
    check(pz::fs::exists(cfg.data_dir / "installed.tsv"), "installed.tsv written");
    check(mm.list().size() == 1, "one db record after add");
    check(!mm.list().empty() && mm.list()[0].time_updated == 1700000000,
          "add records remote time_updated");
    check(read_file(curl_log).find("itemcount=1&publishedfileids[0]=123456789") != std::string::npos,
          "Steam API POST body built");
    check(mm.add("123456789", logfn).ok == false, "duplicate add rejected");

    // A check records update availability without downloading.
    set_json_time(1700000500);
    const std::string downloads_before_check = read_file(steam_log);
    log.clear();
    auto check_result = mm.check_updates(logfn);
    check(check_result.ok, "check_updates succeeds: " + check_result.message);
    check(mm.update_available(123456789), "update availability recorded");
    check(read_file(steam_log) == downloads_before_check, "check does not download");

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

    // New remote time: update reinstalls once and records the new timestamp.
    set_json_time(1700000500);
    u = mm.update_all(logfn);
    check(u.ok, "update_all with new time succeeds: " + u.message);
    check(mm.list().size() == 1 && mm.list()[0].time_updated == 1700000500,
          "update records new time_updated");
    check(read_file(steam_log).size() > downloads_before.size(), "changed mod downloaded again");
    check(pz::fs::exists(cfg.data_dir / "backups"), "changed mod backed up before replace");

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
    check(mm.list().size() == 1 && mm.list()[0].time_updated == 1700000900,
          "single update records new time");
    check(read_file(steam_log).size() > downloads_before_single.size(),
          "single update downloaded");

    // Failed steamcmd download surfaces as an error, nothing installed.
    auto bad = mm.add("555", logfn);
    check(!bad.ok && bad.message.find("steamcmd failed") != std::string::npos,
          "failed download reported: " + bad.message);
    check(mm.list().size() == 1, "failed download does not change db");

    // A first install whose mod.info targets another major game build is refused.
    setenv("FAKE_STEAMCMD_VERSION_MIN", "43.0", 1);
    log.clear();
    auto incompatible = mm.add("777", logfn);
    check(!incompatible.ok && incompatible.message.find("does not support") != std::string::npos,
          "incompatible first install rejected: " + incompatible.message);
    check(!pz::fs::exists(cfg.user_mods_dir / "FixtureMod777"),
          "incompatible first install leaves no mod folder");
    check(!pz::fs::exists(cfg.data_dir / "steam_cache" / "steamapps" / "workshop" /
                          "content" / "108600" / "777"),
          "incompatible download cache removed");
    unsetenv("FAKE_STEAMCMD_VERSION_MIN");

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
    check(mm.remove(333, logfn).ok && mm.remove(444, logfn).ok, "bare-add mods removed");

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
    unsetenv("FAKE_STEAMCMD_PER_ITEM");
    unsetenv("FAKE_CURL_COLLECTION_JSON");

    // The GUI settings dialog saves through the config command.
    auto cfg_cmd = pz::run_command(mm, "config steam_user ui-user", logfn);
    check(cfg_cmd.ok, "config command succeeds: " + cfg_cmd.message);
    check(read_file(cfg.data_dir / "config.ini").find("steam_user=ui-user") != std::string::npos,
          "config command persists the Steam user");
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
    test_read_mod_info(root);
    test_discover_mod_roots(root);
    test_time_updated_parser();
    test_collection_parser();
    test_install_flow(root);

    std::error_code ec;
    pz::fs::remove_all(root, ec);

    if (g_failures != 0) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all tests passed\n");
    return 0;
}
