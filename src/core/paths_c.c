#include "core/paths_c.h"
#include "core/fs_c.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define PZ_PATH_SEPARATOR '\\'
#else
#define PZ_PATH_SEPARATOR '/'
#endif

static int is_separator(char value) {
#ifdef _WIN32
    return value == '\\' || value == '/';
#else
    return value == '/';
#endif
}

#ifdef _WIN32
static int is_ascii_alpha(char value) {
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
}
#endif

static int key_is(const char* key, size_t length, const char* expected) {
    const size_t expected_length = strlen(expected);
    return length == expected_length && memcmp(key, expected, length) == 0;
}

static int assign_string(pz_str* out, const char* data, size_t length) {
    return pz_str_assign(out, data, length);
}

static int add_locked_target(pz_paths_config* config, const char* value,
                             size_t length) {
    size_t i;
    pz_str* slot;

    if (length == 0) return 1;
    for (i = 0; i < config->locked_targets.length; ++i) {
        const pz_str* existing =
            (const pz_str*)pz_vec_at_const(&config->locked_targets, i);
        if (pz_str_equals(existing, value, length)) return 1;
    }
    slot = (pz_str*)pz_vec_push(&config->locked_targets);
    if (!slot) return 0;
    if (!pz_str_assign(slot, value, length)) {
        --config->locked_targets.length;
        return 0;
    }
    return 1;
}

void pz_paths_config_init(pz_paths_config* config) {
    if (!config) return;
    memset(config, 0, sizeof(*config));
    (void)pz_vec_init(&config->locked_targets, sizeof(pz_str));
}

void pz_paths_config_free(pz_paths_config* config) {
    size_t i;
    if (!config) return;
    for (i = 0; i < config->locked_targets.length; ++i)
        pz_str_free((pz_str*)pz_vec_at(&config->locked_targets, i));
    pz_vec_free(&config->locked_targets);
    pz_str_free(&config->steamcmd_exe);
    pz_str_free(&config->steamcmd_root);
    pz_str_free(&config->steam_user);
    pz_str_free(&config->zomboid_dir);
    pz_str_free(&config->user_mods_dir);
    pz_str_free(&config->data_dir);
    pz_str_free(&config->config_file);
    pz_paths_config_init(config);
}

int pz_paths_is_absolute(const char* path, size_t length) {
    if (!path || length == 0) return 0;
#ifdef _WIN32
    if (length >= 3 && is_ascii_alpha(path[0]) && path[1] == ':' &&
        is_separator(path[2]))
        return 1;
    if (length >= 2 && is_separator(path[0]) && is_separator(path[1])) {
        size_t i = 2;
        while (i < length && !is_separator(path[i])) ++i;
        return i > 2 && i < length;
    }
    return 0;
#else
    return path[0] == '/';
#endif
}

int pz_paths_join(pz_str* out, const char* base, size_t base_length,
                 const char* child, size_t child_length) {
    if (!out) return 0;
    if (child_length == 0) return pz_str_assign(out, base, base_length);
    if (base_length == 0 || pz_paths_is_absolute(child, child_length))
        return pz_str_assign(out, child, child_length);

    if (!pz_str_assign(out, base, base_length)) return 0;
    if (out->length == 0 || !is_separator(out->data[out->length - 1])) {
        if (!pz_str_append_char(out, PZ_PATH_SEPARATOR)) return 0;
    }
    return pz_str_append(out, child, child_length);
}

int pz_paths_builtin_steamcmd(const char* data_dir, size_t length, pz_str* out) {
    pz_str directory;
    int ok;

    if (!out) return 0;
#ifdef _WIN32
    {
        static const char kName[] = "steamcmd.exe";
        pz_str_init(&directory);
        ok = pz_paths_join(&directory, data_dir, length, "steamcmd", 8) &&
             pz_paths_join(out, directory.data, directory.length,
                          kName, sizeof(kName) - 1);
    }
#else
    {
        static const char kName[] = "steamcmd.sh";
        pz_str_init(&directory);
        ok = pz_paths_join(&directory, data_dir, length, "steamcmd", 8) &&
             pz_paths_join(out, directory.data, directory.length,
                          kName, sizeof(kName) - 1);
    }
#endif
    pz_str_free(&directory);
    return ok;
}

static int set_path_text(pz_str* out, const pz_path* path) {
    return pz_str_assign(out, path->text.data, path->text.length);
}

int pz_paths_config_default(pz_paths_config* config) {
    const char* home_text;
    pz_path home;
    pz_path zomboid;
    pz_path mods;
    pz_path data;
    pz_path exe;
    pz_path exe_dir;
    pz_path installed;
    pz_fs_error error;
    int installed_exists = 0;
    int ok = 0;

    if (!config) return 0;
    pz_path_init(&home);
    pz_path_init(&zomboid);
    pz_path_init(&mods);
    pz_path_init(&data);
    pz_path_init(&exe);
    pz_path_init(&exe_dir);
    pz_path_init(&installed);
    pz_fs_error_clear(&error);

#ifdef _WIN32
    home_text = getenv("USERPROFILE");
#else
    home_text = getenv("HOME");
#endif
    if (home_text && home_text[0] != '\0') {
        if (!pz_path_assign_cstr(&home, home_text)) goto done;
    } else if (!pz_fs_current_dir(&home, &error)) {
        goto done;
    }

    if (!pz_path_join(&zomboid, &home, "Zomboid", 7) ||
        !pz_path_join(&mods, &zomboid, "mods", 4) ||
        !pz_path_join(&data, &zomboid, "pzmodmgr", 8) ||
        !set_path_text(&config->zomboid_dir, &zomboid) ||
        !set_path_text(&config->user_mods_dir, &mods) ||
        !set_path_text(&config->data_dir, &data))
        goto done;

    if (pz_fs_executable_path(&exe, &error) && pz_path_parent(&exe, &exe_dir) &&
        exe_dir.text.length != 0 &&
        pz_path_join(&installed, &exe_dir, "config.ini", 10) &&
        pz_fs_exists(&installed, &installed_exists, &error) && installed_exists) {
        if (!set_path_text(&config->config_file, &installed)) goto done;
    } else {
        pz_path default_file;
        pz_path_init(&default_file);
        if (!pz_path_join(&default_file, &data, "config.ini", 10) ||
            !set_path_text(&config->config_file, &default_file)) {
            pz_path_free(&default_file);
            goto done;
        }
        pz_path_free(&default_file);
    }

    if (!pz_paths_builtin_steamcmd(config->data_dir.data,
                                  config->data_dir.length,
                                  &config->steamcmd_exe))
        goto done;
    ok = 1;

done:
    pz_path_free(&installed);
    pz_path_free(&exe_dir);
    pz_path_free(&exe);
    pz_path_free(&data);
    pz_path_free(&mods);
    pz_path_free(&zomboid);
    pz_path_free(&home);
    return ok;
}

int pz_paths_config_copy(pz_paths_config* out, const pz_paths_config* in) {
    size_t i;

    if (!out || !in) return 0;
    pz_paths_config_init(out);
    if (!pz_str_assign(&out->steamcmd_exe, in->steamcmd_exe.data,
                       in->steamcmd_exe.length) ||
        !pz_str_assign(&out->steamcmd_root, in->steamcmd_root.data,
                       in->steamcmd_root.length) ||
        !pz_str_assign(&out->steam_user, in->steam_user.data,
                       in->steam_user.length) ||
        !pz_str_assign(&out->zomboid_dir, in->zomboid_dir.data,
                       in->zomboid_dir.length) ||
        !pz_str_assign(&out->user_mods_dir, in->user_mods_dir.data,
                       in->user_mods_dir.length) ||
        !pz_str_assign(&out->data_dir, in->data_dir.data, in->data_dir.length) ||
        !pz_str_assign(&out->config_file, in->config_file.data,
                       in->config_file.length))
        goto fail;
    out->dark_mode = in->dark_mode;
    out->auto_enable = in->auto_enable;
    for (i = 0; i < in->locked_targets.length; ++i) {
        const pz_str* key =
            (const pz_str*)pz_vec_at_const(&in->locked_targets, i);
        if (!add_locked_target(out, key->data, key->length)) goto fail;
    }
    return 1;

fail:
    pz_paths_config_free(out);
    return 0;
}

static unsigned long long g_path_temp_counter;

static int write_file_atomic(const pz_path* file, const pz_str* text) {
    pz_path parent;
    pz_path temp;
    pz_str suffix;
    pz_fs_file* handle = NULL;
    pz_fs_error error;
    int64_t now = 0;
    size_t written = 0;
    int ok = 0;

    pz_path_init(&parent);
    pz_path_init(&temp);
    pz_str_init(&suffix);
    pz_fs_error_clear(&error);

    if (!pz_path_parent(file, &parent)) goto done;
    if (parent.text.length != 0 &&
        !pz_fs_create_directories(&parent, &error))
        goto done;
    if (!pz_fs_file_time_now(&now)) now = 0;
    if (!pz_str_append_u64(&suffix, (uint64_t)now) ||
        !pz_str_append_cstr(&suffix, ".tmp.") ||
        !pz_str_append_u64(&suffix, ++g_path_temp_counter) ||
        !pz_path_assign(&temp, file->text.data, file->text.length) ||
        !pz_str_append(&temp.text, suffix.data, suffix.length))
        goto done;

    if (!pz_fs_open_write(&temp, &handle, &error)) goto done;
    while (written < text->length) {
        const size_t put =
            pz_fs_write(handle, text->data + written, text->length - written);
        if (put == 0) break;
        written += put;
    }
    if (!pz_fs_close(handle, &error)) {
        handle = NULL;
        goto done;
    }
    handle = NULL;
    if (written != text->length) goto done;
    if (!pz_fs_replace_file(&temp, file, &error)) goto done;
    ok = 1;

done:
    if (handle) {
        pz_fs_error ignored;
        pz_fs_error_clear(&ignored);
        pz_fs_close(handle, &ignored);
    }
    if (!ok && temp.text.data) {
        pz_fs_error ignored;
        pz_fs_error_clear(&ignored);
        pz_fs_remove(&temp, &ignored);
    }
    pz_str_free(&suffix);
    pz_path_free(&temp);
    pz_path_free(&parent);
    return ok;
}

int pz_paths_config_load(pz_paths_config* config) {
    pz_path file;
    pz_paths_config parsed;
    pz_str text;
    pz_fs_error error;
    int ok = 0;

    if (!config) return 0;
    pz_path_init(&file);
    pz_str_init(&text);
    pz_fs_error_clear(&error);
    if (!pz_path_assign(&file, config->config_file.data,
                        config->config_file.length) ||
        !pz_fs_read_all(&file, &text, &error)) {
        ok = 1;
        goto done;
    }
    if (!pz_paths_config_copy(&parsed, config)) goto done;
    if (!pz_paths_config_parse(&parsed, text.data, text.length)) {
        pz_paths_config_free(&parsed);
        ok = 1;
        goto done;
    }
    pz_paths_config_free(config);
    *config = parsed;
    ok = 1;

done:
    pz_str_free(&text);
    pz_path_free(&file);
    return ok;
}

int pz_paths_config_save(const pz_paths_config* config) {
    pz_path file;
    pz_str text;
    int ok = 0;

    if (!config) return 0;
    pz_path_init(&file);
    pz_str_init(&text);
    if (config->config_file.length != 0) {
        if (!pz_path_assign(&file, config->config_file.data,
                            config->config_file.length))
            goto done;
    } else {
        if (!pz_path_assign(&file, config->data_dir.data,
                            config->data_dir.length) ||
            !pz_path_join(&file, &file, "config.ini", 10))
            goto done;
    }
    if (!pz_paths_config_format(config, &text)) goto done;
    ok = write_file_atomic(&file, &text);

done:
    pz_str_free(&text);
    pz_path_free(&file);
    return ok;
}

int pz_paths_config_parse(pz_paths_config* config, const char* text,
                          size_t length) {
    pz_str steamcmd_exe;
    pz_str steamcmd_root;
    pz_str steam_user;
    pz_str zomboid_dir;
    int has_dark_mode = 0;
    int dark_mode = 0;
    int has_auto_enable = 0;
    int auto_enable = 0;
    int ok = 1;
    size_t start = 0;

    if (!config || (!text && length != 0)) return 0;
    pz_str_init(&steamcmd_exe);
    pz_str_init(&steamcmd_root);
    pz_str_init(&steam_user);
    pz_str_init(&zomboid_dir);

    while (start < length) {
        size_t end = start;
        size_t line_end;
        const char* line;
        size_t line_length;
        const char* equals = NULL;

        while (end < length && text[end] != '\n') ++end;
        line_end = end;
        if (line_end > start && text[line_end - 1] == '\r') --line_end;
        line = text + start;
        line_length = line_end - start;
        equals = (const char*)memchr(line, '=', line_length);
        if (equals) {
            const size_t key_length = (size_t)(equals - line);
            const char* value = equals + 1;
            const size_t value_length = line_length - key_length - 1;

            if (key_is(line, key_length, "steamcmd_exe"))
                ok = assign_string(&steamcmd_exe, value, value_length);
            else if (key_is(line, key_length, "steamcmd_root"))
                ok = assign_string(&steamcmd_root, value, value_length);
            else if (key_is(line, key_length, "steam_user"))
                ok = assign_string(&steam_user, value, value_length);
            else if (key_is(line, key_length, "zomboid_dir"))
                ok = assign_string(&zomboid_dir, value, value_length);
            else if (key_is(line, key_length, "dark_mode")) {
                has_dark_mode = 1;
                dark_mode = (value_length == 1 && value[0] == '1') ||
                            (value_length == 2 && memcmp(value, "on", 2) == 0) ||
                            (value_length == 4 && memcmp(value, "true", 4) == 0) ||
                            (value_length == 3 && memcmp(value, "yes", 3) == 0) ||
                            (value_length == 4 && memcmp(value, "dark", 4) == 0);
            } else if (key_is(line, key_length, "auto_enable")) {
                has_auto_enable = 1;
                auto_enable = (value_length == 1 && value[0] == '1') ||
                              (value_length == 2 && memcmp(value, "on", 2) == 0) ||
                              (value_length == 4 && memcmp(value, "true", 4) == 0) ||
                              (value_length == 3 && memcmp(value, "yes", 3) == 0);
            } else if (key_is(line, key_length, "locked_target")) {
                if (!add_locked_target(config, value, value_length)) ok = 0;
            }
        }
        if (!ok) break;
        if (end == length) break;
        start = end + 1;
    }

    if (ok && zomboid_dir.length != 0 &&
        pz_paths_is_absolute(zomboid_dir.data, zomboid_dir.length)) {
        pz_str old_builtin;
        pz_str new_builtin;
        pz_str_init(&old_builtin);
        pz_str_init(&new_builtin);
        if (!pz_paths_builtin_steamcmd(config->data_dir.data,
                                       config->data_dir.length,
                                       &old_builtin)) {
            ok = 0;
        }
        if (ok && !pz_str_assign(&config->zomboid_dir, zomboid_dir.data,
                                 zomboid_dir.length))
            ok = 0;
        if (ok && !pz_paths_join(&config->user_mods_dir, zomboid_dir.data,
                                zomboid_dir.length, "mods", 4))
            ok = 0;
        if (ok && !pz_paths_join(&config->data_dir, zomboid_dir.data,
                                zomboid_dir.length, "pzmodmgr", 8))
            ok = 0;
        if (ok && !pz_paths_builtin_steamcmd(config->data_dir.data,
                                             config->data_dir.length,
                                             &new_builtin))
            ok = 0;
        if (ok && (config->steamcmd_exe.length == 0 ||
                   pz_str_equals(&config->steamcmd_exe, old_builtin.data,
                                 old_builtin.length))) {
            if (!pz_str_assign(&config->steamcmd_exe, new_builtin.data,
                               new_builtin.length))
                ok = 0;
        }
        pz_str_free(&old_builtin);
        pz_str_free(&new_builtin);
    }

    if (ok && steamcmd_exe.length != 0 &&
        !pz_str_assign(&config->steamcmd_exe, steamcmd_exe.data,
                       steamcmd_exe.length))
        ok = 0;
    if (ok && steamcmd_root.length != 0 &&
        !pz_str_assign(&config->steamcmd_root, steamcmd_root.data,
                       steamcmd_root.length))
        ok = 0;
    if (ok && !pz_str_assign(&config->steam_user, steam_user.data,
                             steam_user.length))
        ok = 0;
    if (ok && has_dark_mode) config->dark_mode = dark_mode;
    if (ok && has_auto_enable) config->auto_enable = auto_enable;

    pz_str_free(&steamcmd_exe);
    pz_str_free(&steamcmd_root);
    pz_str_free(&steam_user);
    pz_str_free(&zomboid_dir);
    return ok;
}

static int append_field(pz_str* out, const char* name, const pz_str* value) {
    return pz_str_append_cstr(out, name) &&
           pz_str_append(out, value->data, value->length) &&
           pz_str_append_char(out, '\n');
}

int pz_paths_config_format(const pz_paths_config* config, pz_str* out) {
    size_t i;

    if (!config || !out) return 0;
    pz_str_clear(out);
    if (!append_field(out, "steamcmd_exe=", &config->steamcmd_exe)) return 0;
    if (!append_field(out, "steamcmd_root=", &config->steamcmd_root)) return 0;
    if (!append_field(out, "steam_user=", &config->steam_user)) return 0;
    if (!append_field(out, "zomboid_dir=", &config->zomboid_dir)) return 0;
    if (!pz_str_append_cstr(out, config->dark_mode ? "dark_mode=1\n"
                                                   : "dark_mode=0\n"))
        return 0;
    if (!pz_str_append_cstr(out, config->auto_enable ? "auto_enable=1\n"
                                                     : "auto_enable=0\n"))
        return 0;
    for (i = 0; i < config->locked_targets.length; ++i) {
        const pz_str* key =
            (const pz_str*)pz_vec_at_const(&config->locked_targets, i);
        if (!append_field(out, "locked_target=", key)) return 0;
    }
    return 1;
}
