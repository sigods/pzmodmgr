# C99/C89 migration assessment

Status after the 0.3.0 native-host reduction and URL C-API pass.

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
- No libstdc++ archive member is pulled into the final link. The remaining
  `std::string`, `std::vector`, `std::wstring`, `std::optional`, and
  `std::string_view` uses are header-instantiated or covered by the small
  abort-based runtime shim.
- `std::function` is gone. `LogFn` is now a function pointer plus an opaque
  context, with a small templated constructor for existing lambdas. The
  internal capture sink in `steamcmd.cpp` is a template instead of
  `std::function`.
- The URL parser is a C99 translation unit and the only URL implementation.
  `src/core/mod_manager.cpp`, the browser native host, and the batch command
  parser call the C API directly; tests do the same. The old
  `url.cpp`/`url.hpp` adapter has been removed. CMake enables `C` and builds
  the parser with `C_STANDARD 99`.
- The browser native host calls `ModManager::add`/`update_one` instead of
  `run_command`, so this add/update-only process no longer links command
  dispatch or the unused update helpers. The stripped host is 96,408 bytes on
  Linux (was 125,080) and 286,700 on Windows (was 318,444).
- Linux executables are linked with the static libstdc++ and libgcc runtimes,
  so the shipped binaries have no external C++ runtime dependency. This is a
  runtime milestone, not a source-level C99 conversion: the binary still
  contains C++ code and the build still uses a C++ compiler.

Measured sizes (before = `v0.2.4`, after = this branch; Windows packaging uses
UPX 4.2.4 for both):

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
`libstdc++`/`libgcc_s` dependency. `ldd` now lists only Xlib, libm, libc, and
Xlib's transitive X libraries. Windows imports only system DLLs. The remaining
size is primarily the two UIs and header-instantiated container code.

On the unreleased `0.3.0` branch, the native-host-only change lowers that host
from 125,080 to 96,408 bytes on Linux (-22.9%) and from 318,444 to 286,700 bytes
on Windows (-9.97%). The unchanged app is still 463,161 bytes stripped and
208,697 bytes with UPX; the smaller host payload takes the Windows setup from
441,582 to 427,758 bytes and the ZIP from 449,983 to 433,777 bytes.

## Non-std binary and memory work

The final pass removed costs outside the container/runtime surface:

- **LTO on Linux** replaces the previous non-LTO link and takes the stripped
  binary from 243,976 to 215,192 bytes. It is enabled by default for Release
  when the compiler supports it. MinGW GCC 15.2 has a reproducible LTO ICE in
  `binds_to_current_def_p()` on this UI, so the default is automatically off for
  MinGW; `PZMODMGR_LTO=ON` can be tried once that toolchain bug is fixed.
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
`try`/`catch` sites left; application targets compile with `-fno-exceptions`,
and the small `runtime_min.cpp` shim turns allocation/string/mutex failures
into abort paths instead of pulling in the C++ unwinder.

## Where the work is concentrated

- UI: `src/ui/win32/win32_ui.cpp` (2,708 lines) and
  `src/ui/x11/x11_ui.cpp` (2,210 lines) are the largest C++ consumers:
  containers, lambdas, threads, `std::wstring`, and callbacks.
- Core orchestration: `mod_manager.cpp` (1,062), `updater.cpp` (736), and
  `steamcmd.cpp` (703) hold most of the string/vector/map/error-code surface.
- Leaf code is already close to C in shape: `url.c`, `mod_info.cpp`,
  `paths.cpp`, `activation.cpp`, the SHA-256/JSON helpers, and `fs_posix.cpp`.
- Third-party `stb_image.h`, Xlib, and Win32 are C APIs. They do not require a
  C++ runtime.

## Recommended migration order

1. **Introduce C utility types.** Add a tiny string/vector/error/log layer with
   ownership rules and tests. Keep the C++ wrappers temporarily so the app still
   builds while modules move.
2. **Port leaf modules to C.** The URL parser is done. Continue with
   `mod_info`, `paths`, `activation`, SHA-256/JSON, then `fs_posix`/`fs_win`.
   These have narrow interfaces and test coverage. Replace `error_code` with
   `int` at the same time.
3. **Port process and update code.** `steamcmd` and `updater` need C process
   spawning, temporary buffers, and callback-based progress. Remove the
   `std::optional`/`std::map`/`std::chrono` uses here.
4. **Port `ModManager`.** Replace the installed-mod vector, update map, sets,
   and worker pool. This is the largest core change.
5. **Port the UI last.** Keep a C ABI between `pzcore` and the UI while moving
   one UI at a time. The Win32 UI should become C before X11, or vice versa;
   do not convert both at once.
6. **Remove the C++ runtime from the link.** Build with C99 and no libstdc++.
   On Linux, verify `ldd` no longer lists `libstdc++.so.6`/`libgcc_s.so.1`; on
   Windows, verify `objdump -p` has no `libstdc++`, `libgcc`, or `winpthread`.

## Exit criteria

- CMake keeps `C_STANDARD 99` enabled and has no C++ source/compiler after the
  last module moves. The current self-contained Linux/Windows binaries meet the
  no-external-runtime criterion but not this source criterion yet.
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
- The Windows installer and uninstaller are separate C++ programs. They also
  need conversion before the whole distribution is libstdc++-free.
- The native messaging host links `pzcore`, but now references only the
  add/update path; it inherits the remaining core migration and must remain a
  console-subsystem program.
