#include "core/mod_manager_c.h"

#include "core/url_c.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PZ_MM_APP_ID 108600ULL
#define PZ_MM_CHECK_INTERVAL (24ULL * 60ULL * 60ULL)

typedef struct pz_update_entry {
    uint64_t workshop_id;
    int available;
} pz_update_entry;

struct pz_mod_manager {
    pz_paths_config config;
    pz_str game_version;
    pz_vec db;      /* pz_installed_mod */
    pz_vec updates; /* pz_update_entry */
    int app_update_available;
    const pz_mm_services* services;
};

/* ------------------------------------------------------------------ strings */

static int str_assign(pz_str* out, const pz_str* value) {
    return pz_str_assign(out, value ? value->data : NULL, value ? value->length : 0);
}

static int str_assign_text(pz_str* out, const char* data, size_t length) {
    return pz_str_assign(out, data, length);
}

static int str_equals_text(const pz_str* value, const char* data, size_t length) {
    return pz_str_equals(value, data, length);
}

static int str_equals_cstr(const pz_str* value, const char* text) {
    return pz_str_equals(value, text, strlen(text));
}

static int parse_u64(const char* text, size_t length, uint64_t* value) {
    uint64_t parsed = 0;
    size_t i;

    if (!value || length == 0) return 0;
    for (i = 0; i < length; ++i) {
        const uint64_t digit = (uint64_t)(text[i] - '0');
        if (text[i] < '0' || text[i] > '9') return 0;
        if (parsed > (UINT64_MAX - digit) / 10) return 0;
        parsed = parsed * 10 + digit;
    }
    *value = parsed;
    return 1;
}

static int fold_equals(const pz_str* left, const pz_str* right) {
    size_t i;
    if (!left || !right || left->length != right->length) return 0;
    for (i = 0; i < left->length; ++i) {
        if (tolower((unsigned char)left->data[i]) !=
            tolower((unsigned char)right->data[i]))
            return 0;
    }
    return 1;
}

static int make_folded(const pz_str* value, pz_str* out) {
    size_t i;
    pz_str_clear(out);
    for (i = 0; i < value->length; ++i) {
        const char c = (char)tolower((unsigned char)value->data[i]);
        if (!pz_str_append_char(out, c)) return 0;
    }
    return 1;
}

/* -------------------------------------------------------------- string lists */

static int string_list_contains(const pz_vec* list, const pz_str* value) {
    size_t i;
    for (i = 0; i < list->length; ++i) {
        const pz_str* item = (const pz_str*)pz_vec_at_const(list, i);
        if (str_equals_text(item, value->data, value->length)) return 1;
    }
    return 0;
}

static int string_list_contains_cstr(const pz_vec* list, const char* value) {
    size_t i;
    for (i = 0; i < list->length; ++i) {
        const pz_str* item = (const pz_str*)pz_vec_at_const(list, i);
        if (str_equals_cstr(item, value)) return 1;
    }
    return 0;
}

static int string_list_add_unique(pz_vec* list, const pz_str* value) {
    pz_str* slot;
    if (value->length == 0 || string_list_contains(list, value)) return 1;
    slot = (pz_str*)pz_vec_push(list);
    if (!slot) return 0;
    pz_str_init(slot);
    if (!str_assign(slot, value)) {
        pz_str_free(slot);
        --list->length;
        return 0;
    }
    return 1;
}

static void string_list_free(pz_vec* list) {
    pz_activation_string_list_free(list);
}

static int u64_list_contains(const pz_vec* list, uint64_t value) {
    size_t i;
    for (i = 0; i < list->length; ++i) {
        const uint64_t* item = (const uint64_t*)pz_vec_at_const(list, i);
        if (*item == value) return 1;
    }
    return 0;
}

static int u64_list_add_unique(pz_vec* list, uint64_t value) {
    uint64_t* slot;
    if (value == 0 || u64_list_contains(list, value)) return 1;
    slot = (uint64_t*)pz_vec_push(list);
    if (!slot) return 0;
    *slot = value;
    return 1;
}

static void u64_list_free(pz_vec* list) {
    pz_vec_free(list);
}

/* ------------------------------------------------------------------ records */

void pz_installed_mod_init(pz_installed_mod* mod) {
    if (!mod) return;
    memset(mod, 0, sizeof(*mod));
    pz_str_init(&mod->folder);
    pz_str_init(&mod->name);
}

void pz_installed_mod_free(pz_installed_mod* mod) {
    if (!mod) return;
    pz_str_free(&mod->folder);
    pz_str_free(&mod->name);
}

void pz_missing_requirement_init(pz_missing_requirement* value) {
    if (!value) return;
    memset(value, 0, sizeof(*value));
    pz_str_init(&value->mod_name);
    pz_str_init(&value->folder);
    pz_str_init(&value->required_id);
}

void pz_missing_requirement_free(pz_missing_requirement* value) {
    if (!value) return;
    pz_str_free(&value->mod_name);
    pz_str_free(&value->folder);
    pz_str_free(&value->required_id);
}

void pz_activation_target_data_init(pz_activation_target_data* target) {
    if (!target) return;
    memset(target, 0, sizeof(*target));
    pz_str_init(&target->name);
    pz_str_init(&target->label);
    pz_str_init(&target->path);
    pz_str_init(&target->thumbnail);
    pz_vec_init(&target->mod_ids, sizeof(pz_str));
    pz_vec_init(&target->workshop_ids, sizeof(uint64_t));
}

void pz_activation_target_data_free(pz_activation_target_data* target) {
    if (!target) return;
    pz_str_free(&target->name);
    pz_str_free(&target->label);
    pz_str_free(&target->path);
    pz_str_free(&target->thumbnail);
    string_list_free(&target->mod_ids);
    u64_list_free(&target->workshop_ids);
}

void pz_remote_details_init(pz_remote_details* details) {
    if (!details) return;
    memset(details, 0, sizeof(*details));
    pz_str_init(&details->title);
}

void pz_remote_details_free(pz_remote_details* details) {
    if (!details) return;
    pz_str_free(&details->title);
}

void pz_mm_result_init(pz_mm_result* result) {
    if (!result) return;
    result->ok = 0;
    pz_str_init(&result->message);
}

void pz_mm_result_free(pz_mm_result* result) {
    if (result) pz_str_free(&result->message);
}

static pz_mm_result result_ok_text(const char* text) {
    pz_mm_result result;
    pz_mm_result_init(&result);
    result.ok = 1;
    if (!pz_str_assign_cstr(&result.message, text)) result.ok = 0;
    return result;
}

static pz_mm_result result_fail_text(const char* text) {
    pz_mm_result result;
    pz_mm_result_init(&result);
    result.ok = 0;
    if (!pz_str_assign_cstr(&result.message, text)) result.ok = 0;
    return result;
}

static pz_mm_result result_fail_str(const pz_str* text) {
    pz_mm_result result;
    pz_mm_result_init(&result);
    if (!str_assign(&result.message, text)) result.ok = 0;
    return result;
}

/* --------------------------------------------------------------- db helpers */

static pz_installed_mod* db_at(pz_mod_manager* manager, size_t index) {
    return (pz_installed_mod*)pz_vec_at(&manager->db, index);
}

static const pz_installed_mod* db_at_const(const pz_mod_manager* manager,
                                           size_t index) {
    return (const pz_installed_mod*)pz_vec_at_const(&manager->db, index);
}

static int db_is_installed(const pz_mod_manager* manager, uint64_t id) {
    size_t i;
    for (i = 0; i < manager->db.length; ++i)
        if (db_at_const(manager, i)->workshop_id == id) return 1;
    return 0;
}

static uint64_t db_recorded_time(const pz_mod_manager* manager, uint64_t id) {
    uint64_t value = 0;
    size_t i;
    for (i = 0; i < manager->db.length; ++i) {
        const pz_installed_mod* mod = db_at_const(manager, i);
        if (mod->workshop_id == id && mod->time_updated > value)
            value = mod->time_updated;
    }
    return value;
}

static int db_add(pz_mod_manager* manager, uint64_t id, const char* folder,
                  size_t folder_length, const char* name, size_t name_length,
                  uint64_t time_updated, int owned, int locked, int imported) {
    pz_installed_mod* mod = (pz_installed_mod*)pz_vec_push(&manager->db);
    if (!mod) return 0;
    pz_installed_mod_init(mod);
    mod->workshop_id = id;
    mod->time_updated = time_updated;
    mod->owned = owned;
    mod->locked = locked;
    mod->imported = imported;
    if (!str_assign_text(&mod->folder, folder, folder_length) ||
        !str_assign_text(&mod->name, name, name_length)) {
        pz_installed_mod_free(mod);
        --manager->db.length;
        return 0;
    }
    return 1;
}

static void db_remove_id(pz_mod_manager* manager, uint64_t id) {
    size_t i = 0;
    while (i < manager->db.length) {
        pz_installed_mod* mod = db_at(manager, i);
        if (mod->workshop_id == id) {
            pz_installed_mod_free(mod);
            memmove(mod, mod + 1,
                    (manager->db.length - i - 1) * sizeof(*mod));
            --manager->db.length;
        } else {
            ++i;
        }
    }
}

static int db_same(const pz_vec* left, const pz_vec* right) {
    size_t i;
    if (left->length != right->length || left->item_size != right->item_size)
        return 0;
    for (i = 0; i < left->length; ++i) {
        const pz_installed_mod* a = (const pz_installed_mod*)pz_vec_at_const(left, i);
        const pz_installed_mod* b = (const pz_installed_mod*)pz_vec_at_const(right, i);
        if (a->workshop_id != b->workshop_id ||
            a->time_updated != b->time_updated || a->owned != b->owned ||
            a->locked != b->locked || a->imported != b->imported ||
            !str_equals_text(&a->folder, b->folder.data, b->folder.length) ||
            !str_equals_text(&a->name, b->name.data, b->name.length))
            return 0;
    }
    return 1;
}

/* ------------------------------------------------------------- update state */

static pz_update_entry* update_find(pz_mod_manager* manager, uint64_t id) {
    size_t i;
    for (i = 0; i < manager->updates.length; ++i) {
        pz_update_entry* entry = (pz_update_entry*)pz_vec_at(&manager->updates, i);
        if (entry->workshop_id == id) return entry;
    }
    return NULL;
}

static const pz_update_entry* update_find_const(const pz_mod_manager* manager,
                                                uint64_t id) {
    size_t i;
    for (i = 0; i < manager->updates.length; ++i) {
        const pz_update_entry* entry =
            (const pz_update_entry*)pz_vec_at_const(&manager->updates, i);
        if (entry->workshop_id == id) return entry;
    }
    return NULL;
}

static int update_set(pz_mod_manager* manager, uint64_t id, int available) {
    pz_update_entry* entry = update_find(manager, id);
    if (!entry) {
        entry = (pz_update_entry*)pz_vec_push(&manager->updates);
        if (!entry) return 0;
        entry->workshop_id = id;
    }
    entry->available = available;
    return 1;
}

static void update_erase(pz_mod_manager* manager, uint64_t id) {
    size_t i = 0;
    while (i < manager->updates.length) {
        pz_update_entry* entry = (pz_update_entry*)pz_vec_at(&manager->updates, i);
        if (entry->workshop_id == id) {
            memmove(entry, entry + 1,
                    (manager->updates.length - i - 1) * sizeof(*entry));
            --manager->updates.length;
        } else {
            ++i;
        }
    }
}

/* ---------------------------------------------------------------- paths/io */

static int path_from_str(pz_path* path, const pz_str* value) {
    pz_path_init(path);
    return pz_path_assign(path, value->data, value->length);
}

static int path_join_text(pz_path* out, const pz_path* base, const char* leaf) {
    pz_path joined;
    pz_path_init(&joined);
    if (!pz_path_join(&joined, base, leaf, strlen(leaf))) {
        pz_path_free(&joined);
        return 0;
    }
    pz_path_free(out);
    *out = joined;
    return 1;
}

static int path_join_u64(pz_path* out, const pz_path* base, uint64_t value,
                         const char* suffix) {
    pz_str text;
    pz_path joined;
    int ok;
    pz_path_init(&joined);
    pz_str_init(&text);
    ok = pz_str_append_u64(&text, value);
    if (ok && suffix) ok = pz_str_append_cstr(&text, suffix);
    if (ok) ok = pz_path_join(&joined, base, text.data, text.length);
    pz_str_free(&text);
    if (!ok) {
        pz_path_free(&joined);
        return 0;
    }
    pz_path_free(out);
    *out = joined;
    return 1;
}

static int path_is_dir(const pz_path* path) {
    pz_fs_kind kind;
    pz_fs_error error;
    pz_fs_error_clear(&error);
    return pz_fs_kind_of(path, &kind, &error) && kind == PZ_FS_DIR;
}

static int path_is_file(const pz_path* path) {
    pz_fs_kind kind;
    pz_fs_error error;
    pz_fs_error_clear(&error);
    return pz_fs_kind_of(path, &kind, &error) &&
           (kind == PZ_FS_FILE || kind == PZ_FS_LINK);
}

static int path_exists(const pz_path* path) {
    int exists = 0;
    pz_fs_error error;
    pz_fs_error_clear(&error);
    return pz_fs_exists(path, &exists, &error) && exists;
}

static int read_file(const pz_path* path, pz_str* out) {
    pz_fs_error error;
    pz_fs_error_clear(&error);
    return pz_fs_read_all(path, out, &error);
}

static unsigned long long g_temp_counter;

static int write_file_atomic(const pz_path* file, const pz_str* text) {
    pz_path parent;
    pz_path temp;
    pz_str suffix;
    pz_str name;
    pz_fs_file* handle = NULL;
    pz_fs_error error;
    int64_t now = 0;
    size_t written = 0;
    int ok = 0;

    pz_path_init(&parent);
    pz_path_init(&temp);
    pz_str_init(&suffix);
    pz_str_init(&name);
    pz_fs_error_clear(&error);

    if (!pz_path_parent(file, &parent)) goto done;
    if (parent.text.length != 0 &&
        !pz_fs_create_directories(&parent, &error))
        goto done;

    if (!pz_fs_file_time_now(&now)) now = 0;
    if (!pz_str_append_u64(&suffix, (uint64_t)now) ||
        !pz_str_append_cstr(&suffix, ".tmp.") ||
        !pz_str_append_u64(&suffix, ++g_temp_counter))
        goto done;
    if (!pz_path_filename(file, &name) ||
        !pz_path_assign(&temp, file->text.data, file->text.length) ||
        !pz_str_append(&temp.text, suffix.data, suffix.length))
        goto done;
    (void)name;

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
    pz_str_free(&name);
    pz_str_free(&suffix);
    pz_path_free(&temp);
    pz_path_free(&parent);
    return ok;
}

static int write_text_file(const pz_path* file, const char* text, size_t length) {
    pz_str value;
    int ok;
    pz_str_init(&value);
    ok = str_assign_text(&value, text, length) && write_file_atomic(file, &value);
    pz_str_free(&value);
    return ok;
}

/* --------------------------------------------------------------- callbacks */

static int read_info(pz_mod_manager* manager, const pz_path* dir, pz_mod_info* out) {
    const char* path = dir->text.data ? dir->text.data : "";
    pz_mod_info_init(out);
    if (!manager->services->read_mod_info(
            manager->services->context, path, dir->text.length,
            manager->game_version.data, manager->game_version.length, out)) {
        pz_mod_info_free(out);
        return 0;
    }
    return 1;
}

static int discover_roots(pz_mod_manager* manager, const pz_path* path, pz_vec* out) {
    pz_vec_init(out, sizeof(pz_str));
    return manager->services->discover_mod_roots(
        manager->services->context, path->text.data, path->text.length, out);
}

static int discover_targets(pz_mod_manager* manager, pz_vec* out) {
    const pz_path zomboid = {manager->config.zomboid_dir};
    pz_vec_init(out, sizeof(pz_activation_target_data));
    return manager->services->discover_targets(
        manager->services->context, zomboid.text.data, zomboid.text.length,
        manager->game_version.data, manager->game_version.length, out);
}

static int target_key(const pz_activation_target_data* target, pz_str* key) {
    pz_str_clear(key);
    if (target->kind == 0) return pz_str_assign_cstr(key, "default");
    if (!pz_str_assign_cstr(key, target->kind == 1 ? "save:" : "server:"))
        return 0;
    return pz_str_append(key, target->name.data, target->name.length);
}

static int config_has_target(const pz_paths_config* config, const pz_str* key) {
    return string_list_contains(&config->locked_targets, key);
}

/* --------------------------------------------------------------- state load */

static int next_line(const char* text, size_t length, size_t* position,
                     const char** line, size_t* line_length) {
    size_t start = *position;
    size_t end;
    if (start >= length) return 0;
    end = start;
    while (end < length && text[end] != '\n') ++end;
    *line = text + start;
    *line_length = end - start;
    if (*line_length != 0 && (*line)[*line_length - 1] == '\r')
        --*line_length;
    *position = end < length ? end + 1 : length;
    return 1;
}

static void load_db(pz_mod_manager* manager) {
    pz_path file;
    pz_str text;
    size_t position = 0;
    const char* line;
    size_t line_length;

    while (manager->db.length != 0) {
        pz_installed_mod* mod = db_at(manager, manager->db.length - 1);
        pz_installed_mod_free(mod);
        --manager->db.length;
    }
    pz_path_init(&file);
    pz_str_init(&text);
    if (!path_from_str(&file, &manager->config.data_dir) ||
        !path_join_text(&file, &file, "installed.tsv") ||
        !read_file(&file, &text))
        goto done;

    while (next_line(text.data, text.length, &position, &line, &line_length)) {
        const char* fields[7];
        size_t lengths[7];
        size_t count = 0;
        size_t start = 0;
        size_t i;
        uint64_t id;
        uint64_t time_updated;

        for (i = 0; i <= line_length && count < 7; ++i) {
            if (i == line_length || line[i] == '\t') {
                fields[count] = line + start;
                lengths[count] = i - start;
                ++count;
                start = i + 1;
            }
        }
        if (count < 4) continue;
        if (!parse_u64(fields[0], lengths[0], &id) ||
            !parse_u64(fields[3], lengths[3], &time_updated))
            continue;
        db_add(manager, id, fields[1], lengths[1], fields[2], lengths[2],
               time_updated, !(count >= 5 && lengths[4] == 1 && fields[4][0] == '0'),
               count >= 6 && !(lengths[5] == 1 && fields[5][0] == '0'),
               count >= 7 && !(lengths[6] == 1 && fields[6][0] == '0'));
    }

done:
    pz_str_free(&text);
    pz_path_free(&file);
}

static void load_update_state(pz_mod_manager* manager) {
    pz_path file;
    pz_str text;
    size_t position = 0;
    const char* line;
    size_t line_length;
    size_t i;

    manager->updates.length = 0;
    pz_path_init(&file);
    pz_str_init(&text);
    if (!path_from_str(&file, &manager->config.data_dir) ||
        !path_join_text(&file, &file, "updates.tsv") ||
        !read_file(&file, &text))
        goto done;

    while (next_line(text.data, text.length, &position, &line, &line_length)) {
        const char* tab = (const char*)memchr(line, '\t', line_length);
        const size_t id_length = tab ? (size_t)(tab - line) : line_length;
        uint64_t id;
        if (!parse_u64(line, id_length, &id)) continue;
        if (tab && line_length - id_length - 1 >= 1 && tab[1] == '0') continue;
        if (!db_is_installed(manager, id)) continue;
        if (!update_set(manager, id, 1)) break;
    }
    for (i = 0; i < manager->updates.length;) {
        pz_update_entry* entry = (pz_update_entry*)pz_vec_at(&manager->updates, i);
        if (pz_mod_manager_locked(manager, entry->workshop_id)) {
            update_erase(manager, entry->workshop_id);
        } else {
            ++i;
        }
    }

done:
    pz_str_free(&text);
    pz_path_free(&file);
}

static void load_app_state(pz_mod_manager* manager) {
    pz_path file;
    pz_str text;
    pz_str latest;
    const char* app_version;

    manager->app_update_available = 0;
    pz_path_init(&file);
    pz_str_init(&text);
    pz_str_init(&latest);
    if (!path_from_str(&file, &manager->config.data_dir) ||
        !path_join_text(&file, &file, "app_update") ||
        !read_file(&file, &text))
        goto done;
    {
        size_t end = 0;
        while (end < text.length && !isspace((unsigned char)text.data[end]))
            ++end;
        if (end != 0 && str_assign_text(&latest, text.data, end)) {
            app_version = manager->services->app_version(manager->services->context);
            if (app_version &&
                pz_app_version_newer(latest.data, latest.length, app_version,
                                     strlen(app_version)))
                manager->app_update_available = 1;
        }
    }

done:
    pz_str_free(&latest);
    pz_str_free(&text);
    pz_path_free(&file);
}

static uint64_t read_timestamp(pz_mod_manager* manager, const char* name) {
    pz_path file;
    pz_str text;
    uint64_t value = 0;

    pz_path_init(&file);
    pz_str_init(&text);
    if (path_from_str(&file, &manager->config.data_dir) &&
        path_join_text(&file, &file, name) && read_file(&file, &text)) {
        size_t end = 0;
        while (end < text.length && isspace((unsigned char)text.data[end])) ++end;
        {
            size_t start = end;
            while (end < text.length && !isspace((unsigned char)text.data[end]))
                ++end;
            if (!parse_u64(text.data + start, end - start, &value)) value = 0;
        }
    }
    pz_str_free(&text);
    pz_path_free(&file);
    return value;
}

static void record_timestamp(pz_mod_manager* manager, const char* name) {
    pz_path file;
    pz_str text;

    pz_path_init(&file);
    pz_str_init(&text);
    if (path_from_str(&file, &manager->config.data_dir) &&
        path_join_text(&file, &file, name) &&
        pz_str_append_u64(&text, (uint64_t)time(NULL)) &&
        pz_str_append_char(&text, '\n'))
        write_text_file(&file, text.data, text.length);
    pz_str_free(&text);
    pz_path_free(&file);
}

static void save_db(pz_mod_manager* manager) {
    pz_path file;
    pz_str text;
    size_t i;
    int ok = 1;

    pz_path_init(&file);
    pz_str_init(&text);
    for (i = 0; ok && i < manager->db.length; ++i) {
        const pz_installed_mod* mod = db_at_const(manager, i);
        ok = pz_str_append_u64(&text, mod->workshop_id) &&
             pz_str_append_char(&text, '\t') &&
             pz_str_append(&text, mod->folder.data, mod->folder.length) &&
             pz_str_append_char(&text, '\t') &&
             pz_str_append(&text, mod->name.data, mod->name.length) &&
             pz_str_append_char(&text, '\t') &&
             pz_str_append_u64(&text, mod->time_updated) &&
             pz_str_append_char(&text, '\t') &&
             pz_str_append_char(&text, mod->owned ? '1' : '0') &&
             pz_str_append_char(&text, '\t') &&
             pz_str_append_char(&text, mod->locked ? '1' : '0') &&
             pz_str_append_char(&text, '\t') &&
             pz_str_append_char(&text, mod->imported ? '1' : '0') &&
             pz_str_append_char(&text, '\n');
    }
    if (ok && path_from_str(&file, &manager->config.data_dir) &&
        path_join_text(&file, &file, "installed.tsv"))
        write_text_file(&file, text.data, text.length);
    pz_str_free(&text);
    pz_path_free(&file);
}

static void save_update_state(pz_mod_manager* manager) {
    pz_path file;
    pz_str text;
    size_t i;
    int ok = 1;

    pz_path_init(&file);
    pz_str_init(&text);
    for (i = 0; ok && i < manager->updates.length; ++i) {
        const pz_update_entry* entry =
            (const pz_update_entry*)pz_vec_at_const(&manager->updates, i);
        if (!entry->available || pz_mod_manager_locked(manager, entry->workshop_id))
            continue;
        ok = pz_str_append_u64(&text, entry->workshop_id) &&
             pz_str_append_cstr(&text, "\t1\n");
    }
    if (ok && path_from_str(&file, &manager->config.data_dir) &&
        path_join_text(&file, &file, "updates.tsv"))
        write_text_file(&file, text.data, text.length);
    pz_str_free(&text);
    pz_path_free(&file);
}

static void read_game_version(pz_mod_manager* manager) {
    pz_path file;
    pz_str text;
    size_t position = 0;
    const char* line;
    size_t line_length;

    pz_str_clear(&manager->game_version);
    pz_path_init(&file);
    pz_str_init(&text);
    if (path_from_str(&file, &manager->config.zomboid_dir) &&
        path_join_text(&file, &file, "version.txt") && read_file(&file, &text)) {
        while (next_line(text.data, text.length, &position, &line, &line_length)) {
            size_t start = 0;
            size_t end;
            while (start < line_length && isspace((unsigned char)line[start])) ++start;
            end = start;
            while (end < line_length && !isspace((unsigned char)line[end])) ++end;
            if (end > start) {
                str_assign_text(&manager->game_version, line + start, end - start);
                break;
            }
        }
    }
    pz_str_free(&text);
    pz_path_free(&file);
}

/* ---------------------------------------------------------------- pruning */

static void prune_children(pz_mod_manager* manager, const pz_path* dir,
                           int64_t max_age) {
    pz_vec entries;
    (void)manager;
    pz_fs_error error;
    int64_t now = 0;
    size_t i;

    pz_fs_error_clear(&error);
    pz_vec_init(&entries, sizeof(pz_fs_entry));
    if (!pz_fs_list_dir(dir, &entries, &error)) {
        pz_fs_entries_free(&entries);
        return;
    }
    if (!pz_fs_file_time_now(&now)) now = 0;
    for (i = 0; i < entries.length; ++i) {
        const pz_fs_entry* entry = (const pz_fs_entry*)pz_vec_at_const(&entries, i);
        pz_path child;
        int64_t modified = 0;
        pz_fs_error item_error;
        pz_path_init(&child);
        if (pz_path_join(&child, dir, entry->name.data, entry->name.length)) {
            pz_fs_error_clear(&item_error);
            if (pz_fs_last_write_time(&child, &modified, &item_error) &&
                modified < now - max_age) {
                pz_fs_remove_all(&child, NULL, &item_error);
            }
        }
        pz_path_free(&child);
    }
    pz_fs_entries_free(&entries);
    pz_fs_remove(dir, &error);
}

static void prune_stale_state(pz_mod_manager* manager) {
    pz_path data;
    pz_path child;
    pz_fs_error error;

    pz_path_init(&data);
    if (!path_from_str(&data, &manager->config.data_dir)) return;
    pz_path_init(&child);
    if (pz_path_join(&child, &data, "steam_cache", 11))
        prune_children(manager, &child, 24 * 60 * 60);
    pz_path_free(&child);
    if (pz_path_join(&child, &data, "backups", 7))
        prune_children(manager, &child, 7 * 24 * 60 * 60);
    pz_path_free(&child);
    pz_path_free(&data);
    (void)error;
}

/* ---------------------------------------------------------------- config */

static int config_assign(pz_paths_config* out, const pz_paths_config* in) {
    size_t i;
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
        const pz_str* key = (const pz_str*)pz_vec_at_const(&in->locked_targets, i);
        if (!string_list_add_unique(&out->locked_targets, key)) goto fail;
    }
    return 1;
fail:
    pz_paths_config_free(out);
    return 0;
}

static int copy_installed_mod(pz_installed_mod* out,
                              const pz_installed_mod* in) {
    pz_installed_mod_init(out);
    out->workshop_id = in->workshop_id;
    out->time_updated = in->time_updated;
    out->owned = in->owned;
    out->locked = in->locked;
    out->imported = in->imported;
    if (!str_assign(&out->folder, &in->folder) ||
        !str_assign(&out->name, &in->name)) {
        pz_installed_mod_free(out);
        return 0;
    }
    return 1;
}

pz_mod_manager* pz_mod_manager_create(const pz_paths_config* config,
                                      const pz_mm_services* services) {
    pz_mod_manager* manager;
    pz_path path;
    pz_fs_error error;

    if (!config || !services) return NULL;
    manager = (pz_mod_manager*)calloc(1, sizeof(*manager));
    if (!manager) return NULL;
    pz_paths_config_init(&manager->config);
    pz_str_init(&manager->game_version);
    pz_vec_init(&manager->db, sizeof(pz_installed_mod));
    pz_vec_init(&manager->updates, sizeof(pz_update_entry));
    manager->services = services;
    if (!config_assign(&manager->config, config)) {
        pz_mod_manager_destroy(manager);
        return NULL;
    }

    pz_path_init(&path);
    pz_fs_error_clear(&error);
    if (path_from_str(&path, &manager->config.user_mods_dir))
        pz_fs_create_directories(&path, &error);
    pz_path_free(&path);
    pz_path_init(&path);
    pz_fs_error_clear(&error);
    if (path_from_str(&path, &manager->config.data_dir))
        pz_fs_create_directories(&path, &error);
    pz_path_free(&path);

    prune_stale_state(manager);
    read_game_version(manager);
    load_db(manager);
    load_update_state(manager);
    save_update_state(manager);
    load_app_state(manager);
    return manager;
}

void pz_mod_manager_destroy(pz_mod_manager* manager) {
    size_t i;
    if (!manager) return;
    for (i = 0; i < manager->db.length; ++i)
        pz_installed_mod_free(db_at(manager, i));
    pz_vec_free(&manager->db);
    pz_vec_free(&manager->updates);
    pz_str_free(&manager->game_version);
    pz_paths_config_free(&manager->config);
    free(manager);
}

void pz_mod_manager_get_config(const pz_mod_manager* manager,
                               pz_paths_config* out) {
    if (!manager || !out) return;
    config_assign(out, &manager->config);
}

void pz_mod_manager_set_config(pz_mod_manager* manager,
                               const pz_paths_config* config) {
    if (!manager || !config) return;
    pz_paths_config_free(&manager->config);
    config_assign(&manager->config, config);
    prune_stale_state(manager);
    read_game_version(manager);
    load_db(manager);
    load_update_state(manager);
    save_update_state(manager);
    load_app_state(manager);
}

int pz_mod_manager_save_config(pz_mod_manager* manager) {
    if (!manager || !manager->services || !manager->services->save_config)
        return 0;
    return manager->services->save_config(manager->services->context,
                                          &manager->config);
}

const pz_str* pz_mod_manager_game_version(const pz_mod_manager* manager) {
    return manager ? &manager->game_version : NULL;
}

int pz_mod_manager_app_update_available(const pz_mod_manager* manager) {
    return manager ? manager->app_update_available : 0;
}

static uint64_t now_seconds(void) {
    return (uint64_t)time(NULL);
}

int pz_mod_manager_app_update_check_due(const pz_mod_manager* manager) {
    uint64_t last;
    uint64_t now;
    if (!manager) return 0;
    last = read_timestamp((pz_mod_manager*)manager, "last_app_check");
    if (last == 0) return 1;
    now = now_seconds();
    return last <= now && now - last >= PZ_MM_CHECK_INTERVAL;
}

int pz_mod_manager_update_check_due(const pz_mod_manager* manager) {
    uint64_t last;
    uint64_t now;
    if (!manager) return 0;
    if (pz_mod_manager_app_update_check_due(manager)) return 1;
    if (manager->db.length == 0) return 0;
    last = read_timestamp((pz_mod_manager*)manager, "last_check");
    if (last == 0) return 1;
    now = now_seconds();
    return last <= now && now - last >= PZ_MM_CHECK_INTERVAL;
}

int pz_mod_manager_reload_installed(pz_mod_manager* manager) {
    pz_vec previous;
    pz_str old_version;
    size_t i;
    int changed = 0;
    if (!manager) return 0;
    pz_vec_init(&previous, sizeof(pz_installed_mod));
    pz_str_init(&old_version);
    for (i = 0; i < manager->db.length; ++i) {
        pz_installed_mod* copy = (pz_installed_mod*)pz_vec_push(&previous);
        if (!copy || !copy_installed_mod(copy, db_at_const(manager, i))) {
            if (copy) --previous.length;
            break;
        }
    }
    str_assign(&old_version, &manager->game_version);
    load_db(manager);
    read_game_version(manager);
    if (!str_equals_text(&old_version, manager->game_version.data,
                         manager->game_version.length))
        changed = 1;
    if (!changed) changed = !db_same(&previous, &manager->db);
    for (i = 0; i < manager->updates.length;) {
        pz_update_entry* entry = (pz_update_entry*)pz_vec_at(&manager->updates, i);
        if (!db_is_installed(manager, entry->workshop_id) ||
            pz_mod_manager_locked(manager, entry->workshop_id)) {
            update_erase(manager, entry->workshop_id);
            changed = 1;
        } else {
            ++i;
        }
    }
    if (changed) save_update_state(manager);
    for (i = 0; i < previous.length; ++i)
        pz_installed_mod_free((pz_installed_mod*)pz_vec_at(&previous, i));
    pz_vec_free(&previous);
    pz_str_free(&old_version);
    return changed;
}

int pz_mod_manager_update_available(const pz_mod_manager* manager,
                                    uint64_t workshop_id) {
    const pz_update_entry* entry;
    if (!manager) return 0;
    if (pz_mod_manager_locked((pz_mod_manager*)manager, workshop_id)) return 0;
    entry = update_find_const(manager, workshop_id);
    return entry && entry->available;
}

int pz_mod_manager_manually_locked(const pz_mod_manager* manager,
                                   uint64_t workshop_id) {
    size_t i;
    if (!manager) return 0;
    for (i = 0; i < manager->db.length; ++i) {
        const pz_installed_mod* mod = db_at_const(manager, i);
        if (mod->workshop_id == workshop_id && mod->locked) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------- lists/targets */

static void append_untracked(pz_mod_manager* manager, pz_vec* out,
                             const pz_vec* known) {
    pz_path base;
    pz_vec entries;
    pz_fs_error error;
    size_t i;

    pz_path_init(&base);
    pz_vec_init(&entries, sizeof(pz_fs_entry));
    pz_fs_error_clear(&error);
    if (!path_from_str(&base, &manager->config.user_mods_dir) ||
        !pz_fs_list_dir(&base, &entries, &error))
        goto done;
    for (i = 0; i < entries.length; ++i) {
        const pz_fs_entry* entry = (const pz_fs_entry*)pz_vec_at_const(&entries, i);
        pz_path child;
        pz_str folder;
        pz_mod_info info;
        pz_installed_mod* mod;
        if (entry->kind != PZ_FS_DIR || entry->name.length == 0 ||
            entry->name.data[0] == '.')
            continue;
        pz_str_init(&folder);
        if (!str_assign(&folder, &entry->name)) {
            pz_str_free(&folder);
            continue;
        }
        if (string_list_contains_cstr(known, folder.data)) {
            pz_str_free(&folder);
            continue;
        }
        pz_path_init(&child);
        if (!pz_path_join(&child, &base, folder.data, folder.length)) {
            pz_path_free(&child);
            pz_str_free(&folder);
            continue;
        }
        if (read_info(manager, &child, &info) && info.id.length != 0) {
            mod = (pz_installed_mod*)pz_vec_push(out);
            if (mod) {
                pz_installed_mod_init(mod);
                mod->workshop_id = 0;
                str_assign(&mod->folder, &folder);
                if (info.name.length != 0)
                    str_assign(&mod->name, &info.name);
                else
                    str_assign(&mod->name, &folder);
            }
        }
        pz_mod_info_free(&info);
        pz_path_free(&child);
        pz_str_free(&folder);
    }
done:
    pz_fs_entries_free(&entries);
    pz_path_free(&base);
}

void pz_mod_manager_list(const pz_mod_manager* manager, pz_vec* out) {
    size_t i;
    if (!out) return;
    pz_vec_init(out, sizeof(pz_installed_mod));
    if (!manager) return;
    for (i = 0; i < manager->db.length; ++i) {
        pz_installed_mod* copy = (pz_installed_mod*)pz_vec_push(out);
        if (!copy || !copy_installed_mod(copy, db_at_const(manager, i))) {
            if (copy) --out->length;
            return;
        }
    }
}

void pz_mod_manager_list_all(pz_mod_manager* manager, pz_vec* out) {
    pz_vec known;
    size_t i;
    if (!out) return;
    pz_vec_init(out, sizeof(pz_installed_mod));
    pz_vec_init(&known, sizeof(pz_str));
    if (!manager) return;
    for (i = 0; i < manager->db.length; ++i) {
        const pz_installed_mod* mod = db_at_const(manager, i);
        pz_installed_mod* copy = (pz_installed_mod*)pz_vec_push(out);
        if (!copy || !copy_installed_mod(copy, mod)) {
            if (copy) --out->length;
            string_list_free(&known);
            return;
        }
        string_list_add_unique(&known, &mod->folder);
    }
    append_untracked(manager, out, &known);
    string_list_free(&known);
}

void pz_mod_manager_activation_targets(pz_mod_manager* manager, pz_vec* out) {
    size_t i;
    if (!out) return;
    pz_vec_init(out, sizeof(pz_activation_target_data));
    if (!manager || !discover_targets(manager, out)) return;
    for (i = 0; i < out->length; ++i) {
        pz_activation_target_data* target =
            (pz_activation_target_data*)pz_vec_at(out, i);
        pz_str key;
        pz_str_init(&key);
        if (target_key(target, &key))
            target->lock_updates = config_has_target(&manager->config, &key);
        pz_str_free(&key);
    }
}

void pz_mod_manager_mod_ids_for(pz_mod_manager* manager, uint64_t workshop_id,
                                pz_vec* out) {
    size_t i;
    if (!out) return;
    pz_vec_init(out, sizeof(pz_str));
    if (!manager) return;
    for (i = 0; i < manager->db.length; ++i) {
        const pz_installed_mod* mod = db_at_const(manager, i);
        pz_path dir;
        pz_mod_info info;
        if (mod->workshop_id != workshop_id) continue;
        pz_path_init(&dir);
        if (!path_from_str(&dir, &manager->config.user_mods_dir) ||
            !pz_path_join_path(&dir, &dir, &(pz_path){mod->folder}))
            goto next;
        if (read_info(manager, &dir, &info) && info.id.length != 0)
            string_list_add_unique(out, &info.id);
        pz_mod_info_free(&info);
next:
        pz_path_free(&dir);
    }
}

static long long trailing_number(const pz_str* value) {
    size_t end = value->length;
    size_t begin;
    long long number = 0;
    size_t i;
    while (end > 0 && !isdigit((unsigned char)value->data[end - 1])) --end;
    begin = end;
    while (begin > 0 && isdigit((unsigned char)value->data[begin - 1])) --begin;
    if (begin == end) return 0;
    for (i = begin; i < end; ++i) number = number * 10 + (value->data[i] - '0');
    return number;
}

static long long numeric_version(const pz_str* value) {
    long long number = 0;
    int any = 0;
    size_t i;
    for (i = 0; i < value->length; ++i) {
        const unsigned char c = (unsigned char)value->data[i];
        if (isdigit(c)) {
            any = 1;
            number = number * 10 + (c - '0');
        } else if (any) {
            break;
        }
    }
    return any ? number : 0;
}

int pz_mod_manager_preferred_mod_id(pz_mod_manager* manager,
                                    uint64_t workshop_id, pz_str* out) {
    size_t i;
    int have = 0;
    pz_str best_id;
    int best_compatible = 0;
    long long best_min = 0, best_folder = 0, best_id_version = 0;
    pz_str best_folder_name;

    if (!out) return 0;
    pz_str_clear(out);
    if (!manager) return 0;
    pz_str_init(&best_id);
    pz_str_init(&best_folder_name);
    for (i = 0; i < manager->db.length; ++i) {
        const pz_installed_mod* mod = db_at_const(manager, i);
        pz_path dir;
        pz_mod_info info;
        int compatible;
        long long version_min, folder_version, id_version;
        int better;
        if (mod->workshop_id != workshop_id) continue;
        pz_path_init(&dir);
        if (!path_from_str(&dir, &manager->config.user_mods_dir) ||
            !pz_path_join_path(&dir, &dir, &(pz_path){mod->folder}))
            goto next;
        if (!read_info(manager, &dir, &info) || info.id.length == 0) {
            pz_mod_info_free(&info);
            goto next;
        }
        compatible = pz_mod_info_supports_game_version(
            &info, manager->game_version.data, manager->game_version.length);
        version_min = numeric_version(&info.version_min);
        folder_version = trailing_number(&mod->folder);
        id_version = trailing_number(&info.id);
        better = !have || (compatible != best_compatible
            ? compatible > best_compatible
            : version_min != best_min
                  ? version_min > best_min
                  : folder_version != best_folder
                        ? folder_version > best_folder
                        : id_version != best_id_version
                              ? id_version > best_id_version
                              : 0);
        if (better) {
            str_assign(&best_id, &info.id);
            str_assign(&best_folder_name, &mod->folder);
            best_compatible = compatible;
            best_min = version_min;
            best_folder = folder_version;
            best_id_version = id_version;
            have = 1;
        }
        pz_mod_info_free(&info);
next:
        pz_path_free(&dir);
    }
    if (have) str_assign(out, &best_id);
    pz_str_free(&best_folder_name);
    pz_str_free(&best_id);
    return have;
}

void pz_mod_manager_workshop_ids_for(pz_mod_manager* manager,
                                     const pz_activation_target_data* target,
                                     pz_vec* out) {
    size_t i, j;
    if (!out) return;
    pz_vec_init(out, sizeof(uint64_t));
    if (!manager || !target) return;
    for (i = 0; i < manager->db.length; ++i) {
        const pz_installed_mod* mod = db_at_const(manager, i);
        pz_path dir;
        pz_mod_info info;
        int enabled = 0;
        pz_path_init(&dir);
        if (!path_from_str(&dir, &manager->config.user_mods_dir) ||
            !pz_path_join_path(&dir, &dir, &(pz_path){mod->folder}))
            goto next;
        if (!read_info(manager, &dir, &info) || info.id.length == 0) {
            pz_mod_info_free(&info);
            goto next;
        }
        for (j = 0; j < target->mod_ids.length; ++j) {
            const pz_str* id = (const pz_str*)pz_vec_at_const(&target->mod_ids, j);
            if (str_equals_text(&info.id, id->data, id->length)) {
                enabled = 1;
                break;
            }
        }
        pz_mod_info_free(&info);
        if (enabled) u64_list_add_unique(out, mod->workshop_id);
next:
        pz_path_free(&dir);
    }
    if (target->kind == 2 && out->length == 0) {
        for (i = 0; i < target->workshop_ids.length; ++i)
            u64_list_add_unique(out,
                *(const uint64_t*)pz_vec_at_const(&target->workshop_ids, i));
    }
}

void pz_mod_manager_unmapped_mod_ids(pz_mod_manager* manager,
                                     const pz_activation_target_data* target,
                                     pz_vec* out) {
    size_t i, j;
    if (!out) return;
    pz_vec_init(out, sizeof(pz_str));
    if (!manager || !target) return;
    for (i = 0; i < target->mod_ids.length; ++i) {
        const pz_str* id = (const pz_str*)pz_vec_at_const(&target->mod_ids, i);
        int found = 0;
        for (j = 0; j < manager->db.length && !found; ++j) {
            const pz_installed_mod* mod = db_at_const(manager, j);
            pz_path dir;
            pz_mod_info info;
            pz_path_init(&dir);
            if (!path_from_str(&dir, &manager->config.user_mods_dir) ||
                !pz_path_join_path(&dir, &dir, &(pz_path){mod->folder})) {
                pz_path_free(&dir);
                continue;
            }
            if (read_info(manager, &dir, &info) && info.id.length != 0 &&
                str_equals_text(&info.id, id->data, id->length))
                found = 1;
            pz_mod_info_free(&info);
            pz_path_free(&dir);
        }
        if (!found) string_list_add_unique(out, id);
    }
}

void pz_mod_manager_enabled_mod_names(pz_mod_manager* manager,
                                      const pz_activation_target_data* target,
                                      pz_vec* out) {
    size_t i, j;
    if (!out) return;
    pz_vec_init(out, sizeof(pz_str));
    if (!manager || !target) return;
    for (i = 0; i < target->mod_ids.length; ++i) {
        const pz_str* id = (const pz_str*)pz_vec_at_const(&target->mod_ids, i);
        pz_str name;
        int named = 0;
        pz_str_init(&name);
        str_assign(&name, id);
        for (j = 0; j < manager->db.length; ++j) {
            const pz_installed_mod* mod = db_at_const(manager, j);
            pz_path dir;
            pz_mod_info info;
            pz_path_init(&dir);
            if (!path_from_str(&dir, &manager->config.user_mods_dir) ||
                !pz_path_join_path(&dir, &dir, &(pz_path){mod->folder})) {
                pz_path_free(&dir);
                continue;
            }
            if (read_info(manager, &dir, &info) && info.id.length != 0 &&
                str_equals_text(&info.id, id->data, id->length)) {
                if (info.name.length != 0) {
                    str_assign(&name, &info.name);
                    named = 1;
                }
                pz_mod_info_free(&info);
                pz_path_free(&dir);
                break;
            }
            pz_mod_info_free(&info);
            pz_path_free(&dir);
        }
        if (named) {
            pz_str_append_cstr(&name, " [");
            pz_str_append(&name, id->data, id->length);
            pz_str_append_char(&name, ']');
        }
        string_list_add_unique(out, &name);
        pz_str_free(&name);
    }
}

void pz_mod_manager_missing_requirements(pz_mod_manager* manager, pz_vec* out) {
    pz_vec installed;
    pz_vec seen_folders, reported;
    pz_path base;
    pz_vec entries;
    pz_fs_error error;
    size_t i, j, k;

    if (!out) return;
    pz_vec_init(out, sizeof(pz_missing_requirement));
    pz_vec_init(&installed, sizeof(pz_str));
    pz_vec_init(&seen_folders, sizeof(pz_str));
    pz_vec_init(&reported, sizeof(pz_str));
    pz_path_init(&base);
    pz_vec_init(&entries, sizeof(pz_fs_entry));
    pz_fs_error_clear(&error);
    if (!manager) goto done;

    if (path_from_str(&base, &manager->config.user_mods_dir) &&
        pz_fs_list_dir(&base, &entries, &error)) {
        for (i = 0; i < entries.length; ++i) {
            const pz_fs_entry* entry = (const pz_fs_entry*)pz_vec_at_const(&entries, i);
            pz_path child;
            pz_mod_info info;
            if (entry->kind != PZ_FS_DIR) continue;
            pz_path_init(&child);
            if (pz_path_join(&child, &base, entry->name.data, entry->name.length) &&
                read_info(manager, &child, &info) && info.id.length != 0) {
                string_list_add_unique(&installed, &info.id);
            }
            pz_mod_info_free(&info);
            pz_path_free(&child);
        }
    }

    for (i = 0; i < manager->db.length; ++i) {
        const pz_installed_mod* mod = db_at_const(manager, i);
        pz_path dir;
        pz_mod_info info;
        pz_str owner;
        if (string_list_contains(&seen_folders, &mod->folder)) continue;
        string_list_add_unique(&seen_folders, &mod->folder);
        pz_path_init(&dir);
        if (!path_from_str(&dir, &manager->config.user_mods_dir) ||
            !pz_path_join_path(&dir, &dir, &(pz_path){mod->folder})) {
            pz_path_free(&dir);
            continue;
        }
        if (!read_info(manager, &dir, &info)) {
            pz_path_free(&dir);
            continue;
        }
        pz_str_init(&owner);
        str_assign(&owner, info.name.length ? &info.name : &mod->folder);
        for (j = 0; j < info.required_ids.length; ++j) {
            const pz_str* required =
                (const pz_str*)pz_vec_at_const(&info.required_ids, j);
            pz_str key;
            int present = 0;
            if (required->length == 0) continue;
            pz_str_init(&key);
            make_folded(&mod->folder, &key);
            pz_str_append_char(&key, '\n');
            {
                pz_str folded;
                pz_str_init(&folded);
                make_folded(required, &folded);
                pz_str_append(&key, folded.data, folded.length);
                pz_str_free(&folded);
            }
            if (string_list_contains(&reported, &key)) {
                pz_str_free(&key);
                continue;
            }
            string_list_add_unique(&reported, &key);
            pz_str_free(&key);
            if (info.id.length != 0 && fold_equals(required, &info.id)) continue;
            for (k = 0; k < installed.length; ++k) {
                const pz_str* id = (const pz_str*)pz_vec_at_const(&installed, k);
                if (fold_equals(required, id)) {
                    present = 1;
                    break;
                }
            }
            if (!present) {
                pz_missing_requirement* missing =
                    (pz_missing_requirement*)pz_vec_push(out);
                if (missing) {
                    pz_missing_requirement_init(missing);
                    str_assign(&missing->mod_name, &owner);
                    str_assign(&missing->folder, &mod->folder);
                    str_assign(&missing->required_id, required);
                }
            }
        }
        pz_str_free(&owner);
        pz_mod_info_free(&info);
        pz_path_free(&dir);
    }

done:
    pz_fs_entries_free(&entries);
    pz_path_free(&base);
    string_list_free(&reported);
    string_list_free(&seen_folders);
    string_list_free(&installed);
}

void pz_mod_manager_free_mods(pz_vec* mods) {
    size_t i;
    if (!mods) return;
    for (i = 0; i < mods->length; ++i)
        pz_installed_mod_free((pz_installed_mod*)pz_vec_at(mods, i));
    pz_vec_free(mods);
}

void pz_mod_manager_free_targets(pz_vec* targets) {
    size_t i;
    if (!targets) return;
    for (i = 0; i < targets->length; ++i)
        pz_activation_target_data_free(
            (pz_activation_target_data*)pz_vec_at(targets, i));
    pz_vec_free(targets);
}

void pz_mod_manager_free_requirements(pz_vec* requirements) {
    size_t i;
    if (!requirements) return;
    for (i = 0; i < requirements->length; ++i)
        pz_missing_requirement_free(
            (pz_missing_requirement*)pz_vec_at(requirements, i));
    pz_vec_free(requirements);
}

/* -------------------------------------------------------------- logging */

static void log_text(const pz_log* log, const char* text) {
    if (log) pz_log_emit(log, text);
}

static void log_line(const pz_log* log, const char* prefix, uint64_t value,
                     const char* suffix) {
    pz_str line;
    pz_str_init(&line);
    if (prefix) pz_str_append_cstr(&line, prefix);
    pz_str_append_u64(&line, value);
    if (suffix) pz_str_append_cstr(&line, suffix);
    if (log) pz_log_emit_str(log, &line);
    pz_str_free(&line);
}

/* --------------------------------------------------------------- locking */

static void server_locked_ids(pz_mod_manager* manager, pz_vec* ids) {
    pz_vec targets;
    size_t i, j, k;
    pz_vec_init(ids, sizeof(uint64_t));
    if (!manager || !discover_targets(manager, &targets)) {
        pz_vec_free(&targets);
        return;
    }
    for (i = 0; i < targets.length; ++i) {
        pz_activation_target_data* target =
            (pz_activation_target_data*)pz_vec_at(&targets, i);
        pz_str key;
        pz_str_init(&key);
        target_key(target, &key);
        target->lock_updates = config_has_target(&manager->config, &key);
        pz_str_free(&key);
        if (!target->lock_updates) continue;
        for (j = 0; j < manager->db.length; ++j) {
            const pz_installed_mod* mod = db_at_const(manager, j);
            pz_path dir;
            pz_mod_info info;
            int enabled = 0;
            pz_path_init(&dir);
            if (!path_from_str(&dir, &manager->config.user_mods_dir) ||
                !pz_path_join_path(&dir, &dir, &(pz_path){mod->folder})) {
                pz_path_free(&dir);
                continue;
            }
            if (!read_info(manager, &dir, &info) || info.id.length == 0) {
                pz_mod_info_free(&info);
                pz_path_free(&dir);
                continue;
            }
            for (k = 0; k < target->mod_ids.length; ++k) {
                const pz_str* id =
                    (const pz_str*)pz_vec_at_const(&target->mod_ids, k);
                if (str_equals_text(&info.id, id->data, id->length)) {
                    enabled = 1;
                    break;
                }
            }
            pz_mod_info_free(&info);
            pz_path_free(&dir);
            if (enabled) u64_list_add_unique(ids, mod->workshop_id);
        }
    }
    pz_mod_manager_free_targets(&targets);
}

int pz_mod_manager_server_locked(pz_mod_manager* manager, uint64_t workshop_id) {
    pz_vec ids;
    int found;
    if (!manager) return 0;
    server_locked_ids(manager, &ids);
    found = u64_list_contains(&ids, workshop_id);
    pz_vec_free(&ids);
    return found;
}

int pz_mod_manager_locked(pz_mod_manager* manager, uint64_t workshop_id) {
    return manager && (pz_mod_manager_manually_locked(manager, workshop_id) ||
                       pz_mod_manager_server_locked(manager, workshop_id));
}

void pz_mod_manager_lock_reason(pz_mod_manager* manager, uint64_t workshop_id,
                                pz_str* out) {
    if (!out) return;
    pz_str_clear(out);
    if (!manager) return;
    if (pz_mod_manager_manually_locked(manager, workshop_id))
        pz_str_assign_cstr(out, "pinned");
    else if (pz_mod_manager_server_locked(manager, workshop_id))
        pz_str_assign_cstr(out, "target-enabled");
}

pz_mm_result pz_mod_manager_set_locked(pz_mod_manager* manager,
                                       const uint64_t* workshop_ids, size_t count,
                                       int locked, const pz_log* log) {
    pz_mm_result result;
    size_t i, j;
    size_t matched = 0, missing = 0, changed = 0;
    int updates_changed = 0;
    (void)log;
    pz_mm_result_init(&result);
    if (!manager) return result_fail_text("No manager");

    for (i = 0; i < count; ++i) {
        const uint64_t id = workshop_ids[i];
        int found = 0;
        for (j = 0; j < manager->db.length; ++j) {
            pz_installed_mod* mod = db_at(manager, j);
            if (mod->workshop_id != id) continue;
            found = 1;
            ++matched;
            if (mod->locked != locked) {
                mod->locked = locked;
                ++changed;
            }
        }
        if (!found) ++missing;
        if (locked) {
            if (update_find(manager, id)) {
                update_erase(manager, id);
                updates_changed = 1;
            }
        }
    }
    if (changed) save_db(manager);
    if (updates_changed) save_update_state(manager);
    result.ok = missing == 0;
    pz_str_assign_cstr(&result.message, locked ? "locked " : "unlocked ");
    pz_str_append_u64(&result.message, (uint64_t)matched);
    pz_str_append_cstr(&result.message, " mod folder(s)");
    if (missing) {
        pz_str_append_cstr(&result.message, ", ");
        pz_str_append_u64(&result.message, (uint64_t)missing);
        pz_str_append_cstr(&result.message, " Workshop item(s) not installed");
    }
    return result;
}

/* ------------------------------------------------------------- selection */

static pz_activation_target_data* find_target(pz_vec* targets, int kind,
                                              const char* name, size_t length) {
    size_t i;
    for (i = 0; i < targets->length; ++i) {
        pz_activation_target_data* target =
            (pz_activation_target_data*)pz_vec_at(targets, i);
        if (target->kind != kind) continue;
        if (kind == 0) return target;
        if (target->name.length == length &&
            (length == 0 || memcmp(target->name.data, name, length) == 0))
            return target;
    }
    return NULL;
}

static int write_target(pz_mod_manager* manager,
                        pz_activation_target_data* target, const pz_vec* add,
                        const pz_vec* workshops, int enabled) {
    return manager->services->write_target(manager->services->context, target,
                                           add, workshops, enabled);
}

pz_mm_result pz_mod_manager_set_target_locked(pz_mod_manager* manager,
                                              int kind, const char* name,
                                              size_t name_length, int locked,
                                              const pz_log* log) {
    pz_vec targets;
    pz_activation_target_data* target;
    pz_mm_result result;
    pz_str key;
    size_t old_length;
    size_t i;
    int changed = 0;

    pz_mm_result_init(&result);
    if (!manager) return result_fail_text("No manager");
    pz_vec_init(&targets, sizeof(pz_activation_target_data));
    if (!discover_targets(manager, &targets)) {
        pz_vec_free(&targets);
        return result_fail_text("Activation target not found");
    }
    target = find_target(&targets, kind, name, name_length);
    if (!target) {
        pz_mod_manager_free_targets(&targets);
        return result_fail_text("Activation target not found");
    }
    pz_str_init(&key);
    target_key(target, &key);
    old_length = manager->config.locked_targets.length;
    if (locked) {
        if (!config_has_target(&manager->config, &key)) {
            if (!string_list_add_unique(&manager->config.locked_targets, &key)) {
                pz_str_free(&key);
                pz_mod_manager_free_targets(&targets);
                return result_fail_text("Could not save the target lock setting");
            }
            changed = 1;
        }
    } else {
        for (i = 0; i < manager->config.locked_targets.length; ++i) {
            pz_str* value =
                (pz_str*)pz_vec_at(&manager->config.locked_targets, i);
            if (str_equals_text(value, key.data, key.length)) {
                pz_str_free(value);
                memmove(value, value + 1,
                        (manager->config.locked_targets.length - i - 1) *
                            sizeof(*value));
                --manager->config.locked_targets.length;
                changed = 1;
                break;
            }
        }
    }

    if (changed && !manager->services->save_config(manager->services->context,
                                                    &manager->config)) {
        if (locked && old_length < manager->config.locked_targets.length) {
            --manager->config.locked_targets.length;
        } else if (!locked) {
            string_list_add_unique(&manager->config.locked_targets, &key);
        }
        pz_str_free(&key);
        pz_mod_manager_free_targets(&targets);
        return result_fail_text("Could not save the target lock setting");
    }

    if (locked) {
        size_t j, k;
        int updates_changed = 0;
        for (j = 0; j < manager->db.length; ++j) {
            const pz_installed_mod* mod = db_at_const(manager, j);
            pz_path dir;
            pz_mod_info info;
            int enabled = 0;
            pz_path_init(&dir);
            if (!path_from_str(&dir, &manager->config.user_mods_dir) ||
                !pz_path_join_path(&dir, &dir, &(pz_path){mod->folder})) {
                pz_path_free(&dir);
                continue;
            }
            if (read_info(manager, &dir, &info) && info.id.length != 0) {
                for (k = 0; k < target->mod_ids.length; ++k) {
                    const pz_str* id =
                        (const pz_str*)pz_vec_at_const(&target->mod_ids, k);
                    if (str_equals_text(&info.id, id->data, id->length)) {
                        enabled = 1;
                        break;
                    }
                }
            }
            pz_mod_info_free(&info);
            pz_path_free(&dir);
            if (enabled && update_find(manager, mod->workshop_id)) {
                update_erase(manager, mod->workshop_id);
                updates_changed = 1;
            }
        }
        if (updates_changed) save_update_state(manager);
    }

    result.ok = 1;
    pz_str_assign_cstr(&result.message, locked ? "Updates locked for "
                                               : "Updates unlocked for ");
    pz_str_append(&result.message, target->label.data, target->label.length);
    if (log) pz_log_emit_str(log, &result.message);
    pz_str_free(&key);
    pz_mod_manager_free_targets(&targets);
    return result;
}

/* Returns 1 when add_ids should be written, 0 when remove_ids, -1 on error. */
static pz_mm_result add_requirement_ids(pz_mod_manager* manager, pz_vec* add_ids,
                                        const pz_log* log) {
    typedef struct installed_info {
        pz_str id;
        pz_vec requires;
    } installed_info;
    pz_vec all, installed, queue, visited, warned;
    size_t i, j, k;
    pz_mm_result result;

    pz_mm_result_init(&result);
    pz_vec_init(&all, sizeof(pz_installed_mod));
    pz_vec_init(&installed, sizeof(installed_info));
    pz_vec_init(&queue, sizeof(pz_str));
    pz_vec_init(&visited, sizeof(pz_str));
    pz_vec_init(&warned, sizeof(pz_str));
    pz_mod_manager_list_all(manager, &all);
    for (i = 0; i < all.length; ++i) {
        const pz_installed_mod* mod = (const pz_installed_mod*)pz_vec_at_const(&all, i);
        pz_path dir;
        pz_mod_info info;
        installed_info* entry;
        pz_path_init(&dir);
        if (!path_from_str(&dir, &manager->config.user_mods_dir) ||
            !pz_path_join_path(&dir, &dir, &(pz_path){mod->folder})) {
            pz_path_free(&dir);
            continue;
        }
        if (!read_info(manager, &dir, &info) || info.id.length == 0) {
            pz_mod_info_free(&info);
            pz_path_free(&dir);
            continue;
        }
        entry = (installed_info*)pz_vec_push(&installed);
        if (entry) {
            pz_str_init(&entry->id);
            pz_vec_init(&entry->requires, sizeof(pz_str));
            str_assign(&entry->id, &info.id);
            for (j = 0; j < info.required_ids.length; ++j)
                string_list_add_unique(&entry->requires,
                    (const pz_str*)pz_vec_at_const(&info.required_ids, j));
        }
        pz_mod_info_free(&info);
        pz_path_free(&dir);
    }

    for (i = 0; i < add_ids->length; ++i) {
        const pz_str* id = (const pz_str*)pz_vec_at_const(add_ids, i);
        pz_str folded;
        pz_str_init(&folded);
        make_folded(id, &folded);
        string_list_add_unique(&queue, id);
        string_list_add_unique(&visited, &folded);
        pz_str_free(&folded);
    }
    for (i = 0; i < queue.length; ++i) {
        const pz_str* owner = (const pz_str*)pz_vec_at_const(&queue, i);
        pz_str owner_folded;
        const installed_info* owner_info = NULL;
        pz_str_init(&owner_folded);
        make_folded(owner, &owner_folded);
        for (j = 0; j < installed.length; ++j) {
            installed_info* entry = (installed_info*)pz_vec_at(&installed, j);
            if (fold_equals(&entry->id, owner)) {
                owner_info = entry;
                break;
            }
        }
        pz_str_free(&owner_folded);
        if (!owner_info) continue;
        for (j = 0; j < owner_info->requires.length; ++j) {
            const pz_str* required =
                (const pz_str*)pz_vec_at_const(&owner_info->requires, j);
            pz_str key;
            const installed_info* requirement = NULL;
            if (required->length == 0) continue;
            pz_str_init(&key);
            make_folded(required, &key);
            for (k = 0; k < installed.length; ++k) {
                installed_info* entry = (installed_info*)pz_vec_at(&installed, k);
                if (fold_equals(&entry->id, required)) {
                    requirement = entry;
                    break;
                }
            }
            if (!requirement) {
                if (!string_list_contains(&warned, &key)) {
                    pz_str message;
                    string_list_add_unique(&warned, &key);
                    pz_str_init(&message);
                    pz_str_append_cstr(&message, "required mod is not installed: ");
                    pz_str_append(&message, required->data, required->length);
                    pz_str_append_cstr(&message, " (needed by ");
                    pz_str_append(&message, owner->data, owner->length);
                    pz_str_append_char(&message, ')');
                    if (log) pz_log_emit_str(log, &message);
                    pz_str_free(&message);
                }
                pz_str_free(&key);
                continue;
            }
            if (string_list_contains(&visited, &key)) {
                pz_str_free(&key);
                continue;
            }
            string_list_add_unique(&visited, &key);
            if (!string_list_contains(add_ids, &requirement->id)) {
                pz_str message;
                string_list_add_unique(add_ids, &requirement->id);
                string_list_add_unique(&queue, &requirement->id);
                pz_str_init(&message);
                pz_str_append_cstr(&message, "auto-enabled required mod ");
                pz_str_append(&message, requirement->id.data,
                              requirement->id.length);
                pz_str_append_cstr(&message, " (needed by ");
                pz_str_append(&message, owner->data, owner->length);
                pz_str_append_char(&message, ')');
                if (log) pz_log_emit_str(log, &message);
                pz_str_free(&message);
            }
            pz_str_free(&key);
        }
    }

    for (i = 0; i < installed.length; ++i) {
        installed_info* entry = (installed_info*)pz_vec_at(&installed, i);
        string_list_free(&entry->requires);
        pz_str_free(&entry->id);
    }
    pz_vec_free(&installed);
    pz_vec_free(&queue);
    string_list_free(&visited);
    string_list_free(&warned);
    pz_mod_manager_free_mods(&all);
    return result;
}

pz_mm_result pz_mod_manager_set_selection_enabled(
    pz_mod_manager* manager, int kind, const char* name, size_t name_length,
    const uint64_t* workshop_ids, size_t count, const char* const* folders,
    const size_t* folder_lengths, size_t folder_count, int enabled,
    const pz_log* log) {
    pz_vec targets, add_ids, remove_ids, workshops;
    pz_activation_target_data* target;
    pz_mm_result result;
    size_t i, j;

    pz_mm_result_init(&result);
    if (!manager) return result_fail_text("No manager");
    pz_vec_init(&targets, sizeof(pz_activation_target_data));
    pz_vec_init(&add_ids, sizeof(pz_str));
    pz_vec_init(&remove_ids, sizeof(pz_str));
    pz_vec_init(&workshops, sizeof(uint64_t));
    if (!discover_targets(manager, &targets)) {
        pz_vec_free(&targets);
        return result_fail_text("Unknown activation target");
    }
    target = find_target(&targets, kind, name, name_length);
    if (!target) {
        pz_vec_free(&add_ids);
        pz_vec_free(&remove_ids);
        pz_vec_free(&workshops);
        pz_mod_manager_free_targets(&targets);
        return result_fail_text("Unknown activation target");
    }

    for (i = 0; i < count; ++i) {
        pz_vec known;
        u64_list_add_unique(&workshops, workshop_ids[i]);
        pz_mod_manager_mod_ids_for(manager, workshop_ids[i], &known);
        if (enabled) {
            pz_str preferred;
            pz_str_init(&preferred);
            pz_mod_manager_preferred_mod_id(manager, workshop_ids[i], &preferred);
            if (preferred.length) string_list_add_unique(&add_ids, &preferred);
            for (j = 0; j < known.length; ++j) {
                const pz_str* mod_id = (const pz_str*)pz_vec_at_const(&known, j);
                if (!str_equals_text(mod_id, preferred.data, preferred.length))
                    string_list_add_unique(&remove_ids, mod_id);
            }
            pz_str_free(&preferred);
        } else {
            for (j = 0; j < known.length; ++j)
                string_list_add_unique(&remove_ids,
                    (const pz_str*)pz_vec_at_const(&known, j));
        }
        string_list_free(&known);
    }
    for (i = 0; i < folder_count; ++i) {
        pz_path dir;
        pz_mod_info info;
        pz_path_init(&dir);
        if (path_from_str(&dir, &manager->config.user_mods_dir) &&
            pz_path_join(&dir, &dir, folders[i], folder_lengths[i]) &&
            read_info(manager, &dir, &info) && info.id.length != 0)
            string_list_add_unique(enabled ? &add_ids : &remove_ids, &info.id);
        pz_mod_info_free(&info);
        pz_path_free(&dir);
    }

    if (enabled) {
        add_requirement_ids(manager, &add_ids, log);
        for (i = 0; i < remove_ids.length;) {
            pz_str* id = (pz_str*)pz_vec_at(&remove_ids, i);
            if (string_list_contains(&add_ids, id)) {
                pz_str_free(id);
                memmove(id, id + 1,
                        (remove_ids.length - i - 1) * sizeof(*id));
                --remove_ids.length;
            } else {
                ++i;
            }
        }
        if (add_ids.length == 0) {
            pz_vec_free(&add_ids);
            pz_vec_free(&remove_ids);
            pz_vec_free(&workshops);
            pz_mod_manager_free_targets(&targets);
            return result_fail_text("Could not enable: no installed Mod ID was found for the selection");
        }
    } else if (remove_ids.length == 0) {
        pz_vec_free(&add_ids);
        pz_vec_free(&remove_ids);
        pz_vec_free(&workshops);
        pz_mod_manager_free_targets(&targets);
        return result_fail_text("Could not disable: no Mod ID was found for the selection");
    }

    {
        const pz_vec* write_ids = enabled ? &add_ids : &remove_ids;
        pz_str where;
        pz_str_init(&where);
        str_assign(&where, &target->label);
        if (enabled && target->kind == 1) {
            pz_activation_target_data* defaults = find_target(&targets, 0, NULL, 0);
            if (!defaults) {
                pz_str_free(&where);
                pz_vec_free(&add_ids);
                pz_vec_free(&remove_ids);
                pz_vec_free(&workshops);
                pz_mod_manager_free_targets(&targets);
                return result_fail_text("Could not find mods/default.txt");
            }
            if ((remove_ids.length &&
                 !write_target(manager, defaults, &remove_ids, NULL, 0)) ||
                !write_target(manager, defaults, write_ids, &workshops, enabled)) {
                pz_str_free(&where);
                pz_vec_free(&add_ids);
                pz_vec_free(&remove_ids);
                pz_vec_free(&workshops);
                pz_mod_manager_free_targets(&targets);
                return result_fail_text("Could not write mods/default.txt");
            }
            pz_str_append_cstr(&where, " + Default");
            log_text(log, "enabled in Default because save targets also use mods/default.txt");
        }
        if ((remove_ids.length &&
             !write_target(manager, target, &remove_ids, NULL, 0)) ||
            !write_target(manager, target, write_ids, &workshops, enabled)) {
            pz_str_free(&where);
            pz_vec_free(&add_ids);
            pz_vec_free(&remove_ids);
            pz_vec_free(&workshops);
            pz_mod_manager_free_targets(&targets);
            return result_fail_text("Could not write the activation target");
        }
        result.ok = 1;
        pz_str_assign_cstr(&result.message, enabled ? "Enabled " : "Disabled ");
        pz_str_append_u64(&result.message, (uint64_t)write_ids->length);
        pz_str_append_cstr(&result.message, " Mod ID(s) in ");
        pz_str_append(&result.message, where.data, where.length);
        pz_str_free(&where);
    }

    pz_vec_free(&add_ids);
    pz_vec_free(&remove_ids);
    pz_vec_free(&workshops);
    pz_mod_manager_free_targets(&targets);
    return result;
}

pz_mm_result pz_mod_manager_set_mods_enabled(pz_mod_manager* manager, int kind,
                                             const char* name, size_t name_length,
                                             const uint64_t* workshop_ids,
                                             size_t count, int enabled,
                                             const pz_log* log) {
    return pz_mod_manager_set_selection_enabled(manager, kind, name, name_length,
                                                workshop_ids, count, NULL, NULL,
                                                0, enabled, log);
}

/* ----------------------------------------------------------- filesystem */

static int path_from_data(pz_mod_manager* manager, const char* name,
                          pz_path* out) {
    pz_path_init(out);
    return path_from_str(out, &manager->config.data_dir) &&
           pz_path_join(out, out, name, strlen(name));
}

static int path_from_folder(pz_mod_manager* manager, const pz_str* folder,
                            pz_path* out) {
    pz_path_init(out);
    return path_from_str(out, &manager->config.user_mods_dir) &&
           pz_path_join(out, out, folder->data, folder->length);
}

static int path_equals(const pz_path* left, const pz_path* right) {
    return str_equals_text(&left->text, right->text.data, right->text.length);
}

static int move_tree(const pz_path* source, const pz_path* destination) {
    pz_path parent;
    pz_fs_error error;
    pz_path_init(&parent);
    pz_fs_error_clear(&error);
    if (pz_path_parent(destination, &parent) && parent.text.length != 0)
        pz_fs_create_directories(&parent, &error);
    pz_path_free(&parent);
    pz_fs_error_clear(&error);
    if (pz_fs_rename(source, destination, &error)) return 1;
    pz_fs_error_clear(&error);
    if (!pz_fs_copy_tree(source, destination,
                         PZ_FS_COPY_RECURSIVE | PZ_FS_COPY_OVERWRITE, &error))
        return 0;
    pz_fs_error_clear(&error);
    pz_fs_remove_all(source, NULL, &error);
    return 1;
}

static void remove_path(const pz_path* path) {
    pz_fs_error error;
    pz_fs_error_clear(&error);
    pz_fs_remove_all(path, NULL, &error);
}

static void cleanup_cache(pz_mod_manager* manager, uint64_t id) {
    pz_path cache;
    pz_path data;
    pz_path_init(&cache);
    pz_path_init(&data);
    if (path_from_str(&data, &manager->config.data_dir) &&
        pz_path_join(&cache, &data, "steam_cache", 11) &&
        pz_path_join(&cache, &cache, "", 0)) {
        /* The numeric child is appended below; pz_path_join with an empty leaf
         * is intentionally not used for ids. */
    }
    pz_path_free(&cache);
    pz_path_free(&data);
    pz_path_init(&cache);
    if (path_from_data(manager, "steam_cache", &cache)) {
        pz_path child;
        pz_path_init(&child);
        if (path_join_u64(&child, &cache, id, NULL)) remove_path(&child);
        pz_path_free(&child);
    }
    pz_path_free(&cache);
}

static int64_t now_i64(void) {
    return (int64_t)time(NULL);
}

static int stamp_text(pz_str* out) {
    pz_str_clear(out);
    return pz_str_append_i64(out, now_i64()) && pz_str_append_char(out, '-') &&
           pz_str_append_u64(out, ++g_temp_counter);
}

static int copy_info_name(const pz_mod_info* info, const pz_str* folder,
                          pz_str* out) {
    return str_assign(out, info->name.length ? &info->name : folder);
}

static void log_info_warning(pz_mod_manager* manager, const pz_log* log,
                             const pz_mod_info* info, const pz_path* root) {
    pz_str name, range, message, filename;
    pz_str_init(&name);
    pz_str_init(&range);
    pz_str_init(&message);
    pz_str_init(&filename);
    if (info->name.length)
        str_assign(&name, &info->name);
    else if (info->id.length)
        str_assign(&name, &info->id);
    else if (pz_path_filename(root, &filename))
        str_assign(&name, &filename);
    pz_mod_info_version_range(info, &range);
    pz_str_append_cstr(&message, "warning: ");
    pz_str_append(&message, name.data, name.length);
    pz_str_append_cstr(&message, " targets ");
    pz_str_append(&message, range.data, range.length);
    pz_str_append_cstr(&message, ", installed ");
    pz_str_append(&message, manager->game_version.data,
                  manager->game_version.length);
    pz_str_append_cstr(&message, "; installing anyway");
    if (log) pz_log_emit_str(log, &message);
    pz_str_free(&message);
    pz_str_free(&range);
    pz_str_free(&name);
    pz_str_free(&filename);
}

static pz_mm_result unavailable_message(uint64_t id, uint32_t result_code) {
    pz_mm_result result;
    pz_mm_result_init(&result);
    pz_str_append_cstr(&result.message, "Workshop item ");
    pz_str_append_u64(&result.message, id);
    pz_str_append_cstr(&result.message, " is unavailable");
    if (result_code == 9)
        pz_str_append_cstr(&result.message,
            " (removed, private, or not a public Workshop item)");
    else if (result_code == 15)
        pz_str_append_cstr(&result.message,
            " (Steam denied access: private or removed)");
    else {
        pz_str_append_cstr(&result.message, " (Steam result ");
        pz_str_append_u64(&result.message, result_code);
        pz_str_append_char(&result.message, ')');
    }
    return result;
}

static pz_mm_result install_item(pz_mod_manager* manager, uint64_t id,
                                 uint64_t remote_time, const pz_log* log,
                                 int* changed, int enable_new) {
    pz_remote_details details;
    pz_mm_result result;
    pz_path cache, data, content;
    pz_str detail;
    pz_vec roots, usable, moved, fresh, old_folders;
    size_t i, j;
    int installed_any = 0;
    int kept = 0;
    int adopted_external = 0;

    pz_mm_result_init(&result);
    if (changed) *changed = 0;
    pz_remote_details_init(&details);
    pz_str_init(&detail);
    pz_path_init(&cache);
    pz_path_init(&data);
    pz_path_init(&content);
    pz_vec_init(&roots, sizeof(pz_str));
    pz_vec_init(&usable, sizeof(pz_str));
    pz_vec_init(&moved, sizeof(pz_path) * 3);
    pz_vec_init(&fresh, sizeof(pz_installed_mod));
    pz_vec_init(&old_folders, sizeof(pz_str));

    if (manager->services->remote_details(manager->services->context, id,
                                          &details) &&
        details.result != 0 && details.result != 1) {
        result = unavailable_message(id, details.result);
        goto done;
    }

    if (!path_from_data(manager, "steam_cache", &data) ||
        !path_join_u64(&cache, &data, id, NULL)) {
        result = result_fail_text("Could not build the download cache path");
        goto done;
    }
    remove_path(&cache);
    if (!manager->services->download(manager->services->context,
                                     &manager->config, id, &cache, &content,
                                     &detail, log)) {
        remove_path(&cache);
        result = result_fail_str(&detail);
        if (result.message.length == 0)
            result = result_fail_text("steamcmd download failed");
        goto done;
    }

    if (!discover_roots(manager, &content, &roots) || roots.length == 0) {
        pz_str message;
        pz_str_init(&message);
        pz_str_append_cstr(&message, "No mod.info in workshop item ");
        pz_str_append_u64(&message, id);
        result = result_fail_str(&message);
        pz_str_free(&message);
        goto done;
    }

    for (i = 0; i < roots.length; ++i) {
        const pz_str* root_text = (const pz_str*)pz_vec_at_const(&roots, i);
        pz_path root;
        pz_mod_info info;
        int compatible;
        pz_path_init(&root);
        if (!pz_path_assign(&root, root_text->data, root_text->length)) {
            pz_path_free(&root);
            continue;
        }
        if (!read_info(manager, &root, &info)) {
            pz_path_free(&root);
            continue;
        }
        compatible = pz_mod_info_supports_game_version(
            &info, manager->game_version.data, manager->game_version.length);
        if (!compatible) log_info_warning(manager, log, &info, &root);
        {
            pz_str* slot = (pz_str*)pz_vec_push(&usable);
            if (slot) {
                pz_str_init(slot);
                if (!str_assign(slot, root_text)) {
                    pz_str_free(slot);
                    --usable.length;
                } else {
                    /* Keep compatibility in the high bit of the path vector's
                     * ordering by putting compatible roots first. */
                    if (!compatible) {
                        pz_str* last = (pz_str*)pz_vec_at(&usable, usable.length - 1);
                        if (usable.length > 1) {
                            pz_str temp;
                            pz_str_init(&temp);
                            str_assign(&temp, last);
                            str_assign(last, (pz_str*)pz_vec_at(&usable, 0));
                            str_assign((pz_str*)pz_vec_at(&usable, 0), &temp);
                            pz_str_free(&temp);
                        }
                    }
                }
            }
        }
        pz_mod_info_free(&info);
        pz_path_free(&root);
    }

    /* Only use incompatible roots when no compatible root exists. */
    {
        size_t compatible_count = 0;
        for (i = 0; i < usable.length; ++i) {
            const pz_str* path = (const pz_str*)pz_vec_at_const(&usable, i);
            pz_path root;
            pz_mod_info info;
            pz_path_init(&root);
            if (pz_path_assign(&root, path->data, path->length) &&
                read_info(manager, &root, &info)) {
                if (pz_mod_info_supports_game_version(
                        &info, manager->game_version.data,
                        manager->game_version.length))
                    ++compatible_count;
            }
            pz_mod_info_free(&info);
            pz_path_free(&root);
        }
        if (compatible_count != 0) {
            for (i = 0; i < usable.length;) {
                const pz_str* path = (const pz_str*)pz_vec_at_const(&usable, i);
                pz_path root;
                pz_mod_info info;
                int keep = 0;
                pz_path_init(&root);
                if (pz_path_assign(&root, path->data, path->length) &&
                    read_info(manager, &root, &info))
                    keep = pz_mod_info_supports_game_version(
                        &info, manager->game_version.data,
                        manager->game_version.length);
                pz_mod_info_free(&info);
                pz_path_free(&root);
                if (keep) {
                    ++i;
                } else {
                    pz_str* slot = (pz_str*)pz_vec_at(&usable, i);
                    pz_str_free(slot);
                    memmove(slot, slot + 1,
                            (usable.length - i - 1) * sizeof(*slot));
                    --usable.length;
                }
            }
        }
    }

    for (i = 0; i < usable.length; ++i) {
        const pz_str* root_text = (const pz_str*)pz_vec_at_const(&usable, i);
        pz_path root, dest, backup;
        pz_mod_info info;
        pz_str folder, display, backup_name;
        int tracked = 0, was_owned = 0, was_locked = 0, was_imported = 0;
        int same_version = 0;

        pz_path_init(&root);
        pz_path_init(&dest);
        pz_path_init(&backup);
        pz_str_init(&folder);
        pz_str_init(&display);
        pz_str_init(&backup_name);
        if (!pz_path_assign(&root, root_text->data, root_text->length) ||
            !read_info(manager, &root, &info)) {
            pz_mod_info_free(&info);
            goto next_root;
        }
        if (path_equals(&root, &content)) {
            if (info.id.length)
                str_assign(&folder, &info.id);
            else
                pz_str_append_u64(&folder, id);
        } else {
            pz_path_filename(&root, &folder);
        }
        if (folder.length == 0) {
            pz_mod_info_free(&info);
            goto next_root;
        }
        if (!path_from_folder(manager, &folder, &dest)) {
            pz_mod_info_free(&info);
            goto next_root;
        }
        for (j = 0; j < manager->db.length; ++j) {
            const pz_installed_mod* existing = db_at_const(manager, j);
            if (existing->workshop_id == id &&
                str_equals_text(&existing->folder, folder.data, folder.length)) {
                tracked = 1;
                was_owned = existing->owned;
                was_locked = existing->locked;
                was_imported = existing->imported;
                break;
            }
        }
        if (path_exists(&dest)) {
            pz_mod_info existing;
            if (read_info(manager, &dest, &existing)) {
                same_version = existing.mod_version.length != 0 &&
                    existing.id.length != 0 &&
                    str_equals_text(&existing.id, info.id.data, info.id.length) &&
                    str_equals_text(&existing.mod_version, info.mod_version.data,
                                    info.mod_version.length);
                pz_mod_info_free(&existing);
            }
        }
        if (same_version) {
            pz_str message;
            pz_str_init(&message);
            pz_str_append_cstr(&message, "kept existing ");
            pz_str_append(&message, folder.data, folder.length);
            pz_str_append_cstr(&message, " (version ");
            pz_str_append(&message, info.mod_version.data, info.mod_version.length);
            pz_str_append_char(&message, ')');
            if (log) pz_log_emit_str(log, &message);
            pz_str_free(&message);
            if (!tracked) adopted_external = 1;
            {
                pz_installed_mod* record =
                    (pz_installed_mod*)pz_vec_push(&fresh);
                if (record) {
                    pz_installed_mod_init(record);
                    record->workshop_id = id;
                    record->time_updated = remote_time;
                    record->owned = was_owned;
                    record->locked = was_locked;
                    record->imported = was_imported;
                    str_assign(&record->folder, &folder);
                    copy_info_name(&info, &folder, &record->name);
                }
            }
            ++kept;
            pz_mod_info_free(&info);
            goto next_root;
        }

        if (path_exists(&dest)) {
            pz_str backup_text;
            pz_path backups;
            pz_str_init(&backup_text);
            pz_path_init(&backups);
            if (!path_from_data(manager, "backups", &backups) ||
                !str_assign(&backup_text, &folder) ||
                !pz_str_append_char(&backup_text, '-') ||
                !stamp_text(&backup_name)) {
                pz_path_free(&backups);
                pz_str_free(&backup_text);
                pz_str_free(&folder);
                pz_str_free(&display);
                pz_mod_info_free(&info);
                pz_path_free(&root);
                pz_path_free(&dest);
                pz_path_free(&backup);
                continue;
            }
            pz_str_append(&backup_text, backup_name.data, backup_name.length);
            if (!pz_path_join(&backup, &backups, backup_text.data,
                              backup_text.length)) {
                pz_path_free(&backups);
                pz_str_free(&backup_text);
                pz_mod_info_free(&info);
                goto next_root;
            }
            {
                pz_path parent;
                pz_fs_error error;
                pz_path_init(&parent);
                pz_fs_error_clear(&error);
                if (pz_path_parent(&backup, &parent))
                    pz_fs_create_directories(&parent, &error);
                pz_path_free(&parent);
            }
            pz_fs_error_clear(&(pz_fs_error){0});
            {
                pz_fs_error error;
                pz_fs_error_clear(&error);
                if (!pz_fs_rename(&dest, &backup, &error)) {
                    pz_str message;
                    pz_str_init(&message);
                    pz_str_append_cstr(&message, "Could not move aside ");
                    pz_str_append(&message, folder.data, folder.length);
                    result = result_fail_str(&message);
                    pz_str_free(&message);
                    pz_str_free(&backup_text);
                    pz_path_free(&backups);
                    pz_mod_info_free(&info);
                    goto rollback;
                }
            }
            pz_str_free(&backup_text);
            pz_path_free(&backups);
            log_line(log, "replacing existing ", 0, NULL);
        }

        if (!move_tree(&root, &dest)) {
            pz_str message;
            pz_str_init(&message);
            pz_str_append_cstr(&message, "Could not move downloaded mod into ");
            pz_str_append(&message, dest.text.data, dest.text.length);
            result = result_fail_str(&message);
            pz_str_free(&message);
            if (backup.text.length) {
                pz_fs_error error;
                pz_fs_error_clear(&error);
                remove_path(&dest);
                pz_fs_rename(&backup, &dest, &error);
            }
            pz_mod_info_free(&info);
            goto rollback;
        }
        {
            pz_path* record = (pz_path*)pz_vec_push(&moved);
            if (record) {
                pz_path_init(&record[0]);
                pz_path_init(&record[1]);
                pz_path_init(&record[2]);
                pz_path_assign(&record[0], root.text.data, root.text.length);
                pz_path_assign(&record[1], dest.text.data, dest.text.length);
                pz_path_assign(&record[2], backup.text.data, backup.text.length);
            }
        }
        installed_any = 1;
        log_line(log, "installed ", id, NULL);
        {
            pz_installed_mod* record = (pz_installed_mod*)pz_vec_push(&fresh);
            if (record) {
                pz_installed_mod_init(record);
                record->workshop_id = id;
                record->time_updated = remote_time;
                record->owned = 1;
                record->locked = was_locked;
                record->imported = was_imported;
                str_assign(&record->folder, &folder);
                copy_info_name(&info, &folder, &record->name);
            }
        }
        pz_mod_info_free(&info);
next_root:
        pz_str_free(&folder);
        pz_str_free(&display);
        pz_str_free(&backup_name);
        pz_path_free(&root);
        pz_path_free(&dest);
        pz_path_free(&backup);
    }

    if (fresh.length == 0) {
        result = result_fail_text("mod.info unreadable in workshop item");
        goto done;
    }
    cleanup_cache(manager, id);

    for (i = 0; i < fresh.length; ++i) {
        const pz_installed_mod* record =
            (const pz_installed_mod*)pz_vec_at_const(&fresh, i);
        string_list_add_unique(&old_folders, &record->folder);
    }
    for (i = 0; i < manager->db.length; ++i) {
        const pz_installed_mod* record = db_at_const(manager, i);
        pz_path path;
        if (record->workshop_id != id || !record->owned ||
            string_list_contains(&old_folders, &record->folder))
            continue;
        if (!path_from_folder(manager, &record->folder, &path)) continue;
        remove_path(&path);
        pz_path_free(&path);
    }
    db_remove_id(manager, id);
    for (i = 0; i < fresh.length; ++i) {
        pz_installed_mod* copy = (pz_installed_mod*)pz_vec_push(&manager->db);
        if (!copy || !copy_installed_mod(copy, (pz_installed_mod*)pz_vec_at(&fresh, i))) {
            if (copy) --manager->db.length;
            break;
        }
    }
    update_set(manager, id, 0);
    save_db(manager);
    save_update_state(manager);
    for (i = 0; i < moved.length; i += 3) {
        pz_path* record = (pz_path*)pz_vec_at(&moved, i);
        if (record[2].text.length) remove_path(&record[2]);
    }
    {
        pz_path backups;
        pz_path_init(&backups);
        if (path_from_data(manager, "backups", &backups)) {
            pz_fs_error error;
            pz_fs_error_clear(&error);
            pz_fs_remove(&backups, &error);
        }
        pz_path_free(&backups);
    }
    if (enable_new && manager->config.auto_enable) {
        pz_mm_result enabled = pz_mod_manager_set_mods_enabled(
            manager, 0, NULL, 0, &id, 1, 1, log);
        if (!enabled.ok) {
            pz_str message;
            pz_str_init(&message);
            pz_str_append_cstr(&message, "could not enable automatically: ");
            pz_str_append(&message, enabled.message.data, enabled.message.length);
            if (log) pz_log_emit_str(log, &message);
            pz_str_free(&message);
        }
        pz_mm_result_free(&enabled);
    }
    if (changed) *changed = installed_any;
    result.ok = 1;
    if (!installed_any) {
        result.message = result_ok_text(adopted_external
            ? "Tracking existing mod(s) without changing files"
            : "Already up to date (same mod version)").message;
    } else {
        pz_str_append_cstr(&result.message, "Installed ");
        pz_str_append_u64(&result.message, (uint64_t)fresh.length);
        pz_str_append_cstr(&result.message, " mod(s)");
        if (kept) {
            pz_str_append_cstr(&result.message, ", kept ");
            pz_str_append_u64(&result.message, (uint64_t)kept);
            pz_str_append_cstr(&result.message, " existing");
        }
    }
    goto done;

rollback:
    for (i = moved.length; i > 0; i -= 3) {
        pz_path* record = (pz_path*)pz_vec_at(&moved, i - 3);
        if (path_exists(&record[1])) {
            move_tree(&record[1], &record[0]);
            if (record[2].text.length) {
                pz_fs_error error;
                pz_fs_error_clear(&error);
                pz_fs_rename(&record[2], &record[1], &error);
            }
        }
    }

done:
    for (i = 0; i < moved.length; i += 3) {
        pz_path* record = (pz_path*)pz_vec_at(&moved, i);
        pz_path_free(&record[0]);
        pz_path_free(&record[1]);
        pz_path_free(&record[2]);
    }
    pz_vec_free(&moved);
    for (i = 0; i < fresh.length; ++i)
        pz_installed_mod_free((pz_installed_mod*)pz_vec_at(&fresh, i));
    pz_vec_free(&fresh);
    for (i = 0; i < usable.length; ++i)
        pz_str_free((pz_str*)pz_vec_at(&usable, i));
    pz_vec_free(&usable);
    string_list_free(&roots);
    string_list_free(&old_folders);
    pz_remote_details_free(&details);
    pz_str_free(&detail);
    pz_path_free(&content);
    pz_path_free(&cache);
    pz_path_free(&data);
    return result;
}

/* ------------------------------------------------------------- operations */

static void unique_installed_ids(pz_mod_manager* manager, pz_vec* ids) {
    size_t i;
    pz_vec_init(ids, sizeof(uint64_t));
    if (!manager) return;
    for (i = 0; i < manager->db.length; ++i)
        u64_list_add_unique(ids, db_at_const(manager, i)->workshop_id);
}

static pz_mm_result disable_mod_ids(pz_mod_manager* manager, const pz_vec* mod_ids,
                                    int* disabled_targets, const pz_log* log) {
    pz_vec targets;
    size_t i, j;
    *disabled_targets = 0;
    if (!manager || mod_ids->length == 0) return result_ok_text("");
    pz_vec_init(&targets, sizeof(pz_activation_target_data));
    if (!discover_targets(manager, &targets)) {
        pz_vec_free(&targets);
        return result_fail_text("Could not read activation targets");
    }
    for (i = 0; i < targets.length; ++i) {
        pz_activation_target_data* target =
            (pz_activation_target_data*)pz_vec_at(&targets, i);
        pz_vec affected;
        pz_vec_init(&affected, sizeof(pz_str));
        for (j = 0; j < mod_ids->length; ++j) {
            const pz_str* id = (const pz_str*)pz_vec_at_const(mod_ids, j);
            if (string_list_contains(&target->mod_ids, id))
                string_list_add_unique(&affected, id);
        }
        if (affected.length != 0) {
            if (!write_target(manager, target, &affected, NULL, 0)) {
                pz_str message;
                pz_str_init(&message);
                pz_str_append_cstr(&message, "Could not disable ");
                pz_str_append_u64(&message, (uint64_t)affected.length);
                pz_str_append_cstr(&message, " Mod ID(s) in ");
                pz_str_append(&message, target->path.data, target->path.length);
                string_list_free(&affected);
                pz_mod_manager_free_targets(&targets);
                return result_fail_str(&message);
            }
            ++*disabled_targets;
            {
                pz_str message;
                pz_str_init(&message);
                pz_str_append_cstr(&message, "disabled ");
                pz_str_append_u64(&message, (uint64_t)affected.length);
                pz_str_append_cstr(&message, " Mod ID(s) in ");
                pz_str_append(&message, target->label.data, target->label.length);
                if (log) pz_log_emit_str(log, &message);
                pz_str_free(&message);
            }
        }
        string_list_free(&affected);
    }
    pz_mod_manager_free_targets(&targets);
    return result_ok_text("");
}

pz_mm_result pz_mod_manager_remove(pz_mod_manager* manager, uint64_t workshop_id,
                                   const pz_log* log) {
    pz_vec ids;
    pz_mm_result result;
    int disabled = 0;
    int removed = 0, kept = 0;
    size_t i;

    pz_mm_result_init(&result);
    if (!manager) return result_fail_text("No manager");
    if (!db_is_installed(manager, workshop_id)) {
        pz_str message;
        pz_str_init(&message);
        pz_str_append_cstr(&message, "Not installed: ");
        pz_str_append_u64(&message, workshop_id);
        result = result_fail_str(&message);
        pz_str_free(&message);
        return result;
    }
    pz_mod_manager_mod_ids_for(manager, workshop_id, &ids);
    result = disable_mod_ids(manager, &ids, &disabled, log);
    if (!result.ok) {
        string_list_free(&ids);
        return result;
    }
    if (ids.length == 0) {
        pz_str message;
        pz_str_init(&message);
        pz_str_append_cstr(&message, "warning: no Mod ID found for ");
        pz_str_append_u64(&message, workshop_id);
        pz_str_append_cstr(&message, "; mod lists were not changed");
        if (log) pz_log_emit_str(log, &message);
        pz_str_free(&message);
    }
    for (i = 0; i < manager->db.length; ++i) {
        const pz_installed_mod* mod = db_at_const(manager, i);
        if (mod->workshop_id != workshop_id) continue;
        if (mod->owned) {
            pz_path path;
            if (path_from_folder(manager, &mod->folder, &path)) {
                remove_path(&path);
                pz_path_free(&path);
            }
            {
                pz_str message;
                pz_str_init(&message);
                pz_str_append_cstr(&message, "removed ");
                pz_str_append(&message, mod->folder.data, mod->folder.length);
                if (log) pz_log_emit_str(log, &message);
                pz_str_free(&message);
            }
            ++removed;
        } else {
            pz_str message;
            pz_str_init(&message);
            pz_str_append_cstr(&message, "stopped tracking ");
            pz_str_append(&message, mod->folder.data, mod->folder.length);
            pz_str_append_cstr(&message, " (left in place)");
            if (log) pz_log_emit_str(log, &message);
            pz_str_free(&message);
            ++kept;
        }
    }
    update_erase(manager, workshop_id);
    db_remove_id(manager, workshop_id);
    save_db(manager);
    save_update_state(manager);
    result.ok = 1;
    pz_str_assign_cstr(&result.message, removed ? "Removed " : "Stopped tracking ");
    pz_str_append_u64(&result.message, workshop_id);
    if (kept) {
        pz_str_append_cstr(&result.message, " (left ");
        pz_str_append_u64(&result.message, (uint64_t)kept);
        pz_str_append_cstr(&result.message, " existing folder(s) in place)");
    }
    if (disabled) {
        pz_str_append_cstr(&result.message, " (disabled in ");
        pz_str_append_u64(&result.message, (uint64_t)disabled);
        pz_str_append_cstr(&result.message, " mod list(s))");
    }
    string_list_free(&ids);
    return result;
}

pz_mm_result pz_mod_manager_update_one(pz_mod_manager* manager,
                                       uint64_t workshop_id, const pz_log* log) {
    pz_mm_result result;
    uint64_t remote;
    int changed = 0;
    pz_mm_result_init(&result);
    if (!manager) return result_fail_text("No manager");
    if (!db_is_installed(manager, workshop_id)) {
        pz_str message;
        pz_str_init(&message);
        pz_str_append_cstr(&message, "Not installed: ");
        pz_str_append_u64(&message, workshop_id);
        result = result_fail_str(&message);
        pz_str_free(&message);
        return result;
    }
    if (pz_mod_manager_locked(manager, workshop_id)) {
        pz_str reason;
        pz_str_init(&reason);
        pz_mod_manager_lock_reason(manager, workshop_id, &reason);
        pz_str_append_cstr(&result.message, "Locked (");
        pz_str_append(&result.message, reason.data, reason.length);
        pz_str_append_cstr(&result.message, "); skipped update of ");
        pz_str_append_u64(&result.message, workshop_id);
        result.ok = 1;
        pz_str_free(&reason);
        return result;
    }
    remote = manager->services->remote_time(manager->services->context, workshop_id);
    if (remote == 0) {
        pz_str message;
        pz_str_init(&message);
        pz_str_append_cstr(&message, "Update check failed: ");
        pz_str_append_u64(&message, workshop_id);
        result = result_fail_str(&message);
        pz_str_free(&message);
        return result;
    }
    if (remote <= db_recorded_time(manager, workshop_id)) {
        update_set(manager, workshop_id, 0);
        save_update_state(manager);
        pz_str_assign_cstr(&result.message, "Up to date: ");
        pz_str_append_u64(&result.message, workshop_id);
        result.ok = 1;
        return result;
    }
    result = install_item(manager, workshop_id, remote, log, &changed, 0);
    if (!result.ok) return result;
    update_set(manager, workshop_id, 0);
    save_update_state(manager);
    if (!changed) return result;
    {
        pz_str message;
        pz_str_init(&message);
        pz_str_append_cstr(&message, "Updated ");
        pz_str_append_u64(&message, workshop_id);
        pz_str_append_cstr(&message, ": ");
        pz_str_append(&message, result.message.data, result.message.length);
        pz_str_free(&result.message);
        result.message = message;
        result.ok = 1;
    }
    return result;
}

pz_mm_result pz_mod_manager_check_updates(pz_mod_manager* manager,
                                          const pz_log* log) {
    pz_vec ids, server_locks;
    pz_mm_result result;
    size_t i;
    int checked = 0, available = 0, failed = 0, locked = 0;
    pz_mm_result_init(&result);
    if (!manager) return result_fail_text("No manager");
    unique_installed_ids(manager, &ids);
    server_locked_ids(manager, &server_locks);
    for (i = 0; i < ids.length; ++i) {
        const uint64_t id = *(const uint64_t*)pz_vec_at_const(&ids, i);
        uint64_t remote;
        if (pz_mod_manager_manually_locked(manager, id) ||
            u64_list_contains(&server_locks, id)) {
            update_erase(manager, id);
            log_line(log, "locked: ", id, " (not checked)");
            ++locked;
            continue;
        }
        remote = manager->services->remote_time(manager->services->context, id);
        if (remote == 0) {
            ++failed;
            log_line(log, "update check failed: ", id, NULL);
            continue;
        }
        ++checked;
        if (remote > db_recorded_time(manager, id)) {
            update_set(manager, id, 1);
            ++available;
            log_line(log, "update available: ", id, NULL);
        } else {
            update_set(manager, id, 0);
            log_line(log, "up to date: ", id, NULL);
        }
    }
    if (ids.length != 0) record_timestamp(manager, "last_check");
    save_update_state(manager);
    result.ok = 1;
    pz_str_append_cstr(&result.message, "Checked ");
    pz_str_append_u64(&result.message, (uint64_t)checked);
    pz_str_append_cstr(&result.message, " mod(s), ");
    pz_str_append_u64(&result.message, (uint64_t)available);
    pz_str_append_cstr(&result.message, " update(s) available");
    if (locked) {
        pz_str_append_cstr(&result.message, ", skipped ");
        pz_str_append_u64(&result.message, (uint64_t)locked);
        pz_str_append_cstr(&result.message, " locked");
    }
    if (failed) {
        pz_str_append_cstr(&result.message, ", ");
        pz_str_append_u64(&result.message, (uint64_t)failed);
        pz_str_append_cstr(&result.message, " check(s) failed");
    }
    pz_vec_free(&ids);
    pz_vec_free(&server_locks);
    return result;
}

pz_mm_result pz_mod_manager_update_all(pz_mod_manager* manager, const pz_log* log) {
    pz_vec ids, server_locks;
    pz_mm_result result;
    size_t i;
    int updated = 0, skipped = 0, unchanged = 0, locked_skipped = 0;
    pz_mm_result_init(&result);
    if (!manager) return result_fail_text("No manager");
    unique_installed_ids(manager, &ids);
    server_locked_ids(manager, &server_locks);
    for (i = 0; i < ids.length; ++i) {
        const uint64_t id = *(const uint64_t*)pz_vec_at_const(&ids, i);
        uint64_t remote;
        int changed = 0;
        pz_mm_result item;
        if (pz_mod_manager_manually_locked(manager, id) ||
            u64_list_contains(&server_locks, id)) {
            log_line(log, "locked: ", id, " (skipped)");
            ++locked_skipped;
            continue;
        }
        remote = manager->services->remote_time(manager->services->context, id);
        if (remote == 0) {
            ++skipped;
            log_line(log, "update check failed: ", id, " (skipped)");
            continue;
        }
        if (remote <= db_recorded_time(manager, id)) {
            update_set(manager, id, 0);
            log_line(log, "up to date: ", id, NULL);
            continue;
        }
        item = install_item(manager, id, remote, log, &changed, 0);
        if (!item.ok) {
            pz_str message;
            pz_str_init(&message);
            pz_str_append_cstr(&message, "failed: ");
            pz_str_append_u64(&message, id);
            pz_str_append_char(&message, ' ');
            pz_str_append(&message, item.message.data, item.message.length);
            if (log) pz_log_emit_str(log, &message);
            pz_str_free(&message);
            pz_mm_result_free(&item);
            continue;
        }
        update_set(manager, id, 0);
        if (!changed) {
            log_line(log, "up to date: ", id, NULL);
            ++unchanged;
        } else {
            log_line(log, "updated: ", id, NULL);
            ++updated;
        }
        pz_mm_result_free(&item);
    }
    if (ids.length != 0) record_timestamp(manager, "last_check");
    save_update_state(manager);
    result.ok = 1;
    pz_str_append_cstr(&result.message, "Updated ");
    pz_str_append_u64(&result.message, (uint64_t)updated);
    pz_str_append_cstr(&result.message, " mod item(s)");
    if (unchanged) {
        pz_str_append_cstr(&result.message, ", ");
        pz_str_append_u64(&result.message, (uint64_t)unchanged);
        pz_str_append_cstr(&result.message, " unchanged");
    }
    if (locked_skipped) {
        pz_str_append_cstr(&result.message, ", skipped ");
        pz_str_append_u64(&result.message, (uint64_t)locked_skipped);
        pz_str_append_cstr(&result.message, " locked");
    }
    if (skipped) {
        pz_str_append_cstr(&result.message, ", skipped ");
        pz_str_append_u64(&result.message, (uint64_t)skipped);
        pz_str_append_cstr(&result.message, " (update check failed)");
    }
    pz_vec_free(&ids);
    pz_vec_free(&server_locks);
    return result;
}

pz_mm_result pz_mod_manager_check_app_release(pz_mod_manager* manager,
                                              const pz_log* log) {
    pz_app_update update;
    pz_mm_result result;
    pz_path file;
    pz_str text;
    pz_mm_result_init(&result);
    pz_app_update_init(&update);
    if (!manager) return result_fail_text("No manager");
    if (!manager->services->check_app_update(manager->services->context, &update)) {
        record_timestamp(manager, "last_app_check");
        result = result_fail_text("App update check failed");
        pz_app_update_free(&update);
        return result;
    }
    record_timestamp(manager, "last_app_check");
    if (!update.ok) {
        if (log) pz_log_emit_str(log, &update.message);
        result = result_fail_text(update.message.data ? update.message.data
                                                       : "App update check failed");
        pz_app_update_free(&update);
        return result;
    }
    manager->app_update_available = update.available;
    pz_path_init(&file);
    pz_str_init(&text);
    if (path_from_data(manager, "app_update", &file) &&
        pz_str_append(&text, update.latest_version.data,
                      update.latest_version.length) &&
        pz_str_append_char(&text, '\n')) {
        if (!write_text_file(&file, text.data, text.length))
            log_text(log, "warning: could not save the app update state");
    }
    pz_str_free(&text);
    pz_path_free(&file);
    result.ok = 1;
    str_assign(&result.message, &update.message);
    if (log) pz_log_emit_str(log, &result.message);
    pz_app_update_free(&update);
    return result;
}

static pz_mm_result install_collection(pz_mod_manager* manager,
                                       uint64_t collection_id,
                                       const pz_vec* children,
                                       const pz_log* log) {
    pz_vec server_locks;
    pz_mm_result result;
    size_t i;
    int installed = 0, updated = 0, skipped = 0, failed = 0;
    pz_str message;
    pz_mm_result_init(&result);
    pz_str_init(&message);
    pz_str_append_cstr(&message, "collection ");
    pz_str_append_u64(&message, collection_id);
    pz_str_append_cstr(&message, ": ");
    pz_str_append_u64(&message, (uint64_t)children->length);
    pz_str_append_cstr(&message, " item(s)");
    if (log) pz_log_emit_str(log, &message);
    pz_str_free(&message);
    server_locked_ids(manager, &server_locks);
    for (i = 0; i < children->length; ++i) {
        const uint64_t child = *(const uint64_t*)pz_vec_at_const(children, i);
        const int present = db_is_installed(manager, child);
        uint64_t remote;
        int changed = 0;
        pz_mm_result item;
        if (present && (pz_mod_manager_manually_locked(manager, child) ||
                        u64_list_contains(&server_locks, child))) {
            log_line(log, "locked: ", child, " (skipped)");
            ++skipped;
            continue;
        }
        remote = manager->services->remote_time(manager->services->context, child);
        if (remote == 0) {
            log_line(log, "update check failed: ", child, " (skipped)");
            ++skipped;
            continue;
        }
        if (present && remote <= db_recorded_time(manager, child)) {
            log_line(log, "up to date: ", child, NULL);
            ++skipped;
            continue;
        }
        item = install_item(manager, child, remote, log, &changed, 1);
        if (!item.ok) {
            pz_str failure;
            pz_str_init(&failure);
            pz_str_append_cstr(&failure, "failed: ");
            pz_str_append_u64(&failure, child);
            pz_str_append_char(&failure, ' ');
            pz_str_append(&failure, item.message.data, item.message.length);
            if (log) pz_log_emit_str(log, &failure);
            pz_str_free(&failure);
            ++failed;
        } else if (!changed) {
            if (present) ++skipped; else ++installed;
        } else if (present) {
            ++updated;
        } else {
            ++installed;
        }
        pz_mm_result_free(&item);
    }
    pz_vec_free(&server_locks);
    result.ok = failed == 0;
    pz_str_append_cstr(&result.message, "Collection ");
    pz_str_append_u64(&result.message, collection_id);
    pz_str_append_cstr(&result.message, ": installed ");
    pz_str_append_u64(&result.message, (uint64_t)installed);
    pz_str_append_cstr(&result.message, ", updated ");
    pz_str_append_u64(&result.message, (uint64_t)updated);
    pz_str_append_cstr(&result.message, ", skipped ");
    pz_str_append_u64(&result.message, (uint64_t)skipped);
    pz_str_append_cstr(&result.message, ", failed ");
    pz_str_append_u64(&result.message, (uint64_t)failed);
    return result;
}

pz_mm_result pz_mod_manager_add(pz_mod_manager* manager, const char* input,
                                size_t input_length, const pz_log* log) {
    uint64_t id = 0;
    pz_vec children;
    pz_mm_result result;
    pz_mm_result_init(&result);
    if (!manager) return result_fail_text("No manager");
    if (!pz_url_parse_workshop_id(input, input_length, &id))
        return result_fail_text("No Workshop ID found");
    pz_vec_init(&children, sizeof(uint64_t));
    if (manager->services->collection_children(manager->services->context, id,
                                               &children) &&
        children.length != 0) {
        result = install_collection(manager, id, &children, log);
        pz_vec_free(&children);
        return result;
    }
    pz_vec_free(&children);
    if (db_is_installed(manager, id))
        return result_fail_text("Already installed. Use: update");
    {
        uint64_t remote =
            manager->services->remote_time(manager->services->context, id);
        /* install_item checks the published-file result first, so an
         * unavailable item still gets its specific error even when the time
         * endpoint returned no value. */
        return install_item(manager, id, remote, log, NULL, 1);
    }
}

pz_mm_result pz_mod_manager_set_zomboid_dir(pz_mod_manager* manager,
                                            const char* path, size_t length) {
    pz_path normalized, parent, mods, data;
    pz_mm_result result;
    pz_str old_builtin, new_builtin;
    pz_fs_error error;

    pz_mm_result_init(&result);
    if (!manager) return result_fail_text("No manager");
    if (!path || length == 0) return result_fail_text("Zomboid folder is empty");
    pz_path_init(&normalized);
    if (!pz_path_assign(&normalized, path, length) ||
        !pz_path_lexically_normal(&normalized, &normalized))
        goto fail_empty;
    if (!pz_path_is_absolute(&normalized))
        goto fail_relative;
    pz_path_init(&parent);
    if (pz_path_parent(&normalized, &parent) && path_equals(&normalized, &parent))
        goto fail_root;
    pz_path_free(&parent);
    if (path_equals(&normalized,
                    &(pz_path){manager->config.zomboid_dir})) {
        result.ok = 1;
        pz_str_assign_cstr(&result.message, "Zomboid folder unchanged");
        pz_path_free(&normalized);
        return result;
    }
    pz_path_init(&mods);
    pz_path_init(&data);
    if (!pz_path_join(&mods, &normalized, "mods", 4) ||
        !pz_path_join(&data, &normalized, "pzmodmgr", 8))
        goto fail_paths;
    pz_fs_error_clear(&error);
    if (!pz_fs_create_directories(&mods, &error) ||
        !pz_fs_create_directories(&data, &error))
        goto fail_paths;
    pz_str_init(&old_builtin);
    pz_str_init(&new_builtin);
    if (!manager->services->builtin_steamcmd(manager->services->context,
                                              manager->config.data_dir.data,
                                              manager->config.data_dir.length,
                                              &old_builtin))
        goto fail_paths;
    pz_str_assign(&manager->config.zomboid_dir, normalized.text.data,
                  normalized.text.length);
    pz_str_assign(&manager->config.user_mods_dir, mods.text.data, mods.text.length);
    pz_str_assign(&manager->config.data_dir, data.text.data, data.text.length);
    if (!manager->services->builtin_steamcmd(manager->services->context,
                                              manager->config.data_dir.data,
                                              manager->config.data_dir.length,
                                              &new_builtin))
        goto fail_paths;
    if (manager->config.steamcmd_exe.length == 0 ||
        str_equals_text(&manager->config.steamcmd_exe, old_builtin.data,
                        old_builtin.length))
        pz_str_assign(&manager->config.steamcmd_exe, new_builtin.data,
                      new_builtin.length);
    prune_stale_state(manager);
    read_game_version(manager);
    load_db(manager);
    load_update_state(manager);
    save_update_state(manager);
    load_app_state(manager);
    result.ok = 1;
    pz_str_assign_cstr(&result.message, "Zomboid folder set to ");
    pz_str_append(&result.message, normalized.text.data, normalized.text.length);
    pz_str_free(&old_builtin);
    pz_str_free(&new_builtin);
    pz_path_free(&mods);
    pz_path_free(&data);
    pz_path_free(&normalized);
    return result;

fail_root:
    pz_path_free(&parent);
    pz_path_free(&normalized);
    return result_fail_text("Refusing to use a filesystem root as the Zomboid folder");
fail_relative:
    pz_path_free(&normalized);
    return result_fail_text("Zomboid folder must be an absolute path");
fail_paths:
    pz_path_free(&mods);
    pz_path_free(&data);
    pz_path_free(&normalized);
    return result_fail_text("Could not create the new Zomboid folders");
fail_empty:
    pz_path_free(&normalized);
    return result_fail_text("Zomboid folder is empty");
}

/* --------------------------------------------------------------- import */

typedef struct acf_item {
    uint64_t id;
    uint64_t time_updated;
} acf_item;

static int acf_add(pz_vec* items, uint64_t id, uint64_t time_updated) {
    size_t i;
    acf_item* slot;
    if (id == 0) return 1;
    for (i = 0; i < items->length; ++i) {
        const acf_item* item = (const acf_item*)pz_vec_at_const(items, i);
        if (item->id == id) {
            if (time_updated) ((acf_item*)pz_vec_at(items, i))->time_updated = time_updated;
            return 1;
        }
    }
    slot = (acf_item*)pz_vec_push(items);
    if (!slot) return 0;
    slot->id = id;
    slot->time_updated = time_updated;
    return 1;
}

static int acf_read(const pz_path* file, pz_vec* items) {
    pz_str text;
    const char* section;
    const char* open;
    size_t i;
    int depth = 0;
    int active = 0;
    uint64_t current_id = 0, current_time = 0;
    int ok = 0;

    pz_str_init(&text);
    if (!read_file(file, &text)) goto done;
    section = strstr(text.data, "\"WorkshopItemsInstalled\"");
    if (!section) goto done;
    open = strchr(section + 23, '{');
    if (!open) goto done;
    i = (size_t)(open - text.data);
    while (i < text.length) {
        const char c = text.data[i];
        if (c == '"') {
            size_t start = ++i;
            char token[64];
            size_t token_length;
            size_t next;
            while (i < text.length && text.data[i] != '"') {
                if (text.data[i] == '\\' && i + 1 < text.length) ++i;
                ++i;
            }
            if (i >= text.length) break;
            token_length = i - start;
            if (token_length >= sizeof(token)) token_length = sizeof(token) - 1;
            memcpy(token, text.data + start, token_length);
            token[token_length] = '\0';
            ++i;
            next = i;
            while (next < text.length && isspace((unsigned char)text.data[next])) ++next;
            if (depth == 1 && !active) {
                uint64_t id = 0;
                if (parse_u64(token, token_length, &id) && next < text.length &&
                    text.data[next] == '{') {
                    current_id = id;
                    current_time = 0;
                    active = 1;
                }
            } else if (depth == 2 && active && strcmp(token, "timeupdated") == 0 &&
                       next < text.length && text.data[next] == '"') {
                size_t value_start = ++next;
                while (next < text.length && text.data[next] != '"') ++next;
                parse_u64(text.data + value_start, next - value_start, &current_time);
            }
            continue;
        }
        if (c == '{') {
            ++depth;
        } else if (c == '}') {
            if (depth == 2 && active) {
                acf_add(items, current_id, current_time);
                active = 0;
            }
            --depth;
            if (depth == 0) break;
        }
        ++i;
    }
    ok = 1;
done:
    pz_str_free(&text);
    return ok;
}

static int parse_content_id(const pz_str* name, uint64_t* id) {
    return parse_u64(name->data, name->length, id);
}

static int add_candidate(pz_vec* candidates, const pz_str* folder,
                         const pz_str* name, const pz_str* mod_id) {
    size_t i;
    pz_str* slot;
    (void)name;
    (void)mod_id;
    for (i = 0; i < candidates->length; ++i) {
        const pz_str* existing = (const pz_str*)pz_vec_at_const(candidates, i);
        if (fold_equals(existing, folder)) return 1;
    }
    slot = (pz_str*)pz_vec_push(candidates);
    if (!slot) return 0;
    pz_str_init(slot);
    return str_assign(slot, folder);
}

static int copy_mod_roots_to_user(pz_mod_manager* manager, const pz_path* source,
                                  uint64_t id, uint64_t time_updated,
                                  const pz_log* log, int* imported_folders) {
    pz_vec roots, info_roots, copied;
    pz_vec folders;
    size_t i;
    int ok = 0;

    pz_vec_init(&roots, sizeof(pz_str));
    pz_vec_init(&info_roots, sizeof(pz_str));
    pz_vec_init(&folders, sizeof(pz_str));
    pz_vec_init(&copied, sizeof(pz_str));
    if (!discover_roots(manager, source, &info_roots)) goto done;
    for (i = 0; i < info_roots.length; ++i) {
        const pz_str* root_text = (const pz_str*)pz_vec_at_const(&info_roots, i);
        pz_path root;
        pz_mod_info info;
        pz_str folder;
        pz_path_init(&root);
        pz_str_init(&folder);
        if (!pz_path_assign(&root, root_text->data, root_text->length) ||
            !read_info(manager, &root, &info) || info.id.length == 0) {
            pz_mod_info_free(&info);
            pz_str_free(&folder);
            pz_path_free(&root);
            continue;
        }
        if (path_equals(&root, source))
            str_assign(&folder, &info.id);
        else
            pz_path_filename(&root, &folder);
        if (folder.length != 0 && add_candidate(&folders, &folder, &info.name,
                                                &info.id)) {
            pz_str* slot = (pz_str*)pz_vec_push(&roots);
            if (slot) {
                pz_str_init(slot);
                str_assign(slot, root_text);
            }
        }
        pz_mod_info_free(&info);
        pz_str_free(&folder);
        pz_path_free(&root);
    }
    if (roots.length == 0) goto done;

    for (i = 0; i < roots.length; ++i) {
        const pz_str* root_text = (const pz_str*)pz_vec_at_const(&roots, i);
        pz_path root, dest;
        pz_mod_info info;
        pz_str folder;
        pz_path_init(&root);
        pz_path_init(&dest);
        pz_str_init(&folder);
        if (!pz_path_assign(&root, root_text->data, root_text->length) ||
            !read_info(manager, &root, &info) || info.id.length == 0) {
            pz_mod_info_free(&info);
            goto next_root;
        }
        if (path_equals(&root, source))
            str_assign(&folder, &info.id);
        else
            pz_path_filename(&root, &folder);
        if (!path_from_folder(manager, &folder, &dest) ||
            path_exists(&dest)) {
            pz_mod_info_free(&info);
            goto next_root;
        }
        {
            pz_fs_error error;
            pz_fs_error_clear(&error);
            if (!pz_fs_copy_tree(&root, &dest, PZ_FS_COPY_RECURSIVE, &error)) {
                pz_str message;
                pz_str_init(&message);
                pz_str_append_cstr(&message, "import: could not copy ");
                pz_str_append(&message, folder.data, folder.length);
                if (log) pz_log_emit_str(log, &message);
                pz_str_free(&message);
                pz_mod_info_free(&info);
                for (size_t j = 0; j < copied.length; ++j) {
                    const pz_str* path = (const pz_str*)pz_vec_at_const(&copied, j);
                    pz_path cleanup;
                    pz_path_init(&cleanup);
                    if (pz_path_assign(&cleanup, path->data, path->length))
                        remove_path(&cleanup);
                    pz_path_free(&cleanup);
                }
                goto done;
            }
        }
        {
            pz_str* slot = (pz_str*)pz_vec_push(&copied);
            if (slot) {
                pz_str_init(slot);
                str_assign(slot, &dest.text);
            }
        }
        if (!db_add(manager, id, folder.data, folder.length,
                    info.name.length ? info.name.data : folder.data,
                    info.name.length ? info.name.length : folder.length,
                    time_updated, 1, 0, 1)) {
            pz_mod_info_free(&info);
            goto next_root;
        }
        ++*imported_folders;
        {
            pz_str message;
            pz_str_init(&message);
            pz_str_append_cstr(&message, "imported ");
            pz_str_append_u64(&message, id);
            pz_str_append_cstr(&message, ": ");
            pz_str_append(&message, info.name.length ? info.name.data : folder.data,
                          info.name.length ? info.name.length : folder.length);
            if (log) pz_log_emit_str(log, &message);
            pz_str_free(&message);
        }
        ok = 1;
next_root:
        pz_mod_info_free(&info);
        pz_str_free(&folder);
        pz_path_free(&root);
        pz_path_free(&dest);
    }
done:
    for (i = 0; i < copied.length; ++i)
        pz_str_free((pz_str*)pz_vec_at(&copied, i));
    pz_vec_free(&copied);
    string_list_free(&folders);
    for (i = 0; i < roots.length; ++i)
        pz_str_free((pz_str*)pz_vec_at(&roots, i));
    pz_vec_free(&roots);
    string_list_free(&info_roots);
    return ok;
}

static int destination_matches(pz_mod_manager* manager, const pz_path* source,
                               pz_vec* installed_copies, const pz_log* log) {
    pz_path bases[3];
    pz_vec names;
    size_t i, b;
    int found = 0;
    pz_path_init(&bases[0]);
    pz_path_init(&bases[1]);
    pz_path_init(&bases[2]);
    pz_vec_init(&names, sizeof(pz_str));
    pz_path_join(&bases[0], source, "mods", 4);
    {
        pz_path contents;
        pz_path_init(&contents);
        pz_path_join(&contents, source, "Contents", 8);
        pz_path_join(&bases[1], &contents, "mods", 4);
        pz_path_free(&contents);
    }
    pz_path_assign(&bases[2], source->text.data, source->text.length);

    for (b = 0; b < 3; ++b) {
        pz_vec entries;
        pz_fs_error error;
        pz_vec_init(&entries, sizeof(pz_fs_entry));
        pz_fs_error_clear(&error);
        if (pz_fs_list_dir(&bases[b], &entries, &error)) {
            for (i = 0; i < entries.length; ++i) {
                const pz_fs_entry* entry =
                    (const pz_fs_entry*)pz_vec_at_const(&entries, i);
                if (entry->kind == PZ_FS_DIR)
                    string_list_add_unique(&names, &entry->name);
            }
        }
        pz_fs_entries_free(&entries);
    }
    {
        pz_str leaf;
        pz_str_init(&leaf);
        if (pz_path_filename(source, &leaf) && leaf.length)
            string_list_add_unique(&names, &leaf);
        pz_str_free(&leaf);
    }
    for (i = 0; i < names.length; ++i) {
        const pz_str* folder = (const pz_str*)pz_vec_at_const(&names, i);
        pz_path dest;
        pz_mod_info info;
        if (str_equals_cstr(folder, "mods") || str_equals_cstr(folder, "Contents") ||
            str_equals_cstr(folder, "common"))
            continue;
        pz_path_init(&dest);
        if (path_from_folder(manager, folder, &dest) &&
            read_info(manager, &dest, &info) && info.id.length != 0) {
            pz_str folded, message;
            pz_str_init(&folded);
            pz_str_init(&message);
            make_folded(&info.id, &folded);
            string_list_add_unique(installed_copies, &folded);
            pz_str_append_cstr(&message, "import: ");
            pz_str_append_u64(&message, 0);
            pz_str_append_cstr(&message, " matches Zomboid/mods/");
            pz_str_append(&message, folder->data, folder->length);
            pz_str_append_cstr(&message, " (skipped)");
            if (log) pz_log_emit_str(log, &message);
            pz_str_free(&message);
            pz_str_free(&folded);
            found = 1;
        }
        pz_mod_info_free(&info);
        pz_path_free(&dest);
    }
    string_list_free(&names);
    pz_path_free(&bases[0]);
    pz_path_free(&bases[1]);
    pz_path_free(&bases[2]);
    return found;
}

pz_mm_result pz_mod_manager_import(pz_mod_manager* manager,
                                   const uint64_t* only_ids, size_t only_count,
                                   const pz_log* log) {
    pz_mm_result result;
    pz_path root, content_root, acf_path;
    pz_vec acf_items, discovered, all, installed_copies, tracked_mod_ids;
    pz_vec untracked, adopted_folders;
    size_t i, j;
    int imported_items = 0, imported_folders = 0;
    int skipped_tracked = 0, skipped_installed = 0;
    int skipped_missing = 0, skipped_invalid = 0, failed = 0;
    int explicit_request = only_count != 0;
    int have_root = 0;

    pz_mm_result_init(&result);
    if (!manager) return result_fail_text("No manager");
    pz_path_init(&root);
    pz_path_init(&content_root);
    pz_path_init(&acf_path);
    pz_vec_init(&acf_items, sizeof(acf_item));
    pz_vec_init(&discovered, sizeof(acf_item));
    pz_vec_init(&all, sizeof(pz_installed_mod));
    pz_vec_init(&installed_copies, sizeof(pz_str));
    pz_vec_init(&tracked_mod_ids, sizeof(pz_str));
    pz_vec_init(&untracked, sizeof(pz_str));
    pz_vec_init(&adopted_folders, sizeof(pz_str));

    if (!manager->config.steamcmd_root.length) {
        if (!explicit_request) {
            result = result_fail_text("Set the SteamCMD download root in Settings first");
            goto done;
        }
    } else if (!path_from_str(&root, &manager->config.steamcmd_root) ||
               !pz_path_lexically_normal(&root, &root) ||
               !pz_path_is_absolute(&root)) {
        if (!explicit_request) {
            result = result_fail_text("SteamCMD download root must be an absolute path");
            goto done;
        }
    } else if (!path_is_dir(&root)) {
        if (!explicit_request) {
            result = result_fail_text("SteamCMD download root not found");
            goto done;
        }
    } else {
        have_root = 1;
        if (path_equals(&root, &(pz_path){manager->config.user_mods_dir}) ||
            path_equals(&root, &(pz_path){manager->config.data_dir})) {
            result = result_fail_text(
                "The SteamCMD download root cannot be the Zomboid mods or data folder");
            goto done;
        }
    }
    if (!have_root)
        log_text(log, "import: SteamCMD downloads unavailable; matching local folders by Workshop title");

    if (have_root) {
        pz_path steamapps;
        pz_path_init(&steamapps);
        if (pz_path_join(&steamapps, &root, "steamapps", 9) &&
            pz_path_join(&steamapps, &steamapps, "workshop", 8)) {
            pz_path_join(&content_root, &steamapps, "content", 7);
            pz_path_join(&content_root, &content_root, "108600", 6);
            pz_path_join(&acf_path, &steamapps, "appworkshop_108600.acf", 22);
        }
        pz_path_free(&steamapps);
    }
    if (have_root && path_is_file(&acf_path)) acf_read(&acf_path, &acf_items);

    if (explicit_request) {
        for (i = 0; i < only_count; ++i) {
            uint64_t time = 0;
            for (j = 0; j < acf_items.length; ++j) {
                const acf_item* item = (const acf_item*)pz_vec_at_const(&acf_items, j);
                if (item->id == only_ids[i]) { time = item->time_updated; break; }
            }
            acf_add(&discovered, only_ids[i], time);
        }
    } else if (acf_items.length != 0) {
        for (i = 0; i < acf_items.length; ++i)
            acf_add(&discovered,
                    ((const acf_item*)pz_vec_at_const(&acf_items, i))->id,
                    ((const acf_item*)pz_vec_at_const(&acf_items, i))->time_updated);
    } else if (have_root) {
        pz_vec entries;
        pz_fs_error error;
        pz_vec_init(&entries, sizeof(pz_fs_entry));
        pz_fs_error_clear(&error);
        if (pz_fs_list_dir(&content_root, &entries, &error)) {
            for (i = 0; i < entries.length; ++i) {
                const pz_fs_entry* entry =
                    (const pz_fs_entry*)pz_vec_at_const(&entries, i);
                uint64_t id;
                if (entry->kind == PZ_FS_DIR && parse_content_id(&entry->name, &id))
                    acf_add(&discovered, id, 0);
            }
        }
        pz_fs_entries_free(&entries);
    }
    if (discovered.length == 0) {
        result = result_ok_text(have_root ? "No Workshop items found"
                                          : "No local Workshop items matched");
        goto done;
    }
    for (i = 1; i < discovered.length; ++i) {
        acf_item value = *(acf_item*)pz_vec_at(&discovered, i);
        size_t j = i;
        while (j > 0 &&
               ((acf_item*)pz_vec_at(&discovered, j - 1))->id > value.id) {
            *(acf_item*)pz_vec_at(&discovered, j) =
                *(acf_item*)pz_vec_at(&discovered, j - 1);
            --j;
        }
        *(acf_item*)pz_vec_at(&discovered, j) = value;
    }

    pz_mod_manager_list_all(manager, &all);
    for (i = 0; i < all.length; ++i) {
        const pz_installed_mod* mod = (const pz_installed_mod*)pz_vec_at_const(&all, i);
        pz_path dir;
        pz_mod_info info;
        pz_path_init(&dir);
        if (!path_from_folder(manager, &mod->folder, &dir) ||
            !read_info(manager, &dir, &info) || info.id.length == 0) {
            pz_mod_info_free(&info);
            pz_path_free(&dir);
            continue;
        }
        {
            pz_str folded;
            pz_str_init(&folded);
            make_folded(&info.id, &folded);
            if (mod->workshop_id == 0) {
                if (string_list_add_unique(&untracked, &mod->folder)) {
                    string_list_add_unique(&untracked,
                        info.name.length ? &info.name : &mod->folder);
                    string_list_add_unique(&untracked, &folded);
                }
            } else {
                string_list_add_unique(&tracked_mod_ids, &folded);
            }
            string_list_add_unique(&installed_copies, &folded);
            pz_str_free(&folded);
        }
        pz_mod_info_free(&info);
        pz_path_free(&dir);
    }

    for (i = 0; i < discovered.length; ++i) {
        const acf_item* item = (const acf_item*)pz_vec_at_const(&discovered, i);
        pz_path source;
        pz_vec roots;
        pz_mod_info first_info;
        int item_failed = 0;
        int already = 0;
        if (db_is_installed(manager, item->id)) {
            log_line(log, "import: ", item->id, " already tracked (skipped)");
            ++skipped_tracked;
            continue;
        }
        pz_path_init(&source);
        if (have_root) path_join_u64(&source, &content_root, item->id, NULL);
        if (!have_root || !path_is_dir(&source)) {
            if (explicit_request) {
                pz_remote_details details;
                pz_remote_details_init(&details);
                if (manager->services->remote_details(manager->services->context,
                                                      item->id, &details) &&
                    details.result == 1 && details.title.length) {
                    pz_str title_folded;
                    pz_str_init(&title_folded);
                    make_folded(&details.title, &title_folded);
                    for (j = 0; j + 2 < untracked.length; j += 3) {
                        const pz_str* folder = (const pz_str*)pz_vec_at_const(&untracked, j);
                        const pz_str* title = (const pz_str*)pz_vec_at_const(&untracked, j + 1);
                        const pz_str* mod_id = (const pz_str*)pz_vec_at_const(&untracked, j + 2);
                        if (string_list_contains(&adopted_folders, folder)) continue;
                        if (string_list_contains(&tracked_mod_ids, mod_id)) continue;
                        if (fold_equals(folder, &title_folded) ||
                            fold_equals(title, &title_folded)) {
                            pz_path dest;
                            pz_mod_info info;
                            pz_path_init(&dest);
                            if (path_from_folder(manager, folder, &dest) &&
                                read_info(manager, &dest, &info) && info.id.length) {
                                db_add(manager, item->id, folder->data, folder->length,
                                       info.name.length ? info.name.data : folder->data,
                                       info.name.length ? info.name.length : folder->length,
                                       details.time_updated, 0, 0, 1);
                                string_list_add_unique(&adopted_folders, folder);
                                string_list_add_unique(&tracked_mod_ids, mod_id);
                                ++imported_items;
                                ++imported_folders;
                                already = 1;
                                {
                                    pz_str message;
                                    pz_str_init(&message);
                                    pz_str_append_cstr(&message, "imported ");
                                    pz_str_append_u64(&message, item->id);
                                    pz_str_append_cstr(&message, ": ");
                                    pz_str_append(&message, folder->data, folder->length);
                                    pz_str_append_cstr(&message,
                                        " (tracked existing folder)");
                                    if (log) pz_log_emit_str(log, &message);
                                    pz_str_free(&message);
                                }
                            }
                            pz_mod_info_free(&info);
                            pz_path_free(&dest);
                            break;
                        }
                    }
                    pz_str_free(&title_folded);
                } else {
                    log_line(log, "import: ", item->id,
                             " could not read Workshop details (skipped)");
                    pz_remote_details_free(&details);
                    pz_path_free(&source);
                    ++skipped_missing;
                    continue;
                }
                pz_remote_details_free(&details);
            }
            if (!already) {
                log_line(log, "import: ", item->id, " has no content folder (skipped)");
                ++skipped_missing;
            }
            pz_path_free(&source);
            continue;
        }

        pz_vec_init(&roots, sizeof(pz_str));
        pz_mod_info_init(&first_info);
        if (!copy_mod_roots_to_user(manager, &source, item->id,
                                    item->time_updated, log, &imported_folders)) {
            if (destination_matches(manager, &source, &installed_copies, log)) {
                log_line(log, "import: ", item->id,
                         " is already in Zomboid/mods (skipped)");
                ++skipped_installed;
            } else {
                log_line(log, "import: ", item->id,
                         " has no valid mod.info (skipped)");
                ++skipped_invalid;
            }
        } else {
            ++imported_items;
        }
        (void)item_failed;
        pz_mod_info_free(&first_info);
        pz_vec_free(&roots);
        pz_path_free(&source);
    }
    if (imported_folders != 0) save_db(manager);
    result.ok = !(explicit_request && (skipped_missing != 0 || skipped_invalid != 0 ||
                                       failed != 0));
    pz_str_append_cstr(&result.message, "Imported ");
    pz_str_append_u64(&result.message, (uint64_t)imported_items);
    pz_str_append_cstr(&result.message, " item(s), ");
    pz_str_append_u64(&result.message, (uint64_t)imported_folders);
    pz_str_append_cstr(&result.message, " mod folder(s)");
    if (skipped_tracked) {
        pz_str_append_cstr(&result.message, ", ");
        pz_str_append_u64(&result.message, (uint64_t)skipped_tracked);
        pz_str_append_cstr(&result.message, " already tracked");
    }
    if (skipped_installed) {
        pz_str_append_cstr(&result.message, ", ");
        pz_str_append_u64(&result.message, (uint64_t)skipped_installed);
        pz_str_append_cstr(&result.message, " already installed");
    }
    if (skipped_invalid) {
        pz_str_append_cstr(&result.message, ", ");
        pz_str_append_u64(&result.message, (uint64_t)skipped_invalid);
        pz_str_append_cstr(&result.message, " without valid mod.info");
    }
    if (skipped_missing) {
        pz_str_append_cstr(&result.message, ", ");
        pz_str_append_u64(&result.message, (uint64_t)skipped_missing);
        pz_str_append_cstr(&result.message, " missing content");
    }
    if (failed) {
        pz_str_append_cstr(&result.message, ", ");
        pz_str_append_u64(&result.message, (uint64_t)failed);
        pz_str_append_cstr(&result.message, " copy failure(s)");
    }

done:
    for (i = 0; i < all.length; ++i)
        pz_installed_mod_free((pz_installed_mod*)pz_vec_at(&all, i));
    pz_vec_free(&all);
    string_list_free(&adopted_folders);
    string_list_free(&untracked);
    string_list_free(&tracked_mod_ids);
    string_list_free(&installed_copies);
    pz_vec_free(&discovered);
    pz_vec_free(&acf_items);
    pz_path_free(&acf_path);
    pz_path_free(&content_root);
    pz_path_free(&root);
    return result;
}
