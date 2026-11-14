#define _POSIX_C_SOURCE 200809L

#include "core/steamcmd_c.h"

#include "core/net_c.h"
#include "core/paths_c.h"
#include "core/process_c.h"
#include "core/steam_json_c.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

static const char* const kDetailsUrl =
    "https://api.steampowered.com/ISteamRemoteStorage/GetPublishedFileDetails/v1/";
static const char* const kCollectionUrl =
    "https://api.steampowered.com/ISteamRemoteStorage/GetCollectionDetails/v1/";

/* ------------------------------------------------------------- utilities */

static int assign_string(pz_str* out, const char* data, size_t length) {
    if (!data) length = 0;
    return pz_str_assign(out, data, length);
}

static int join_leaf(pz_path* out, const pz_path* base, const char* leaf) {
    return pz_path_join(out, base, leaf, strlen(leaf));
}

/* Appends one component without aliasing out and its base. */
static int append_leaf(pz_path* path, const char* leaf) {
    pz_path next;
    int ok;

    pz_path_init(&next);
    ok = pz_path_join(&next, path, leaf, strlen(leaf)) &&
         pz_path_assign(path, next.text.data, next.text.length);
    pz_path_free(&next);
    return ok;
}

static int is_file(const pz_path* path) {
    pz_fs_kind kind;
    pz_fs_error error;

    pz_fs_error_clear(&error);
    return pz_fs_kind_of(path, &kind, &error) && kind == PZ_FS_FILE;
}

static int is_directory(const pz_path* path) {
    pz_fs_kind kind;
    pz_fs_error error;

    pz_fs_error_clear(&error);
    return pz_fs_kind_of(path, &kind, &error) && kind == PZ_FS_DIR;
}

static int is_empty(const pz_path* path) {
    int empty = 0;
    pz_fs_error error;

    pz_fs_error_clear(&error);
    return pz_fs_is_empty(path, &empty, &error) && empty != 0;
}

static int executable_text(const char* text, size_t length) {
    pz_path path;
    pz_fs_error error;
    int executable = 0;
    int ok;

    pz_path_init(&path);
    pz_fs_error_clear(&error);
    ok = pz_path_assign(&path, text, length) &&
         pz_fs_is_executable(&path, &executable, &error) && executable != 0;
    pz_path_free(&path);
    return ok;
}

static int try_path(pz_str* out, const char* base, size_t base_length,
                    const char* name) {
    pz_str candidate;
    int ok = 0;

    pz_str_init(&candidate);
    if (pz_paths_join(&candidate, base, base_length, name, strlen(name)) &&
        executable_text(candidate.data, candidate.length) &&
        pz_str_assign(out, candidate.data, candidate.length))
        ok = 1;
    pz_str_free(&candidate);
    return ok;
}

static int try_relative(pz_str* out, const char* root, const char* first,
                        const char* second) {
    pz_str directory;
    pz_str candidate;
    int ok = 0;

    pz_str_init(&directory);
    pz_str_init(&candidate);
    if (pz_paths_join(&directory, root, strlen(root), first, strlen(first)) &&
        pz_paths_join(&candidate, directory.data, directory.length, second,
                      strlen(second)) &&
        executable_text(candidate.data, candidate.length) &&
        pz_str_assign(out, candidate.data, candidate.length))
        ok = 1;
    pz_str_free(&candidate);
    pz_str_free(&directory);
    return ok;
}

static int find_in_path(pz_str* out) {
    const char* env = getenv("PATH");
#ifdef _WIN32
    const char sep = ';';
#else
    const char sep = ':';
#endif
    const char* start;

    if (!env) return 0;
    start = env;
    for (;;) {
        const char* end = strchr(start, sep);
        const size_t length = end ? (size_t)(end - start) : strlen(start);

        if (length) {
#ifdef _WIN32
            if (try_path(out, start, length, "steamcmd.exe")) return 1;
#else
            if (try_path(out, start, length, "steamcmd")) return 1;
            if (try_path(out, start, length, "steamcmd.sh")) return 1;
#endif
        }
        if (!end) break;
        start = end + 1;
    }
    return 0;
}

static int find_system(pz_str* out) {
    static const char* const vars[] = {"PZMODMGR_STEAMCMD", "STEAMCMD"};
    size_t i;

    for (i = 0; i < sizeof(vars) / sizeof(vars[0]); ++i) {
        const char* value = getenv(vars[i]);

        if (value && *value && executable_text(value, strlen(value)))
            return pz_str_assign_cstr(out, value);
    }
    if (find_in_path(out)) return 1;

#ifdef _WIN32
    {
        static const char* const roots[] = {"ProgramFiles", "ProgramFiles(x86)",
                                            "LOCALAPPDATA", "USERPROFILE"};
        size_t r;

        for (r = 0; r < sizeof(roots) / sizeof(roots[0]); ++r) {
            const char* root = getenv(roots[r]);

            if (!root || !*root) continue;
            if (try_relative(out, root, "steamcmd", "steamcmd.exe")) return 1;
            if (try_relative(out, root, "Steam", "steamcmd.exe")) return 1;
            if (try_path(out, root, strlen(root), "steamcmd.exe")) return 1;
        }
    }
#else
    {
        static const char* const common[] = {
            "/usr/bin/steamcmd", "/usr/games/steamcmd",
            "/usr/local/bin/steamcmd", "/opt/steamcmd/steamcmd.sh",
            "/snap/bin/steamcmd"};
        const char* home = getenv("HOME");

        for (i = 0; i < sizeof(common) / sizeof(common[0]); ++i)
            if (executable_text(common[i], strlen(common[i])))
                return pz_str_assign_cstr(out, common[i]);
        if (home && *home) {
            if (try_relative(out, home, "steamcmd", "steamcmd.sh")) return 1;
            if (try_relative(out, home, "Steam", "steamcmd.sh")) return 1;
        }
    }
#endif
    return 0;
}

static void log_detail(const pz_log* log, const char* prefix,
                       const pz_str* detail) {
    pz_str line;

    pz_str_init(&line);
    if (pz_str_append_cstr(&line, prefix) &&
        pz_str_append(&line, detail->data, detail->length))
        pz_log_emit_str(log, &line);
    pz_str_free(&line);
}

/* ---------------------------------------------------------------- public */

void pz_download_outcome_init(pz_download_outcome* outcome) {
    if (!outcome) return;
    outcome->ok = 0;
    pz_path_init(&outcome->content_dir);
    pz_str_init(&outcome->detail);
}

void pz_download_outcome_free(pz_download_outcome* outcome) {
    if (!outcome) return;
    pz_path_free(&outcome->content_dir);
    pz_str_free(&outcome->detail);
}

int pz_steamcmd_ensure(const pz_path* data_dir, pz_str* exe, int* changed,
                       const pz_log* log) {
    pz_str builtin;
    pz_path builtin_path;
    pz_path parent;
    pz_path archive_path;
    pz_str detail;
    pz_fs_error error;
    int ok = 0;

    if (changed) *changed = 0;
    if (!data_dir || !exe || !data_dir->text.length) return 0;

    if (exe->length) {
        pz_path configured;

        pz_path_init(&configured);
        if (pz_path_assign(&configured, exe->data, exe->length) &&
            is_file(&configured)) {
            pz_path_free(&configured);
            return 1;
        }
        pz_path_free(&configured);
    }

    pz_str_init(&builtin);
    pz_path_init(&builtin_path);
    pz_path_init(&parent);
    pz_path_init(&archive_path);
    pz_str_init(&detail);
    pz_fs_error_clear(&error);

    if (!pz_paths_builtin_steamcmd(data_dir->text.data, data_dir->text.length,
                                   &builtin) ||
        !pz_path_assign(&builtin_path, builtin.data, builtin.length) ||
        !pz_path_parent(&builtin_path, &parent))
        goto done;

    if (is_file(&builtin_path)) {
        ok = assign_string(exe, builtin.data, builtin.length);
        if (ok && changed) *changed = 1;
        goto done;
    }

    pz_log_emit(log, "steamcmd not found; downloading the built-in copy (one time)...");
    pz_fs_error_clear(&error);
    pz_fs_create_directories(&parent, &error);

#ifdef _WIN32
    {
        static const char* const url =
            "https://steamcdn-a.akamaihd.net/client/installer/steamcmd.zip";

        if (!join_leaf(&archive_path, &parent, "steamcmd-download.zip")) goto done;
        if (!pz_net_download(url, strlen(url), &archive_path, &detail)) {
            log_detail(log, "built-in steamcmd download failed: ", &detail);
            goto done;
        }
    }
#else
    {
        static const char* const url =
            "https://steamcdn-a.akamaihd.net/client/installer/steamcmd_linux.tar.gz";

        if (!join_leaf(&archive_path, &parent, "steamcmd-download.tar.gz")) goto done;
        if (!pz_net_download(url, strlen(url), &archive_path, &detail)) {
            log_detail(log, "built-in steamcmd download failed: ", &detail);
            goto done;
        }
    }
#endif

    if (!pz_net_extract(&archive_path, &parent, &detail))
        log_detail(log, "built-in steamcmd extract failed: ", &detail);

    {
        pz_fs_error ignored;

        pz_fs_error_clear(&ignored);
        pz_fs_remove(&archive_path, &ignored);
    }

    if (is_file(&builtin_path)) {
        pz_fs_error ignored;

        pz_fs_error_clear(&ignored);
        pz_fs_set_executable(&builtin_path, 1, &ignored);
        ok = assign_string(exe, builtin.data, builtin.length);
        if (ok && changed) *changed = 1;
        if (ok) {
            pz_str line;

            pz_str_init(&line);
            if (pz_str_append_cstr(&line, "using built-in steamcmd: ") &&
                pz_str_append(&line, builtin.data, builtin.length))
                pz_log_emit_str(log, &line);
            pz_str_free(&line);
        }
        goto done;
    }

    {
        pz_str system;

        pz_str_init(&system);
        if (find_system(&system) && system.length) {
            ok = assign_string(exe, system.data, system.length);
            if (ok && changed) *changed = 1;
            if (ok) {
                pz_str line;

                pz_str_init(&line);
                if (pz_str_append_cstr(&line, "using system steamcmd: ") &&
                    pz_str_append(&line, system.data, system.length))
                    pz_log_emit_str(log, &line);
                pz_str_free(&line);
            }
        }
        pz_str_free(&system);
    }
    if (ok) goto done;

    pz_str_clear(exe);
    if (changed) *changed = 1;
    pz_log_emit(log, "steamcmd is unavailable. Install it or check your network.");

done:
    pz_str_free(&detail);
    pz_path_free(&archive_path);
    pz_path_free(&parent);
    pz_path_free(&builtin_path);
    pz_str_free(&builtin);
    return ok;
}

int pz_steamcmd_find(pz_str* out) {
    if (!out) return 0;
    pz_str_clear(out);
    return find_system(out) && out->length != 0;
}

int pz_steamcmd_download_workshop(const pz_path* data_dir, pz_str* exe,
                                  const pz_str* steam_user, uint64_t item_id,
                                  const pz_path* install_dir, const pz_log* log,
                                  pz_download_outcome* outcome) {
    pz_str command;
    pz_str output;
    pz_path content;
    pz_fs_error error;
    int exit_code = -1;
    int changed = 0;
    int ok = 0;

    if (!outcome) return 0;
    outcome->ok = 0;
    if (!steam_user || !steam_user->length) {
        pz_str_assign_cstr(&outcome->detail,
                           "Set the Steam username in Settings first (no password is stored).");
        return 0;
    }
    if (!data_dir || !exe || !install_dir) return 0;
    if (!pz_steamcmd_ensure(data_dir, exe, &changed, log)) {
        pz_str_assign_cstr(&outcome->detail,
                           "steamcmd is unavailable. Check your network or set its path in Settings.");
        return 0;
    }

    pz_str_init(&command);
    pz_str_init(&output);
    pz_path_init(&content);
    pz_fs_error_clear(&error);
    pz_fs_create_directories(install_dir, &error);

    /* Order matters: force_install_dir must come before +login. No password is
     * passed; steamcmd uses its cached login token. */
    ok = pz_process_quote(&command, exe->data, exe->length) &&
         pz_str_append_cstr(&command, " +force_install_dir ") &&
         pz_process_quote(&command, install_dir->text.data, install_dir->text.length) &&
         pz_str_append_cstr(&command, " +login ") &&
         pz_str_append(&command, steam_user->data, steam_user->length) &&
         pz_str_append_cstr(&command, " +workshop_download_item 108600 ") &&
         pz_str_append_u64(&command, item_id) &&
         pz_str_append_cstr(&command, " +quit");
#ifdef _WIN32
    if (ok) ok = pz_str_append_cstr(&command, " <nul 2>&1");
#else
    if (ok) ok = pz_str_append_cstr(&command, " </dev/null 2>&1");
#endif
    if (!ok) {
        pz_str_assign_cstr(&outcome->detail, "out of memory");
        goto done;
    }

    if (!pz_process_capture(command.data, command.length, &output, log, &exit_code))
        exit_code = -1;

    ok = pz_path_assign(&content, install_dir->text.data,
                        install_dir->text.length) &&
         append_leaf(&content, "steamapps") &&
         append_leaf(&content, "workshop") &&
         append_leaf(&content, "content") &&
         append_leaf(&content, "108600");
    if (ok) {
        pz_str item_text;

        pz_str_init(&item_text);
        ok = pz_str_append_u64(&item_text, item_id) &&
             append_leaf(&content, item_text.data);
        pz_str_free(&item_text);
    }
    if (!ok) {
        pz_str_assign_cstr(&outcome->detail, "out of memory");
        goto done;
    }
    ok = 0;

    {
        const int has_content = is_directory(&content) && !is_empty(&content);

        if (exit_code == 0 && has_content) {
            ok = 1;
            outcome->ok = 1;
        } else if (strstr(output.data ? output.data : "", "Access Denied")) {
            pz_str_assign_cstr(&outcome->detail, "Steam refused item ");
            pz_str_append_u64(&outcome->detail, item_id);
            pz_str_append_cstr(&outcome->detail,
                               " (Access Denied): it is private or removed, so it cannot be downloaded");
        } else if (exit_code == 0) {
            pz_str_assign_cstr(&outcome->detail,
                               "steamcmd finished without downloading item ");
            pz_str_append_u64(&outcome->detail, item_id);
            pz_str_append_cstr(&outcome->detail, " (item unavailable?)");
        } else {
            pz_str_assign_cstr(&outcome->detail, "steamcmd failed (rc=");
            pz_str_append_i64(&outcome->detail, exit_code);
            pz_str_append_cstr(&outcome->detail,
                               "). Check: logged in once manually? Steam Guard? account owns PZ?");
        }
    }
    if (ok) {
        if (!pz_path_assign(&outcome->content_dir, content.text.data,
                            content.text.length))
            ok = 0;
    }

done:
    pz_path_free(&content);
    pz_str_free(&output);
    pz_str_free(&command);
    return ok;
}

static int fetch_details(uint64_t item_id, pz_str* response) {
    pz_str body;
    int ok;

    pz_str_init(&body);
    ok = pz_str_append_cstr(&body, "itemcount=1&publishedfileids[0]=") &&
         pz_str_append_u64(&body, item_id) &&
         pz_net_post(kDetailsUrl, strlen(kDetailsUrl), body.data, body.length,
                     response);
    pz_str_free(&body);
    return ok;
}

uint32_t pz_steamcmd_fetch_result(uint64_t item_id) {
    pz_str response;
    uint32_t result = 0;

    pz_str_init(&response);
    if (fetch_details(item_id, &response))
        result = pz_parse_published_file_result(response.data, response.length);
    pz_str_free(&response);
    return result;
}

uint64_t pz_steamcmd_fetch_time_updated(uint64_t item_id) {
    pz_str response;
    uint64_t time_updated = 0;

    pz_str_init(&response);
    if (fetch_details(item_id, &response))
        time_updated =
            pz_parse_published_file_time_updated(response.data, response.length);
    pz_str_free(&response);
    return time_updated;
}

int pz_steamcmd_fetch_details(uint64_t item_id, pz_remote_details* details) {
    pz_str response;
    int ok = 0;

    if (!details) return 0;
    pz_str_init(&response);
    if (fetch_details(item_id, &response)) {
        details->result =
            pz_parse_published_file_result(response.data, response.length);
        details->time_updated =
            pz_parse_published_file_time_updated(response.data, response.length);
        if (!pz_parse_published_file_title(response.data, response.length,
                                           &details->title))
            pz_str_clear(&details->title);
        ok = 1;
    }
    pz_str_free(&response);
    return ok;
}

int pz_steamcmd_fetch_collection(uint64_t collection_id, pz_vec* out) {
    pz_str body;
    pz_str response;
    pz_vec ids;
    size_t i;
    int ok = 0;

    if (!out) return 0;
    pz_str_init(&body);
    pz_str_init(&response);
    pz_vec_init(&ids, sizeof(uint64_t));

    if (!pz_str_append_cstr(&body, "collectioncount=1&publishedfileids[0]=") ||
        !pz_str_append_u64(&body, collection_id))
        goto done;
    if (!pz_net_post(kCollectionUrl, strlen(kCollectionUrl), body.data,
                     body.length, &response))
        goto done;
    if (!pz_parse_collection_children(response.data, response.length, &ids))
        goto done;

    pz_vec_clear(out);
    for (i = 0; i < ids.length; ++i) {
        const uint64_t* id = (const uint64_t*)pz_vec_at_const(&ids, i);

        if (!pz_vec_push_copy(out, id)) goto done;
    }
    ok = 1;

done:
    pz_vec_free(&ids);
    pz_str_free(&response);
    pz_str_free(&body);
    return ok;
}
