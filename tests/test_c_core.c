#define _POSIX_C_SOURCE 200809L

/* C99 integration tests for the pure-C core, manager, and command parser. */

#include "core/activation_targets_c.h"
#include "core/commands_c.h"
#include "core/fs_c.h"
#include "core/hash_c.h"
#include "core/mod_info_fs_c.h"
#include "core/mod_manager_c.h"
#include "core/paths_c.h"
#include "core/process_c.h"
#include "core/steamcmd_c.h"
#include "core/updater_c.h"
#include "core/url_c.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PZMODMGR_VERSION
#define PZMODMGR_VERSION "0.0.0"
#endif

static int failures = 0;

static void check(int condition, const char* message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

static int str_is(const pz_str* value, const char* expected) {
    const size_t length = strlen(expected);
    return value && value->length == length &&
           (length == 0 || memcmp(value->data, expected, length) == 0);
}

static int contains(const pz_str* value, const char* needle) {
    return value && value->data && strstr(value->data, needle) != NULL;
}

static int path_exists(const pz_path* path) {
    pz_fs_error error;
    int exists = 0;
    pz_fs_error_clear(&error);
    return pz_fs_exists(path, &exists, &error) && exists;
}

static void path_child(pz_path* out, const pz_path* base, const char* child) {
    pz_path parent;
    pz_path_init(&parent);
    /* `out` may alias `base`; copy before initializing the result. */
    pz_path_assign(&parent, base->text.data, base->text.length);
    pz_path_init(out);
    if (child && *child)
        pz_path_join(out, &parent, child, strlen(child));
    else
        pz_path_assign(out, parent.text.data, parent.text.length);
    pz_path_free(&parent);
}

static void path_child_str(pz_path* out, const pz_str* base, const char* child) {
    pz_path parent;
    pz_path_init(&parent);
    pz_path_assign(&parent, base->data, base->length);
    path_child(out, &parent, child);
    pz_path_free(&parent);
}

static void path_text(pz_path* out, const char* text) {
    pz_path_init(out);
    pz_path_assign_cstr(out, text);
}

static void str_from_path(pz_str* out, const pz_path* path) {
    pz_str_assign(out, path->text.data, path->text.length);
}

static void write_text(const pz_path* path, const char* text) {
    pz_path parent;
    pz_fs_error error;
    pz_fs_file* file = NULL;
    size_t length = strlen(text);
    size_t put = 0;

    pz_path_init(&parent);
    pz_fs_error_clear(&error);
    if (pz_path_parent(path, &parent) && parent.text.length)
        pz_fs_create_directories(&parent, &error);
    pz_fs_error_clear(&error);
    if (!pz_fs_open_write(path, &file, &error)) {
        check(0, text);
        pz_path_free(&parent);
        return;
    }
    while (put < length) {
        const size_t chunk = pz_fs_write(file, text + put, length - put);
        if (chunk == 0) break;
        put += chunk;
    }
    pz_fs_close(file, &error);
    check(put == length, "write complete");
    pz_path_free(&parent);
}

static int read_text(const pz_path* path, pz_str* out) {
    pz_fs_error error;
    pz_fs_file* file = NULL;
    char buffer[4096];
    size_t got;

    pz_str_clear(out);
    pz_fs_error_clear(&error);
    if (!pz_fs_open_read(path, &file, &error)) return 0;
    while ((got = pz_fs_read(file, buffer, sizeof buffer)) != 0) {
        if (!pz_str_append(out, buffer, got)) break;
    }
    pz_fs_close(file, &error);
    return 1;
}

static void make_executable(const pz_path* path) {
    pz_fs_error error;
    pz_fs_error_clear(&error);
    check(pz_fs_set_executable(path, 1, &error), "set executable");
}

static void config_for(pz_paths_config* config, const pz_path* zomboid) {
    pz_path mods;
    pz_path data;
    pz_path config_file;
    pz_str default_exe;

    pz_paths_config_init(config);
    path_child(&mods, zomboid, "mods");
    path_child(&data, zomboid, "pzmodmgr");
    path_child(&config_file, &data, "config.ini");
    str_from_path(&config->zomboid_dir, zomboid);
    str_from_path(&config->user_mods_dir, &mods);
    str_from_path(&config->data_dir, &data);
    str_from_path(&config->config_file, &config_file);
    pz_str_init(&default_exe);
    if (pz_paths_builtin_steamcmd(data.text.data, data.text.length, &default_exe))
        pz_str_assign(&config->steamcmd_exe, default_exe.data, default_exe.length);
    pz_str_free(&default_exe);
    pz_path_free(&config_file);
    pz_path_free(&data);
    pz_path_free(&mods);
}

typedef struct log_capture {
    pz_vec lines;
} log_capture;

static void capture_line(void* context, const char* line) {
    log_capture* capture = (log_capture*)context;
    pz_str* text = (pz_str*)pz_vec_push(&capture->lines);
    if (!text) return;
    pz_str_init(text);
    if (!pz_str_assign_cstr(text, line ? line : "")) {
        pz_str_free(text);
        --capture->lines.length;
    }
}

static void capture_init(log_capture* capture) {
    pz_vec_init(&capture->lines, sizeof(pz_str));
}

static void capture_free(log_capture* capture) {
    size_t i;
    for (i = 0; i < capture->lines.length; ++i)
        pz_str_free((pz_str*)pz_vec_at(&capture->lines, i));
    pz_vec_free(&capture->lines);
}

static int captured(log_capture* capture, const char* line) {
    size_t i;
    for (i = 0; i < capture->lines.length; ++i) {
        const pz_str* text = (const pz_str*)pz_vec_at_const(&capture->lines, i);
        if (str_is(text, line)) return 1;
    }
    return 0;
}

/* --------------------------------------------------------------- services */

static int read_info_cb(void* context, const char* path, size_t path_length,
                        const char* game_version, size_t game_length,
                        pz_mod_info* out) {
    pz_path dir;
    int ok;
    (void)context;
    pz_path_init(&dir);
    ok = pz_path_assign(&dir, path, path_length) &&
         pz_mod_info_read(&dir, game_version, game_length, out);
    pz_path_free(&dir);
    return ok;
}

static int discover_roots_cb(void* context, const char* path, size_t path_length,
                             pz_vec* out) {
    pz_path dir;
    int ok;
    (void)context;
    pz_path_init(&dir);
    ok = pz_path_assign(&dir, path, path_length) &&
         pz_discover_mod_roots(&dir, out);
    pz_path_free(&dir);
    return ok;
}

static int discover_targets_cb(void* context, const char* zomboid,
                               size_t zomboid_length, const char* game_version,
                               size_t game_length, pz_vec* out) {
    (void)context;
    return pz_activation_targets_discover(zomboid, zomboid_length,
                                          game_version, game_length, out);
}

static int write_target_cb(void* context, pz_activation_target_data* target,
                           const pz_vec* add, const pz_vec* workshops,
                           int enabled) {
    (void)context;
    return pz_activation_targets_write(target, add, workshops, enabled);
}

static uint64_t remote_time_cb(void* context, uint64_t workshop_id) {
    (void)context;
    return pz_steamcmd_fetch_time_updated(workshop_id);
}

static int remote_details_cb(void* context, uint64_t workshop_id,
                             pz_remote_details* out) {
    (void)context;
    pz_remote_details_init(out);
    pz_steamcmd_fetch_details(workshop_id, out);
    return 1;
}

static int collection_cb(void* context, uint64_t collection_id, pz_vec* out) {
    (void)context;
    pz_vec_init(out, sizeof(uint64_t));
    return pz_steamcmd_fetch_collection(collection_id, out);
}

static int download_cb(void* context, const pz_paths_config* config,
                       uint64_t workshop_id, const pz_path* install_dir,
                       pz_path* content_dir, pz_str* detail,
                       const pz_log* log) {
    pz_path data_dir;
    pz_str exe;
    pz_download_outcome outcome;
    int ok;
    (void)context;

    pz_path_init(&data_dir);
    pz_str_init(&exe);
    pz_download_outcome_init(&outcome);
    pz_path_assign(&data_dir, config->data_dir.data, config->data_dir.length);
    pz_str_assign(&exe, config->steamcmd_exe.data, config->steamcmd_exe.length);
    ok = pz_steamcmd_download_workshop(&data_dir, &exe, &config->steam_user,
                                       workshop_id, install_dir, log, &outcome);
    pz_path_init(content_dir);
    pz_str_assign(detail, outcome.detail.data, outcome.detail.length);
    if (ok)
        pz_path_assign(content_dir, outcome.content_dir.text.data,
                       outcome.content_dir.text.length);
    pz_download_outcome_free(&outcome);
    pz_str_free(&exe);
    pz_path_free(&data_dir);
    return ok;
}

static int check_app_update_cb(void* context, pz_app_update* out) {
    const char* platform = pz_updater_platform_name();
    (void)context;
    pz_app_update_init(out);
    return pz_updater_check(PZMODMGR_VERSION, strlen(PZMODMGR_VERSION), platform,
                            strlen(platform), out);
}

static int save_config_cb(void* context, const pz_paths_config* config) {
    (void)context;
    return pz_paths_config_save(config);
}

static int builtin_steamcmd_cb(void* context, const char* data_dir,
                               size_t data_dir_length, pz_str* out) {
    (void)context;
    pz_str_init(out);
    return pz_paths_builtin_steamcmd(data_dir, data_dir_length, out);
}

static const char* app_version_cb(void* context) {
    (void)context;
    return PZMODMGR_VERSION;
}

static pz_mod_manager* make_manager(const pz_paths_config* config) {
    /* The manager keeps this pointer for its lifetime. */
    static pz_mm_services services;
    memset(&services, 0, sizeof services);
    services.read_mod_info = read_info_cb;
    services.discover_mod_roots = discover_roots_cb;
    services.discover_targets = discover_targets_cb;
    services.write_target = write_target_cb;
    services.remote_time = remote_time_cb;
    services.remote_details = remote_details_cb;
    services.collection_children = collection_cb;
    services.download = download_cb;
    services.check_app_update = check_app_update_cb;
    services.save_config = save_config_cb;
    services.builtin_steamcmd = builtin_steamcmd_cb;
    services.app_version = app_version_cb;
    return pz_mod_manager_create(config, &services);
}

/* ----------------------------------------------------------------- helpers */

static size_t mod_count(pz_mod_manager* manager) {
    pz_vec mods;
    size_t count;
    pz_vec_init(&mods, sizeof(pz_installed_mod));
    pz_mod_manager_list(manager, &mods);
    count = mods.length;
    pz_mod_manager_free_mods(&mods);
    return count;
}

static const pz_installed_mod* find_mod(pz_vec* mods, uint64_t id) {
    size_t i;
    for (i = 0; i < mods->length; ++i) {
        const pz_installed_mod* mod = (const pz_installed_mod*)pz_vec_at_const(mods, i);
        if (mod->workshop_id == id) return mod;
    }
    return NULL;
}

static pz_activation_target_data* find_target(pz_vec* targets, int kind,
                                              const char* name) {
    size_t i;
    for (i = 0; i < targets->length; ++i) {
        pz_activation_target_data* target =
            (pz_activation_target_data*)pz_vec_at(targets, i);
        if (target->kind == kind &&
            (name == NULL || str_is(&target->name, name)))
            return target;
    }
    return NULL;
}

/* ------------------------------------------------------------------- tests */

static void test_workshop_ids(void) {
    uint64_t id = 0;
    pz_url_ids ids;

    check(pz_url_parse_workshop_id("123456789", 9, &id) && id == 123456789,
          "plain numeric id");
    check(pz_url_parse_workshop_id("  42  ", 6, &id) && id == 42,
          "numeric id with whitespace");
    check(pz_url_parse_workshop_id(
              "https://steamcommunity.com/sharedfiles/filedetails/?id=123456789&searchtext=x",
              strlen("https://steamcommunity.com/sharedfiles/filedetails/?id=123456789&searchtext=x"),
              &id) &&
              id == 123456789,
          "workshop URL");
    check(pz_url_parse_workshop_id(
              "https://steamcommunity.com/workshop/filedetails/?id=3171167894",
              strlen("https://steamcommunity.com/workshop/filedetails/?id=3171167894"),
              &id) &&
              id == 3171167894,
          "workshop/filedetails URL");
    check(!pz_url_parse_workshop_id("", 0, &id), "empty input rejected");
    check(!pz_url_parse_workshop_id("id=abc", 6, &id), "non-numeric id rejected");
    check(!pz_url_parse_workshop_id("123456789012345678901", 21, &id),
          "overlong id rejected");

    memset(&ids, 0, sizeof ids);
    check(pz_url_parse_workshop_ids(
              "123\t456\nhttps://steamcommunity.com/sharedfiles/filedetails/?id=789 123",
              strlen("123\t456\nhttps://steamcommunity.com/sharedfiles/filedetails/?id=789 123"),
              &ids),
          "multiple IDs parse");
    check(ids.count == 3 && ids.items[0] == 123 && ids.items[1] == 456 &&
              ids.items[2] == 789,
          "multiple IDs ordered and deduplicated");
    pz_url_ids_free(&ids);
    check(pz_url_parse_workshop_ids("   ", 3, &ids) && ids.count == 0,
          "empty batch has no IDs");
    pz_url_ids_free(&ids);
    check(!pz_url_parse_workshop_ids("123 not-an-id", strlen("123 not-an-id"), &ids),
          "a bad token rejects the whole batch");
}

static void test_config_paths(const pz_path* root) {
    pz_path zomboid;
    pz_paths_config config;
    pz_mod_manager* manager;
    pz_mm_result result;
    pz_str custom;
    pz_str text;
    pz_path dir;

    path_child(&zomboid, root, "config-zomboid");
    config_for(&config, &zomboid);
    pz_str_assign_cstr(&config.steam_user, "config-user");
    pz_str_init(&custom);
    path_child(&dir, root, "custom-zomboid");
    str_from_path(&custom, &dir);
    manager = make_manager(&config);
    check(manager != NULL, "config manager created");
    if (!manager) goto done;

    result = pz_command_run(manager, "config zomboid_dir", 18, NULL);
    check(result.ok, "missing config argument rejected");
    pz_mm_result_free(&result);
    result = pz_command_run(manager, "config zomboid_dir ", 19, NULL);
    check(result.ok, "empty config value accepted");
    pz_mm_result_free(&result);

    result = pz_command_run(manager, custom.data, custom.length, NULL);
    check(!result.ok, "bare custom path is not a command");
    pz_mm_result_free(&result);

    {
        pz_str line;
        pz_str_init(&line);
        pz_str_append_cstr(&line, "config zomboid_dir ");
        pz_str_append(&line, custom.data, custom.length);
        result = pz_command_run(manager, line.data, line.length, NULL);
        pz_str_free(&line);
        check(result.ok, "config zomboid_dir succeeds");
        pz_mm_result_free(&result);
    }

    {
        pz_paths_config actual;
        pz_path expected_mods;
        pz_path expected_data;
        pz_paths_config_init(&actual);
        pz_mod_manager_get_config(manager, &actual);
        path_child(&expected_mods, &dir, "mods");
        path_child(&expected_data, &dir, "pzmodmgr");
        check(str_is(&actual.zomboid_dir, custom.data), "custom Zomboid dir applied");
        check(str_is(&actual.user_mods_dir, expected_mods.text.data),
              "custom mods dir applied");
        check(str_is(&actual.data_dir, expected_data.text.data),
              "custom data dir applied");
        check(str_is(&actual.config_file, config.config_file.data),
              "config file stays at its bootstrap path");
        check(path_exists(&expected_mods) && path_exists(&expected_data),
              "custom Zomboid directories created");
        pz_paths_config_free(&actual);
        pz_path_free(&expected_data);
        pz_path_free(&expected_mods);
    }

    {
        pz_path config_path;
        pz_path_init(&config_path);
        pz_path_assign(&config_path, config.config_file.data,
                       config.config_file.length);
        pz_str_init(&text);
        check(read_text(&config_path, &text), "bootstrap config readable");
        check(contains(&text, "zomboid_dir="), "config persists Zomboid dir");
        pz_str_free(&text);
        pz_path_free(&config_path);
    }

    {
        pz_paths_config loaded;
        pz_path expected_mods;
        pz_path expected_data;
        pz_paths_config_init(&loaded);
        pz_str_assign(&loaded.config_file, config.config_file.data,
                      config.config_file.length);
        pz_str_assign(&loaded.zomboid_dir, config.zomboid_dir.data,
                      config.zomboid_dir.length);
        pz_str_assign(&loaded.user_mods_dir, config.user_mods_dir.data,
                      config.user_mods_dir.length);
        pz_str_assign(&loaded.data_dir, config.data_dir.data, config.data_dir.length);
        check(pz_paths_config_load(&loaded), "config reload succeeds");
        path_child(&expected_mods, &dir, "mods");
        path_child(&expected_data, &dir, "pzmodmgr");
        check(str_is(&loaded.zomboid_dir, custom.data),
              "custom Zomboid dir loaded on restart");
        check(str_is(&loaded.user_mods_dir, expected_mods.text.data),
              "custom mods dir loaded on restart");
        check(str_is(&loaded.data_dir, expected_data.text.data),
              "custom data dir loaded on restart");
        check(str_is(&loaded.steam_user, "config-user"),
              "steam user loads with custom path");
        pz_paths_config_free(&loaded);
        pz_path_free(&expected_data);
        pz_path_free(&expected_mods);
    }

    result = pz_mod_manager_set_zomboid_dir(manager, "relative/Zomboid", 17);
    check(!result.ok, "relative Zomboid dir rejected");
    pz_mm_result_free(&result);
    {
        pz_paths_config actual;
        pz_paths_config_init(&actual);
        pz_mod_manager_get_config(manager, &actual);
        check(str_is(&actual.zomboid_dir, custom.data),
              "rejected relative path changes nothing");
        pz_paths_config_free(&actual);
    }

    pz_mod_manager_destroy(manager);
done:
    pz_str_free(&custom);
    pz_path_free(&dir);
    pz_paths_config_free(&config);
    pz_path_free(&zomboid);
}

static void test_mod_info_fs(const pz_path* root) {
    pz_path item;
    pz_path file;
    pz_vec roots;
    pz_mod_info info;
    pz_path icon;
    pz_str expected;

    pz_str_init(&expected);

    path_child(&item, root, "mod-info-item");
    path_child(&file, &item, "mods");
    path_child(&file, &file, "Alpha");
    path_child(&file, &file, "mod.info");
    write_text(&file, "id=AlphaMod\nname=Alpha\nmodversion=1.0\n");
    path_child(&file, &item, "Contents");
    path_child(&file, &file, "mods");
    path_child(&file, &file, "Beta");
    path_child(&file, &file, "mod.info");
    write_text(&file, "id=BetaMod\nname=Beta\n");
    path_child(&file, &item, "mods");
    path_child(&file, &file, "not-a-mod");
    {
        pz_fs_error error;
        pz_fs_error_clear(&error);
        pz_fs_create_directories(&file, &error);
    }

    pz_vec_init(&roots, sizeof(pz_str));
    check(pz_discover_mod_roots(&item, &roots), "root discovery succeeds");
    check(roots.length == 2, "mods/ and Contents/mods/ roots detected");
    pz_activation_string_list_free(&roots);

    path_child(&item, root, "mod-info-versioned");
    path_child(&file, &item, "mods");
    path_child(&file, &file, "HorseMod");
    path_child(&file, &file, "42");
    path_child(&file, &file, "mod.info");
    write_text(&file, "id=HorseMod\nname=Horse Mod\n");
    pz_vec_init(&roots, sizeof(pz_str));
    check(pz_discover_mod_roots(&item, &roots) && roots.length == 1,
          "versioned mod keeps the item folder as root");
    pz_activation_string_list_free(&roots);

    path_child(&item, root, "mod-info-read");
    path_child(&file, &item, "mod.info");
    write_text(&file,
               "name=My Mod\r\nid=mymod\r\nauthor=Alice\r\nmodversion=1.2.3\r\n"
               "icon=art/custom.png\r\nrequire=\\damnlib, \\DamnLibGlass\r\n");
    path_child(&file, &item, "art");
    path_child(&file, &file, "custom.png");
    write_text(&file, "not decoded");
    pz_mod_info_init(&info);
    check(pz_mod_info_read_any(&item, &info), "mod.info readable");
    check(str_is(&info.id, "mymod"), "mod.info id parsed");
    check(str_is(&info.name, "My Mod"), "mod.info CRLF name parsed");
    check(str_is(&info.author, "Alice"), "mod.info author parsed");
    check(str_is(&info.mod_version, "1.2.3"), "mod.info modversion parsed");
    check(pz_mod_info_required_count(&info) == 2 &&
              str_is(pz_mod_info_required_at(&info, 0), "damnlib") &&
              str_is(pz_mod_info_required_at(&info, 1), "DamnLibGlass"),
          "requirements trimmed and de-escaped");
    path_child(&icon, &item, "art");
    path_child(&icon, &icon, "custom.png");
    str_from_path(&expected, &icon);
    check(str_is(&info.icon, expected.data), "mod.info icon resolved");
    pz_str_free(&expected);
    pz_path_free(&icon);
    pz_mod_info_free(&info);

    path_child(&item, root, "mod-info-fixture");
    pz_path_free(&item);
    path_text(&item, PZMODMGR_TEST_FIXTURES);
    path_child(&file, &item, "mod_info");
    path_child(&file, &file, "basic");
    pz_mod_info_init(&info);
    check(pz_mod_info_read_any(&file, &info), "fixture mod.info readable");
    check(str_is(&info.id, "fixturemod") && str_is(&info.name, "Fixture Mod") &&
              str_is(&info.author, "Fixture Author") &&
              str_is(&info.mod_version, "2.0"),
          "fixture mod.info fields parsed");
    check(pz_mod_info_required_count(&info) == 2,
          "fixture requirements deduplicated");
    pz_mod_info_free(&info);
    pz_path_free(&file);
    pz_path_free(&item);

    path_text(&item, PZMODMGR_TEST_FIXTURES);
    path_child(&file, &item, "discover");
    path_child(&file, &file, "multi");
    pz_vec_init(&roots, sizeof(pz_str));
    check(pz_discover_mod_roots(&file, &roots) && roots.length == 2,
          "fixture mods/ and Contents/mods/ discovery");
    pz_activation_string_list_free(&roots);
    pz_path_free(&file);
    pz_path_free(&item);
}

static void test_cache_cleanup(const pz_path* root) {
    pz_path zomboid;
    pz_paths_config config;
    pz_mod_manager* manager;
    pz_path cache;
    pz_path backups;
    pz_path stale;
    pz_path recent;
    int64_t now = 0;
    pz_fs_error error;

    path_child(&zomboid, root, "cache-zomboid");
    config_for(&config, &zomboid);
    path_child(&cache, &zomboid, "pzmodmgr");
    path_child(&cache, &cache, "steam_cache");
    path_child(&stale, &cache, "stale-download");
    path_child(&cache, &cache, "recent-download");
    path_child(&backups, &zomboid, "pzmodmgr");
    path_child(&backups, &backups, "backups");
    path_child(&recent, &backups, "recent-backup");
    path_child(&backups, &backups, "old-backup");

    {
        pz_path file;
        path_child(&file, &stale, "payload");
        write_text(&file, "old");
        pz_path_free(&file);
        path_child(&file, &cache, "payload");
        write_text(&file, "new");
        pz_path_free(&file);
        path_child(&file, &recent, "payload");
        write_text(&file, "new");
        pz_path_free(&file);
        path_child(&file, &backups, "payload");
        write_text(&file, "old");
        pz_path_free(&file);
    }

    check(pz_fs_file_time_now(&now), "current time");
    pz_fs_error_clear(&error);
    pz_fs_set_last_write_time(&stale, now - 24 * 8 * 60 * 60, &error);
    pz_fs_error_clear(&error);
    pz_fs_set_last_write_time(&backups, now - 24 * 8 * 60 * 60, &error);

    manager = make_manager(&config);
    check(manager != NULL, "cache manager created");
    if (manager) pz_mod_manager_destroy(manager);
    check(!path_exists(&stale), "stale download cache pruned on startup");
    check(path_exists(&cache), "recent download cache kept");
    check(!path_exists(&backups), "old backup pruned on startup");
    check(path_exists(&recent), "recent backup kept");

    pz_path_free(&recent);
    pz_path_free(&backups);
    pz_path_free(&cache);
    pz_path_free(&stale);
    pz_paths_config_free(&config);
    pz_path_free(&zomboid);
}

static void test_db_and_locks(const pz_path* root) {
    pz_path zomboid;
    pz_path mods;
    pz_path data;
    pz_path file;
    pz_paths_config config;
    pz_mod_manager* manager;
    pz_vec mods_list;
    pz_str text;
    pz_mm_result result;

    pz_str_init(&text);
    path_child(&zomboid, root, "db-zomboid");
    config_for(&config, &zomboid);
    path_child(&mods, &zomboid, "mods");
    path_child(&data, &zomboid, "pzmodmgr");
    path_child(&file, &mods, "FixtureLock");
    path_child(&file, &file, "mod.info");
    write_text(&file, "id=FixtureLock\nname=Fixture Lock\nmodversion=1.0\n");
    path_child(&file, &data, "installed.tsv");
    write_text(&file,
               "123\tGood\tGood Mod\t1700000000\n"
               "oops\tBad\tBad Mod\t1700000000\n"
               "124\tBad\tBad Mod\tnope\n"
               "125\tShort\tShort Mod\n"
               "126\tAdopted\tAdopted Mod\t42\t0\n"
               "777\tFixtureLock\tFixture Lock\t1700000000\t1\n"
               "127\tOverflow\tOverflow\t999999999999999999999999999\n");
    manager = make_manager(&config);
    check(manager != NULL, "db manager created");
    if (!manager) goto done;

    pz_vec_init(&mods_list, sizeof(pz_installed_mod));
    pz_mod_manager_list(manager, &mods_list);
    check(mods_list.length == 3, "corrupt installed.tsv lines skipped");
    {
        const pz_installed_mod* good = find_mod(&mods_list, 123);
        const pz_installed_mod* adopted = find_mod(&mods_list, 126);
        check(good && good->time_updated == 1700000000 && good->owned,
              "valid owned record survives");
        check(adopted && adopted->time_updated == 42 && !adopted->owned,
              "adopted record and owned flag survive");
    }
    pz_mod_manager_free_mods(&mods_list);
    check(!pz_mod_manager_locked(manager, 777), "old rows load unlocked");

    {
        uint64_t id = 777;
        result = pz_mod_manager_set_locked(manager, &id, 1, 1, NULL);
        check(result.ok && pz_mod_manager_locked(manager, 777) &&
                  pz_mod_manager_manually_locked(manager, 777),
              "manual lock applies");
        pz_mm_result_free(&result);
        path_child(&file, &data, "installed.tsv");
        pz_str_init(&text);
        read_text(&file, &text);
        check(contains(&text, "\t1\t0\n"), "manual lock persisted");
        pz_str_free(&text);
    }
    pz_mod_manager_destroy(manager);
    manager = make_manager(&config);
    check(manager && pz_mod_manager_manually_locked(manager, 777),
          "manual lock survives restart");
    if (manager) {
        result = pz_command_run(manager, "unlock 777", 10, NULL);
        check(result.ok && !pz_mod_manager_locked(manager, 777),
              "unlock by Workshop ID clears the lock");
        pz_mm_result_free(&result);
        result = pz_command_run(manager, "lock Fixture Lock", 17, NULL);
        check(result.ok && pz_mod_manager_manually_locked(manager, 777),
              "lock accepts an installed mod name");
        pz_mm_result_free(&result);
        result = pz_command_run(manager, "unlock 777", 10, NULL);
        pz_mm_result_free(&result);
        pz_mod_manager_destroy(manager);
    }

    /* A server target pins enabled items only after its switch is turned on. */
    path_child(&file, &zomboid, "Server");
    path_child(&file, &file, "local.ini");
    write_text(&file, "Mods=\\FixtureLock\nWorkshopItems=777\n");
    manager = make_manager(&config);
    check(manager != NULL, "lock manager created");
    if (manager) {
        pz_vec targets;
        pz_mod_manager* restarted;
        pz_activation_target_data* server;
        pz_vec_init(&targets, sizeof(pz_activation_target_data));
        pz_mod_manager_activation_targets(manager, &targets);
        server = find_target(&targets, 2, "local");
        check(server != NULL && !pz_mod_manager_server_locked(manager, 777) &&
                  !pz_mod_manager_locked(manager, 777),
              "server-enabled items are unlocked by default");
        if (server) {
            result = pz_mod_manager_set_target_locked(manager, 2, "local", 5, 1,
                                                      NULL);
            check(result.ok && pz_mod_manager_server_locked(manager, 777) &&
                      pz_mod_manager_locked(manager, 777),
                  "target lock pins its enabled items");
            pz_mm_result_free(&result);
            {
                pz_str reason;
                pz_str_init(&reason);
                pz_mod_manager_lock_reason(manager, 777, &reason);
                check(str_is(&reason, "target-enabled"), "target lock reason");
                pz_str_free(&reason);
            }
            path_child(&file, &data, "updates.tsv");
            write_text(&file, "777\t1\n");
            pz_mod_manager_destroy(manager);
            manager = NULL;
            /* Application startup loads config.ini before creating the manager. */
            pz_paths_config_load(&config);
            restarted = make_manager(&config);
            check(restarted && pz_mod_manager_locked(restarted, 777),
                  "target lock survives restart");
            if (restarted) {
                pz_mod_manager_destroy(restarted);
            }
            manager = make_manager(&config);
            if (manager) {
                result = pz_mod_manager_update_one(manager, 777, NULL);
                check(result.ok && contains(&result.message, "Locked (target-enabled)"),
                      "update_one skips target-locked item");
                pz_mm_result_free(&result);
                result = pz_mod_manager_update_all(manager, NULL);
                check(result.ok && contains(&result.message, "skipped 1 locked"),
                      "update_all reports locked skip");
                pz_mm_result_free(&result);
                result = pz_command_run(manager, "unlock 777", 10, NULL);
                check(result.ok && pz_mod_manager_locked(manager, 777) &&
                          !pz_mod_manager_manually_locked(manager, 777),
                      "unlock leaves target lock in place");
                pz_mm_result_free(&result);
                result = pz_mod_manager_set_target_locked(manager, 2, "local", 5,
                                                          0, NULL);
                check(result.ok && !pz_mod_manager_locked(manager, 777),
                      "turning off target lock releases items");
                pz_mm_result_free(&result);
                pz_mod_manager_destroy(manager);
                manager = NULL;
            }
        }
        pz_mod_manager_free_targets(&targets);
    }

    pz_mod_manager_destroy(manager);
    manager = make_manager(&config);
    if (manager) {
        result = pz_command_run(manager, "target-lock 1 on", 16, NULL);
        check(result.ok && pz_mod_manager_locked(manager, 777),
              "target-lock command enables a target lock");
        pz_mm_result_free(&result);
        result = pz_command_run(manager, "target-lock 1 off", 17, NULL);
        check(result.ok && !pz_mod_manager_locked(manager, 777),
              "target-lock command disables a target lock");
        pz_mm_result_free(&result);
        pz_mod_manager_destroy(manager);
    }

    path_child(&file, &zomboid, "Server");
    path_child(&file, &file, "local.ini");
    {
        pz_fs_error error;
        pz_fs_error_clear(&error);
        pz_fs_remove(&file, &error);
    }
    manager = make_manager(&config);
    check(manager && !pz_mod_manager_locked(manager, 777),
          "removing server list releases target lock");
    if (manager) pz_mod_manager_destroy(manager);

    /* Reload detects an external DB change and reports no change twice. */
    manager = make_manager(&config);
    if (manager) {
        check(!pz_mod_manager_reload_installed(manager),
              "reload without a change reports no change");
        path_child(&file, &data, "installed.tsv");
        write_text(&file, "424242\tExternalMod\tExternal Mod\t1700000000\n");
        check(pz_mod_manager_reload_installed(manager),
              "external installed.tsv change detected");
        pz_vec_init(&mods_list, sizeof(pz_installed_mod));
        pz_mod_manager_list(manager, &mods_list);
        check(mods_list.length == 1 &&
                  ((const pz_installed_mod*)pz_vec_at_const(&mods_list, 0))
                          ->workshop_id == 424242,
              "external mod appears after reload");
        pz_mod_manager_free_mods(&mods_list);
        check(!pz_mod_manager_reload_installed(manager),
              "unchanged reload reports no change");
        pz_mod_manager_destroy(manager);
    }

done:
    pz_str_free(&text);
    pz_path_free(&file);
    pz_path_free(&data);
    pz_path_free(&mods);
    pz_paths_config_free(&config);
    pz_path_free(&zomboid);
}

static void test_import(const pz_path* root) {
    pz_path zomboid;
    pz_path steam;
    pz_path content;
    pz_path acf;
    pz_path file;
    pz_paths_config config;
    pz_mod_manager* manager;
    pz_mm_result result;
    pz_vec mods;
    pz_str text;
    log_capture log;
    uint64_t targeted = 111;

    pz_str_init(&text);
    path_child(&zomboid, root, "import-zomboid");
    path_child(&steam, root, "import-steamcmd");
    config_for(&config, &zomboid);
    str_from_path(&config.steamcmd_root, &steam);
    path_child(&content, &steam, "steamapps");
    path_child(&content, &content, "workshop");
    path_child(&content, &content, "content");
    path_child(&content, &content, "108600");

    path_child(&file, &content, "111");
    path_child(&file, &file, "mods");
    path_child(&file, &file, "Alpha");
    path_child(&file, &file, "mod.info");
    write_text(&file, "id=AlphaMod\nname=Alpha\nmodversion=1.0\n");
    path_child(&file, &content, "111");
    path_child(&file, &file, "mods");
    path_child(&file, &file, "Alpha");
    path_child(&file, &file, "source-marker.txt");
    write_text(&file, "keep\n");
    path_child(&file, &content, "222");
    path_child(&file, &file, "Contents");
    path_child(&file, &file, "mods");
    path_child(&file, &file, "Beta");
    path_child(&file, &file, "mod.info");
    write_text(&file, "id=BetaMod\nname=Beta\n");
    path_child(&file, &content, "333");
    path_child(&file, &file, "mods");
    path_child(&file, &file, "EmptyCopy");
    {
        pz_fs_error error;
        pz_fs_error_clear(&error);
        pz_fs_create_directories(&file, &error);
    }

    path_child(&acf, &steam, "steamapps");
    path_child(&acf, &acf, "workshop");
    path_child(&acf, &acf, "appworkshop_108600.acf");
    write_text(&acf,
               "\"AppWorkshop\"\n{\n"
               "\t\"appid\" \"108600\"\n"
               "\t\"WorkshopItemsInstalled\"\n\t{\n"
               "\t\t\"111\"\n\t\t{\n"
               "\t\t\t\"size\" \"777\"\n"
               "\t\t\t\"timeupdated\" \"1700002222\"\n"
               "\t\t}\n"
               "\t\t\"222\"\n\t\t{\n"
               "\t\t\t\"timeupdated\" \"1700003333\"\n"
               "\t\t}\n"
               "\t\t\"333\" { }\n"
               "\t\t\"444\" { }\n"
               "\t}\n}\n");

    manager = make_manager(&config);
    check(manager != NULL, "import manager created");
    if (!manager) goto done;
    capture_init(&log);
    result = pz_mod_manager_import(manager, NULL, 0, NULL);
    check(result.ok, "SteamCMD import succeeds");
    check(contains(&result.message, "Imported 2 item(s), 2 mod folder(s)"),
          "ACF import reports both valid items");
    pz_mm_result_free(&result);

    path_child_str(&file, &config.user_mods_dir, "Alpha");
    path_child(&file, &file, "mod.info");
    check(path_exists(&file), "Alpha copied into Zomboid/mods");
    path_child_str(&file, &config.user_mods_dir, "Beta");
    path_child(&file, &file, "mod.info");
    check(path_exists(&file), "Beta copied into Zomboid/mods");
    path_child(&file, &content, "111");
    path_child(&file, &file, "mods");
    path_child(&file, &file, "Alpha");
    path_child(&file, &file, "source-marker.txt");
    check(path_exists(&file), "import leaves SteamCMD source untouched");
    pz_vec_init(&mods, sizeof(pz_installed_mod));
    pz_mod_manager_list(manager, &mods);
    check(mods.length == 2, "import tracks both copied folders");
    {
        const pz_installed_mod* alpha = find_mod(&mods, 111);
        check(alpha && alpha->imported && alpha->owned &&
                  alpha->time_updated == 1700002222,
              "imported record keeps source flag and ACF time");
    }
    pz_mod_manager_free_mods(&mods);
    path_child_str(&file, &config.user_mods_dir, "EmptyCopy");
    check(!path_exists(&file), "empty source folder without mod.info skipped");

    {
        pz_mod_manager* restarted = make_manager(&config);
        pz_vec again;
        pz_vec_init(&again, sizeof(pz_installed_mod));
        if (restarted) {
            pz_mod_manager_list(restarted, &again);
            check(again.length == 2 &&
                      ((const pz_installed_mod*)pz_vec_at_const(&again, 0))->imported,
                  "imported flag survives installed.tsv reload");
            pz_mod_manager_free_mods(&again);
            result = pz_mod_manager_import(restarted, NULL, 0, NULL);
            check(result.ok && contains(&result.message, "Imported 0 item(s)"),
                  "second import skips tracked IDs");
            pz_mm_result_free(&result);
            pz_mod_manager_destroy(restarted);
        } else {
            pz_vec_free(&again);
            check(0, "restarted import manager created");
        }
    }

    /* A locally copied folder matches an empty SteamCMD folder by name. */
    path_child_str(&file, &config.user_mods_dir, "EmptyCopy");
    path_child(&file, &file, "mod.info");
    write_text(&file, "id=EmptyCopy\nname=Already copied\n");
    path_child_str(&file, &config.user_mods_dir, "EmptyCopy");
    path_child(&file, &file, "manual-marker.txt");
    write_text(&file, "keep\n");
    result = pz_mod_manager_import(manager, NULL, 0, NULL);
    check(result.ok && contains(&result.message, "1 already installed"),
          "empty source matches copied Zomboid folder");
    pz_mm_result_free(&result);
    path_child_str(&file, &config.user_mods_dir, "EmptyCopy");
    path_child(&file, &file, "manual-marker.txt");
    check(path_exists(&file), "matching copied folder is not rewritten");

    result = pz_mod_manager_remove(manager, 111, NULL);
    check(result.ok, "imported record removed");
    pz_mm_result_free(&result);
    path_child_str(&file, &config.user_mods_dir, "Alpha");
    check(!path_exists(&file), "removing imported record deletes Zomboid copy");
    path_child(&file, &content, "111");
    path_child(&file, &file, "mods");
    path_child(&file, &file, "Alpha");
    path_child(&file, &file, "source-marker.txt");
    check(path_exists(&file), "removing imported record leaves source alone");

    capture_free(&log);
    capture_init(&log);
    result = pz_mod_manager_import(manager, &targeted, 1, NULL);
    check(result.ok && contains(&result.message, "Imported 1 item(s), 1 mod folder(s)"),
          "targeted import restores one item");
    pz_mm_result_free(&result);
    path_child_str(&file, &config.user_mods_dir, "Alpha");
    path_child(&file, &file, "mod.info");
    check(path_exists(&file), "targeted import restores Alpha");

    {
        pz_fs_error error;
        pz_fs_error_clear(&error);
        pz_fs_remove(&acf, &error);
    }
    path_child(&file, &content, "666");
    path_child(&file, &file, "mod.info");
    write_text(&file, "id=GammaMod\nname=Gamma\n");
    result = pz_mod_manager_import(manager, NULL, 0, NULL);
    check(result.ok, "content-folder import succeeds without ACF");
    pz_mm_result_free(&result);
    path_child_str(&file, &config.user_mods_dir, "GammaMod");
    path_child(&file, &file, "mod.info");
    check(path_exists(&file), "numeric content folder imported without ACF");

    pz_str_clear(&config.steamcmd_root);
    {
        pz_mod_manager* no_root = make_manager(&config);
        if (no_root) {
            result = pz_mod_manager_import(no_root, NULL, 0, NULL);
            check(!result.ok && contains(&result.message, "download root"),
                  "import requires the SteamCMD download root");
            pz_mm_result_free(&result);
            pz_mod_manager_destroy(no_root);
        }
    }

    pz_mod_manager_destroy(manager);
    manager = NULL;
    capture_free(&log);
done:
    pz_str_free(&text);
    pz_path_free(&acf);
    pz_path_free(&file);
    pz_path_free(&content);
    pz_path_free(&steam);
    pz_paths_config_free(&config);
    pz_path_free(&zomboid);
}

static void test_activation_integration(const pz_path* root) {
    pz_path zomboid;
    pz_path mods;
    pz_path data;
    pz_path file;
    pz_paths_config config;
    pz_mod_manager* manager;
    pz_vec targets;
    pz_activation_target_data* defaults;
    pz_activation_target_data* save;
    pz_activation_target_data* server;
    pz_mm_result result;
    pz_str text;
    log_capture log;

    pz_str_init(&text);
    path_child(&zomboid, root, "activation-integration");
    config_for(&config, &zomboid);
    path_child(&file, &zomboid, "version.txt");
    write_text(&file, "42.21.0\n");
    path_child(&mods, &zomboid, "mods");
    path_child(&data, &zomboid, "pzmodmgr");
    path_child(&file, &mods, "default.txt");
    write_text(&file, "VERSION = 1,\n\nmods\n{\n}\n\nmaps\n{\n}\n");
    path_child(&file, &zomboid, "Saves");
    path_child(&file, &file, "Apocalypse");
    path_child(&file, &file, "Save1");
    path_child(&file, &file, "players.db");
    write_text(&file, "");
    path_child(&file, &zomboid, "Saves");
    path_child(&file, &file, "Apocalypse");
    path_child(&file, &file, "Save1");
    path_child(&file, &file, "thumb.png");
    write_text(&file, "preview");
    path_child(&file, &zomboid, "Server");
    path_child(&file, &file, "servertest.ini");
    write_text(&file, "Mods=\nWorkshopItems=\n");

    manager = make_manager(&config);
    check(manager != NULL, "activation manager created");
    if (!manager) goto done;
    pz_vec_init(&targets, sizeof(pz_activation_target_data));
    pz_mod_manager_activation_targets(manager, &targets);
    check(targets.length == 3, "default, save, and server targets discovered");
    defaults = find_target(&targets, 0, "");
    save = find_target(&targets, 1, "Apocalypse/Save1");
    server = find_target(&targets, 2, "servertest");
    check(defaults != NULL, "default target discovered");
    check(save != NULL, "save target discovered");
    check(server != NULL, "server target discovered");
    if (save) {
        pz_path thumb;
        path_child(&thumb, &zomboid, "Saves");
        path_child(&thumb, &thumb, "Apocalypse");
        path_child(&thumb, &thumb, "Save1");
        path_child(&thumb, &thumb, "thumb.png");
        check(str_is(&save->thumbnail, thumb.text.data), "save thumbnail discovered");
        pz_path_free(&thumb);
    }

    if (defaults) {
        pz_vec add;
        pz_vec_init(&add, sizeof(pz_str));
        {
            pz_str* value = (pz_str*)pz_vec_push(&add);
            pz_str_init(value);
            pz_str_assign_cstr(value, "Foo");
        }
        check(pz_activation_targets_write(defaults, &add, NULL, 1),
              "default.txt enable writes");
        pz_path target_path;
        path_text(&target_path, defaults->path.data);
        check(read_text(&target_path, &text) && contains(&text, "mod = Foo,"),
              "default.txt contains plain enabled Mod ID");
        pz_path_free(&target_path);
        pz_str_free(&text);
        memset(&text, 0, sizeof text);
        pz_activation_string_list_free(&add);
    }

    /* Legacy backslash IDs are repaired during discovery. */
    path_child(&file, &mods, "default.txt");
    write_text(&file, "VERSION = 1,\n\nmods\n{\n    mod = \\Plain,\n}\n");
    pz_mod_manager_free_targets(&targets);
    pz_vec_init(&targets, sizeof(pz_activation_target_data));
    pz_mod_manager_activation_targets(manager, &targets);
    defaults = find_target(&targets, 0, "");
    save = find_target(&targets, 1, "Apocalypse/Save1");
    server = find_target(&targets, 2, "servertest");
    check(defaults != NULL, "default target rediscovered");
    if (defaults) {
        pz_path target_path;
        path_text(&target_path, defaults->path.data);
        check(read_text(&target_path, &text) &&
                  contains(&text, "mod = Plain,") &&
                  !contains(&text, "mod = \\Plain,"),
              "default.txt discovery repairs backslash IDs");
        pz_str_free(&text);
        memset(&text, 0, sizeof text);
        pz_path_free(&target_path);
    }

    if (save) {
        pz_vec add;
        pz_vec_init(&add, sizeof(pz_str));
        {
            pz_str* value = (pz_str*)pz_vec_push(&add);
            pz_str_init(value);
            pz_str_assign_cstr(value, "Bar");
        }
        check(pz_activation_targets_write(save, &add, NULL, 1),
              "save enable writes");
        pz_path target_path;
        path_text(&target_path, save->path.data);
        check(read_text(&target_path, &text) && contains(&text, "mod = Bar,"),
              "save mods.txt uses plain Mod IDs");
        pz_str_free(&text);
        memset(&text, 0, sizeof text);
        pz_path_free(&target_path);
        pz_activation_string_list_free(&add);
    }

    if (server) {
        pz_vec add;
        pz_vec workshops;
        uint64_t id = 123;
        pz_vec_init(&add, sizeof(pz_str));
        pz_vec_init(&workshops, sizeof(uint64_t));
        {
            pz_str* value = (pz_str*)pz_vec_push(&add);
            pz_str_init(value);
            pz_str_assign_cstr(value, "Baz");
        }
        pz_vec_push_copy(&workshops, &id);
        check(pz_activation_targets_write(server, &add, &workshops, 1),
              "server enable writes both lists");
        {
            pz_path target_path;
            path_text(&target_path, server->path.data);
            read_text(&target_path, &text);
            check(contains(&text, "Mods=\\Baz"),
                  "server uses the B42 backslash form");
            check(contains(&text, "WorkshopItems=123"),
                  "server WorkshopItems receives the ID");
            check(pz_activation_targets_write(server, &add, NULL, 0) &&
                      read_text(&target_path, &text) && !contains(&text, "Baz"),
                  "server disable removes the Mod ID but keeps WorkshopItems");
            pz_path_free(&target_path);
            pz_str_free(&text);
            memset(&text, 0, sizeof text);
        }
        pz_vec_free(&workshops);
        pz_activation_string_list_free(&add);
    }

    /* Recursive requirements are enabled in both Default and the save. */
    path_child_str(&file, &config.user_mods_dir, "BaseReq");
    path_child(&file, &file, "mod.info");
    write_text(&file, "id=BaseReq\nname=Base Requirement\n");
    path_child_str(&file, &config.user_mods_dir, "ExtraReq");
    path_child(&file, &file, "mod.info");
    write_text(&file, "id=ExtraReq\nname=Extra Requirement\nrequire=BaseReq\n");
    path_child_str(&file, &config.user_mods_dir, "Owner");
    path_child(&file, &file, "mod.info");
    write_text(&file, "id=OwnerMod\nname=Owner\nrequire=ExtraReq\n");
    capture_init(&log);
    {
        const char* folders[] = {"Owner"};
        size_t folder_lengths[] = {5};
        result = pz_mod_manager_set_selection_enabled(
            manager, 1, "Apocalypse/Save1", strlen("Apocalypse/Save1"), NULL,
            0, folders, folder_lengths, 1, 1,
            &(pz_log){capture_line, &log});
        check(result.ok, "save enable succeeds");
        pz_mm_result_free(&result);
    }
    path_child(&file, &mods, "default.txt");
    read_text(&file, &text);
    check(contains(&text, "mod = OwnerMod,"), "save enable writes Owner to Default");
    check(contains(&text, "mod = ExtraReq,"), "save enable writes Extra to Default");
    check(contains(&text, "mod = BaseReq,"), "save enable writes Base to Default");
    pz_str_free(&text);
    memset(&text, 0, sizeof text);
    path_child(&file, &zomboid, "Saves");
    path_child(&file, &file, "Apocalypse");
    path_child(&file, &file, "Save1");
    path_child(&file, &file, "mods.txt");
    read_text(&file, &text);
    check(contains(&text, "mod = OwnerMod,"), "save writes Owner");
    check(contains(&text, "mod = ExtraReq,"), "save writes Extra");
    check(contains(&text, "mod = BaseReq,"), "save writes Base");
    check(captured(&log, "auto-enabled required mod ExtraReq (needed by OwnerMod)"),
          "first required mod auto-enable is logged");
    check(captured(&log, "auto-enabled required mod BaseReq (needed by ExtraReq)"),
          "recursive required mod auto-enable is logged");
    pz_str_free(&text);
    memset(&text, 0, sizeof text);
    {
        const char* folders[] = {"Owner"};
        size_t folder_lengths[] = {5};
        result = pz_mod_manager_set_selection_enabled(
            manager, 1, "Apocalypse/Save1", strlen("Apocalypse/Save1"), NULL,
            0, folders, folder_lengths, 1, 0, NULL);
        check(result.ok, "save disable succeeds");
        pz_mm_result_free(&result);
    }
    path_child(&file, &zomboid, "Saves");
    path_child(&file, &file, "Apocalypse");
    path_child(&file, &file, "Save1");
    path_child(&file, &file, "mods.txt");
    read_text(&file, &text);
    check(!contains(&text, "mod = OwnerMod,"), "save disable removes selected mod");
    pz_str_free(&text);
    memset(&text, 0, sizeof text);
    path_child(&file, &mods, "default.txt");
    read_text(&file, &text);
    check(contains(&text, "mod = OwnerMod,"), "save disable leaves Default alone");
    pz_str_free(&text);
    memset(&text, 0, sizeof text);
    capture_free(&log);

    pz_mod_manager_free_targets(&targets);
    pz_mod_manager_destroy(manager);
    manager = NULL;
done:
    pz_path_free(&file);
    pz_path_free(&data);
    pz_path_free(&mods);
    pz_paths_config_free(&config);
    pz_path_free(&zomboid);
}

static void set_env(const char* name, const char* value) {
    if (value)
        check(setenv(name, value, 1) == 0, name);
    else
        unsetenv(name);
}

static void unset_env(const char* name) {
    unsetenv(name);
}

static void set_json_time(uint64_t when) {
    char json[512];
    snprintf(json, sizeof json,
             "{\"response\":{\"publishedfiledetails\":[{\"publishedfileid\":\"123456789\","
             "\"result\":1,\"title\":\"Fixture\",\"time_created\":1600000000,"
             "\"time_updated\":%llu}]}}",
             (unsigned long long)when);
    set_env("FAKE_CURL_JSON", json);
}

static void test_install_and_update(const pz_path* root) {
    pz_path zomboid;
    pz_paths_config config;
    pz_mod_manager* manager;
    pz_mod_manager* restarted;
    pz_mm_result result;
    pz_vec mods;
    pz_str text;
    pz_path file;
    pz_path fake;
    pz_path bin;
    pz_path curl_log;
    pz_path steam_log;
    const char* old_path;
    log_capture log;
    uint64_t id;

    pz_str_init(&text);
    path_child(&zomboid, root, "install-zomboid");
    config_for(&config, &zomboid);
    path_child(&file, &zomboid, "version.txt");
    write_text(&file, "42.21.0 4a0e9546ec\n");

    path_child(&fake, root, "fake_steamcmd.sh");
    {
        pz_path source;
        pz_path_init(&source);
        pz_path_assign_cstr(&source, FAKE_STEAMCMD_PATH);
        read_text(&source, &text);
        write_text(&fake, text.data);
        pz_str_clear(&text);
        pz_path_free(&source);
    }
    make_executable(&fake);
    str_from_path(&config.steamcmd_exe, &fake);
    pz_str_assign_cstr(&config.steam_user, "test-user");

    path_child(&bin, root, "bin");
    path_child(&curl_log, root, "curl.log");
    path_child(&steam_log, root, "steamcmd.log");
    path_child(&file, &bin, "curl");
    write_text(&file,
               "#!/bin/sh\n"
               "if [ -n \"${FAKE_CURL_LOG:-}\" ]; then printf '%s\\n' \"$*\" >> \"$FAKE_CURL_LOG\"; fi\n"
               "case \"$*\" in\n"
               "  *api.github.com/repos/sigods/pzmodmgr/releases/latest*)\n"
               "    [ -n \"${FAKE_CURL_APP_JSON:-}\" ] && printf '%s\\n' \"$FAKE_CURL_APP_JSON\" ;;\n"
               "  *collectioncount*)\n"
               "    [ -n \"${FAKE_CURL_COLLECTION_JSON:-}\" ] && printf '%s\\n' \"$FAKE_CURL_COLLECTION_JSON\" ;;\n"
               "  *) [ -n \"${FAKE_CURL_JSON:-}\" ] && printf '%s\\n' \"$FAKE_CURL_JSON\" ;;\n"
               "esac\n"
               "exit \"${FAKE_CURL_EXIT:-0}\"\n");
    make_executable(&file);

    old_path = getenv("PATH");
    {
        pz_str path;
        pz_str_init(&path);
        pz_str_assign(&path, bin.text.data, bin.text.length);
        pz_str_append_cstr(&path, ":");
        pz_str_append_cstr(&path, old_path ? old_path : "/usr/bin:/bin");
        set_env("PATH", path.data);
        pz_str_free(&path);
    }
    set_env("FAKE_STEAMCMD_LOG", steam_log.text.data);
    set_env("FAKE_CURL_LOG", curl_log.text.data);
    set_env("FAKE_CURL_EXIT", "0");
    set_env("FAKE_STEAMCMD_MOD_VERSION", "1.0");
    set_json_time(1700000000);

    path_child_str(&file, &config.user_mods_dir, "not-ours");
    path_child(&file, &file, "mod.info");
    write_text(&file, "id=notours\nname=Not Ours\n");

    manager = make_manager(&config);
    check(manager != NULL, "install manager created");
    if (!manager) goto done;
    capture_init(&log);
    result = pz_mod_manager_add(
        manager,
        "https://steamcommunity.com/sharedfiles/filedetails/?id=123456789",
        strlen("https://steamcommunity.com/sharedfiles/filedetails/?id=123456789"),
        &(pz_log){capture_line, &log});
    check(result.ok, "add succeeds with fake steamcmd");
    pz_mm_result_free(&result);
    path_child_str(&file, &config.user_mods_dir, "FixtureMod");
    path_child(&file, &file, "mod.info");
    check(path_exists(&file), "mod folder installed");
    path_child_str(&file, &config.data_dir, "steam_cache");
    path_child(&file, &file, "123456789");
    check(!path_exists(&file), "per-download cache removed after install");
    path_child_str(&file, &config.data_dir, "installed.tsv");
    check(path_exists(&file), "installed.tsv written");
    pz_vec_init(&mods, sizeof(pz_installed_mod));
    pz_mod_manager_list(manager, &mods);
    check(mods.length == 1, "one db record after add");
    id = 123456789;
    {
        const pz_installed_mod* installed = find_mod(&mods, id);
        check(installed && installed->time_updated == 1700000000,
              "add records remote time_updated");
    }
    pz_mod_manager_free_mods(&mods);
    path_child_str(&file, &config.data_dir, "curl.log");
    read_text(&curl_log, &text);
    check(contains(&text, "itemcount=1&publishedfileids[0]=123456789"),
          "Steam API POST body built");
    pz_str_clear(&text);
    result = pz_mod_manager_add(manager, "123456789", 9,
                                &(pz_log){capture_line, &log});
    check(!result.ok, "duplicate add rejected");
    pz_mm_result_free(&result);

    check(pz_mod_manager_update_check_due(manager), "first update check is due");
    set_json_time(1700000500);
    result = pz_mod_manager_check_updates(manager,
                                          &(pz_log){capture_line, &log});
    check(result.ok, "check_updates succeeds");
    pz_mm_result_free(&result);
    check(pz_mod_manager_update_available(manager, id),
          "update availability recorded");
    path_child_str(&file, &config.data_dir, "updates.tsv");
    check(path_exists(&file), "update availability persisted");
    restarted = make_manager(&config);
    check(restarted && pz_mod_manager_update_available(restarted, id),
          "update availability survives restart");
    if (restarted) pz_mod_manager_destroy(restarted);

    set_env("FAKE_CURL_EXIT", "1");
    result = pz_mod_manager_check_updates(manager,
                                          &(pz_log){capture_line, &log});
    check(result.ok && pz_mod_manager_update_available(manager, id),
          "failed check preserves known update");
    pz_mm_result_free(&result);
    set_env("FAKE_CURL_EXIT", "0");
    set_json_time(1700000500);

    /* Unchanged remote time: no download, no backup. */
    set_json_time(1700000000);
    write_text(&steam_log, "");
    result = pz_mod_manager_update_all(manager, &(pz_log){capture_line, &log});
    check(result.ok, "update_all succeeds when unchanged");
    pz_mm_result_free(&result);
    {
        const int empty = read_text(&steam_log, &text) && text.length == 0;
        if (!empty)
            fprintf(stderr, "steamcmd after unchanged update: %s\n",
                    text.data ? text.data : "(unreadable)");
        check(empty, "unchanged mod is not downloaded");
    }
    pz_str_clear(&text);

    /* New time, same version: inspect download and keep files. */
    set_json_time(1700000500);
    result = pz_mod_manager_update_all(manager, &(pz_log){capture_line, &log});
    check(result.ok, "same-version update succeeds");
    pz_mm_result_free(&result);
    pz_vec_init(&mods, sizeof(pz_installed_mod));
    pz_mod_manager_list(manager, &mods);
    {
        const pz_installed_mod* installed = find_mod(&mods, id);
        check(installed && installed->time_updated == 1700000500,
              "same-version update records new time");
    }
    pz_mod_manager_free_mods(&mods);
    path_child_str(&file, &config.data_dir, "backups");
    check(!path_exists(&file), "same-version update keeps no backup");

    /* A changed version replaces the folder and clears update availability. */
    set_env("FAKE_STEAMCMD_MOD_VERSION", "2.0");
    set_json_time(1700000600);
    result = pz_mod_manager_update_all(manager, &(pz_log){capture_line, &log});
    check(result.ok, "version update succeeds");
    pz_mm_result_free(&result);
    check(!path_exists(&file), "successful update removes temporary backup");
    check(!pz_mod_manager_update_available(manager, id),
          "successful update clears update availability");

    /* API failure skips the item without downloading. */
    read_text(&steam_log, &text);
    {
        const size_t before = text.length;
        pz_str_clear(&text);
        set_env("FAKE_CURL_EXIT", "1");
        result = pz_mod_manager_update_all(manager,
                                           &(pz_log){capture_line, &log});
        check(result.ok && captured(&log, "update check failed: 123456789 (skipped)"),
              "API failure reported and skipped");
        pz_mm_result_free(&result);
        check(read_text(&steam_log, &text) && text.length == before,
              "no download when check fails");
        pz_str_clear(&text);
        set_env("FAKE_CURL_EXIT", "0");
    }

    /* Single-item update through the command parser. */
    set_json_time(1700000900);
    result = pz_command_run(manager, "update 123456789", 16,
                            &(pz_log){capture_line, &log});
    check(result.ok && contains(&result.message, "Already up to date"),
          "single update reports unchanged version");
    pz_mm_result_free(&result);

    /* Failed download and unavailable item change nothing in the DB. */
    result = pz_mod_manager_add(manager, "555", 3,
                                &(pz_log){capture_line, &log});
    check(!result.ok && contains(&result.message, "steamcmd failed"),
          "failed download reported");
    pz_mm_result_free(&result);
    set_env("FAKE_CURL_JSON",
            "{\"response\":{\"result\":1,\"resultcount\":1,\"publishedfiledetails\":[{\"publishedfileid\":\"999\",\"result\":9}]}}");
    result = pz_mod_manager_add(manager, "999", 3,
                                &(pz_log){capture_line, &log});
    check(!result.ok && contains(&result.message, "unavailable"),
          "unavailable item reported before download");
    pz_mm_result_free(&result);
    set_json_time(1700000900);

    /* Bare URL/id and batch add through the command parser. */
    set_env("FAKE_STEAMCMD_PER_ITEM", "1");
    result = pz_command_run(
        manager,
        "https://steamcommunity.com/sharedfiles/filedetails/?id=333",
        strlen("https://steamcommunity.com/sharedfiles/filedetails/?id=333"),
        &(pz_log){capture_line, &log});
    check(result.ok, "bare workshop URL acts as add");
    pz_mm_result_free(&result);
    path_child_str(&file, &config.user_mods_dir, "FixtureMod333");
    path_child(&file, &file, "mod.info");
    check(path_exists(&file), "bare URL installed");
    result = pz_command_run(manager, "add 444 445", 11,
                            &(pz_log){capture_line, &log});
    check(result.ok && contains(&result.message, "Installed 2 of 2"),
          "batch add installs both items");
    pz_mm_result_free(&result);
    result = pz_command_run(manager, "remove 333 444 445", 18,
                            &(pz_log){capture_line, &log});
    check(result.ok, "batch remove succeeds");
    pz_mm_result_free(&result);

    /* Collection expansion and duplicate-child skip. */
    set_env("FAKE_CURL_COLLECTION_JSON",
            "{\"response\":{\"result\":1,\"resultcount\":1,\"collectiondetails\":[{\"publishedfileid\":\"3417834390\",\"result\":1,\"children\":[{\"publishedfileid\":\"111\",\"sortorder\":0,\"filetype\":0},{\"publishedfileid\":\"222\",\"sortorder\":1,\"filetype\":0}]}]}}");
    result = pz_mod_manager_add(manager, "3417834390", 10,
                                &(pz_log){capture_line, &log});
    check(result.ok, "collection add succeeds");
    pz_mm_result_free(&result);
    path_child_str(&file, &config.user_mods_dir, "FixtureMod111");
    path_child(&file, &file, "mod.info");
    check(path_exists(&file), "collection child 111 installed");
    path_child_str(&file, &config.user_mods_dir, "FixtureMod222");
    path_child(&file, &file, "mod.info");
    check(path_exists(&file), "collection child 222 installed");
    result = pz_command_run(manager, "remove 111 222", 14,
                            &(pz_log){capture_line, &log});
    check(result.ok, "collection children removed");
    pz_mm_result_free(&result);
    unset_env("FAKE_CURL_COLLECTION_JSON");

    /* One item with two folders, then an update that drops one variant. */
    set_env("FAKE_STEAMCMD_EXTRA_MOD", "VariantB");
    set_json_time(1700001100);
    result = pz_mod_manager_add(manager, "1010", 4,
                                &(pz_log){capture_line, &log});
    check(result.ok, "multi-folder install succeeds");
    pz_mm_result_free(&result);
    path_child_str(&file, &config.user_mods_dir, "FixtureMod1010");
    path_child(&file, &file, "mod.info");
    check(path_exists(&file), "primary variant installed");
    path_child_str(&file, &config.user_mods_dir, "VariantB");
    path_child(&file, &file, "mod.info");
    check(path_exists(&file), "sibling variant installed");
    unset_env("FAKE_STEAMCMD_EXTRA_MOD");
    set_json_time(1700001200);
    result = pz_mod_manager_update_one(manager, 1010,
                                       &(pz_log){capture_line, &log});
    check(result.ok, "multi-folder update succeeds");
    pz_mm_result_free(&result);
    path_child_str(&file, &config.user_mods_dir, "VariantB");
    check(!path_exists(&file), "variant no longer shipped is removed");
    result = pz_mod_manager_remove(manager, 1010,
                                   &(pz_log){capture_line, &log});
    pz_mm_result_free(&result);
    unset_env("FAKE_STEAMCMD_PER_ITEM");

    /* Auto-enable writes the Mod ID and remove cleans all activation lists. */
    {
        pz_paths_config enabled;
        pz_paths_config_init(&enabled);
        pz_mod_manager_get_config(manager, &enabled);
        enabled.auto_enable = 1;
        pz_mod_manager_set_config(manager, &enabled);
        pz_paths_config_free(&enabled);
    }
    set_env("FAKE_STEAMCMD_PER_ITEM", "1");
    set_json_time(1700001300);
    result = pz_mod_manager_add(manager, "2020", 4,
                                &(pz_log){capture_line, &log});
    check(result.ok, "auto-enable install succeeds");
    pz_mm_result_free(&result);
    result = pz_mod_manager_remove(manager, 2020,
                                   &(pz_log){capture_line, &log});
    check(result.ok, "auto-enabled mod removed");
    pz_mm_result_free(&result);
    unset_env("FAKE_STEAMCMD_PER_ITEM");

    /* Settings save through the command parser. */
    result = pz_command_run(manager, "config steam_user ui-user", 25, NULL);
    check(result.ok, "config command saves Steam user");
    pz_mm_result_free(&result);
    path_child_str(&file, &config.data_dir, "config.ini");
    read_text(&file, &text);
    check(contains(&text, "steam_user=ui-user"), "Steam user persisted");
    pz_str_clear(&text);

    /* Daily check records an available app release. */
    set_env("FAKE_CURL_APP_JSON",
            "{\"tag_name\":\"v99.0.0\",\"assets\":[{\"name\":\"PZModManager-99.0.0-linux-x86_64.tar.gz\",\"browser_download_url\":\"https://example/linux.tar.gz\",\"size\":1,\"digest\":\"sha256:abc\"}]}");
    result = pz_command_run(manager, "check", 5,
                            &(pz_log){capture_line, &log});
    check(result.ok, "check with app release succeeds");
    pz_mm_result_free(&result);
    check(pz_mod_manager_app_update_available(manager),
          "app update availability recorded");
    path_child_str(&file, &config.data_dir, "app_update");
    check(path_exists(&file), "app update state persisted");
    unset_env("FAKE_CURL_APP_JSON");

    /* Remove deletes only the recorded folder and disables managed lists. */
    result = pz_command_run(manager, "remove 123456789", 16,
                            &(pz_log){capture_line, &log});
    check(result.ok, "remove command succeeds");
    pz_mm_result_free(&result);
    path_child_str(&file, &config.user_mods_dir, "FixtureMod");
    check(!path_exists(&file), "installed folder removed");
    path_child_str(&file, &config.user_mods_dir, "not-ours");
    check(path_exists(&file), "unmanaged folder preserved");
    check(mod_count(manager) == 0, "db empty after remove");

    capture_free(&log);
    pz_mod_manager_destroy(manager);
    manager = NULL;
    set_env("PATH", old_path);
    unset_env("FAKE_STEAMCMD_LOG");
    unset_env("FAKE_CURL_LOG");
    unset_env("FAKE_CURL_JSON");
    unset_env("FAKE_CURL_EXIT");
    unset_env("FAKE_STEAMCMD_MOD_VERSION");
    unset_env("FAKE_STEAMCMD_PER_ITEM");
    unset_env("FAKE_STEAMCMD_EXTRA_MOD");
done:
    pz_str_free(&text);
    pz_path_free(&file);
    pz_path_free(&steam_log);
    pz_path_free(&curl_log);
    pz_path_free(&bin);
    pz_path_free(&fake);
    pz_paths_config_free(&config);
    pz_path_free(&zomboid);
}

int main(void) {
    char root_buffer[128];
    pz_path root;

    snprintf(root_buffer, sizeof root_buffer, "/tmp/pzmodmgr-c-test-XXXXXX");
    if (!mkdtemp(root_buffer)) {
        perror("mkdtemp");
        return 1;
    }
    path_text(&root, root_buffer);

    test_workshop_ids();
    test_config_paths(&root);
    test_mod_info_fs(&root);
    test_cache_cleanup(&root);
    test_db_and_locks(&root);
    test_import(&root);
    test_activation_integration(&root);
    test_install_and_update(&root);

    {
        pz_fs_error error;
        pz_fs_error_clear(&error);
        pz_fs_remove_all(&root, NULL, &error);
    }
    pz_path_free(&root);

    if (failures != 0) {
        fprintf(stderr, "%d C core test(s) failed\n", failures);
        return 1;
    }
    printf("C core tests passed\n");
    return 0;
}
