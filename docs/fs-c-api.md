# C filesystem API

Status: **implemented** on `c99/leaf-modules`. `src/core/fs_c.{h,c}`,
`src/core/fs_posix.c`, and `src/core/fs_win.c` build the `pzfs` static
library, and `tests/test_c_fs.c` runs against both backends (Linux natively,
the Win32 backend under Wine). `pzfs` is linked into the app and
`mod_manager_c.c` uses it for every filesystem operation. `native_fs` remains
as the C++ path type for the UI and the not-yet-ported `mod.info`/activation
wrappers; `fs_posix.cpp` / `fs_win.cpp` are unchanged.

Layering:

- `fs_c.c` (platform-neutral): `pz_path` helpers, normalization, error names and
  formatting, entry sorting and cleanup, copy-tree walk.
- `fs_posix.c`: `pz_fs_*` operations with POSIX calls.
- `fs_win.c`: the same operations with Win32 wide calls and the UTF-8→UTF-16
  conversion at the boundary.

Nothing above this layer sees `wchar_t`, `errno`, or `DWORD`.

## Decisions

### 1. Directory iteration is a sorted snapshot

`pz_fs_list_dir` fills a `pz_vec` of `pz_fs_entry` and returns. There is no
open/next/close handle and no borrowed pointer.

Why: `native_fs::directory_iterator` already preloads a vector, so this keeps
behaviour identical on both platforms, gives each entry one owner
(`pz_fs_entries_free`), and never holds an OS handle across UI or worker work. A
`mods/` folder holds hundreds of entries, not millions; the listing already
allocates today. A lazy iterator can be added later behind the same name if
profiling ever demands it.

### 2. The API sorts entries by name

Entries are ordered by byte-wise `pz_str` comparison of the leaf name, ascending.
Callers no longer depend on `readdir`/`FindFirstFile` order.

Why: `list_all()` and `missing_requirements()` show records in directory order,
which differs between NTFS, ext4, and Wine. Sorting once in the snapshot makes
the UI and tests deterministic and lets the `ModManager` port drop its
`std::sort` for discovery. The cost is O(n log n) over a few hundred short
strings.

### 3. A bad entry does not fail the listing

`pz_fs_list_dir` fails only when the directory itself cannot be opened. An entry
that disappears or cannot be inspected during the scan is still returned, with
`kind = PZ_FS_UNKNOWN` and `entry.error` set.

Why: SteamCMD writes into its content folder while the manager reads it, so a
vanishing entry is normal. Aborting the listing would turn a race into a failed
import.

### 4. Recursive copy has explicit flags

`pz_fs_copy_tree(from, to, flags, error)`:

- `PZ_FS_COPY_RECURSIVE` is required to copy a directory; without it a directory
  fails with `PZ_FS_INVALID`.
- It creates `to` and everything below it, but **not** `to`'s parent. The
  current caller already does that explicitly (`mod_manager.cpp:38`), and keeping
  it explicit prevents a typo from creating a tree outside the Zomboid folder.
- Existing destinations are kept unless `PZ_FS_COPY_OVERWRITE` is set. Without
  it a collision fails with `PZ_FS_EXISTS`, not a partial copy.
- A file/directory collision fails with `PZ_FS_INVALID`.
- On POSIX the executable bit is preserved. Timestamps and the remaining mode
  bits are not: copied mods do not rely on them, and the manager's staleness
  checks only cover its own cache directories.
- On Windows, `MoveFileEx`/`CopyFile` attributes are mapped the same way: the
  read-only attribute is preserved, ACLs and alternate streams are not copied.

### 5. Links are never followed implicitly

`pz_fs_kind_of` reports symlinks, junctions, and any other reparse point as
`PZ_FS_LINK`. `pz_fs_kind_of` does not follow them; `is_directory` on a junction
is false at this layer, so callers that want the target ask explicitly.

- `pz_fs_remove_all` unlinks a link and never recurses into it.
- `pz_fs_copy_tree` copies a link as a link on POSIX; on Windows that path
  fails with `PZ_FS_INVALID` because creating symbolic links needs a privilege
  the manager must not assume. With `PZ_FS_COPY_FOLLOW_LINKS` a link to a file
  is copied by content and a link to a directory is refused, so a copy cannot
  climb out of the tree. Directory recursion is capped at 32 levels.
- `pz_fs_copy_file` on a link copies the target contents (it is a file copy).

Why: the mods folder is user-writable. A junction planted there must not let
`remove_all` or a copy walk outside it. `AGENTS.md` already forbids deleting
folders the manager did not install; this is the mechanism that enforces it.

### 6. One portable error enum

`pz_fs_error { pz_fs_code code; int system_code; }`. Call sites branch on
`code`; `system_code` exists only for diagnostics and `pz_fs_error_format`.

| `pz_fs_code` | POSIX | Windows |
| --- | --- | --- |
| `PZ_FS_NOT_FOUND` | `ENOENT`, `ENOTDIR` | `ERROR_FILE_NOT_FOUND`, `ERROR_PATH_NOT_FOUND` |
| `PZ_FS_EXISTS` | `EEXIST` | `ERROR_ALREADY_EXISTS`, `ERROR_FILE_EXISTS` |
| `PZ_FS_ACCESS` | `EACCES`, `EPERM`, `EROFS` | `ERROR_ACCESS_DENIED`, `ERROR_PRIVILEGE_NOT_HELD` |
| `PZ_FS_BUSY` | `EBUSY`, `ETXTBSY` | `ERROR_SHARING_VIOLATION`, `ERROR_LOCK_VIOLATION` |
| `PZ_FS_CROSS_DEVICE` | `EXDEV` | `ERROR_NOT_SAME_DEVICE` |
| `PZ_FS_NOT_EMPTY` | `ENOTEMPTY` | `ERROR_DIR_NOT_EMPTY` |
| `PZ_FS_INVALID` | `EINVAL`, `ELOOP`, `ENOSYS` | `ERROR_INVALID_NAME`, `ERROR_CANT_ACCESS_FILE` |
| `PZ_FS_TOO_LONG` | `ENAMETOOLONG` | `ERROR_FILENAME_EXCED_RANGE` |
| `PZ_FS_NO_SPACE` | `ENOSPC`, `EDQUOT` | `ERROR_DISK_FULL`, `ERROR_HANDLE_DISK_FULL` |
| `PZ_FS_IO` | `EIO` | `ERROR_CRC`, `ERROR_READ_FAULT`, `ERROR_WRITE_FAULT` |
| `PZ_FS_SYSTEM` | anything else | anything else |

`PZ_FS_CROSS_DEVICE` is what lets `move_tree` choose copy+remove instead of
reporting a failure (`mod_manager.cpp:40-46`).

### 7. `remove_all` removes read-only files on Windows

The Windows backend clears `FILE_ATTRIBUTE_READONLY` before each delete, as
`fs_win.cpp:74` already does in one path. POSIX needs no special case beyond a
writable parent directory. The asymmetry is documented and covered by a test
that removes a read-only file on both platforms.

### 8. Timestamps are epoch seconds in UTC

`pz_fs_last_write_time`, `pz_fs_set_last_write_time`, and `pz_fs_file_time_now`
use `int64_t` seconds since 1970-01-01 UTC. Windows converts `FILETIME`
(100 ns since 1601, UTC) and POSIX uses `stat`/`utimensat`, both truncated to
seconds. The setter requires an existing path and never creates one. Two-second
FAT granularity and one-second NTFS/ext4 truncation are acceptable because every
consumer compares ages in hours (24 h update check, cache pruning).

### 9. Paths are immutable UTF-8; no cached wide buffer

`pz_path` wraps a `pz_str` and has no wide cache. `fs_win.c` converts to UTF-16
per call into a local buffer. Normalization is lexical only: it collapses `.`,
`..`, repeated separators, and a trailing separator, and preserves a drive
prefix, a leading separator, and a UNC prefix. It never stats or resolves links.

Why: the current `native_fs::path` keeps a `mutable std::wstring` cache that is
not synchronized, while `ModManager` touches paths from four update-check
workers. Removing the cache removes the race; conversion cost is irrelevant next
to the syscall.

### 10. Long and UNC paths are handled inside `fs_win.c`

The public path stays in normal UTF-8 form. Before each Win32 call the backend
converts to the extended-length form when the path is absolute and either longer
than 248 characters or a UNC path (`\\?\UNC\server\share`). Normalization runs
before that conversion, because `\\?\` disables `.`/`..` interpretation.

Messages, TSV rows, and comparisons always use the API form, so a path echoed to
the user stays recognisable.

### 11. Invalid UTF-8 is an error, never replaced

If the UTF-8→UTF-16 conversion fails, the call fails with `PZ_FS_INVALID`. Paths
come from `installed.tsv`, Steam titles, and the user; silently substituting
replacement characters could address a different folder. A lossy helper can be
added as a separate, explicit call if a UI ever needs one.

### 12. Atomic replace lives in the filesystem layer

`pz_fs_replace_file(from, to)` writes a unique temporary file in `to`'s
directory, flushes it, and renames over `to` (`rename` on POSIX,
`MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)` on Windows).
On failure the temporary is removed and `to` is untouched.

`paths.cpp` implements this pattern for its C++ callers. `mod_manager_c.c`
writes its database/state files through the same C-level atomic replace, and
`net_c.c` downloads through it. The remaining C++ copy in `paths.cpp` can move
to this call when its callers are ported.

### 13. File I/O uses an opaque handle

`pz_fs_open_read`/`pz_fs_open_write` return a `pz_fs_file*`; `pz_fs_read`,
`pz_fs_write`, `pz_fs_close`, and `pz_fs_read_all` operate on it. The text module
uses these instead of `std::FILE*`, so `_wfopen` and the UTF-16 conversion stay
inside `fs_win.c`.

`pz_fs_read` returns 0 both at end of file and on error; `pz_fs_file_error`
distinguishes them without a second out-parameter on the hot path.

### 14. Tests are one C99 binary, run on both platforms

New `pzcore_fs_c_tests` in the existing `if(UNIX)` block, built from
`tests/test_c_fs.c` plus the `fs_c` sources. It creates its own temporary
fixtures, like `test_core.cpp`, so there are no repository fixtures. The Windows
half is the same source cross-built and run under Wine in the existing Windows
verification step; `ctest` does not cross-run today, and this does not change
that.

Coverage: path joins/normalization (including `..`), sorted listing and entry
kinds, copy tree with and without overwrite, file/dir collision, symlink
removal, timestamp round-trip, error formatting, handles/`read_all`, and the
executable bit. The same source runs on both backends: 8/8 CTest tests pass on
Linux, and the Win32 build passes under Wine.

## API (`src/core/fs_c.h`)

Proposed signatures and types, kept here so the design can be reviewed before
a header exists. C99, `extern "C"`, and the same return convention as the
other `_c` modules (1 on success, 0 on failure with `pz_fs_error`).

```c
#ifndef PZ_CORE_FS_C_H
#define PZ_CORE_FS_C_H

/* C99 filesystem contract for the C migration.
 *
 * Paths are UTF-8 byte strings on both platforms, always stored in a pz_path.
 * Windows converts to UTF-16 inside fs_win.c and never leaks wchar_t through
 * this header. `fs_posix.cpp` / `fs_win.cpp` keep their current design for the
 * C++ UI/path type; the C ModManager uses this API directly.
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
```

## Mapping from the current backend

| Current use | C API |
| --- | --- |
| `fs::path`, `operator/`, `parent_path`, `filename`, `lexically_normal` | `pz_path`, `pz_path_join`, `pz_path_parent`, `pz_path_filename`, `pz_path_lexically_normal` |
| `fs::exists` | `pz_fs_exists` |
| `fs::is_directory`, `is_regular_file` | `pz_fs_kind_of` |
| `fs::is_empty` | `pz_fs_is_empty` |
| `fs::file_size` | `pz_fs_file_size` |
| `fs::create_directories` | `pz_fs_create_directories` |
| `fs::remove` | `pz_fs_remove` |
| `fs::remove_all` | `pz_fs_remove_all` |
| `fs::rename` | `pz_fs_rename` |
| `fs::copy`, `copy_options` | `pz_fs_copy_tree`, `pz_fs_copy_file` |
| `fs::copy_file(overwrite_existing)` | `pz_fs_copy_file(..., PZ_FS_COPY_OVERWRITE, ...)` |
| `fs::directory_iterator` | `pz_fs_list_dir` |
| `fs::last_write_time`, `file_time_now` | `pz_fs_last_write_time`, `pz_fs_set_last_write_time`, `pz_fs_file_time_now` |
| `fs::permissions(owner_exec …)` | `pz_fs_set_executable` |
| `fs::current_path`, `temp_directory_path` | `pz_fs_current_dir`, `pz_fs_temp_dir` |
| `GetModuleFileNameW` / `/proc/self/exe` in `paths.cpp` | `pz_fs_executable_path` |
| `_wfopen` + `fread`/`fwrite` in `text.hpp` | `pz_fs_open_read/write`, `pz_fs_read/write`, `pz_fs_read_all` |
| `std::FILE*` config writes with temp+rename | `pz_fs_replace_file` |

Deliberately out of scope: ACLs, extended attributes, hard-link identity,
sparse files, case-insensitive path comparison, and a recursive copy that
preserves timestamps. None are used by the manager today.

## Implementation status

- [x] `fs_c.c`: `pz_path`, normalization, error names/formatting, entry list
      sorting, `remove_all`, `copy_tree`, and `read_all`.
- [x] `fs_posix.c`: the whole header with POSIX calls; `pzcore_fs_c_tests`
      green on Linux.
- [x] `fs_win.c`: the same operations, extended-length prefixes, strict UTF-8,
      read-only clearing, `MoveFileExW` replace; the same test source passes
      under Wine.
- [x] `mod_manager_c.c` uses `pzfs` for database/state I/O, installation,
      backup moves, import copies, and remove operations.
- [ ] Remove the remaining `native_fs` uses from the UI, `mod_info`, and
      activation wrappers, then delete the duplicate C++ path implementation.
      Those are outside the filesystem backend itself; the POSIX and Win32 C
      backends are complete.
