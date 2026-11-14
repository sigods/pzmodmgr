# C99/C89 migration assessment

Status for 0.3.1: the C99 conversion, C test suite, and C++ removal are complete.

## Final state: the repository is C99-only

- `src/main.c` owns `main()` and `src/ui/win32/win32_entry.c` owns `wWinMain`.
  Both call `pz_app_run()` in `src/app/app_c.c`, which loads the config, builds
  the service table, and runs the CLI or native UI.
- `pzcore`, `pzfs`, both native UIs, the app, and every test are C99.
  `CMakeLists.txt` declares `project(pzmodmgr ... LANGUAGES C)`.
- There are no `.cpp`, `.hpp`, or `.cc` files under `src/` or `tests/`, and no
  `pzcore_cpp` test adapter. `rg 'std::' src tests` returns nothing.
- Linux `ldd` lists only `libX11` and `libc`; Windows imports only system DLLs.
  There is no `libstdc++`, `libgcc_s`, `winpthread`, or MinGW runtime import.
- Final stripped sizes: Linux 194,560 bytes (from 231,424, -15.9%); Windows
  226,304 bytes (from 342,528, -33.9%).
- A fresh Xvfb screenshot pair of the pre-C99 and final Linux UI is pixel
  identical at 1100×900 after the logo baseline and filter chevron fixes.
- The C test suite has ten CTest entries, including the C manager/import/
  update-flow integration test in `tests/test_c_core.c`.

### Runtime memory

Measured on Linux under Xvfb with the same empty/custom `HOME`:

| State | Pre-C99 RSS | C99 RSS | Pre-C99 VmData | C99 VmData |
| --- | ---: | ---: | ---: | ---: |
| Empty install | 3,496 KB | **3,176 KB** | 8,572 KB | 8,844 KB |
| 512 installed mods | 4,124 KB | **4,864 KB** | 42,368 KB | 10,612 KB |

The idle C process uses slightly less resident memory. With 512 mods it uses
+740 KB because the C UI's rows hold fixed inline name/folder buffers instead
of `std::string` allocations; the total is still under 5 MB. VmData on the old
build is larger because its four update-check threads reserve malloc arenas.
There is no leak indication: `pzcore_c_core_tests` passes under ASan/UBSan, and
repeated runs stay flat. Shrinking the row structure would need dynamic strings
and is not worth the complexity for a sub-5 MB footprint.

The sections below record the migration steps and historical measurements.
Names such as `mod_manager.cpp` refer to files that have since been deleted.

## C99 migration start (`c99/leaf-modules`)

### Completed steps

- [x] Shared C99 layer: `src/core/c_util.{h,c}` provides `pz_str`, `pz_vec`,
      `pz_error`, and `pz_log`, with allocation-failure-returning APIs and no
      exception use. `tests/test_c_util.c` covers string growth/aliasing,
      vectors, error formatting, and log callbacks.
- [x] `error_code` stores a `pz_error`; its C++ API is unchanged.
- [x] `mod_info_c.{h,c}` owns `mod.info` parsing, required-ID normalization,
      B42 version-folder ranking, and the game-version/range rules.
      `mod_info_fs_c.{h,c}` now owns filesystem traversal, version-folder
      selection, and icon resolution; the C++ `ModInfo`/`read_mod_info` layer
      is a 98-line conversion wrapper.
- [x] `paths_c.{h,c}` owns config parsing/formatting, default-path discovery,
      load/save, path joins, and the built-in steamcmd path.
- [x] `activation_c.{h,c}` owns block/INI parsing and rewriting;
      `activation_targets_c.{h,c}` owns target discovery, repair, and writes.
- [x] `hash_c.{h,c}` is a one-shot SHA-256 (`pz_sha256_hex`) with no exceptions
      and one allocation for the padded block.
- [x] `json_c.{h,c}` is the shared JSON scanner/string/uint layer. It decodes
      every short escape, `\uXXXX`, and UTF-16 surrogate pairs, and rejects
      lone surrogates and unknown escapes.
- [x] `steam_json_c.{h,c}` parses GetPublishedFileDetails (title, per-item
      `result`, `time_updated`) and GetCollectionDetails (`children`, with
      de-duplication and the collection's own ID excluded).
- [x] `updater_json_c.{h,c}` parses GitHub release metadata, selects the
      Windows/Linux asset, and compares dotted versions.
- [x] Early C++ wrappers (`parse_published_file_*`, `parse_collection_children`,
      `parse_latest_app_release`, `verify_sha256`) were thin adapters over the C
      parsers. The steamcmd/updater headers that held them were removed once
      their callers moved to the C API directly.
- [x] C99 tests: `pzcore_json_c_tests` covers the SHA-256 known vectors
      (empty, `abc`, the quick-brown-fox string, and 10^6 × `a`), every short
      JSON escape, `\u00e9`, a surrogate-pair emoji, lone-surrogate and
      unknown-escape rejection, nested uint lookup, the Steam item `result`
      versus the outer call result, `time_updated` hidden inside a string,
      collection de-duplication, and Windows/Linux asset selection.
- [x] C filesystem layer: `src/core/fs_c.{h,c}` (paths, portable errors,
      sorted listing, `remove_all`, `copy_tree`, `read_all`), `fs_posix.c`, and
      `fs_win.c` form the `pzfs` library. The decisions are recorded in
      [`fs-c-api.md`](fs-c-api.md). `pzcore_fs_c_tests` passes on Linux and the
      Win32 backend passes the same test source under Wine. `fs_posix.cpp` /
      `fs_win.cpp` remain as the legacy C++ path type for the UI and the
      not-yet-ported `mod.info`/activation wrappers; `ModManager` now uses
      `pzfs` directly.
- [x] C99 `ModManager` core: `mod_manager_c.{h,c}` owns config, the
      `installed.tsv` database, update flags, locks, activation selection,
      install/import/update, and the SteamCMD metadata checks. The C++
      `ModManager` is a thin adapter over `pz_mod_manager`; its services table
      supplies mod-info discovery, activation writes, Steam Web API details,
      steamcmd downloads, updater checks, and config persistence. The full
      inventory remains in [`modmanager-deps.md`](modmanager-deps.md).
- [x] C99 process layer: `process_c.{h,c}` owns shell quoting, combined-output
      capture with line-by-line logging, detached children, and the
      wait/running/PID probes. `steamcmd_c.c`, `net_c.c`, and `updater_c.c`
      call it for curl, tar, steamcmd, and the update helper, and the duplicated
      Win32 `CreateProcessW` /
      POSIX `posix_spawn` code is gone. The Windows wrapper adds
      `cmd.exe /c "..."` itself, so callers no longer repeat the outer-quote
      rule. `pz_process_wait` and `pz_process_is_running` also use
      `waitpid(WNOHANG)` for children of this process, so a child that exited
      but was never reaped is no longer reported as running forever.
- [x] C99 network/archive layer: `net_c.{h,c}` owns the GitHub GET/POST, the
      atomic curl download, and tar/unzip extraction. `updater_c` and
      `steamcmd_c` call it directly; no C++ wrapper remains.
- [x] C99 updater: `updater_c.{h,c}` owns the release check, cache cleanup,
      staging (download, size and digest verification, extraction, helper
      copy), and the `--apply-update` helper (wait, copy, preserve config,
      swap, restart, clean). `app_c.c` and the C manager/command layers call it
      directly through `pz_updater_*`.
- [x] C99 steamcmd: `steamcmd_c.{h,c}` owns the configured/built-in/system
      lookup, the one-time Valve download, the Workshop download command and
      its Access Denied classification, and the Steam Web API POST calls
      (item result, `time_updated`, title, collection children). No C++ core
      file contains process, curl, tar, or update logic any more.
- [x] C++ conversion shims removed: `steamcmd.cpp`/`.hpp`,
      `updater.cpp`/`.hpp`, `main.cpp`, the UI bridges, and `mod_icon.cpp` are
      deleted. The manager services, CLI, entry point, and tests call
      `steamcmd_c.h`/`updater_c.h` directly; `pz_steamcmd_find` and
      `pz_updater_platform_name` were added so no caller needs the old
      adapters.
- [x] C99 command layer: `commands_c.{h,c}` owns the parser, batch handling,
      lock/name resolution, target selection, config persistence, and
      self-update staging. `commands.cpp` is now a 24-line bridge that adapts
      `LogFn` and `ModManager::core_handle()`.
- [x] C99 icon decoder: `src/ui/mod_icon_c.{h,c}` owns the PNG decode and
      center-crop. Both native UIs call `pz_mod_icon_load` directly.
- [x] X11 UI: `src/ui/x11/x11_ui_c.c` owns the window, drawing, rows, worker,
      settings, targets, clipboard, and inotify path. Button glyphs moved to
      `src/ui/glyphs_c.h`.
- [x] Win32 UI: `src/ui/win32/win32_ui_c.c` owns the native controls,
      owner-draw list/buttons, settings and targets dialogs, worker thread, and
      directory watcher; the C entry point is `win32_entry.c`. Button glyphs use
      the shared `src/ui/glyphs_c.h` table.
- [x] Remove the C++ runtime from the link: `src/main.c`, `src/app/app_c.c`,
      the C UI entry points, and `pzcore` replace the C++ app path. The C++
      facade remains only in `pzcore_cpp` for `pzcore_tests`.

All ten CTest tests pass on Linux, including `pzcore_json_c_tests`,
`pzcore_fs_c_tests`, `pzcore_process_c_tests` (shell quoting, capture, line
streaming, exit status, detached spawn, wait), and `pzcore_updater_c_tests`
(marker-based cache pruning, helper invocation, unsupported archive
rejection). The filesystem test also passes when cross-built and run under
Wine. The rest of the Windows app remains cross-build verification only.

### Size checkpoints (`size -A`)

Each checkpoint was rebuilt in a scratch tree from `eae87d5` plus the branch's
uncommitted modules: Release, size-optimized, LTO, stripped
(`strip --strip-unneeded` on Linux, `x86_64-w64-mingw32-strip --strip-all` on
Windows), then measured with GNU `size -A`. Section totals exclude the ELF/PE
headers, so they sit about 6.3 KB (Linux) and 2.7 KB (Windows) below the file
size.

| Checkpoint | Linux `.text` | Linux total | Windows `.text` | Windows total |
| --- | ---: | ---: | ---: | ---: |
| Baseline `eae87d5` | 189,163 | 225,075 | 269,792 | 339,776 |
| + shared layer, `mod.info` | 190,357 | 226,269 | 271,376 | 341,360 |
| + `paths` | 192,239 | 228,215 | 273,984 | 344,016 |
| + `activation` | 194,205 | 230,129 | 275,952 | 345,952 |
| + SHA-256 / JSON / Steam | 194,275 | 230,199 | 276,096 | 346,096 |
| + updater JSON (final) | **192,445** | **228,505** | **275,424** | **345,488** |

Stripped images: Linux 231,424 bytes at both ends; Windows 342,528 →
**348,160** (+5,632). The SHA-256/JSON port is nearly size-neutral (+70 bytes of
Linux `.text`, +144 on Windows) because it replaced the C++ scanner with the
same algorithms; the updater JSON parser then removed 1,694 / 608 bytes. The
remaining Windows growth is the C wrapper plus the static MinGW runtime. This is
source-migration progress, not yet a size win.

After the filesystem backends and the `ModManager` port were linked into the
app, the same stripped Release build measured:

| Platform | Before `ModManager` | After `ModManager` | Change |
| --- | ---: | ---: | ---: |
| Linux | 231,424 | **415,992** | +184,568 (+79.7%) |
| Windows | 348,160 | **363,520** | +15,360 (+4.4%) |

These are file sizes, not section totals, measured with
`strip --strip-unneeded` on Linux and `x86_64-w64-mingw32-strip --strip-all` on
Windows. The C manager is intentionally explicit: it carries its own file I/O,
path construction, ACF parser, activation-selection logic, and error strings
instead of reusing the C++ containers. That is the source-migration cost; the
next size pass should remove the now-unused C helpers and merge the duplicated
path/file wrappers before claiming a reduction. Linux grew more because the old
C++ manager was already LTO-specialized and inlined into the UI path.

The process layer moved the same Win32 calls from `steamcmd.cpp` into
`process_c.c`, so Windows grew by only 512 bytes. Linux gained 4,096 bytes: the
C capture path keeps a `pz_str` output buffer plus a pending-line buffer, where
the old template appended straight into `std::string`.

| Platform | After `ModManager` | After the process layer | Change |
| --- | ---: | ---: | ---: |
| Linux | 415,992 | **420,088** | +4,096 (+1.0%) |
| Windows | 363,520 | **364,032** | +512 (+0.1%) |

Moving the updater's staging and helper out of C++ removed more code than the
C core added. Linux returns to its pre-process-layer 415,992 bytes; Windows is
2,048 bytes below its 363,520 pre-process-layer size.

| Platform | After the process layer | After the updater port | Change |
| --- | ---: | ---: | ---: |
| Linux | 420,088 | **415,992** | -4,096 (-1.0%) |
| Windows | 364,032 | **361,472** | -2,560 (-0.7%) |

The steamcmd port then reused the same C process/curl path, so it was size
neutral on Linux and slightly smaller on Windows.

| Platform | After the updater port | After the steamcmd port | Change |
| --- | ---: | ---: | ---: |
| Linux | 415,992 | **415,992** | 0 |
| Windows | 361,472 | **360,448** | -1,024 (-0.3%) |

Removing the two conversion shims needed no new C code:

| Platform | After the steamcmd port | After removing the shims | Change |
| --- | ---: | ---: | ---: |
| Linux | 415,992 | **235,520** | -180,472 (-43.4%) |
| Windows | 360,448 | **357,376** | -3,072 (-0.9%) |

The Linux image falls to roughly its pre-`ModManager` size because the adapters
were the last users of several C++ template and exception paths; with them gone
the linker drops those static libstdc++ members. The Windows image shares more
of that runtime with the UI, so it only loses the adapter code. A smoke run of
the stripped Linux binary with a temporary `HOME` (`pzmodmgr list`) reports
`OK: 0 mod(s)`, and `ldd` still lists only Xlib, libm, libc, and Xlib's
transitive libraries.

### `ModManager` interface decisions

- The C core owns all mutable manager state. `ModManager` keeps only `Config`
  and an opaque `pz_mod_manager*`; no parallel installed list or update map
  remains in C++.
- C++ services are supplied through `pz_mm_services` rather than linking the C
  file to C++. This keeps `mod_manager_c.c` a C99 translation unit and lets the
  existing mod-info, activation, Steam Web API, steamcmd, and updater wrappers
  stay authoritative during the transition.
- `write_target` receives nullable add/workshop vectors: `NULL` means "no IDs"
  for a removal, while a non-NULL empty vector means "clear this list". This
  avoids allocating throwaway vectors for the common remove path.
- Remote `time_updated` and collection-child lookups are serial in the C core.
  The old C++ collection path used a four-thread pool. This is a temporary
  latency regression; the C++ service boundary is the place to reintroduce a
  worker pool without putting threads back into the state machine.
- `Config& config()` remains a mutable C++ reference. Before an operation the
  adapter copies it into the C core, and after an operation that changes paths
  or locks it copies the C config back. This preserves the existing public API
  while keeping one owner for persisted state.

### C++ removal finished

The test adapter was the last C++ consumer. `tests/test_core.cpp` was replaced
by `tests/test_c_core.c`, which drives `pz_mod_manager` and `pz_command_run`
directly, including install/update/import, activation lists, locks, config,
mod.info discovery, and the fake steamcmd/curl flows. The C++ facade, path
backend, command bridge, and `pzcore_cpp` target were then deleted.

The app entry points are `src/main.c` and `src/ui/win32/win32_entry.c`; both
call `pz_app_run()` in `src/app/app_c.c`.

### UI size checkpoints (local rebuilds)

These are stripped release binaries rebuilt on this host with the same CMake
configuration. They are not the historical 0.3.0 packaging checkpoint.

| Platform | Before UI port | After UI port | Change |
| --- | ---: | ---: | ---: |
| Linux X11, stripped | 235,520 | **206,848** | -28,672 (-12.2%) |
| Windows Win32, stripped | 357,376 | **330,752** | -26,624 (-7.4%) |

Final app conversion (fresh builds from the pre-C99 source snapshot):

| Platform | Pre-C99 app | Final C99 app | Change |
| --- | ---: | ---: | ---: |
| Linux X11, stripped | 231,424 | **194,560** | -36,864 (-15.9%) |
| Windows Win32, stripped | 342,528 | **226,304** | -116,224 (-33.9%) |

The final Linux `NEEDED` list is only `libX11.so.6` and `libc.so.6`; `libm` is
gone as well. The Windows import table contains only `GDI32`, `KERNEL32`,
`msvcrt`, `SHELL32`, and `USER32`.

## What is already done

- `std::filesystem` is gone from both the Windows and Linux builds.
  `src/core/fs_native.hpp` declares the small API, `src/core/fs_win.cpp` uses
  Win32 wide calls, and `src/core/fs_posix.cpp` uses POSIX calls.
- Windows paths remain UTF-8 inside the program and are converted to UTF-16 only
  at Win32 API boundaries. Linux paths are byte strings.
- The existing Linux test suite runs through the POSIX backend, and the Windows
  backend is exercised under Wine (recursive copy/remove, timestamps, non-ASCII
  paths, UNC paths, CLI, and GUI startup).
- The v0.2.4 Linux binary had 32 undefined `std::filesystem` symbols. The
  current Linux binary has 0. It imports 0 `GLIBCXX` symbols and has no
  `libstdc++`/`libgcc_s` dynamic dependency.
- The optimization pass removed `std::thread`, `std::mutex`,
  `std::error_code`, `std::chrono`, `std::map`, `std::set`, and `std::deque`
  from the application's hot paths. The replacements are `pz::thread`,
  `pz::mutex`/`pz::lock_guard`, `pz::error_code`, epoch-seconds timestamps, and
  the vector-backed `pz::flat_map`/`pz::flat_set`.
- Linux still links only the small header-instantiated `std::string`,
  `std::vector`, `std::wstring`, `std::optional`, and `std::string_view`
  surface. Windows also pulls libstdc++'s static string/exception support and
  winpthreads; this pass removes the avoidable terminate/demangler and printf
  formatter paths, but that runtime remains in the app image.
- `std::function` is gone. `LogFn` is now a function pointer plus an opaque
  context, with a small templated constructor for existing lambdas. The process
  capture sink in `process_c.c` uses a `pz_log` callback with no C++ types.
- Small-list sorting no longer instantiates `std::sort`/`std::stable_sort`.
  Mod-info version selection, save/server target ordering, and UI row ordering
  use local stable insertion sorts. The stripped Linux app fell from 219,288
  to 207,000 bytes and the native host from 96,408 to 92,312 bytes on the
  rebased `0.3.0` branch. The UI sort benchmark was unchanged within noise
  (hyperfine: +1.4% over 20,000 runs). `std::from_chars` was also replaced by
  bounded decimal parsing in `mod_info.cpp`; the C parsers parse manually.
- The URL parser is a C99 translation unit and the only URL implementation.
  `src/core/mod_manager.cpp`, the browser native host, and the batch command
  parser call the C API directly; tests do the same. The old
  `url.cpp`/`url.hpp` adapter has been removed. CMake enables `C` and builds
  the parser with `C_STANDARD 99`.
- The browser native host is now C99 (`src/native/native_host.c`). It locates
  the sibling `pzmodmgr` executable, runs the normal `add`/`update` CLI, captures
  its output, and returns the same framed JSON. It links only `url.c` and the C
  runtime; it does not link `pzcore` or any C++ runtime. The stripped host is
  10,160 bytes on Linux and 19,456 on Windows after the path-join fix below.
- Linux executables are linked with the static libstdc++ and libgcc runtimes,
  so the shipped binaries have no external C++ runtime dependency. This is a
  runtime milestone, not a source-level C99 conversion: the binary still
  contains C++ code and the build still uses a C++ compiler.

Historical sizes from the first reduction pass (before = `v0.2.4`; Windows
packaging used UPX 4.2.4 for both). Current final sizes are below in the 0.3.0
Windows size pass:

| Artifact | Before | Now |
| --- | ---: | ---: |
| Windows `pzmodmgr.exe` stripped | 1,431,747 | 463,161 |
| Windows `pzmodmgr.exe` UPX | 450,755 | 208,697 |
| Windows native host stripped | 1,270,245 | 286,700 |
| Windows setup (UPX) | 1,005,399 | 427,758 |
| Windows ZIP (UPX build) | 1,031,282 | 433,732 |
| Linux `pzmodmgr` stripped, dynamic runtime | 338,280 | 334,184 |
| Linux `pzmodmgr` stripped, self-contained runtime | 338,280 | 215,192 |
| Linux dynamic `libstdc++` imports | 56 | 0 |

The shipped Linux binary has no `GLIBCXX` imports and no
`libstdc++`/`libgcc_s` dependency. `ldd` lists only Xlib, libm, libc, and Xlib's
transitive X libraries. Windows imports only system DLLs. The remaining size is
mostly the two UIs, header-instantiated container code, and the Windows runtime
described above.

## 0.3.0 Windows size pass

The Windows app was measured after the 0.2.6 rebase, then reduced without
changing its feature set. Sizes are stripped release binaries; UPX is not
included in these comparisons.

| Artifact | Before | After | Change |
| --- | ---: | ---: | ---: |
| Windows `pzmodmgr.exe` | 450,328 | **310,272** | -140,056 (-31.1%) |
| Windows native host | 281,068 | **19,456** | -261,612 (-93.1%) |
| Linux `pzmodmgr` | 194,560 | **194,560** | unchanged |
| Linux native host | 79,872 | **10,160** | -69,712 (-87.3%) |

Each Windows change was measured separately:

| Change | App before | App after | Host before | Host after |
| --- | ---: | ---: | ---: | ---: |
| Minimal verbose terminate handler (drops `cp-demangle.o`) | 450,328 | 391,448 | 281,068 | 221,676 |
| Replace `snprintf` hex/decimal formatting | 391,448 | 391,960 | 221,676 | 196,520 |
| Replace the last `fprintf` in CLI output | 391,960 | **365,718** | 196,520 | **196,520** |
| C99 native host, no `pzcore` link | 365,718 | **365,718** | 196,520 | **48,728** |
| Strip-all overlay + five-frame ICO + LTO | 365,718 | **310,272** | 48,728 | **46,080** |
| Native-host path join without `swprintf` | 310,272 | **310,272** | 46,080 | **19,456** |

The app temporarily grew by 512 bytes when `snprintf` was removed because
`main.cpp` still called `fprintf`; replacing that final formatted write removed
MinGW's `__mingw_pformat`/`gdtoa` chain from the app as well. The host source had
no `fprintf`, so it dropped 25,156 bytes at the `snprintf` step. A later
`swprintf` removal cut the host to 19,456 bytes.

### Section comparison

| Section/group | Linux app | Windows app | Difference |
| --- | ---: | ---: | ---: |
| `.text` | 161,497 | 256,016 | +94,519 |
| read-only data (`.rodata`+`.data.rel.ro` / `.rdata`) | 14,856 | 23,288 | +8,432 |
| unwind/SEH (`.eh_frame*` / `.pdata`+`.xdata`) | 172 | 15,016 | +14,844 |
| resources (`.rsrc`) | 0 | 46,072 | +46,072 |
| imports/dynamic tables | 13,877 | 8,776 | -5,101 |
| relocations | 7,056 | 612 | -6,444 |
| headers/alignment | 3,807 | 12,642 | +8,835 |
| **File size** | **194,560** | **365,718** | **+171,158** |

The same pass produced a 323,540-byte Windows ZIP (from 433,732 in the first
UPX build); the setup was 807,089 bytes without UPX here, so it is not compared
to the earlier UPX-packed setup.

The Windows-only parts are therefore:

- `.rsrc` 46,072 bytes at the time: a 43,233-byte nine-frame PNG icon plus
  manifest and version info. The icon is already PNG-compressed; trimming it
  means dropping resolutions, not changing the encoding.
- `.pdata` + `.xdata` 15,016 bytes: x64 SEH/unwind metadata. Linux carries only
  172 bytes of `.eh_frame` because it is built without unwind tables. Removing
  the Windows metadata would break stack walking and crash handling.
- PE headers/alignment 12,642 bytes versus about 3,807 bytes of ELF headers.
- `.idata` 8,776 bytes of import tables, against Linux dynamic symbols and
  relocations.
- Static runtime still embedded in `.text`: about 27.5 KB of winpthreads plus
  MinGW startup/CRT and the libstdc++ string/exception path. The pthread code
  is pulled by libstdc++'s exception/TLS support, not by app code.

What was tried and rejected:

- `-Wl,--no-seh` would remove the 15 KB unwind section but breaks x64 stack
  unwinding; not used.
- `-Wl,--disable-dynamicbase` would remove 612 bytes of `.reloc` but disables
  ASLR; not used.
- `-fno-threadsafe-statics` saved only 479 bytes (the UI statics are already
  single-threaded), not enough to justify the weaker guarantee.
- LTO originally hit the `binds_to_current_def_p` ICE on MinGW GCC 15.2; the
  single-partition workaround below makes it usable.

### PE overlay and icon follow-up

A later pass found that `strip --strip-unneeded` left a 252-entry COFF symbol
table after the last PE section. It is a 9,399-byte overlay with no runtime
value, and its size changes with build paths. Switching the Windows packaging
script to `strip --strip-all` removes it. At that stage the native host also
drops from 48,728 to 46,080 bytes.

The nine-frame ICO was reduced to the five sizes used by normal Explorer and
desktop layouts: 16, 24, 32, 48, and 64 pixels. The 20, 40, 128, and 256 pixel
frames were removed; Windows scales the nearest available frame for uncommon
DPI steps. The 256 pixel frame only served Explorer's extra-large view, which
this project does not target. The ICO goes from 43,233 to 13,463 bytes
(-29,770), and `.rsrc` from 46,072 to 16,096. A lossless recompression attempt
only saved 402 bytes, so it was rejected.

| Section/group | Before | After | Change |
| --- | ---: | ---: | ---: |
| `.text` | 256,624 | 256,624 | 0 |
| read-only data (`.rdata`) | 23,768 | 23,768 | 0 |
| unwind/SEH (`.pdata`+`.xdata`) | 15,088 | 15,088 | 0 |
| resources (`.rsrc`) | 46,072 | 16,096 | -29,976 |
| imports/dynamic tables | 8,808 | 8,808 | 0 |
| relocations | 600 | 600 | 0 |
| COFF symbol overlay | 9,399 | 0 | -9,399 |
| headers/padding | 3,216 | 3,496 | +280 |
| **Windows app file** | **367,287** | **328,192** | **-39,095 (-10.6%)** |

The rebuilt baseline is 367,287 rather than the earlier 365,718 because the
COFF symbol table includes build paths; `strip --strip-all` makes the result
independent of them. Dropping the 256 pixel frame alone took the app from
344,576 to 328,192 bytes. Before LTO, the no-UPX package was 695,296 bytes for
the setup and 258,948 bytes for the ZIP, against 807,089 and 323,540 before the
pass.

### MinGW LTO ICE fix

The MinGW GCC 15.2 linker ICE (`binds_to_current_def_p`) only occurs with the
default parallel LTO partitioning. Linking with `-flto=1 -flto-partition=one`
lets full LTO optimize the UI, including `selected_ids`, without excluding it.
CMake now enables LTO by default on MinGW and appends those options to every
LTO target. This is a compiler workaround, not a feature or safety change.

| Section/group | Before LTO | After LTO | Change |
| --- | ---: | ---: | ---: |
| `.text` | 256,624 | 241,920 | -14,704 |
| read-only data (`.rdata`) | 23,768 | 20,696 | -3,072 |
| resources (`.rsrc`) | 16,096 | 16,096 | 0 |
| unwind/SEH | 15,088 | 15,088 | 0 |
| headers/padding | 3,496 | 3,376 | -120 |
| **Windows app file** | **328,192** | **310,272** | **-17,920 (-5.5%)** |

The native host is 19,456 bytes with this LTO build; the source-level path-join
fix below accounts for its reduction. With that host, the no-UPX setup is
650,752 bytes and the ZIP is 237,273.

### Native host path join

The Windows host built its sibling executable path with
`swprintf(..., L"%ls\\pzmodmgr.exe", exe_dir)`. That one call pulled in
MinGW's `__mingw_wpformat` (3,104 bytes), the gdtoa float-conversion chain
(roughly 3.5 KB), and extra msvcrt imports such as `fwprintf`, `fputwc` and
`localeconv`. Replacing it with `wcslen` and `memcpy` keeps the same path but
drops the formatter and its conversion runtime.

| Section/group | Before | After | Change |
| --- | ---: | ---: | ---: |
| `.text` | 34,928 | 10,464 | -24,464 |
| `.rdata` | 2,696 | 1,672 | -1,024 |
| unwind/SEH (`.pdata`+`.xdata`) | 2,048 | 948 | -1,100 |
| imports/dynamic tables (`.idata`) | 2,420 | 2,068 | -352 |
| other (`.data`, `.reloc`) | 224 | 168 | -56 |
| headers/alignment | 3,748 | 4,120 | +372 |
| **Windows native host file** | **46,080** | **19,456** | **-26,624 (-57.8%)** |

The stripped host is 19,456 bytes both with and without LTO, so LTO adds no
benefit here. Linux is 10,160 bytes; Windows is still about 1.9x larger because
of PE headers and alignment, the import table, unwind metadata, and the
`CreateProcessW`/wide-path code that replaces Linux's `popen` call.

A custom `-nostartfiles` entry was also prototyped. With a `mainCRTStartup`
that calls the renamed host main and `ExitProcess`, the host measured 9,728
bytes (slightly below Linux). That removes `__tmainCRTStartup` (1,024),
`_pei386_runtime_relocator` (912), `_gnu_exception_handler` (464),
`__mingw_TLScallback` (272), and `__main` (32), along with their imports and
unwind data. It handled a framed request under Wine, including the sibling
`CreateProcessW` launch. It is not shipped: it skips CRT argv, TLS, `atexit`,
and SEH setup, was only validated under Wine, and would break if the host later
uses a CRT feature that expects that initialization. It needs a real Windows
test before it can be considered.

## LodePNG evaluation

LodePNG was built alone with its encoder and disk support disabled, both as C++
and as C99. It was tested against the nine unique real icon images in the
project: the 16, 20, 24, 32, 40, 48, 64, 128, and 256 pixel ICO frames. The
duplicated extension/assets copies were not counted. All are 8-bit RGBA,
non-interlaced PNGs; no malformed PNG tests were added because upstream covers
that surface.

| Item | stb_image (PNG only) | LodePNG decoder-only |
| --- | ---: | ---: |
| Object, C99 build | 48,496 | 95,840 |
| Object, C++ build | 48,496 | 100,584 |
| Identical linked decoder | 198,992 | 223,568 (+24,576) |

All nine decoded images matched byte-for-byte by RGBA hash. Peak RSS for the
256px decode was 11,920 KB for both. LodePNG does not save binary size here, so
stb_image remains.


## Non-std binary and memory work

The final pass removed costs outside the container/runtime surface:

- **LTO on Linux and MinGW** replaces the previous non-LTO links. On Linux the
  stripped binary goes from 243,976 to 215,192 bytes; on MinGW the app goes from
  328,192 to 310,272 after the single-partition ICE workaround. LTO is enabled
  by default for Release when the compiler supports it.
- **Only Xlib is linked on Linux.** The old `X11_LIBRARIES` line made SM, ICE,
  and Xext direct dependencies even though no symbol was used. The binary now
  has only `libX11`, `libm`, and `libc` as direct dependencies, so the loader no
  longer maps those three unused libraries.
- **stb_image is trimmed to the PNG path.** `STBI_NO_LINEAR`,
  `STBI_NO_FAILURE_STRINGS`, and `STBI_NO_THREAD_LOCALS` remove float image
  decoding, error strings, and TLS state. This saves about 5.7 KB in the
  stripped Windows executable and removes the math path from the icon loader.
- **X11 icons use server pixmaps.** The old cache kept a 48x48 `XImage` for
  every installed mod. Each icon is now uploaded once to a pixmap and the
  client-side image is freed. With all icons loaded under Xvfb, client RSS fell
  from 4,676 KB to 4,256 KB (420 KB less), and repaints use `XCopyArea` instead
  of repeating `XPutImage` uploads.

## Why std::string and std::vector were not replaced

The libstdc++ archive is already empty of pulled members: the remaining
`std::string` (649 references), `std::vector` (200), and `std::wstring` (54)
uses are header-instantiated templates. Replacing them with custom containers
would not remove a runtime dependency, and it would be a cross-cutting rewrite
of path, parser, database, and UI ownership. The measured heap does not justify
that risk: with 47 installed mods, the activity log is capped at 300 lines, the
visible-row and info caches hold only the installed mods, and the former 420 KB
icon cache was the dominant retained allocation. Targeted fixes should be based
on a profiler if a larger mod collection shows a real problem; a wholesale
container replacement is deferred.

## Target: C99, not C89

C99 is the practical target. C89 would also require replacing `stdint.h`,
`long long`/64-bit IDs, `snprintf`, `strtoull`, `//` comments, declarations in
`for` initializers, `inline`, and `stdbool.h`. That adds work without helping the
libstdc++ goal. The codebase already relies on C99-era libc facilities.

## Remaining C++ dependencies

| Dependency | Rough use | Why it blocks a C build | Replacement direction |
| --- | ---: | --- | --- |
| `std::string` | 649 refs / 29 files | Allocation, ownership, and all path/config/parser signatures | `char*` + length/capacity, or one small string builder |
| `std::vector` | 200 refs / 24 files | Dynamic arrays in db, parsers, UI, arguments | Typed vector macros/functions or a generic byte buffer |
| `std::optional` | 13 refs / 8 files | URL and mod.info lookup results | `bool` + out parameter, or a sentinel |
| `pz::flat_map` / `pz::flat_set` | Replaces std map/set | Vector-backed, linear lookup is sufficient for dozens of entries | No action needed |
| `std::thread` / `std::mutex` / `std::atomic` | thread/mutex replaced; atomic remains | Collection worker pool and UI workers | `pz::thread`, `pz::mutex`, `pz::lock_guard` |
| `std::chrono` | Removed from app code | File timestamps, cache ages, sleeps | `time_t`, `clock_gettime`, `Sleep`/`usleep` |
| `std::error_code` | Removed | Every filesystem failure path | `pz::error_code` with `strerror`/`FormatMessageA` |
| `std::wstring` | 54 refs / 5 files | Windows UI and Win32 boundary | UTF-16 buffer/array; keep UTF-8 as the stored form |
| `std::string_view`, `std::from_chars` | steamcmd, mod.info | C++17 parsing helpers | pointer + length and manual decimal parsing |
| C++ language features | `auto`, lambdas, templates, classes, overloads | C has no equivalents | explicit types, functions with context structs, manual dispatch |

The application does not use iostreams, regular expressions, RTTI,
`std::filesystem`, or `std::function`. RTTI is disabled. There are no
`try`/`catch` sites left; application targets compile with `-fno-exceptions`.
The standalone Windows installer still uses the C++ standard library and keeps
`installer/runtime_min.cpp` to turn allocation/string failures into abort paths
instead of pulling in the C++ unwinder.

## Where the work is concentrated

- The app path is C99: `main.c`, `app_c.c`, `x11_ui_c.c`, `win32_ui_c.c`,
  `win32_entry.c`, `mod_icon_c.c`, and the pure-C `pzcore` library.
- There are no C++ sources under `src/` or `tests/`; CMake enables only the C
  language.
- Leaf modules are C99: `url.c`, `mod_info_c.c`, `paths_c.c`,
  `activation_c.c`, `activation_targets_c.c`, `hash_c.c`, `json_c.c`,
  `steam_json_c.c`, and `updater_json_c.c`. `paths_c.c` now also owns config
  discovery/load/save; `activation_targets_c.c` owns target discovery and
  writes.
- The standalone Windows installer and uninstaller remain C++ utilities under
  `installer/`; they are not part of the app target.
- Third-party `stb_image.h`, Xlib, and Win32 are C APIs. They do not require a
  C++ runtime.

## Recommended migration order

1. **Introduce C utility types.** Add a tiny string/vector/error/log layer with
   ownership rules and tests. Keep the C++ wrappers temporarily so the app still
   builds while modules move.
2. **Port leaf modules to C.** The URL parser and the `mod_info`, `paths`,
   `activation`, SHA-256/JSON, and updater release parsers are done. Continue
   with `fs_posix`/`fs_win`. These have narrow interfaces and test coverage.
   Replace `error_code` with `int` at the same time.
3. **Port process and update code.** Done: `process_c` owns quoting, capture,
   and detached children; `net_c` owns curl/tar downloads; `updater_c` owns
   staging and the helper; `steamcmd_c` owns bootstrap, download, and the Steam
   Web API. Every non-UI behavior is C99, and the `steamcmd`/`updater` C++
   headers are deleted.
4. **Port `ModManager`.** Done in this branch with a C-owned state machine and
   a C++ adapter.
5. **Port the UI last.** Done: X11 and Win32 both call the C API directly. The
   X11 design record is [`c99-x11-port.md`](c99-x11-port.md).
6. **Remove the C++ runtime from the link.** Done: `main.c`/`app_c.c` replace
   `main.cpp` and the facade in the app target. Linux `ldd` lists only `libX11`
   and `libc`; Windows `objdump -p` lists only system DLLs. No app or test target
   links C++.

## Exit criteria

- CMake enables only C and uses `C_STANDARD 99`; there are no `.cpp`, `.hpp`,
  or `.cc` files under `src/` or `tests/`.
- `nm -D`/`nm` shows no `std::` symbols in the shipped binaries.
- Linux `ldd` has no `libstdc++`; Windows has no MinGW runtime DLL imports.
- `ctest` passes; Windows CLI and GUI smoke tests pass under Wine.
- The installer/uninstaller and native host still build without the C++ runtime.
- Path handling still stores UTF-8 and converts only at the Win32 boundary.

## Risks

- String ownership is the main correctness risk. Use one allocation policy and
  audit every parser/config path before changing signatures.
- Threading is already an open TODO. Converting it should not silently make UI
  actions blocking.
- The Windows installer and uninstaller are separate C++ programs under
  `installer/`. They also need conversion before the whole distribution is
  libstdc++-free.
- The native messaging host is C99 but now depends on the sibling `pzmodmgr`
  executable being present and runnable. It must remain a console-subsystem
  program, and its add/update behavior is covered by the native-host test.
