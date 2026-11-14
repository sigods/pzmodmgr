#include "core/activation_targets_c.h"

#include "core/fs_c.h"

#include <ctype.h>
#include <stdint.h>
#include <string.h>

static int str_assign(pz_str* out, const pz_str* value) {
    return pz_str_assign(out, value->data, value->length);
}

static int string_list_contains(const pz_vec* list, const pz_str* value) {
    size_t i;
    for (i = 0; i < list->length; ++i) {
        const pz_str* item = (const pz_str*)pz_vec_at_const(list, i);
        if (pz_str_equals(item, value->data, value->length)) return 1;
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

static void string_list_remove(pz_vec* list, const pz_str* value) {
    size_t i = 0;
    while (i < list->length) {
        pz_str* item = (pz_str*)pz_vec_at(list, i);
        if (pz_str_equals(item, value->data, value->length)) {
            pz_str_free(item);
            memmove(item, item + 1,
                    (list->length - i - 1) * sizeof(*item));
            --list->length;
        } else {
            ++i;
        }
    }
}

static int u64_list_contains(const pz_vec* list, uint64_t value) {
    size_t i;
    for (i = 0; i < list->length; ++i) {
        if (*(const uint64_t*)pz_vec_at_const(list, i) == value) return 1;
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

static int read_file(const pz_path* path, pz_str* out) {
    pz_fs_error error;
    pz_fs_error_clear(&error);
    return pz_fs_read_all(path, out, &error);
}

static int path_kind_is_file(pz_fs_kind kind) {
    return kind == PZ_FS_FILE || kind == PZ_FS_LINK;
}

static unsigned long long g_target_temp_counter;

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
        !pz_str_append_u64(&suffix, ++g_target_temp_counter) ||
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

static void clear_mod_ids(pz_activation_target_data* target) {
    pz_activation_string_list_free(&target->mod_ids);
    pz_vec_init(&target->mod_ids, sizeof(pz_str));
}

static void clear_workshop_ids(pz_activation_target_data* target) {
    pz_vec_free(&target->workshop_ids);
    pz_vec_init(&target->workshop_ids, sizeof(uint64_t));
}

static int read_target(pz_activation_target_data* target) {
    pz_path path;
    pz_str text;
    int readable;

    pz_path_init(&path);
    pz_str_init(&text);
    clear_mod_ids(target);
    clear_workshop_ids(target);
    target->exists = 0;
    if (!pz_path_assign(&path, target->path.data, target->path.length)) {
        pz_path_free(&path);
        pz_str_free(&text);
        return 0;
    }
    readable = read_file(&path, &text);
    if (!readable) {
        pz_path_free(&path);
        pz_str_free(&text);
        return 1;
    }

    target->exists = 1;
    if (target->kind == 2) {
        int detected = 0;
        if (!pz_activation_parse_ini_ids(text.data, text.length,
                                         &target->mod_ids, &detected))
            clear_mod_ids(target);
        if (!pz_activation_parse_ini_workshop_ids(text.data, text.length,
                                                  &target->workshop_ids))
            clear_workshop_ids(target);
        if (target->mod_ids.length != 0) target->backslash = detected != 0;
    } else {
        pz_activation_parse_block_ids(text.data, text.length, &target->mod_ids);
        target->backslash = 0;
        if (pz_activation_block_uses_backslash(text.data, text.length)) {
            pz_str out;
            int changed = 0;
            pz_str_init(&out);
            if (pz_activation_rewrite_block_ids(text.data, text.length,
                                                &target->mod_ids, &out,
                                                &changed) &&
                changed)
                write_file_atomic(&path, &out);
            pz_str_free(&out);
        }
    }

    pz_path_free(&path);
    pz_str_free(&text);
    return 1;
}

static int target_name_less(const pz_str* left, const pz_str* right) {
    const size_t shared =
        left->length < right->length ? left->length : right->length;
    const int compared = shared == 0 ? 0 : memcmp(left->data, right->data, shared);
    if (compared != 0) return compared < 0;
    return left->length < right->length;
}

static void sort_targets(pz_vec* targets, size_t first) {
    size_t i;
    for (i = first + 1; i < targets->length; ++i) {
        pz_activation_target_data value;
        size_t j = i;
        value = *(pz_activation_target_data*)pz_vec_at(targets, i);
        while (j > first) {
            pz_activation_target_data* previous =
                (pz_activation_target_data*)pz_vec_at(targets, j - 1);
            if (!target_name_less(&value.name, &previous->name)) break;
            *(pz_activation_target_data*)pz_vec_at(targets, j) = *previous;
            --j;
        }
        *(pz_activation_target_data*)pz_vec_at(targets, j) = value;
    }
}

static pz_activation_target_data* append_target(pz_vec* out) {
    pz_activation_target_data* target =
        (pz_activation_target_data*)pz_vec_push(out);
    if (target) pz_activation_target_data_init(target);
    return target;
}

static void drop_last(pz_vec* out) {
    if (out->length == 0) return;
    pz_activation_target_data_free(
        (pz_activation_target_data*)pz_vec_at(out, out->length - 1));
    --out->length;
}

static int set_default_target(pz_activation_target_data* target,
                              const pz_path* zomboid) {
    pz_path mods;
    pz_path file;
    int ok;

    pz_path_init(&mods);
    pz_path_init(&file);
    ok = pz_path_join(&mods, zomboid, "mods", 4) &&
         pz_path_join(&file, &mods, "default.txt", 11) &&
         pz_str_assign(&target->path, file.text.data, file.text.length) &&
         pz_str_assign_cstr(&target->label, "Default (mods/default.txt)") &&
         read_target(target);
    pz_path_free(&file);
    pz_path_free(&mods);
    return ok;
}

static int add_save_targets(pz_vec* out, const pz_path* zomboid) {
    pz_path saves;
    pz_vec modes;
    pz_fs_error error;
    size_t i;
    size_t first = out->length;

    pz_path_init(&saves);
    pz_vec_init(&modes, sizeof(pz_fs_entry));
    pz_fs_error_clear(&error);
    if (!pz_path_join(&saves, zomboid, "Saves", 5) ||
        !pz_fs_list_dir(&saves, &modes, &error))
        goto done;

    for (i = 0; i < modes.length; ++i) {
        const pz_fs_entry* mode = (const pz_fs_entry*)pz_vec_at_const(&modes, i);
        pz_path mode_dir;
        pz_vec games;
        size_t j;

        if (mode->kind != PZ_FS_DIR) continue;
        pz_path_init(&mode_dir);
        pz_vec_init(&games, sizeof(pz_fs_entry));
        if (pz_path_join(&mode_dir, &saves, mode->name.data, mode->name.length) &&
            pz_fs_list_dir(&mode_dir, &games, &error)) {
            for (j = 0; j < games.length; ++j) {
                const pz_fs_entry* game =
                    (const pz_fs_entry*)pz_vec_at_const(&games, j);
                pz_path game_dir;
                pz_path file;
                pz_path thumb;
                pz_fs_kind thumb_kind;
                pz_activation_target_data* target;

                if (game->kind != PZ_FS_DIR) continue;
                pz_path_init(&game_dir);
                pz_path_init(&file);
                pz_path_init(&thumb);
                if (!pz_path_join(&game_dir, &mode_dir, game->name.data,
                                  game->name.length) ||
                    !pz_path_join(&file, &game_dir, "mods.txt", 8)) {
                    pz_path_free(&thumb);
                    pz_path_free(&file);
                    pz_path_free(&game_dir);
                    continue;
                }
                target = append_target(out);
                if (!target) {
                    pz_path_free(&thumb);
                    pz_path_free(&file);
                    pz_path_free(&game_dir);
                    pz_fs_entries_free(&games);
                    pz_fs_entries_free(&modes);
                    pz_path_free(&saves);
                    return 0;
                }
                target->kind = 1;
                if (!pz_str_assign(&target->name, mode->name.data,
                                   mode->name.length) ||
                    !pz_str_append_char(&target->name, '/') ||
                    !pz_str_append(&target->name, game->name.data,
                                   game->name.length) ||
                    !pz_str_assign_cstr(&target->label, "Save: ") ||
                    !pz_str_append(&target->label, target->name.data,
                                   target->name.length) ||
                    !pz_str_assign(&target->path, file.text.data,
                                   file.text.length)) {
                    drop_last(out);
                    pz_path_free(&thumb);
                    pz_path_free(&file);
                    pz_path_free(&game_dir);
                    continue;
                }
                pz_fs_error_clear(&error);
                if (pz_path_join(&thumb, &game_dir, "thumb.png", 9) &&
                    pz_fs_kind_of(&thumb, &thumb_kind, &error) &&
                    path_kind_is_file(thumb_kind))
                    pz_str_assign(&target->thumbnail, thumb.text.data,
                                  thumb.text.length);
                read_target(target);
                pz_path_free(&thumb);
                pz_path_free(&file);
                pz_path_free(&game_dir);
            }
        }
        pz_fs_entries_free(&games);
        pz_path_free(&mode_dir);
    }
    sort_targets(out, first);

done:
    pz_fs_entries_free(&modes);
    pz_path_free(&saves);
    return 1;
}

static int game_major(const char* version, size_t length) {
    int major = 0;
    int found = 0;
    size_t i;
    for (i = 0; i < length; ++i) {
        const unsigned char c = (unsigned char)version[i];
        if (!isdigit(c)) {
            if (found) break;
            continue;
        }
        found = 1;
        major = major * 10 + (int)(c - '0');
    }
    return found ? major : 0;
}

static int has_ini_extension(const pz_str* name) {
    return name->length >= 4 &&
           memcmp(name->data + name->length - 4, ".ini", 4) == 0;
}

static int add_server_targets(pz_vec* out, const pz_path* zomboid,
                              const char* game_version,
                              size_t game_version_length) {
    pz_path servers;
    pz_vec entries;
    pz_fs_error error;
    size_t i;
    size_t first = out->length;
    const int backslash = game_major(game_version, game_version_length) >= 42;

    pz_path_init(&servers);
    pz_vec_init(&entries, sizeof(pz_fs_entry));
    pz_fs_error_clear(&error);
    if (!pz_path_join(&servers, zomboid, "Server", 6) ||
        !pz_fs_list_dir(&servers, &entries, &error))
        goto done;

    for (i = 0; i < entries.length; ++i) {
        const pz_fs_entry* entry =
            (const pz_fs_entry*)pz_vec_at_const(&entries, i);
        pz_path file;
        pz_activation_target_data* target;
        size_t stem_length;

        if (!path_kind_is_file(entry->kind) ||
            !has_ini_extension(&entry->name))
            continue;
        pz_path_init(&file);
        if (!pz_path_join(&file, &servers, entry->name.data,
                          entry->name.length)) {
            pz_path_free(&file);
            continue;
        }
        target = append_target(out);
        if (!target) {
            pz_path_free(&file);
            pz_fs_entries_free(&entries);
            pz_path_free(&servers);
            return 0;
        }
        stem_length = entry->name.length - 4;
        target->kind = 2;
        target->backslash = backslash;
        if (!pz_str_assign(&target->name, entry->name.data, stem_length) ||
            !pz_str_assign_cstr(&target->label, "Server: ") ||
            !pz_str_append(&target->label, target->name.data,
                           target->name.length) ||
            !pz_str_assign(&target->path, file.text.data, file.text.length)) {
            drop_last(out);
            pz_path_free(&file);
            continue;
        }
        read_target(target);
        pz_path_free(&file);
    }
    sort_targets(out, first);

done:
    pz_fs_entries_free(&entries);
    pz_path_free(&servers);
    return 1;
}

int pz_activation_targets_discover(const char* zomboid_dir,
                                   size_t zomboid_length,
                                   const char* game_version,
                                   size_t game_version_length, pz_vec* out) {
    pz_path zomboid;
    pz_activation_target_data* target;
    size_t i;

    if (!out || (!zomboid_dir && zomboid_length != 0) ||
        (!game_version && game_version_length != 0))
        return 0;
    pz_vec_init(out, sizeof(pz_activation_target_data));
    pz_path_init(&zomboid);
    if (!pz_path_assign(&zomboid, zomboid_dir, zomboid_length)) goto fail;

    target = append_target(out);
    if (!target || !set_default_target(target, &zomboid)) goto fail;
    if (!add_save_targets(out, &zomboid)) goto fail;
    if (!add_server_targets(out, &zomboid, game_version, game_version_length))
        goto fail;
    pz_path_free(&zomboid);
    return 1;

fail:
    for (i = 0; i < out->length; ++i)
        pz_activation_target_data_free(
            (pz_activation_target_data*)pz_vec_at(out, i));
    pz_vec_free(out);
    pz_path_free(&zomboid);
    return 0;
}

int pz_activation_targets_write(pz_activation_target_data* target,
                                const pz_vec* mod_ids,
                                const pz_vec* workshop_ids, int enabled) {
    pz_path path;
    pz_str text;
    pz_str out;
    pz_vec ids;
    pz_vec workshops;
    size_t i;
    int readable;
    int changed = 0;
    int ok = 0;

    if (!target) return 0;
    pz_path_init(&path);
    pz_str_init(&text);
    pz_str_init(&out);
    pz_vec_init(&ids, sizeof(pz_str));
    pz_vec_init(&workshops, sizeof(uint64_t));

    if (!pz_path_assign(&path, target->path.data, target->path.length))
        goto done;
    readable = read_file(&path, &text);
    if (!readable && !enabled) {
        ok = 1;
        goto done;
    }

    if (target->kind == 2) {
        int detected = 0;
        int had_mods = 0;
        int mods_changed = 0;
        int workshops_changed = 0;

        if (!pz_activation_parse_ini_ids(text.data, text.length, &ids,
                                         &detected))
            goto done;
        had_mods = ids.length != 0;
        if (!pz_activation_parse_ini_workshop_ids(text.data, text.length,
                                                  &workshops))
            goto done;

        for (i = 0; mod_ids && i < mod_ids->length; ++i) {
            const pz_str* id = (const pz_str*)pz_vec_at_const(mod_ids, i);
            if (enabled) {
                if (!string_list_add_unique(&ids, id)) goto done;
            } else {
                string_list_remove(&ids, id);
            }
        }
        /* An empty server list inherits the discovered B42 convention. */
        if (had_mods) target->backslash = detected != 0;

        if (!pz_activation_rewrite_ini_mods(text.data, text.length, &ids,
                                            target->backslash, &out,
                                            &mods_changed))
            goto done;

        if (enabled && workshop_ids && workshop_ids->length != 0) {
            for (i = 0; i < workshop_ids->length; ++i) {
                const uint64_t id =
                    *(const uint64_t*)pz_vec_at_const(workshop_ids, i);
                if (!u64_list_add_unique(&workshops, id)) goto done;
            }
            {
                pz_str with_workshops;
                pz_str_init(&with_workshops);
                if (!pz_activation_rewrite_ini_workshop_items(
                        out.data, out.length, &workshops, &with_workshops,
                        &workshops_changed)) {
                    pz_str_free(&with_workshops);
                    goto done;
                }
                pz_str_free(&out);
                out = with_workshops;
            }
        }

        if ((mods_changed || workshops_changed) &&
            !write_file_atomic(&path, &out))
            goto done;
        ok = 1;
    } else {
        if (!pz_activation_parse_block_ids(text.data, text.length, &ids))
            goto done;
        for (i = 0; mod_ids && i < mod_ids->length; ++i) {
            const pz_str* id = (const pz_str*)pz_vec_at_const(mod_ids, i);
            if (enabled) {
                if (!string_list_add_unique(&ids, id)) goto done;
            } else {
                string_list_remove(&ids, id);
            }
        }
        if (!pz_activation_rewrite_block_ids(text.data, text.length, &ids,
                                             &out, &changed))
            goto done;
        if (changed && !write_file_atomic(&path, &out)) goto done;
        ok = 1;
    }

    if (ok) {
        pz_activation_string_list_free(&target->mod_ids);
        target->mod_ids = ids;
        pz_vec_init(&ids, sizeof(pz_str));
        if (target->kind == 2) {
            pz_vec_free(&target->workshop_ids);
            target->workshop_ids = workshops;
            pz_vec_init(&workshops, sizeof(uint64_t));
        }
        target->exists = 1;
    }

done:
    pz_activation_string_list_free(&ids);
    pz_vec_free(&workshops);
    pz_str_free(&out);
    pz_str_free(&text);
    pz_path_free(&path);
    return ok;
}
