#include "core/fs_c.h"

#include "core/fs_c_internal.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ paths */

int pz_fs_separator_is(char value) {
#ifdef _WIN32
    return value == '\\' || value == '/';
#else
    return value == '/';
#endif
}

char pz_path_separator(void) {
#ifdef _WIN32
    return '\\';
#else
    return '/';
#endif
}

void pz_path_init(pz_path* path) {
    if (path) pz_str_init(&path->text);
}

void pz_path_free(pz_path* path) {
    if (path) pz_str_free(&path->text);
}

int pz_path_assign(pz_path* path, const char* utf8, size_t length) {
    if (!path) return 0;
    return pz_str_assign(&path->text, utf8, length);
}

int pz_path_assign_cstr(pz_path* path, const char* text) {
    if (!path || !text) return 0;
    return pz_str_assign_cstr(&path->text, text);
}

int pz_path_is_absolute(const pz_path* path) {
    const char* s;
    size_t n;

    if (!path || !path->text.data) return 0;
    s = path->text.data;
    n = path->text.length;
#ifdef _WIN32
    if (n >= 3 && ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z')) &&
        s[1] == ':' && pz_fs_separator_is(s[2]))
        return 1;
    if (n >= 2 && pz_fs_separator_is(s[0]) && pz_fs_separator_is(s[1])) return 1;
    return 0;
#else
    return n >= 1 && s[0] == '/';
#endif
}

static int path_text_is_absolute(const char* text, size_t length) {
#ifdef _WIN32
    if (length >= 3 &&
        ((text[0] >= 'A' && text[0] <= 'Z') ||
         (text[0] >= 'a' && text[0] <= 'z')) &&
        text[1] == ':' && pz_fs_separator_is(text[2]))
        return 1;
    if (length >= 2 && pz_fs_separator_is(text[0]) &&
        pz_fs_separator_is(text[1]))
        return 1;
    if (length != 0 && pz_fs_separator_is(text[0])) return 1;
    return 0;
#else
    return length != 0 && text[0] == '/';
#endif
}

int pz_path_join(pz_path* out, const pz_path* base, const char* leaf,
                 size_t leaf_length) {
    pz_str joined;
    int ok;

    if (!out || !base || (!leaf && leaf_length != 0)) return 0;
    if (leaf_length == 0)
        return pz_str_assign(&out->text, base->text.data, base->text.length);
    pz_str_init(&joined);

    if (path_text_is_absolute(leaf, leaf_length) || base->text.length == 0) {
        ok = pz_str_assign(&joined, leaf, leaf_length);
    } else {
        ok = pz_str_assign(&joined, base->text.data, base->text.length);
        if (ok && !pz_fs_separator_is(joined.data[joined.length - 1]))
            ok = pz_str_append_char(&joined, pz_path_separator());
        if (ok) ok = pz_str_append(&joined, leaf, leaf_length);
    }

    if (ok) ok = pz_str_assign(&out->text, joined.data, joined.length);
    pz_str_free(&joined);
    return ok;
}

int pz_path_join_path(pz_path* out, const pz_path* base, const pz_path* leaf) {
    if (!leaf) return 0;
    return pz_path_join(out, base, leaf->text.data, leaf->text.length);
}

int pz_path_parent(const pz_path* path, pz_path* out) {
    const char* s;
    size_t end;
    size_t original_end;
    size_t i;

    if (!path || !out) return 0;
    s = path->text.data;
    end = path->text.length;
    original_end = end;
    if (!s || end == 0) return pz_str_assign(&out->text, "", 0);

    while (end > 1 && pz_fs_separator_is(s[end - 1])) --end;

#ifdef _WIN32
    if (end >= 2 && s[1] == ':' &&
        ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z'))) {
        if (end == 2 && original_end >= 3 && pz_fs_separator_is(s[2]))
            return pz_str_assign(&out->text, s, 3);
        if (end == 2) return pz_str_assign(&out->text, s, 2);
        if (end == 3 && pz_fs_separator_is(s[2]))
            return pz_str_assign(&out->text, s, 3);
        i = end;
        while (i > 3 && !pz_fs_separator_is(s[i - 1])) --i;
        if (i == 3) return pz_str_assign(&out->text, s, 3);
        return pz_str_assign(&out->text, s, i - 1);
    }
    if (end >= 2 && pz_fs_separator_is(s[0]) && pz_fs_separator_is(s[1])) {
        size_t server_end = 2;
        while (server_end < end && !pz_fs_separator_is(s[server_end]))
            ++server_end;
        if (server_end >= end) return pz_str_assign(&out->text, "", 0);
        if (server_end + 1 >= end)
            return pz_str_assign(&out->text, s, server_end + 1);
        i = end;
        while (i > server_end + 1 && !pz_fs_separator_is(s[i - 1])) --i;
        if (i == server_end + 1) return pz_str_assign(&out->text, s, server_end + 1);
        return pz_str_assign(&out->text, s, i - 1);
    }
    if (pz_fs_separator_is(s[0])) {
        if (end == 1) return pz_str_assign(&out->text, s, 1);
        i = end;
        while (i > 1 && !pz_fs_separator_is(s[i - 1])) --i;
        if (i == 1) return pz_str_assign(&out->text, s, 1);
        return pz_str_assign(&out->text, s, i - 1);
    }
#endif

    i = end;
    while (i > 0 && !pz_fs_separator_is(s[i - 1])) --i;
    if (i == 0) return pz_str_assign(&out->text, "", 0);
    return pz_str_assign(&out->text, s, i - 1);
}

int pz_path_filename(const pz_path* path, pz_str* out) {
    const char* s;
    size_t end;
    size_t i;

    if (!path || !out) return 0;
    s = path->text.data;
    end = path->text.length;
    if (!s || end == 0) return pz_str_assign(out, "", 0);

    while (end > 1 && pz_fs_separator_is(s[end - 1])) --end;
#ifdef _WIN32
    if (end == 2 && s[1] == ':') return pz_str_assign(out, "", 0);
#endif
    i = end;
    while (i > 0 && !pz_fs_separator_is(s[i - 1])) --i;
    return pz_str_assign(out, s + i, end - i);
}

int pz_path_lexically_normal(const pz_path* path, pz_path* out) {
    const char* s;
    size_t n;
    size_t i = 0;
    pz_str prefix;
    pz_vec parts;
    pz_str result;
    int rooted = 0;
    int ok = 1;
    size_t p;

    if (!path || !out) return 0;
    s = path->text.data;
    n = path->text.length;
    pz_str_init(&prefix);
    pz_str_init(&result);
    pz_vec_init(&parts, sizeof(pz_str));

    if (!s || n == 0) {
        ok = pz_str_assign(&out->text, "", 0);
        goto done;
    }

#ifdef _WIN32
    if (n >= 2 && ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z')) &&
        s[1] == ':') {
        if (n >= 3 && pz_fs_separator_is(s[2])) {
            ok = pz_str_assign(&prefix, s, 2);
            if (ok) ok = pz_str_append_char(&prefix, pz_path_separator());
            i = 3;
            rooted = 1;
        }
        /* A drive-relative path such as C:foo has no root; the whole text is
         * parsed as relative components, matching native_fs::path. */
    } else if (n >= 2 && pz_fs_separator_is(s[0]) && pz_fs_separator_is(s[1])) {
        size_t server_end = 2;
        while (server_end < n && !pz_fs_separator_is(s[server_end]))
            ++server_end;
        if (server_end < n) {
            ok = pz_str_assign(&prefix, s, server_end + 1);
            i = server_end + 1;
        } else {
            ok = pz_str_assign(&prefix, "//", 2);
            i = 2;
        }
        rooted = 1;
    } else
#endif
    if (pz_fs_separator_is(s[0])) {
        ok = pz_str_assign(&prefix, s, 1);
        i = 1;
        rooted = 1;
    }
    if (!ok) goto done;

    while (i < n) {
        size_t start;
        size_t length;

        while (i < n && pz_fs_separator_is(s[i])) ++i;
        start = i;
        while (i < n && !pz_fs_separator_is(s[i])) ++i;
        length = i - start;
        if (length == 0) continue;

        if (length == 1 && s[start] == '.') continue;
        if (length == 2 && s[start] == '.' && s[start + 1] == '.') {
            int removed = 0;

            if (parts.length != 0) {
                pz_str* last = (pz_str*)pz_vec_at(&parts, parts.length - 1);
                if (!(last->length == 2 && last->data[0] == '.' &&
                      last->data[1] == '.')) {
                    pz_str_free(last);
                    --parts.length;
                    removed = 1;
                }
            }
            if (!removed && !rooted) {
                pz_str* slot = (pz_str*)pz_vec_push(&parts);
                if (!slot) { ok = 0; break; }
                pz_str_init(slot);
                if (!pz_str_assign(slot, "..", 2)) { ok = 0; break; }
            }
            continue;
        }

        {
            pz_str* slot = (pz_str*)pz_vec_push(&parts);
            if (!slot) { ok = 0; break; }
            pz_str_init(slot);
            if (!pz_str_assign(slot, s + start, length)) { ok = 0; break; }
        }
    }

    if (ok) {
        ok = pz_str_assign(&result, prefix.data, prefix.length);
        for (p = 0; ok && p < parts.length; ++p) {
            const pz_str* part = (const pz_str*)pz_vec_at_const(&parts, p);
            const int drive_relative = p == 0 && !rooted &&
                result.length == 2 && result.data[1] == ':';
            if (result.length != 0 && !drive_relative &&
                !pz_fs_separator_is(result.data[result.length - 1]))
                ok = pz_str_append_char(&result, pz_path_separator());
            if (ok) ok = pz_str_append(&result, part->data, part->length);
        }
        if (ok && result.length == 0) ok = pz_str_assign(&result, ".", 1);
        if (ok) ok = pz_str_assign(&out->text, result.data, result.length);
    }

done:
    for (p = 0; p < parts.length; ++p) {
        pz_str* part = (pz_str*)pz_vec_at(&parts, p);
        pz_str_free(part);
    }
    pz_vec_free(&parts);
    pz_str_free(&prefix);
    pz_str_free(&result);
    return ok;
}

/* ----------------------------------------------------------------- errors */

void pz_fs_error_set(pz_fs_error* error, pz_fs_code code, int system_code) {
    if (!error) return;
    error->code = code;
    error->system_code = system_code;
}

void pz_fs_error_clear(pz_fs_error* error) {
    pz_fs_error_set(error, PZ_FS_OK, 0);
}

int pz_fs_error_is_set(const pz_fs_error* error) {
    return error && error->code != PZ_FS_OK;
}

const char* pz_fs_code_name(pz_fs_code code) {
    switch (code) {
    case PZ_FS_OK: return "ok";
    case PZ_FS_NOT_FOUND: return "not found";
    case PZ_FS_EXISTS: return "already exists";
    case PZ_FS_ACCESS: return "access denied";
    case PZ_FS_BUSY: return "busy";
    case PZ_FS_CROSS_DEVICE: return "cross-device link";
    case PZ_FS_NOT_EMPTY: return "directory not empty";
    case PZ_FS_INVALID: return "invalid path or type";
    case PZ_FS_TOO_LONG: return "path too long";
    case PZ_FS_NO_SPACE: return "no space left";
    case PZ_FS_IO: return "I/O error";
    case PZ_FS_SYSTEM: return "system error";
    }
    return "unknown error";
}

int pz_fs_error_format(const pz_fs_error* error, pz_str* out) {
    int ok;

    if (!out) return 0;
    if (!error || error->code == PZ_FS_OK) return pz_str_assign(out, "ok", 2);
    ok = pz_str_assign_cstr(out, pz_fs_code_name(error->code));
    if (ok && error->system_code != 0) {
        ok = pz_str_append_cstr(out, " (system ");
        if (ok) ok = pz_str_append_i64(out, error->system_code);
        if (ok) ok = pz_str_append_char(out, ')');
    }
    return ok;
}

/* --------------------------------------------------------------- entries */

int pz_fs_entries_add(pz_vec* entries, const char* name, size_t length,
                      pz_fs_kind kind) {
    pz_fs_entry* entry;

    if (!entries) return 0;
    entry = (pz_fs_entry*)pz_vec_push(entries);
    if (!entry) return 0;
    pz_str_init(&entry->name);
    entry->kind = kind;
    entry->error = 0;
    if (!pz_str_assign(&entry->name, name, length)) {
        pz_str_free(&entry->name);
        --entries->length;
        return 0;
    }
    return 1;
}

static int compare_entries(const void* left, const void* right) {
    const pz_fs_entry* a = (const pz_fs_entry*)left;
    const pz_fs_entry* b = (const pz_fs_entry*)right;
    const size_t shorter = a->name.length < b->name.length ? a->name.length
                                                           : b->name.length;
    const int compared = shorter ? memcmp(a->name.data, b->name.data, shorter) : 0;

    if (compared != 0) return compared;
    if (a->name.length < b->name.length) return -1;
    if (a->name.length > b->name.length) return 1;
    return 0;
}

void pz_fs_entries_sort(pz_vec* entries) {
    if (!entries || entries->length < 2) return;
    qsort(entries->data, entries->length, entries->item_size, compare_entries);
}

void pz_fs_entries_free(pz_vec* entries) {
    size_t i;

    if (!entries) return;
    for (i = 0; i < entries->length; ++i) {
        pz_fs_entry* entry = (pz_fs_entry*)pz_vec_at(entries, i);
        pz_str_free(&entry->name);
    }
    pz_vec_free(entries);
}

/* ------------------------------------------------------------ operations */

static int remove_all_inner(const pz_path* path, uint64_t* removed,
                            pz_fs_error* error) {
    pz_fs_kind kind;
    pz_fs_error local;

    pz_fs_error_clear(&local);
    if (!pz_fs_kind_of(path, &kind, &local)) {
        if (local.code == PZ_FS_NOT_FOUND) return 1;
        if (error) *error = local;
        return 0;
    }

    if (kind == PZ_FS_DIR) {
        pz_vec entries;
        size_t i;
        int ok = 1;

        pz_vec_init(&entries, sizeof(pz_fs_entry));
        if (!pz_fs_list_dir(path, &entries, error)) return 0;
        for (i = 0; i < entries.length && ok; ++i) {
            const pz_fs_entry* entry = (const pz_fs_entry*)pz_vec_at_const(&entries, i);
            pz_path child;

            if (entry->error) continue;
            pz_path_init(&child);
            ok = pz_path_join(&child, path, entry->name.data, entry->name.length) &&
                 remove_all_inner(&child, removed, error);
            pz_path_free(&child);
        }
        pz_fs_entries_free(&entries);
        if (!ok) return 0;
    }

    if (!pz_fs_remove(path, error)) return 0;
    if (removed) ++*removed;
    return 1;
}

int pz_fs_remove_all(const pz_path* path, uint64_t* removed, pz_fs_error* error) {
    if (removed) *removed = 0;
    if (!path) {
        pz_fs_error_set(error, PZ_FS_INVALID, 0);
        return 0;
    }
    return remove_all_inner(path, removed, error);
}

static int copy_tree_inner(const pz_path* from, const pz_path* to,
                           unsigned flags, int depth, pz_fs_error* error) {
    pz_fs_kind kind;
    pz_fs_error local;

    pz_fs_error_clear(&local);
    if (!pz_fs_kind_of(from, &kind, &local)) {
        if (error) *error = local;
        return 0;
    }

    if (kind == PZ_FS_LINK) {
        if (flags & PZ_FS_COPY_FOLLOW_LINKS)
            return pz_fs_copy_file(from, to, flags, error);
        return pz_fs_copy_link(from, to, flags, error);
    }
    if (kind == PZ_FS_FILE) return pz_fs_copy_file(from, to, flags, error);
    if (kind != PZ_FS_DIR) {
        pz_fs_error_set(error, PZ_FS_INVALID, 0);
        return 0;
    }

    if (!(flags & PZ_FS_COPY_RECURSIVE)) {
        pz_fs_error_set(error, PZ_FS_INVALID, 0);
        return 0;
    }
    if (depth >= 32) {
        pz_fs_error_set(error, PZ_FS_INVALID, 0);
        return 0;
    }

    {
        pz_fs_kind target_kind;
        pz_fs_error target_error;

        pz_fs_error_clear(&target_error);
        if (pz_fs_kind_of(to, &target_kind, &target_error)) {
            if (target_kind != PZ_FS_DIR) {
                pz_fs_error_set(error, PZ_FS_INVALID, 0);
                return 0;
            }
        } else if (target_error.code != PZ_FS_NOT_FOUND) {
            if (error) *error = target_error;
            return 0;
        }
    }

    if (!pz_fs_create_directories(to, error)) return 0;

    {
        pz_vec entries;
        size_t i;
        int ok = 1;

        pz_vec_init(&entries, sizeof(pz_fs_entry));
        if (!pz_fs_list_dir(from, &entries, error)) return 0;
        for (i = 0; i < entries.length && ok; ++i) {
            const pz_fs_entry* entry = (const pz_fs_entry*)pz_vec_at_const(&entries, i);
            pz_path child_from;
            pz_path child_to;

            if (entry->error) continue;
            pz_path_init(&child_from);
            pz_path_init(&child_to);
            ok = pz_path_join(&child_from, from, entry->name.data, entry->name.length) &&
                 pz_path_join(&child_to, to, entry->name.data, entry->name.length) &&
                 copy_tree_inner(&child_from, &child_to, flags, depth + 1, error);
            pz_path_free(&child_from);
            pz_path_free(&child_to);
        }
        pz_fs_entries_free(&entries);
        return ok;
    }
}

int pz_fs_copy_tree(const pz_path* from, const pz_path* to, unsigned flags,
                    pz_fs_error* error) {
    if (!from || !to) {
        pz_fs_error_set(error, PZ_FS_INVALID, 0);
        return 0;
    }
    return copy_tree_inner(from, to, flags, 0, error);
}

/* ------------------------------------------------------------- timestamps */

int pz_fs_file_time_now(int64_t* seconds) {
    const time_t now = time(NULL);

    if (!seconds || now == (time_t)-1) return 0;
    *seconds = (int64_t)now;
    return 1;
}

/* ----------------------------------------------------------- file handles */

int pz_fs_read_all(const pz_path* path, pz_str* out, pz_fs_error* error) {
    pz_fs_file* file = NULL;
    char buffer[4096];
    int ok = 1;

    if (!out) return 0;
    pz_str_clear(out);
    if (!pz_fs_open_read(path, &file, error)) return 0;

    for (;;) {
        const size_t got = pz_fs_read(file, buffer, sizeof buffer);
        if (got == 0) break;
        if (!pz_str_append(out, buffer, got)) {
            pz_fs_error_set(error, PZ_FS_NO_SPACE, 0);
            ok = 0;
            break;
        }
    }
    if (ok && pz_fs_file_error(file) != PZ_FS_OK) {
        pz_fs_error_set(error, pz_fs_file_error(file), 0);
        ok = 0;
    }
    if (!pz_fs_close(file, error)) ok = 0;
    return ok;
}
