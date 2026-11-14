# `ModManager` dependency inventory

Scope: the original `src/core/mod_manager.cpp` (1,706 lines) and
`src/core/mod_manager.hpp` (139 lines) on `c99/leaf-modules`. The counts come
from `grep -o 'std::[A-Za-z_][A-Za-z_0-9:]*'` and
`grep -o '\bfs::[A-Za-z_][A-Za-z_0-9]*'` over both files, so they include
comments and header signatures. The inventory records the work needed for the
port; the port is now implemented in `src/core/mod_manager_c.{h,c}` with a thin
C++ adapter in `src/core/mod_manager.cpp`.

The target is a C99 `pz_mod_manager.c` with a thin C++ wrapper for the existing
`ModManager` API. Every row below is one thing that has to exist in C or in the
wrapper before that can happen.

## Language and standard-library use

| Use | cpp | hpp | What it does in `ModManager` | Leaf module that replaces it | Status |
| --- | ---: | ---: | --- | --- | --- |
| `std::string` | 98 | 17 | Folders, mod IDs, messages, `installed.tsv` fields | `pz_str` (`c_util`) | done |
| `std::vector` | 58 | 19 | Installed records, candidates, paths to move, ID lists | `pz_vec` (`c_util`) | done |
| `std::uint64_t` / `std::uint32_t` | 62 / 2 | 28 | Workshop IDs, sizes, `time_updated` | `<stdint.h>` types | n/a |
| `std::size_t` | 23 | — | Container indices and loop counters | `<stddef.h>` `size_t` | n/a |
| `std::to_string` | 83 | — | Building log lines, cache folder names, TSV rows | `pz_str_append_u64` (`c_util`) | done |
| `std::find` | 12 | — | Linear scans of mod-ID and target lists | `pz_vec_find` / `pz_str` key compare (`c_util`) | helper missing |
| `std::find_if` | 4 | — | Pick an activation target by kind and name | same, with a predicate callback | helper missing |
| `std::any_of` | 2 | — | "Is this Workshop ID tracked?" | loop over `pz_vec` | mechanical |
| `std::remove_if` | 3 | — | Drop records from `db_` and IDs from a list | `pz_vec_remove_if` (`c_util`) | helper missing |
| `std::sort` | 1 | — | Sort discovered ACF items by Workshop ID (line 424) | `pz_vec_sort` (insertion sort like `mod_info`/UI) | helper missing |
| `std::move` | 6 | — | Transfer ownership on construct, swap, pick-best | assignment / swap of owned pointers | mechanical |
| `std::min` / `std::max` | 1 / 1 | — | Clamp worker count, take newest timestamp | `pz_min` / `pz_max` inline (`c_util`) | trivial |
| `std::atomic<std::size_t>` | 1 | — | Worker index for the four parallel API lookups (line 245) | `<stdatomic.h>` in planned `thread_c` | module missing |
| `std::strtoull` / `std::strtoll` | 3 / 1 | — | Parse `installed.tsv` numbers and version fields | `pz_parse_u64` / `pz_parse_i64` (`c_util`) | helper missing |
| `std::time` | 2 | — | Cache stamp and `last_check` epoch seconds | `<time.h>` `time()` | mechanical |
| `std::isspace` / `std::isdigit` / `std::tolower` | 3 / 3 / 1 | — | Trim lines, validate folder names, case-fold IDs | `<ctype.h>` | mechanical |
| `std::string::npos` | 3 | — | `find_first_not_of` / `find` sentinels | `SIZE_MAX` from `pz_str_find` | helper missing |
| `std::optional<ModInfo>` | — | 1 | `info_for(folder)` "found or not" return (line 74) | `int` return + `ModInfo` out-param (`mod_info_c`) | wrapper concern |

There is no `std::stoull` call left: the only hit is the comment at line 21
explaining why it was removed.

`std::pair`, `std::make_pair`, and `std::swap` do not appear; the six
`std::move` sites are all plain ownership transfers.

## Filesystem use (`pz::fs`, the `native_fs` backend)

`fs_posix.cpp` and `fs_win.cpp` stay as they are for this pass. The mapping
below is what the C API has to offer; the design questions are in the next
section.

| Use | cpp | hpp | Where it is used | C entry point needed |
| --- | ---: | ---: | --- | --- |
| `fs::path` | 29 | 6 | Every function signature, joins with `operator/` | `pz_path` type + `pz_path_join` |
| `fs::remove_all` | 10 | — | Swap cleanup, cache cleanup, remove, import rollback (46, 65, 618, 673, 1089, 1092, 1097, 1213, 1242, 1262) | `pz_fs_remove_all` |
| `fs::create_directories` | 6 | — | Config dirs, mods dir, backup parent, swap dirs (38, 287, 288, 308, 314, 1201) | `pz_fs_create_directories` |
| `fs::exists` | 5 | — | Collision checks, restore branches, backup presence (589, 1144, 1182, 1198, 1212) | `pz_fs_exists` |
| `fs::directory_iterator` | 5 | — | Content root, `mods/`, a mod's version folders (59, 414, 544, 1288, 1355) | `pz_fs_dir_open/next/close` or a filled `pz_vec` |
| `fs::rename` | 4 | — | Atomic move, backup and restore (40, 1146, 1202, 1214) | `pz_fs_rename` |
| `fs::copy` | 2 | — | Cross-device fallback and import from SteamCMD (44, 607) | `pz_fs_copy_tree` |
| `fs::copy_options` | 3 | — | `recursive` and `overwrite_existing` flags (44, 607) | flag bits on `pz_fs_copy_tree` |
| `fs::remove` | 2 | — | Delete an empty dir (67, 1265) | `pz_fs_remove` |
| `fs::is_directory` | 2 | — | Content root and ACF folder check (370, 505) | `pz_fs_is_directory` |
| `fs::is_regular_file` | 1 | — | ACF file check (393) | `pz_fs_is_regular_file` |
| `fs::last_write_time` | 1 | — | Prune stale swap copies by age (64) | `pz_fs_last_write_time` |
| `fs::file_time_type` / `file_time_now` | 1 / 1 | — | Age comparison; already epoch seconds (57, 61) | `int64_t` epoch seconds |

`fs::permissions` is not used by `ModManager`; it is called by `steamcmd_c.c`
and the tests, so it stays on the C++ backend for now.

## Other modules `ModManager` already leans on

| Module | Surface used | State |
| --- | --- | --- |
| `mod_info_c` | `read_mod_info`, `ModInfo` (name, id, icon, version bounds) | done |
| `paths_c` | `Config` parsing/formatting, joins, built-in steamcmd path | done |
| `activation_c` | `ActivationTarget`, enabled lists, block rewrite | done |
| `url.c` | Workshop URL/ID parsing | done |
| `steam_json_c` | Workshop title/result/time, collection children | done |
| `updater_json_c`, `hash_c` | Update metadata, asset digest | done |
| `flat_map.hpp` (`flat_map`, `flat_set`) | Installed-ID lookups, folded-name sets | planned `table_c` |
| `thread.hpp` | Worker threads for parallel update checks | planned `thread_c` |
| `text.hpp` | `split_fields`, `split_first_word`, line folding, UTF-8 file I/O | planned `text_c` |
| `types.hpp` | `Result`, `LogFn`, `Config`, `InstalledMod` | C structs in the wrapper |

`LogFn` is already a function pointer plus opaque context, so the C layer can
take `pz_log` directly.

## Call sites that need a design decision, not a rename

1. **Worker pool** (lines 240-270): four threads each pull the next ID from a
   shared vector through `std::atomic<size_t>`. The C version needs
   `pz_thread` (start/join), a `pz_mutex`, and an atomic index. The callback
   keeps the `void*` context because the function pointer cannot capture.
2. **`installed.tsv` record vector** (`db_`): records own two strings and a
   bool. In C this becomes `pz_vec` of a struct with `pz_str` members; the
   wrapper keeps `std::vector<InstalledMod>` by copying at the boundary, or
   exposes a view for the UI to read under the manager's lock.
3. **`pz::flat_set<std::string>` keyed by folded ID** (lines 428-429, 1289,
   1305): needs a small string-keyed table with case folding, or a sorted
   `pz_vec` with the existing insertion sort.
4. **`info_for` returning `std::optional<ModInfo>`** (hpp line 74): the C
   function should be `int pz_mod_info_for(..., ModInfo* out)`, matching
   `read_mod_info`'s existing shape.
5. **`Result` messages**: every failure path builds a string. In C, either
   pass a `pz_str* message` out-param or have the wrapper format it; pick one
   before the port starts or the whole file changes twice.

## Filesystem questions (resolved)

The fourteen questions that were open on this branch are now decided in
[`fs-c-api.md`](fs-c-api.md). Short answers:

1. Directory listing is a sorted snapshot in a `pz_vec`, freed with
   `pz_fs_entries_free`; no open/next/close handle.
2. Entries are sorted by name by the API, so callers stop depending on
   `readdir`/`FindFirstFile` order.
3. An entry that cannot be inspected is returned with `kind = PZ_FS_UNKNOWN`
   and `entry.error`; only a failure to open the directory fails the call.
4. `pz_fs_copy_tree` creates the destination subtree but not its parent,
   overwrites only with `PZ_FS_COPY_OVERWRITE`, preserves the POSIX exec bit,
   and rejects file/dir collisions with `PZ_FS_INVALID`.
5. Links and junctions are never followed: `PZ_FS_LINK` kind, `remove_all`
   unlinks them, copy keeps them as links on POSIX (Windows refuses) and only
   follows file links with `PZ_FS_COPY_FOLLOW_LINKS`, depth-capped at 32.
6. Errors are a portable `pz_fs_code` enum plus the raw `system_code`;
   `PZ_FS_CROSS_DEVICE` drives the move-to-copy fallback.
7. `remove_all` clears `FILE_ATTRIBUTE_READONLY` on Windows before deleting.
8. Timestamps stay `int64_t` epoch seconds UTC; the setter requires an existing
   path and never creates one.
9. `pz_path` is immutable UTF-8 with no cached wide buffer; conversion happens
   per call in `fs_win.c`, so paths are safe to share across worker threads.
10. `fs_win.c` applies the `\\?\` extended-length prefix internally; the API
    and messages keep the normal form.
11. Invalid UTF-8 fails with `PZ_FS_INVALID` rather than substituting
    replacement characters.
12. Atomic replace moves into the fs layer as `pz_fs_replace_file`, replacing
    the duplicate temp+rename code in `paths.cpp` and `net_c.c`.
13. File I/O is an opaque `pz_fs_file` handle (`pz_fs_open_read/write`,
    `pz_fs_read/write`, `pz_fs_read_all`), so the text module never converts
    paths.
14. One C99 `pzcore_fs_c_tests` binary runs on Linux and, cross-built, under
    Wine; it builds its own temporary fixtures.
