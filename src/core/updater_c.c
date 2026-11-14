#define _POSIX_C_SOURCE 200809L

#include "core/updater_c.h"

#include "core/hash_c.h"
#include "core/net_c.h"
#include "core/process_c.h"

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

static const char* const kLatestReleaseUrl =
    "https://api.github.com/repos/sigods/pzmodmgr/releases/latest";

/* ------------------------------------------------------------- utilities */

static int ends_with(const char* text, size_t length, const char* suffix) {
    const size_t suffix_length = strlen(suffix);

    if (!text || length < suffix_length) return 0;
    return memcmp(text + length - suffix_length, suffix, suffix_length) == 0;
}

static int assign_string(pz_str* out, const char* data, size_t length) {
    if (!data) length = 0;
    return pz_str_assign(out, data, length);
}

static int copy_path(pz_path* out, const pz_path* source) {
    return assign_string(&out->text, source->text.data, source->text.length);
}

static int join_leaf(pz_path* out, const pz_path* base, const char* leaf) {
    return pz_path_join(out, base, leaf, strlen(leaf));
}

static void format_fs_error(const pz_fs_error* error, pz_str* out) {
    pz_error mapped;

    pz_error_clear(&mapped);
    if (error->system_code != 0)
        pz_error_set_system(&mapped, error->system_code);
    else
        pz_error_set(&mapped, (int)error->code);
    pz_error_format(&mapped, out);
}

static void set_fs_message(pz_str* message, const char* prefix,
                           const pz_fs_error* error) {
    pz_str text;

    pz_str_init(&text);
    format_fs_error(error, &text);
    pz_str_assign_cstr(message, prefix);
    pz_str_append(message, text.data, text.length);
    pz_str_free(&text);
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

static int exists(const pz_path* path) {
    int found = 0;
    pz_fs_error error;

    pz_fs_error_clear(&error);
    return pz_fs_exists(path, &found, &error) && found != 0;
}

static void remove_tree(const pz_path* path) {
    pz_fs_error error;
    uint64_t removed = 0;

    pz_fs_error_clear(&error);
    pz_fs_remove_all(path, &removed, &error);
}

static void remove_file(const pz_path* path) {
    pz_fs_error error;

    pz_fs_error_clear(&error);
    pz_fs_remove(path, &error);
}

static void append_update_log(const pz_path* log_path, const char* message) {
    pz_path parent;
    pz_fs_error error;
    pz_fs_file* file = NULL;
    size_t length;

    if (!log_path || !log_path->text.length || !message) return;
    pz_path_init(&parent);
    pz_fs_error_clear(&error);
    if (pz_path_parent(log_path, &parent) && parent.text.length)
        pz_fs_create_directories(&parent, &error);

    pz_fs_error_clear(&error);
    if (pz_fs_open_append(log_path, &file, &error)) {
        length = strlen(message);
        pz_fs_write(file, message, length);
        pz_fs_write(file, "\n", 1);
        pz_fs_close(file, &error);
    }
    pz_path_free(&parent);
}

/* Replaces file through a sibling ".tmp" so a crash never leaves a truncated
 * marker behind. Mirrors pz::write_text_atomic. */
static int write_text_atomic(const pz_path* file, const char* text, size_t length) {
    pz_path temp;
    pz_fs_error error;
    pz_fs_file* handle = NULL;
    size_t written = 0;
    int ok = 0;

    pz_path_init(&temp);
    if (!copy_path(&temp, file) || !pz_str_append_cstr(&temp.text, ".tmp")) goto done;

    pz_fs_error_clear(&error);
    if (!pz_fs_open_write(&temp, &handle, &error)) {
        handle = NULL;
        goto done;
    }
    while (written < length) {
        const size_t put = pz_fs_write(handle, text + written, length - written);
        if (put == 0) break;
        written += put;
    }
    ok = pz_fs_close(handle, &error) && written == length;
    handle = NULL;
    if (!ok) goto done;
    pz_fs_error_clear(&error);
    ok = pz_fs_replace_file(&temp, file, &error) != 0;

done:
    if (handle) {
        pz_fs_error ignored;
        pz_fs_error_clear(&ignored);
        pz_fs_close(handle, &ignored);
    }
    if (!ok) remove_file(&temp);
    pz_path_free(&temp);
    return ok;
}

/* Reads pid= out of a pending marker. */
static unsigned long marker_pid(const pz_path* marker) {
    pz_str text;
    unsigned long pid = 0;

    pz_str_init(&text);
    {
        pz_fs_error error;

        pz_fs_error_clear(&error);
        if (pz_fs_read_all(marker, &text, &error) && text.data) {
            size_t start = 0;

            while (start < text.length) {
                const char* line = text.data + start;
                size_t line_length = 0;

                while (start + line_length < text.length &&
                       line[line_length] != '\n')
                    ++line_length;
                if (line_length > 4 && memcmp(line, "pid=", 4) == 0) {
                    pid = strtoul(line + 4, NULL, 10);
                    break;
                }
                start += line_length + 1;
            }
        }
    }
    pz_str_free(&text);
    return pid;
}

static int active_staged_update(const pz_path* updates_root, pz_str* name) {
    pz_vec entries;
    pz_fs_error error;
    size_t i;
    int found = 0;

    pz_vec_init(&entries, sizeof(pz_fs_entry));
    pz_fs_error_clear(&error);
    if (!pz_fs_list_dir(updates_root, &entries, &error)) {
        pz_fs_entries_free(&entries);
        return 0;
    }
    for (i = 0; i < entries.length && !found; ++i) {
        const pz_fs_entry* entry = (const pz_fs_entry*)pz_vec_at_const(&entries, i);
        pz_path directory;
        pz_path marker;

        if (entry->error || entry->kind != PZ_FS_DIR) continue;
        pz_path_init(&directory);
        pz_path_init(&marker);
        if (pz_path_join(&directory, updates_root, entry->name.data,
                         entry->name.length) &&
            join_leaf(&marker, &directory, "pending.txt") &&
            is_file(&marker)) {
            const unsigned long pid = marker_pid(&marker);

            if (pid != 0 && pz_process_is_running(pid))
                found = assign_string(name, entry->name.data, entry->name.length);
        }
        pz_path_free(&marker);
        pz_path_free(&directory);
    }
    pz_fs_entries_free(&entries);
    return found;
}

/* The release may put the executable at the archive root or inside one wrapper
 * folder. Returns 1 and fills out with the folder that contains it. */
static int staged_update_root(const pz_path* stage, const pz_str* exe_name,
                              pz_path* out) {
    pz_path candidate;
    pz_vec entries;
    pz_fs_error error;
    size_t i;
    int found = 0;

    pz_path_init(&candidate);
    if (pz_path_join(&candidate, stage, exe_name->data, exe_name->length) &&
        is_file(&candidate)) {
        found = copy_path(out, stage);
        pz_path_free(&candidate);
        return found;
    }
    pz_path_free(&candidate);

    pz_vec_init(&entries, sizeof(pz_fs_entry));
    pz_fs_error_clear(&error);
    if (!pz_fs_list_dir(stage, &entries, &error)) {
        pz_fs_entries_free(&entries);
        return 0;
    }
    for (i = 0; i < entries.length && !found; ++i) {
        const pz_fs_entry* entry = (const pz_fs_entry*)pz_vec_at_const(&entries, i);
        pz_path directory;
        pz_path executable;

        if (entry->error || entry->kind != PZ_FS_DIR) continue;
        pz_path_init(&directory);
        pz_path_init(&executable);
        if (pz_path_join(&directory, stage, entry->name.data, entry->name.length) &&
            pz_path_join(&executable, &directory, exe_name->data,
                         exe_name->length) &&
            is_file(&executable))
            found = copy_path(out, &directory);
        pz_path_free(&executable);
        pz_path_free(&directory);
    }
    pz_fs_entries_free(&entries);
    return found;
}

static int hash_file(const pz_path* file, pz_str* hex) {
    pz_str data;
    pz_fs_error error;
    int ok = 0;

    pz_str_init(&data);
    pz_fs_error_clear(&error);
    if (pz_fs_read_all(file, &data, &error))
        ok = pz_sha256_hex(data.data, data.length, hex);
    pz_str_free(&data);
    return ok;
}

/* Case-insensitive comparison after dropping an optional "sha256:" prefix. */
static int digest_matches(const char* expected, size_t expected_length,
                          const pz_str* actual) {
    size_t i;

    if (expected_length >= 7 && memcmp(expected, "sha256:", 7) == 0) {
        expected += 7;
        expected_length -= 7;
    }
    if (expected_length != actual->length) return 0;
    for (i = 0; i < expected_length; ++i) {
        char left = expected[i];
        char right = actual->data[i];

        if (left >= 'A' && left <= 'Z') left = (char)(left - 'A' + 'a');
        if (right >= 'A' && right <= 'Z') right = (char)(right - 'A' + 'a');
        if (left != right) return 0;
    }
    return 1;
}

static int safe_component(pz_str* out, const char* value, size_t length) {
    size_t i;

    for (i = 0; i < length; ++i) {
        const char c = value[i];
        const int alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9');

        if (!pz_str_append_char(out, (alnum || c == '.' || c == '-' || c == '_')
                                         ? c
                                         : '_'))
            return 0;
    }
    if (out->length == 0) return pz_str_assign_cstr(out, "latest");
    return 1;
}

static void log_version(const pz_log* log, const char* prefix,
                        const pz_str* version, const char* suffix) {
    pz_str line;

    pz_str_init(&line);
    if (pz_str_append_cstr(&line, prefix) &&
        pz_str_append(&line, version->data, version->length) &&
        pz_str_append_cstr(&line, suffix))
        pz_log_emit(log, line.data);
    pz_str_free(&line);
}

static int update_failed(const pz_path* log_path, const pz_path* marker,
                         const char* text) {
    pz_str line;

    pz_str_init(&line);
    if (pz_str_append_cstr(&line, "ERROR: ") && pz_str_append_cstr(&line, text))
        append_update_log(log_path, line.data);
    pz_str_free(&line);
    remove_file(marker);
#ifdef _WIN32
    MessageBoxA(NULL, text, "PZ Mod Manager update failed", MB_OK | MB_ICONERROR);
#endif
    return 1;
}

static int update_failed_fs(const pz_path* log_path, const pz_path* marker,
                            const char* prefix, const pz_fs_error* error) {
    pz_str text;
    pz_str suffix;
    int result;

    pz_str_init(&text);
    pz_str_init(&suffix);
    format_fs_error(error, &suffix);
    if (pz_str_assign_cstr(&text, prefix))
        pz_str_append(&text, suffix.data, suffix.length);
    result = update_failed(log_path, marker, text.data ? text.data : prefix);
    pz_str_free(&suffix);
    pz_str_free(&text);
    return result;
}

/* ------------------------------------------------------------ public API */

int pz_updater_check(const char* current_version, size_t current_version_length,
                     const char* platform, size_t platform_length,
                     pz_app_update* out) {
    pz_str response;
    pz_str detail;
    int ok;

    if (!out) return 0;
    if (!current_version) current_version_length = 0;
    if (!platform) platform_length = 0;

    pz_str_init(&response);
    pz_str_init(&detail);
    if (!pz_net_get(kLatestReleaseUrl, strlen(kLatestReleaseUrl), &response, &detail)) {
        assign_string(&out->current_version, current_version, current_version_length);
        pz_str_assign_cstr(&out->message, "Could not check for updates: ");
        pz_str_append(&out->message, detail.data, detail.length);
        ok = 1;
    } else {
        ok = pz_parse_latest_app_release(response.data, response.length,
                                         current_version, current_version_length,
                                         platform, platform_length, out);
    }
    pz_str_free(&detail);
    pz_str_free(&response);
    return ok;
}

void pz_updater_cleanup_cache(const pz_path* data_dir) {
    pz_path helper_dir;
    pz_path updates_root;
    pz_vec entries;
    pz_fs_error error;
    size_t i;

    if (!data_dir || !data_dir->text.length) return;
    pz_path_init(&helper_dir);
    pz_path_init(&updates_root);

    if (join_leaf(&helper_dir, data_dir, "update_helper"))
        remove_tree(&helper_dir);
    if (!join_leaf(&updates_root, data_dir, "updates")) goto done;

    pz_vec_init(&entries, sizeof(pz_fs_entry));
    pz_fs_error_clear(&error);
    if (pz_fs_list_dir(&updates_root, &entries, &error)) {
        for (i = 0; i < entries.length; ++i) {
            const pz_fs_entry* entry =
                (const pz_fs_entry*)pz_vec_at_const(&entries, i);
            pz_path directory;
            pz_path marker;
            int keep = 0;

            if (entry->error || entry->kind != PZ_FS_DIR) continue;
            pz_path_init(&directory);
            pz_path_init(&marker);

            if (pz_path_join(&directory, &updates_root, entry->name.data,
                             entry->name.length) &&
                join_leaf(&marker, &directory, "pending.txt") &&
                is_file(&marker)) {
                /* A live marker protects the one release the helper is waiting
                 * to apply; everything else is completed, abandoned, or failed
                 * and must not leave a package behind. */
                const unsigned long pid = marker_pid(&marker);
                keep = pid != 0 && pz_process_is_running(pid);
            }
            if (!keep) remove_tree(&directory);
            pz_path_free(&marker);
            pz_path_free(&directory);
        }
    }
    pz_fs_entries_free(&entries);
    remove_file(&updates_root);   /* succeeds only when empty */

done:
    pz_path_free(&updates_root);
    pz_path_free(&helper_dir);
}

int pz_updater_stage(const pz_path* data_dir,
                     const char* current_version, size_t current_version_length,
                     const char* platform, size_t platform_length,
                     const pz_log* log, pz_str* message) {
    pz_path updates_root;
    pz_path base;
    pz_path marker;
    pz_path stage;
    pz_path archive;
    pz_path executable;
    pz_path install_dir;
    pz_path root;
    pz_path helper_dir;
    pz_path helper;
    pz_path log_path;
    pz_app_update update;
    pz_str active;
    pz_str detail;
    pz_str version_component;
    pz_str exe_name;
    pz_str args[13];
    pz_fs_error error;
    unsigned long helper_pid = 0;
    size_t i;
    int args_ok = 1;
    int ok = 0;

    if (message) pz_str_clear(message);
    if (!message || !data_dir || !data_dir->text.length) return 0;
    if (!current_version) current_version_length = 0;
    if (!platform) platform_length = 0;

    pz_path_init(&updates_root);
    pz_path_init(&base);
    pz_path_init(&marker);
    pz_path_init(&stage);
    pz_path_init(&archive);
    pz_path_init(&executable);
    pz_path_init(&install_dir);
    pz_path_init(&root);
    pz_path_init(&helper_dir);
    pz_path_init(&helper);
    pz_path_init(&log_path);
    pz_app_update_init(&update);
    pz_str_init(&active);
    pz_str_init(&detail);
    pz_str_init(&version_component);
    pz_str_init(&exe_name);
    for (i = 0; i < 13; ++i) pz_str_init(&args[i]);

    pz_updater_cleanup_cache(data_dir);
    if (!join_leaf(&updates_root, data_dir, "updates")) {
        pz_str_assign_cstr(message, "out of memory");
        goto done;
    }
    if (active_staged_update(&updates_root, &active) && active.length) {
        pz_str_assign_cstr(message, "PZ Mod Manager ");
        pz_str_append(message, active.data, active.length);
        pz_str_append_cstr(message, " is already staged. Close pzmodmgr to apply it.");
        ok = 1;
        goto done;
    }

    if (!pz_updater_check(current_version, current_version_length, platform,
                          platform_length, &update)) {
        pz_str_assign_cstr(message, "out of memory");
        goto done;
    }
    if (!update.ok) {
        assign_string(message, update.message.data, update.message.length);
        goto done;
    }
    if (!update.available) {
        assign_string(message, update.message.data, update.message.length);
        ok = 1;
        goto done;
    }

    if (!safe_component(&version_component, update.latest_version.data,
                        update.latest_version.length) ||
        !pz_path_join(&base, &updates_root, version_component.data,
                      version_component.length) ||
        !join_leaf(&marker, &base, "pending.txt")) {
        pz_str_assign_cstr(message, "out of memory");
        goto done;
    }
    remove_tree(&base);

    if (!join_leaf(&stage, &base, "stage")) {
        pz_str_assign_cstr(message, "out of memory");
        goto done;
    }
    pz_fs_error_clear(&error);
    if (!pz_fs_create_directories(&stage, &error)) {
        set_fs_message(message, "Could not create the update folder: ", &error);
        goto done;
    }

    {
        const int zip = ends_with(update.asset_name.data, update.asset_name.length,
                                  ".zip");
        if (!join_leaf(&archive, &base,
                       zip ? "package.zip" : "package.tar.gz")) {
            pz_str_assign_cstr(message, "out of memory");
            goto done;
        }
    }

    log_version(log, "Downloading PZ Mod Manager ", &update.latest_version, "...");
    if (!pz_net_download(update.asset_url.data, update.asset_url.length, &archive,
                         &detail)) {
        pz_str_assign_cstr(message, "Update download failed: ");
        pz_str_append(message, detail.data, detail.length);
        remove_tree(&base);
        goto done;
    }

    if (update.asset_size != 0) {
        uint64_t size = 0;

        pz_fs_error_clear(&error);
        if (!pz_fs_file_size(&archive, &size, &error) || size != update.asset_size) {
            pz_str_assign_cstr(message, "Update download is incomplete");
            remove_tree(&base);
            goto done;
        }
    }

    if (update.asset_digest.length) {
        pz_str actual;

        pz_str_init(&actual);
        if (!hash_file(&archive, &actual) ||
            !digest_matches(update.asset_digest.data, update.asset_digest.length,
                            &actual)) {
            pz_str_assign_cstr(message, "Update checksum did not match");
            pz_str_free(&actual);
            remove_tree(&base);
            goto done;
        }
        pz_str_free(&actual);
    } else {
        pz_log_emit(log, "GitHub did not provide a checksum for this release; relying on HTTPS.");
    }

    pz_log_emit(log, "Extracting update...");
    if (!pz_net_extract(&archive, &stage, &detail)) {
        pz_str_assign_cstr(message, "Update extraction failed: ");
        pz_str_append(message, detail.data, detail.length);
        remove_tree(&base);
        goto done;
    }

    pz_fs_error_clear(&error);
    if (!pz_fs_executable_path(&executable, &error) || !executable.text.length) {
        pz_str_assign_cstr(message, "Could not locate the running executable");
        remove_tree(&base);
        goto done;
    }
    if (!pz_path_filename(&executable, &exe_name)) {
        pz_str_assign_cstr(message, "out of memory");
        remove_tree(&base);
        goto done;
    }
    if (!staged_update_root(&stage, &exe_name, &root)) {
        pz_str_assign_cstr(message, "The update archive does not contain ");
        pz_str_append(message, exe_name.data, exe_name.length);
        remove_tree(&base);
        goto done;
    }

    /* The helper lives outside the updates folder so a successful update can
     * remove every staged byte without leaving its running executable behind. */
    if (!join_leaf(&helper_dir, data_dir, "update_helper")) {
        pz_str_assign_cstr(message, "out of memory");
        remove_tree(&base);
        goto done;
    }
    remove_tree(&helper_dir);
    pz_fs_error_clear(&error);
    if (!pz_fs_create_directories(&helper_dir, &error)) {
        set_fs_message(message, "Could not create the update helper folder: ",
                       &error);
        remove_tree(&base);
        goto done;
    }
    if (!pz_path_join(&helper, &helper_dir, exe_name.data, exe_name.length)) {
        pz_str_assign_cstr(message, "out of memory");
        remove_tree(&base);
        goto done;
    }
    pz_fs_error_clear(&error);
    if (!pz_fs_copy_file(&executable, &helper, PZ_FS_COPY_OVERWRITE, &error)) {
        set_fs_message(message, "Could not prepare the update helper: ", &error);
        remove_tree(&base);
        goto done;
    }
    pz_fs_error_clear(&error);
    pz_fs_set_executable(&helper, 1, &error);

    if (!join_leaf(&log_path, &base, "update.log") ||
        !pz_path_parent(&executable, &install_dir) || !install_dir.text.length) {
        pz_str_assign_cstr(message, "out of memory");
        remove_tree(&base);
        goto done;
    }

    args_ok = pz_str_assign_cstr(&args[0], "--apply-update") &&
              pz_str_assign_cstr(&args[1], "--pid") &&
              pz_str_append_u64(&args[2], pz_process_current_id()) &&
              pz_str_assign_cstr(&args[3], "--stage") &&
              assign_string(&args[4], root.text.data, root.text.length) &&
              pz_str_assign_cstr(&args[5], "--install") &&
              assign_string(&args[6], install_dir.text.data, install_dir.text.length) &&
              pz_str_assign_cstr(&args[7], "--exe") &&
              assign_string(&args[8], exe_name.data, exe_name.length) &&
              pz_str_assign_cstr(&args[9], "--marker") &&
              assign_string(&args[10], marker.text.data, marker.text.length) &&
              pz_str_assign_cstr(&args[11], "--log") &&
              assign_string(&args[12], log_path.text.data, log_path.text.length);
    if (!args_ok) {
        pz_str_assign_cstr(message, "out of memory");
        remove_tree(&base);
        goto done;
    }

    if (!pz_process_start_detached(&helper, args, 13, &helper_pid)) {
        pz_str_assign_cstr(message, "Could not start the update helper");
        remove_tree(&base);
        goto done;
    }

    /* The marker doubles as a lock: a second click must not start a second
     * helper while this one waits for the current process to exit. */
    {
        pz_str marker_text;
        int written;

        pz_str_init(&marker_text);
        written = pz_str_append_cstr(&marker_text, "pid=") &&
                  pz_str_append_u64(&marker_text, helper_pid) &&
                  pz_str_append_cstr(&marker_text, "\nversion=") &&
                  pz_str_append(&marker_text, update.latest_version.data,
                                update.latest_version.length) &&
                  pz_str_append_char(&marker_text, '\n') &&
                  write_text_atomic(&marker, marker_text.data,
                                    marker_text.length);
        pz_str_free(&marker_text);
        if (!written)
            pz_log_emit(log, "warning: could not write the update lock file");
    }

    log_version(log, "Update staged. Close pzmodmgr to install ",
                &update.latest_version, ".");
    pz_str_assign_cstr(message, "PZ Mod Manager ");
    pz_str_append(message, update.latest_version.data, update.latest_version.length);
    pz_str_append_cstr(message, " is staged. Close pzmodmgr to apply the update.");
    ok = 1;

done:
    for (i = 0; i < 13; ++i) pz_str_free(&args[i]);
    pz_str_free(&exe_name);
    pz_str_free(&version_component);
    pz_str_free(&detail);
    pz_str_free(&active);
    pz_app_update_free(&update);
    pz_path_free(&log_path);
    pz_path_free(&helper);
    pz_path_free(&helper_dir);
    pz_path_free(&root);
    pz_path_free(&install_dir);
    pz_path_free(&executable);
    pz_path_free(&archive);
    pz_path_free(&stage);
    pz_path_free(&marker);
    pz_path_free(&base);
    pz_path_free(&updates_root);
    return ok;
}

int pz_updater_is_helper_invocation(int argc, char** argv) {
    return argc > 1 && argv && argv[1] && strcmp(argv[1], "--apply-update") == 0;
}

int pz_updater_run_helper(int argc, char** argv) {
    pz_path stage;
    pz_path install;
    pz_path marker;
    pz_path log_path;
    pz_path install_parent;
    pz_path new_dir;
    pz_path old_dir;
    pz_path new_exe;
    pz_path stage_parent;
    pz_path updates_root;
    pz_path source;
    pz_path destination;
    pz_str exe_name;
    pz_str suffix;
    pz_str install_name;
    pz_str new_name;
    pz_str old_name;
    pz_fs_error error;
    unsigned long parent_pid = 0;
    unsigned long new_pid = 0;
    int arg_index;
    size_t i;
    int result = 1;

    pz_path_init(&stage);
    pz_path_init(&install);
    pz_path_init(&marker);
    pz_path_init(&log_path);
    pz_path_init(&install_parent);
    pz_path_init(&new_dir);
    pz_path_init(&old_dir);
    pz_path_init(&new_exe);
    pz_path_init(&stage_parent);
    pz_path_init(&updates_root);
    pz_path_init(&source);
    pz_path_init(&destination);
    pz_str_init(&exe_name);
    pz_str_init(&suffix);
    pz_str_init(&install_name);
    pz_str_init(&new_name);
    pz_str_init(&old_name);

    for (arg_index = 2; arg_index + 1 < argc; arg_index += 2) {
        const char* key = argv[arg_index] ? argv[arg_index] : "";
        const char* value = argv[arg_index + 1] ? argv[arg_index + 1] : "";

        if (strcmp(key, "--pid") == 0)
            parent_pid = strtoul(value, NULL, 10);
        else if (strcmp(key, "--stage") == 0)
            pz_path_assign_cstr(&stage, value);
        else if (strcmp(key, "--install") == 0)
            pz_path_assign_cstr(&install, value);
        else if (strcmp(key, "--exe") == 0)
            pz_str_assign_cstr(&exe_name, value);
        else if (strcmp(key, "--marker") == 0)
            pz_path_assign_cstr(&marker, value);
        else if (strcmp(key, "--log") == 0)
            pz_path_assign_cstr(&log_path, value);
    }

    append_update_log(&log_path, "update helper started");
    if (!stage.text.length || !install.text.length || !exe_name.length) {
        result = update_failed(&log_path, &marker,
                               "The update helper received incomplete paths.");
        goto done;
    }

    append_update_log(&log_path, "waiting for pzmodmgr to exit");
    pz_process_wait(parent_pid);

    if (!is_directory(&stage) || !is_directory(&install)) {
        result = update_failed(&log_path, &marker,
                               "The staged update or installation folder disappeared.");
        goto done;
    }
    if (pz_path_join(&source, &stage, exe_name.data, exe_name.length) &&
        !is_file(&source)) {
        pz_str line;

        pz_str_init(&line);
        if (pz_str_append_cstr(&line, "The staged update does not contain ") &&
            pz_str_append(&line, exe_name.data, exe_name.length) &&
            pz_str_append_char(&line, '.'))
            result = update_failed(&log_path, &marker, line.data);
        else
            result = update_failed(&log_path, &marker,
                                   "The staged update is incomplete.");
        pz_str_free(&line);
        goto done;
    }

    if (!pz_str_append_cstr(&suffix, ".update-") ||
        !pz_str_append_u64(&suffix, pz_process_current_id()) ||
        !pz_path_parent(&install, &install_parent) ||
        !pz_path_filename(&install, &install_name)) {
        result = update_failed(&log_path, &marker, "out of memory");
        goto done;
    }
    if (!pz_str_assign(&new_name, install_name.data, install_name.length) ||
        !pz_str_append(&new_name, suffix.data, suffix.length) ||
        !pz_str_append_cstr(&new_name, "-new") ||
        !pz_str_assign(&old_name, install_name.data, install_name.length) ||
        !pz_str_append(&old_name, suffix.data, suffix.length) ||
        !pz_str_append_cstr(&old_name, "-old") ||
        !pz_path_join(&new_dir, &install_parent, new_name.data, new_name.length) ||
        !pz_path_join(&old_dir, &install_parent, old_name.data, old_name.length)) {
        result = update_failed(&log_path, &marker, "out of memory");
        goto done;
    }
    remove_tree(&new_dir);
    remove_tree(&old_dir);

    append_update_log(&log_path, "building a complete replacement folder");
    pz_fs_error_clear(&error);
    if (!pz_fs_copy_tree(&stage, &new_dir,
                         PZ_FS_COPY_RECURSIVE | PZ_FS_COPY_OVERWRITE, &error)) {
        remove_tree(&new_dir);
        result = update_failed_fs(&log_path, &marker,
                                  "Could not copy the staged update: ", &error);
        goto done;
    }

    /* Configuration and uninstall metadata are not part of the release zip. */
    {
        static const char* const keep[] = {"config.ini", "uninstall.sh",
                                           "uninstall.exe"};

        for (i = 0; i < sizeof(keep) / sizeof(keep[0]); ++i) {
            if (!pz_path_join(&source, &install, keep[i], strlen(keep[i])) ||
                !pz_path_join(&destination, &new_dir, keep[i], strlen(keep[i])))
                continue;
            if (is_file(&source) && !exists(&destination)) {
                pz_fs_error_clear(&error);
                if (!pz_fs_copy_file(&source, &destination, 0, &error)) {
                    remove_tree(&new_dir);
                    result = update_failed_fs(&log_path, &marker,
                                              "Could not preserve ", &error);
                    goto done;
                }
            }
        }
    }

    if (!pz_path_join(&new_exe, &new_dir, exe_name.data, exe_name.length) ||
        !is_file(&new_exe)) {
        remove_tree(&new_dir);
        result = update_failed(&log_path, &marker,
                               "The prepared update does not contain the executable.");
        goto done;
    }
#ifndef _WIN32
    {
        pz_fs_error ignored;

        pz_fs_error_clear(&ignored);
        pz_fs_set_executable(&new_exe, 1, &ignored);
        if (join_leaf(&source, &new_dir, "pzmodmgr_native_host") &&
            is_file(&source)) {
            pz_fs_error_clear(&ignored);
            pz_fs_set_executable(&source, 1, &ignored);
        }
    }
#endif

    append_update_log(&log_path, "swapping the installation folder");
    pz_fs_error_clear(&error);
    if (!pz_fs_rename(&install, &old_dir, &error)) {
        result = update_failed_fs(&log_path, &marker,
                                  "Could not move the old installation aside: ",
                                  &error);
        goto done;
    }
    pz_fs_error_clear(&error);
    if (!pz_fs_rename(&new_dir, &install, &error)) {
        pz_fs_error restore;

        pz_fs_error_clear(&restore);
        pz_fs_rename(&old_dir, &install, &restore);
        remove_tree(&new_dir);
        result = update_failed(&log_path, &marker,
                               "Could not install the new version; the old one was restored.");
        goto done;
    }

    if (!pz_path_join(&new_exe, &install, exe_name.data, exe_name.length) ||
        !pz_process_start_detached(&new_exe, NULL, 0, &new_pid)) {
        pz_fs_error rollback;

        pz_fs_error_clear(&rollback);
        pz_fs_rename(&install, &new_dir, &rollback);
        pz_fs_error_clear(&rollback);
        pz_fs_rename(&old_dir, &install, &rollback);
        remove_tree(&new_dir);
        result = update_failed(&log_path, &marker,
                               "The new version was installed but could not be started; "
                               "the old version was restored.");
        goto done;
    }

    remove_tree(&old_dir);
    remove_tree(&stage);
    if (pz_path_parent(&stage, &stage_parent) && stage_parent.text.length) {
        pz_path package;

        pz_path_init(&package);
        if (join_leaf(&package, &stage_parent, "package.zip"))
            remove_file(&package);
        if (join_leaf(&package, &stage_parent, "package.tar.gz"))
            remove_file(&package);
        pz_path_free(&package);
    }
    remove_file(&marker);
    append_update_log(&log_path, "update installed successfully");

    /* The update is applied. The helper lives outside updates, so remove the
     * complete staged release and the now-empty updates folder immediately. */
    if (pz_path_parent(&stage, &stage_parent) && stage_parent.text.length) {
        remove_tree(&stage_parent);
        if (pz_path_parent(&stage_parent, &updates_root) &&
            updates_root.text.length)
            remove_file(&updates_root);
    }
    result = 0;

done:
    pz_str_free(&old_name);
    pz_str_free(&new_name);
    pz_str_free(&install_name);
    pz_str_free(&suffix);
    pz_str_free(&exe_name);
    pz_path_free(&destination);
    pz_path_free(&source);
    pz_path_free(&updates_root);
    pz_path_free(&stage_parent);
    pz_path_free(&new_exe);
    pz_path_free(&old_dir);
    pz_path_free(&new_dir);
    pz_path_free(&install_parent);
    pz_path_free(&log_path);
    pz_path_free(&marker);
    pz_path_free(&install);
    pz_path_free(&stage);
    return result;
}

const char* pz_updater_platform_name(void) {
#ifdef _WIN32
    return "win";
#else
    return "linux";
#endif
}
