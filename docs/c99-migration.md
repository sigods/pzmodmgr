# C99/C89 migration assessment

Status after the 0.3.0 Windows static-runtime pass and the C99 native-host rewrite.

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
  context, with a small templated constructor for existing lambdas. The
  internal capture sink in `steamcmd.cpp` is a template instead of
  `std::function`.
- Small-list sorting no longer instantiates `std::sort`/`std::stable_sort`.
  Mod-info version selection, save/server target ordering, and UI row ordering
  use local stable insertion sorts. The stripped Linux app fell from 219,288
  to 207,000 bytes and the native host from 96,408 to 92,312 bytes on the
  rebased `0.3.0` branch. The UI sort benchmark was unchanged within noise
  (hyperfine: +1.4% over 20,000 runs). `std::from_chars` was also replaced by
  bounded decimal parsing in `mod_info.cpp` and `steamcmd.cpp`.
- The URL parser is a C99 translation unit and the only URL implementation.
  `src/core/mod_manager.cpp`, the browser native host, and the batch command
  parser call the C API directly; tests do the same. The old
  `url.cpp`/`url.hpp` adapter has been removed. CMake enables `C` and builds
  the parser with `C_STANDARD 99`.
- The browser native host is now C99 (`src/native/native_host.c`). It locates
  the sibling `pzmodmgr` executable, runs the normal `add`/`update` CLI, captures
  its output, and returns the same framed JSON. It links only `url.c` and the C
  runtime; it does not link `pzcore` or any C++ runtime. The stripped host is
  10,160 bytes on Linux and 48,728 on Windows.
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
| Windows `pzmodmgr.exe` | 450,328 | **365,718** | -84,610 (-18.8%) |
| Windows native host | 281,068 | **48,728** | -232,340 (-82.7%) |
| Linux `pzmodmgr` | 194,560 | **194,560** | unchanged |
| Linux native host | 79,872 | **10,160** | -69,712 (-87.3%) |

Each Windows change was measured separately:

| Change | App before | App after | Host before | Host after |
| --- | ---: | ---: | ---: | ---: |
| Minimal verbose terminate handler (drops `cp-demangle.o`) | 450,328 | 391,448 | 281,068 | 221,676 |
| Replace `snprintf` hex/decimal formatting | 391,448 | 391,960 | 221,676 | 196,520 |
| Replace the last `fprintf` in CLI output | 391,960 | **365,718** | 196,520 | **196,520** |
| C99 native host, no `pzcore` link | 365,718 | **365,718** | 196,520 | **48,728** |

The app temporarily grew by 512 bytes when `snprintf` was removed because
`main.cpp` still called `fprintf`; replacing that final formatted write removed
MinGW's `__mingw_pformat`/`gdtoa` chain from the app as well. The host had no
`fprintf`, so it dropped 25,156 bytes at the `snprintf` step.

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
- LTO remains unusable on MinGW GCC 15.2: it reproduces the
  `binds_to_current_def_p` ICE in `selected_ids`.

### PE overlay and icon follow-up

A later pass found that `strip --strip-unneeded` left a 252-entry COFF symbol
table after the last PE section. It is a 9,399-byte overlay with no runtime
value, and its size changes with build paths. Switching the Windows packaging
script to `strip --strip-all` removes it. The native host also drops from
48,728 to 46,080 bytes.

The nine-frame ICO was reduced to the six resolutions Windows uses most:
16, 24, 32, 48, 64, and 256 pixels. The 20, 40, and 128 pixel frames were
removed; Windows scales the nearest available frame for those uncommon DPI
steps. The 256 pixel frame is kept for Explorer's extra-large view. The ICO
goes from 43,233 to 29,684 bytes (-13,549), and `.rsrc` from 46,072 to 32,368.
A lossless recompression attempt only saved 402 bytes, so it was rejected.

| Section/group | Before | After | Change |
| --- | ---: | ---: | ---: |
| `.text` | 256,624 | 256,624 | 0 |
| read-only data (`.rdata`) | 23,768 | 23,768 | 0 |
| unwind/SEH (`.pdata`+`.xdata`) | 15,088 | 15,088 | 0 |
| resources (`.rsrc`) | 46,072 | 32,368 | -13,704 |
| imports/dynamic tables | 8,808 | 8,808 | 0 |
| relocations | 600 | 600 | 0 |
| COFF symbol overlay | 9,399 | 0 | -9,399 |
| headers/padding | 3,216 | 3,608 | +392 |
| **Windows app file** | **367,287** | **344,576** | **-22,711 (-6.2%)** |

The rebuilt baseline is 367,287 rather than the earlier 365,718 because the
COFF symbol table includes build paths; `strip --strip-all` makes the result
independent of them. The final no-UPX package is 744,448 bytes for the setup
and 291,405 bytes for the ZIP, against 807,089 and 323,540 before the pass.

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
- The native messaging host is C99 but now depends on the sibling `pzmodmgr`
  executable being present and runnable. It must remain a console-subsystem
  program, and its add/update behavior is covered by the native-host test.
