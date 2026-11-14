#ifndef PZ_CORE_FS_C_H
#define PZ_CORE_FS_C_H

/* C99 filesystem contract for the C migration.
 *
 * Paths are UTF-8 byte strings on both platforms, always stored in a pz_path.
 * Windows converts to UTF-16 inside fs_win.c and never leaks wchar_t through
 * this header. The core, app, UIs, and tests all use this API directly.
 *
 * Conventions:
 *  - Functions that can fail return 1 on success and 0 on failure, filling
 *    `error` when it is non-NULL. Query functions return 1 when the answer is
 *    known and 0 on failure, with the value through an out-parameter.
 *  - Every `_init` must be paired with the matching `_free`. No function
 *    keeps a borrowed pointer after it returns.
 *  - All functions are reentrant. There is no global state, no cached wide
 *    path, and no hidden allocation on error paths.
 *  - Listing order is not an OS property: the API sorts entries by name.
 *
 * Design decisions and the questions they answer are in docs/fs-c-api.md.
 */

#include "core/c_util.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ paths */

typedef struct pz_path {
    pz_str text; /* UTF-8, no trailing separator except for a root */
} pz_path;

void pz_path_init(pz_path* path);
void pz_path_free(pz_path* path);
int pz_path_assign(pz_path* path, const char* utf8, size_t length);
int pz_path_assign_cstr(pz_path* path, const char* text);

/* Joins base and leaf with the platform separator. An absolute leaf replaces
 * base, matching pz::native_fs::path::operator/=. */
int pz_path_join(pz_path* out, const pz_path* base, const char* leaf,
                 size_t leaf_length);
int pz_path_join_path(pz_path* out, const pz_path* base, const pz_path* leaf);

int pz_path_parent(const pz_path* path, pz_path* out);
/* Writes the last path component to out (a pz_str, not a path). */
int pz_path_filename(const pz_path* path, pz_str* out);
/* Collapses "." and "..", repeated separators, and a trailing separator
 * without touching the filesystem. Preserves a drive prefix, a leading
 * separator, and a UNC prefix. */
int pz_path_lexically_normal(const pz_path* path, pz_path* out);
int pz_path_is_absolute(const pz_path* path);
char pz_path_separator(void);

/* ----------------------------------------------------------------- errors */

typedef enum pz_fs_code {
    PZ_FS_OK = 0,
    PZ_FS_NOT_FOUND,     /* missing path or component */
    PZ_FS_EXISTS,        /* destination already exists */
    PZ_FS_ACCESS,        /* permission denied */
    PZ_FS_BUSY,          /* open handle, sharing violation, or read-only media */
    PZ_FS_CROSS_DEVICE,  /* rename between volumes/filesystems */
    PZ_FS_NOT_EMPTY,     /* non-recursive operation on a non-empty directory */
    PZ_FS_INVALID,       /* bad name, unsupported type, or link policy */
    PZ_FS_TOO_LONG,      /* name or path exceeds the platform limit */
    PZ_FS_NO_SPACE,
    PZ_FS_IO,
    PZ_FS_SYSTEM        /* unclassified; system_code holds errno/GetLastError */
} pz_fs_code;

typedef struct pz_fs_error {
    pz_fs_code code;
    int system_code; /* errno on POSIX, GetLastError on Windows; 0 if unused */
} pz_fs_error;

void pz_fs_error_clear(pz_fs_error* error);
int pz_fs_error_is_set(const pz_fs_error* error);
const char* pz_fs_code_name(pz_fs_code code);
/* "not found", or "access denied (system 13)" when a raw code is available. */
int pz_fs_error_format(const pz_fs_error* error, pz_str* out);

/* --------------------------------------------------------------- queries */

typedef enum pz_fs_kind {
    PZ_FS_UNKNOWN = 0,
    PZ_FS_FILE,
    PZ_FS_DIR,
    PZ_FS_LINK /* symlink, junction, or other reparse point: never followed */
} pz_fs_kind;

int pz_fs_exists(const pz_path* path, int* exists, pz_fs_error* error);
int pz_fs_kind_of(const pz_path* path, pz_fs_kind* kind, pz_fs_error* error);
int pz_fs_file_size(const pz_path* path, uint64_t* size, pz_fs_error* error);
int pz_fs_is_empty(const pz_path* path, int* empty, pz_fs_error* error);
/* True when path is a regular file and, on POSIX, the caller may execute it.
 * Follows a link to a file; a missing path fails with PZ_FS_NOT_FOUND. */
int pz_fs_is_executable(const pz_path* path, int* executable,
                        pz_fs_error* error);

/* ------------------------------------------------------------ operations */

int pz_fs_create_directories(const pz_path* path, pz_fs_error* error);
/* Removes a file, a link, or an empty directory. */
int pz_fs_remove(const pz_path* path, pz_fs_error* error);
/* Recursively removes path. Links and junctions are unlinked, never followed.
 * `removed`, when non-NULL, receives the number of entries deleted. */
int pz_fs_remove_all(const pz_path* path, uint64_t* removed, pz_fs_error* error);
int pz_fs_rename(const pz_path* from, const pz_path* to, pz_fs_error* error);

enum {
    PZ_FS_COPY_RECURSIVE = 1u << 0,  /* required to copy a directory */
    PZ_FS_COPY_OVERWRITE = 1u << 1,  /* replace an existing destination */
    PZ_FS_COPY_FOLLOW_LINKS = 1u << 2 /* copy link targets (depth-capped) */
};

/* Copies one file. Follows a link to a file; does not create the
 * destination's parent directory. */
int pz_fs_copy_file(const pz_path* from, const pz_path* to, unsigned flags,
                    pz_fs_error* error);
/* Copies `from` to `to`. With PZ_FS_COPY_RECURSIVE it creates `to` and the
 * subtree below it; it does not create `to`'s parent. Existing destinations are
 * kept unless PZ_FS_COPY_OVERWRITE is set. Links are copied as links when it is
 * possible (POSIX) and fail with PZ_FS_INVALID on Windows; with
 * PZ_FS_COPY_FOLLOW_LINKS a link to a file is copied by content, while a link
 * to a directory is refused so a copy can never escape the tree. Directory
 * recursion is capped at 32 levels. A file/directory collision fails with
 * PZ_FS_INVALID. */
int pz_fs_copy_tree(const pz_path* from, const pz_path* to, unsigned flags,
                    pz_fs_error* error);
/* Atomically renames an already-written temporary over `to`; consumes `from`.
 * Both paths must be on the same filesystem. */
int pz_fs_replace_file(const pz_path* from, const pz_path* to,
                       pz_fs_error* error);

/* Marks a file executable on POSIX; no-op success on Windows. */
int pz_fs_set_executable(const pz_path* path, int enabled, pz_fs_error* error);

/* ------------------------------------------------------------ directory */

typedef struct pz_fs_entry {
    pz_str name;   /* leaf name, UTF-8, owned */
    pz_fs_kind kind;
    int error;     /* non-zero when the entry could not be inspected */
} pz_fs_entry;

/* Fills out (item_size sizeof(pz_fs_entry)) with the directory's entries,
 * sorted by name. `out` must have been initialized with pz_vec_init; its
 * existing contents are replaced. Fails only when the directory itself cannot
 * be opened. */
int pz_fs_list_dir(const pz_path* dir, pz_vec* out, pz_fs_error* error);
void pz_fs_entries_free(pz_vec* entries);

/* ------------------------------------------------------------- timestamps */

/* Seconds since the Unix epoch, UTC. Precision is one second. */
int pz_fs_file_time_now(int64_t* seconds);
int pz_fs_last_write_time(const pz_path* path, int64_t* seconds,
                          pz_fs_error* error);
/* Requires an existing path; does not create the file. */
int pz_fs_set_last_write_time(const pz_path* path, int64_t seconds,
                              pz_fs_error* error);

/* ------------------------------------------------------------ processes */

/* Absolute path of the running executable. */
int pz_fs_executable_path(pz_path* out, pz_fs_error* error);
int pz_fs_current_dir(pz_path* out, pz_fs_error* error);
int pz_fs_temp_dir(pz_path* out, pz_fs_error* error);

/* ----------------------------------------------------------- file handles */

/* Opaque so the implementation can use FILE*, HANDLE, or fd without changing
 * the ABI. The text module never converts paths itself. */
typedef struct pz_fs_file pz_fs_file;

int pz_fs_open_read(const pz_path* path, pz_fs_file** file, pz_fs_error* error);
int pz_fs_open_write(const pz_path* path, pz_fs_file** file,
                     pz_fs_error* error);
/* Opens for appending, creating the file when it does not exist. */
int pz_fs_open_append(const pz_path* path, pz_fs_file** file,
                      pz_fs_error* error);
/* Returns the number of bytes read; 0 means end of file or error. */
size_t pz_fs_read(pz_fs_file* file, void* buffer, size_t size);
size_t pz_fs_write(pz_fs_file* file, const void* buffer, size_t size);
pz_fs_code pz_fs_file_error(const pz_fs_file* file);
/* Flushes, closes, and frees the handle even when it reports a failure. */
int pz_fs_close(pz_fs_file* file, pz_fs_error* error);
/* Reads a whole file into out (replacing it). */
int pz_fs_read_all(const pz_path* path, pz_str* out, pz_fs_error* error);

#ifdef __cplusplus
}
#endif

#endif
