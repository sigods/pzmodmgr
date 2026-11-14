#include "core/mod_info_fs_c.h"

#include <string.h>

static int path_copy(pz_path* out, const pz_path* source) {
    return pz_path_assign(out, source->text.data, source->text.length);
}

static int path_join_leaf(pz_path* out, const pz_path* base, const char* leaf) {
    return pz_path_join(out, base, leaf, strlen(leaf));
}

static int path_is_file(const pz_path* path) {
    pz_fs_kind kind;
    return pz_fs_kind_of(path, &kind, NULL) && kind == PZ_FS_FILE;
}

static int path_is_dir(const pz_path* path) {
    pz_fs_kind kind;
    return pz_fs_kind_of(path, &kind, NULL) && kind == PZ_FS_DIR;
}

static int read_all(const pz_path* path, pz_str* out) {
    pz_fs_error error;
    pz_fs_error_clear(&error);
    return pz_fs_read_all(path, out, &error);
}

/* Compare bytes, so a shared prefix sorts before the longer name. */
static int name_greater(const pz_str* left, const pz_str* right) {
    const size_t common = left->length < right->length ? left->length
                                                       : right->length;
    const int compared =
        common != 0 ? memcmp(left->data, right->data, common) : 0;
    if (compared != 0) return compared > 0;
    return left->length > right->length;
}

/* Reads just the version bounds; a missing or unparseable file counts as
 * compatible, matching the old wrapper. */
static int info_supports_game(const pz_path* info_dir, const char* game_version,
                              size_t game_length) {
    pz_path file;
    pz_str text;
    int supported = 1;

    pz_path_init(&file);
    pz_str_init(&text);
    if (path_join_leaf(&file, info_dir, "mod.info") && read_all(&file, &text)) {
        pz_mod_info parsed;
        pz_mod_info_init(&parsed);
        if (pz_mod_info_parse(&parsed, text.data ? text.data : "", text.length))
            supported = pz_mod_info_supports_game_version(
                &parsed, game_version, game_length);
        pz_mod_info_free(&parsed);
    }
    pz_str_free(&text);
    pz_path_free(&file);
    return supported;
}

/* Returns the directory directly containing mod.info. When a game version is
 * given, prefer the newest version folder that supports it; otherwise use the
 * highest version folder. Non-numeric folder names rank -1, as before. */
static int find_info_dir(const pz_path* dir, const char* game_version,
                         size_t game_length, pz_path* out) {
    pz_path direct;
    pz_path_init(&direct);
    if (path_join_leaf(&direct, dir, "mod.info") && path_is_file(&direct)) {
        const int ok = path_copy(out, dir);
        pz_path_free(&direct);
        return ok;
    }
    pz_path_free(&direct);

    pz_vec entries;
    pz_fs_error error;
    pz_vec_init(&entries, sizeof(pz_fs_entry));
    pz_fs_error_clear(&error);
    if (!pz_fs_list_dir(dir, &entries, &error)) {
        pz_vec_free(&entries);
        return 0;
    }

    pz_path best_any;
    pz_path best_compatible;
    pz_str best_any_name;
    pz_str best_compatible_name;
    pz_path_init(&best_any);
    pz_path_init(&best_compatible);
    pz_str_init(&best_any_name);
    pz_str_init(&best_compatible_name);
    int best_any_rank = -1;
    int best_compatible_rank = -1;
    int found_any = 0;
    int found_compatible = 0;

    for (size_t i = 0; i < entries.length; ++i) {
        const pz_fs_entry* entry = pz_vec_at_const(&entries, i);
        if (entry->kind != PZ_FS_DIR) continue;

        pz_path child;
        pz_path child_info;
        pz_path_init(&child);
        pz_path_init(&child_info);
        if (pz_path_join(&child, dir, entry->name.data, entry->name.length) &&
            path_join_leaf(&child_info, &child, "mod.info") &&
            path_is_file(&child_info)) {
            const int rank =
                pz_version_rank(entry->name.data, entry->name.length);

            if (rank > best_any_rank ||
                (rank == best_any_rank &&
                 name_greater(&entry->name, &best_any_name))) {
                path_copy(&best_any, &child);
                pz_str_assign(&best_any_name, entry->name.data,
                              entry->name.length);
                best_any_rank = rank;
                found_any = 1;
            }

            if (info_supports_game(&child, game_version, game_length) &&
                (rank > best_compatible_rank ||
                 (rank == best_compatible_rank &&
                  name_greater(&entry->name, &best_compatible_name)))) {
                path_copy(&best_compatible, &child);
                pz_str_assign(&best_compatible_name, entry->name.data,
                              entry->name.length);
                best_compatible_rank = rank;
                found_compatible = 1;
            }
        }
        pz_path_free(&child_info);
        pz_path_free(&child);
    }

    int ok = 0;
    if (found_compatible)
        ok = path_copy(out, &best_compatible);
    else if (found_any)
        ok = path_copy(out, &best_any);

    pz_str_free(&best_compatible_name);
    pz_str_free(&best_any_name);
    pz_path_free(&best_compatible);
    pz_path_free(&best_any);
    pz_fs_entries_free(&entries);
    return ok;
}

static void resolve_icon(const pz_path* root, const pz_path* info_dir,
                         const pz_str* value, pz_path* out) {
    pz_path bases[3];
    pz_path_init(&bases[0]);
    pz_path_init(&bases[1]);
    pz_path_init(&bases[2]);
    path_copy(&bases[0], info_dir);
    path_copy(&bases[1], root);
    path_join_leaf(&bases[2], root, "common");

    int found = 0;
    if (value && value->length != 0) {
        for (size_t i = 0; i < 3 && !found; ++i) {
            pz_path candidate;
            pz_path_init(&candidate);
            if (pz_path_join(&candidate, &bases[i], value->data,
                             value->length) &&
                path_is_file(&candidate)) {
                path_copy(out, &candidate);
                found = 1;
            }
            pz_path_free(&candidate);
        }
    }

    /* Only the canonical names are implicit. icon_alt.png and similar must not
     * be selected by a wildcard. */
    static const char* const kNames[] = {"icon.png", "poster.png",
                                         "preview.png"};
    for (size_t n = 0; n < 3 && !found; ++n) {
        for (size_t i = 0; i < 3 && !found; ++i) {
            pz_path candidate;
            pz_path_init(&candidate);
            if (path_join_leaf(&candidate, &bases[i], kNames[n]) &&
                path_is_file(&candidate)) {
                path_copy(out, &candidate);
                found = 1;
            }
            pz_path_free(&candidate);
        }
    }

    pz_path_free(&bases[2]);
    pz_path_free(&bases[1]);
    pz_path_free(&bases[0]);
}

int pz_mod_info_read(const pz_path* dir, const char* game_version,
                     size_t game_length, pz_mod_info* out) {
    if (!dir || !out) return 0;

    pz_path info_dir;
    pz_path_init(&info_dir);
    if (!find_info_dir(dir, game_version, game_length, &info_dir)) {
        pz_path_free(&info_dir);
        return 0;
    }

    pz_path info_file;
    pz_str text;
    pz_path_init(&info_file);
    pz_str_init(&text);
    int ok = path_join_leaf(&info_file, &info_dir, "mod.info") &&
             read_all(&info_file, &text);

    pz_mod_info parsed;
    pz_mod_info_init(&parsed);
    if (ok)
        ok = pz_mod_info_parse(&parsed, text.data ? text.data : "",
                               text.length);
    if (!ok) {
        pz_mod_info_free(&parsed);
        pz_str_free(&text);
        pz_path_free(&info_file);
        pz_path_free(&info_dir);
        return 0;
    }

    pz_path icon;
    pz_path_init(&icon);
    resolve_icon(dir, &info_dir, &parsed.icon, &icon);
    pz_str_free(&parsed.icon);
    if (icon.text.length != 0)
        pz_str_assign(&parsed.icon, icon.text.data, icon.text.length);

    pz_mod_info_free(out);
    *out = parsed;

    pz_path_free(&icon);
    pz_str_free(&text);
    pz_path_free(&info_file);
    pz_path_free(&info_dir);
    return 1;
}

int pz_mod_info_read_any(const pz_path* dir, pz_mod_info* out) {
    return pz_mod_info_read(dir, "", 0, out);
}

static int push_path(pz_vec* out, const pz_path* path) {
    pz_str* slot = pz_vec_push(out);
    if (!slot) return 0;
    pz_str_init(slot);
    if (!pz_str_assign(slot, path->text.data, path->text.length)) {
        pz_str_free(slot);
        --out->length;
        return 0;
    }
    return 1;
}

int pz_discover_mod_roots(const pz_path* content, pz_vec* out) {
    pz_vec_init(out, sizeof(pz_str));
    if (!content) return 0;

    pz_path info_dir;
    pz_path_init(&info_dir);
    if (find_info_dir(content, "", 0, &info_dir)) {
        push_path(out, content);
        pz_path_free(&info_dir);
        return 1;
    }
    pz_path_free(&info_dir);

    pz_path bases[2];
    pz_path_init(&bases[0]);
    pz_path_init(&bases[1]);
    path_join_leaf(&bases[0], content, "mods");
    pz_path common;
    pz_path_init(&common);
    path_join_leaf(&common, content, "Contents");
    path_join_leaf(&bases[1], &common, "mods");
    pz_path_free(&common);

    for (size_t b = 0; b < 2; ++b) {
        if (!path_is_dir(&bases[b])) continue;

        pz_vec entries;
        pz_fs_error error;
        pz_vec_init(&entries, sizeof(pz_fs_entry));
        pz_fs_error_clear(&error);
        if (!pz_fs_list_dir(&bases[b], &entries, &error)) {
            pz_vec_free(&entries);
            continue;
        }

        for (size_t i = 0; i < entries.length; ++i) {
            const pz_fs_entry* entry = pz_vec_at_const(&entries, i);
            if (entry->kind != PZ_FS_DIR) continue;
            pz_path child;
            pz_path child_info;
            pz_path_init(&child);
            pz_path_init(&child_info);
            if (pz_path_join(&child, &bases[b], entry->name.data,
                             entry->name.length) &&
                find_info_dir(&child, "", 0, &child_info))
                push_path(out, &child);
            pz_path_free(&child_info);
            pz_path_free(&child);
        }
        pz_fs_entries_free(&entries);
    }

    pz_path_free(&bases[1]);
    pz_path_free(&bases[0]);
    return 1;
}
