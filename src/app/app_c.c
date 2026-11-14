#include "app/app_c.h"

#include "core/activation_targets_c.h"
#include "core/commands_c.h"
#include "core/fs_c.h"
#include "core/mod_info_fs_c.h"
#include "core/mod_manager_c.h"
#include "core/paths_c.h"
#include "core/steamcmd_c.h"
#include "core/updater_c.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include "ui/win32/win32_ui_c.h"
#else
#include "ui/x11/x11_ui_c.h"
#endif

#if !defined(PZMODMGR_VERSION)
#define PZMODMGR_VERSION "0.0.0"
#endif

typedef struct pz_app_context {
    pz_paths_config config;
} pz_app_context;

static int read_mod_info_cb(void* context, const char* path, size_t path_length,
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

static int discover_mod_roots_cb(void* context, const char* path,
                                 size_t path_length, pz_vec* out) {
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
    pz_app_context* app = (pz_app_context*)context;
    pz_path data_dir;
    pz_str exe;
    pz_download_outcome outcome;
    int ok;

    pz_path_init(&data_dir);
    pz_str_init(&exe);
    pz_download_outcome_init(&outcome);
    pz_path_assign(&data_dir, config->data_dir.data, config->data_dir.length);
    pz_str_assign(&exe, config->steamcmd_exe.data, config->steamcmd_exe.length);

    ok = pz_steamcmd_download_workshop(
        &data_dir, &exe, &config->steam_user, workshop_id, install_dir, log,
        &outcome);
    if (app) {
        pz_str_assign(&app->config.steamcmd_exe, exe.data, exe.length);
    }
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
    const char* version = PZMODMGR_VERSION;
    const char* platform = pz_updater_platform_name();
    (void)context;
    pz_app_update_init(out);
    return pz_updater_check(version, strlen(version), platform, strlen(platform),
                            out)
               ? 1
               : 0;
}

static int save_config_cb(void* context, const pz_paths_config* config) {
    pz_app_context* app = (pz_app_context*)context;
    pz_paths_config copy;

    if (!app || !config) return 0;
    if (!pz_paths_config_copy(&copy, config)) return 0;
    pz_paths_config_free(&app->config);
    app->config = copy;
    return pz_paths_config_save(&app->config);
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

static void cleanup_update_cache(const pz_paths_config* config) {
    pz_path data_dir;
    pz_path_init(&data_dir);
    if (pz_path_assign(&data_dir, config->data_dir.data,
                       config->data_dir.length))
        pz_updater_cleanup_cache(&data_dir);
    pz_path_free(&data_dir);
}

static void print_log(void* context, const char* line) {
    (void)context;
    fputs(line ? line : "", stdout);
    fputc('\n', stdout);
}

static int run_cli(pz_mod_manager* manager, int argc, char** argv) {
    pz_str line;
    pz_mm_result result;
    pz_log log;
    int i;
    int code;

    pz_str_init(&line);
    for (i = 1; i < argc; ++i) {
        if (i > 1 && !pz_str_append_char(&line, ' ')) break;
        if (!pz_str_append_cstr(&line, argv[i])) break;
    }
    pz_log_init(&log);
    pz_log_set(&log, print_log, NULL);
    result = pz_command_run(manager, line.data, line.length, &log);
    fputs(result.ok ? "OK: " : "ERROR: ", stdout);
    fputs(result.message.data ? result.message.data : "", stdout);
    fputc('\n', stdout);
    code = result.ok ? 0 : 1;
    pz_mm_result_free(&result);
    pz_str_free(&line);
    return code;
}

int pz_app_run(int argc, char** argv) {
    pz_app_context app;
    pz_mm_services services;
    pz_mod_manager* manager;
    int code = 1;

    if (pz_updater_is_helper_invocation(argc, argv))
        return pz_updater_run_helper(argc, argv);

    memset(&app, 0, sizeof(app));
    pz_paths_config_init(&app.config);
    if (!pz_paths_config_default(&app.config)) goto done_config;
    if (!pz_paths_config_load(&app.config)) {
        /* A missing config is normal; a broken one keeps the defaults. */
    }
    cleanup_update_cache(&app.config);

    memset(&services, 0, sizeof(services));
    services.context = &app;
    services.read_mod_info = read_mod_info_cb;
    services.discover_mod_roots = discover_mod_roots_cb;
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

    manager = pz_mod_manager_create(&app.config, &services);
    if (!manager) goto done_config;

    if (argc > 1) {
        code = run_cli(manager, argc, argv);
    } else {
#ifdef _WIN32
        code = pz_win32_run_ui(manager);
#else
        code = pz_x11_run_ui(manager);
#endif
    }

    pz_mod_manager_destroy(manager);

done_config:
    pz_paths_config_free(&app.config);
    return code;
}
