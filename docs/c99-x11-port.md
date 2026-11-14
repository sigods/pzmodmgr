# X11 UI C99 port: investigation and handoff

Status: **implemented** on `c99/leaf-modules`. `src/ui/x11/x11_ui_c.c` is the
X11 UI. The C++ bridge was removed when `src/main.c` and `src/app/app_c.c` took
over the app entry path. The Win32 UI was ported separately to
`src/ui/win32/win32_ui_c.c` and is no longer C++ either.

This document is retained as the design/ownership record. The phase headings
below are historical; all seven phases landed in the implementation. Verification
performed: Release build, strict `-std=c99 -Wall -Wextra -Werror` compile of the
X11 source, 10/10 CTest tests, and an Xvfb smoke run covering the main screen,
settings save (config written), filter menu, and targets dialog. Screenshots were
captured with `scrot`; ImageMagick's `import` lacks the X11 delegate here.

## Goal

Replace `src/ui/x11/x11_ui.cpp` (2,444 lines, the largest C++ consumer) with a
C99 implementation that produces the same window, layout, interactions, and
worker behavior. Keep `run_ui(ModManager&)` as a tiny C++ bridge during the
transition so `main.cpp`, the Win32 UI, and the existing `ModManager` facade do
not have to move in the same change.

This is the first UI port. Do not convert both UIs at once.

## What already exists

| Piece | File | State |
| --- | --- | --- |
| Manager state/actions | `src/core/mod_manager_c.{h,c}` | C99, authoritative |
| Command parser | `src/core/commands_c.{h,c}` | C99, authoritative |
| `mod.info` traversal/icon resolution | `src/core/mod_info_fs_c.{h,c}` | C99, authoritative |
| PNG thumbnail decoder | `src/ui/mod_icon_c.{h,c}` | C99, authoritative |
| X11 UI | `src/ui/x11/x11_ui_c.c` | C99, 3,294 lines |
| X11 bridge | `src/ui/x11/x11_ui.cpp` | 6-line C++ bridge |
| Button glyphs | `src/ui/glyphs_c.h` | C99 table, 16 glyphs |
| C++ bridges | `commands.cpp` 24, `mod_info.cpp` 98, `mod_icon.cpp` 19 | conversions only |

The C manager still receives services from `mod_manager.cpp`; those callbacks
now use the C `mod.info` traversal directly. The X11 UI does not need to know
that. It needs only the C APIs listed below.

## Target call graph

Before:

```
main.cpp -> ModManager -> run_ui(ModManager&) -> x11_ui.cpp
                                      |-> run_command -> commands.cpp -> commands_c
                                      |-> mm.*        -> mod_manager.cpp -> mod_manager_c
                                      |-> load_mod_icon -> stb_image
```

After:

```
main.cpp -> ModManager -> x11_ui.cpp (bridge) -> pz_x11_run_ui(mm.core_handle())
                                                   |-> pz_command_run
                                                   |-> pz_mod_manager_*
                                                   |-> pz_mod_info_read
                                                   |-> pz_mod_icon_load
```

`ModManager::core_handle()` already returns the shared `pz_mod_manager*`, so the
bridge is only:

```cpp
#include "ui/ui.hpp"
#include "ui/x11/x11_ui_c.h"
namespace pz {
int run_ui(ModManager& mm) { return pz_x11_run_ui(mm.core_handle()); }
}
```

## C API the UI should use

All functions are declared in the included headers. Ownership is the main
source of bugs, so follow this table exactly.

### Commands

```c
pz_mm_result pz_command_run(pz_mod_manager* manager, const char* line,
                            size_t length, const pz_log* log);
void pz_mm_result_free(pz_mm_result* result);
```

`pz_command_run` is the equivalent of `run_command`. It must run on the worker
thread for Install, Import, Update, Remove, Check, Self-update, config saves, and
Enable/Disable. The `pz_log` callback runs on that worker thread; it may only
enqueue strings and wake the UI.

### Reads

| C++ call | C replacement | Release |
| --- | --- | --- |
| `mm.list_all()` | `pz_mod_manager_list_all(mm, &vec)` | `pz_mod_manager_free_mods(&vec)` |
| `mm.update_available(id)` | `pz_mod_manager_update_available(mm, id)` | — |
| `mm.locked(id)` | `pz_mod_manager_locked(mm, id)` | — |
| `mm.manually_locked(id)` | `pz_mod_manager_manually_locked(mm, id)` | — |
| `mm.server_locked(id)` | `pz_mod_manager_server_locked(mm, id)` | — |
| `mm.activation_targets()` | `pz_mod_manager_activation_targets(mm, &vec)` | `pz_mod_manager_free_targets(&vec)` |
| `mm.missing_requirements()` | `pz_mod_manager_missing_requirements(mm, &vec)` | `pz_mod_manager_free_requirements(&vec)` |
| `mm.workshop_ids_for(target)` | `pz_mod_manager_workshop_ids_for(mm, target, &vec)` | `pz_vec_free(&vec)` (uint64_t) |
| `mm.unmapped_mod_ids(target)` | `pz_mod_manager_unmapped_mod_ids(mm, target, &vec)` | `pz_activation_string_list_free(&vec)` |
| `mm.enabled_mod_names(target)` | `pz_mod_manager_enabled_mod_names(mm, target, &vec)` | `pz_activation_string_list_free(&vec)` |
| `mm.game_version()` | `pz_mod_manager_game_version(mm)` | borrowed, do not free |
| `mm.config()` | `pz_mod_manager_get_config(mm, &cfg)` | `pz_paths_config_free(&cfg)` |
| `mm.app_update_available()` | `pz_mod_manager_app_update_available(mm)` | — |
| `mm.update_check_due()` | `pz_mod_manager_update_check_due(mm)` | — |
| `mm.reload_installed()` | `pz_mod_manager_reload_installed(mm)` | — |
| `mm.info_for(folder)` | `pz_mod_info_read(&path, game, game_len, &info)` | `pz_mod_info_free(&info)` |
| `load_mod_icon(path, 48, icon)` | `pz_mod_icon_load(path, len, 48, &icon)` | `pz_mod_icon_free(&icon)` |

Build the `pz_path` for `info_for` from
`cfg.user_mods_dir + "/" + folder`; `pz_path_join` does the platform separator.

Manager vectors are initialized by the callee and must be released with the
matching helper. Do not hold `pz_str` pointers after the release call.

### Configuration

The settings dialog does not need a direct save function. It issues six
`config ...` commands through `pz_command_run`, exactly as the C++ UI did:

```
config steamcmd_exe <path>
config steamcmd_root <path>
config steam_user <name>
config zomboid_dir <path>
config dark_mode on|off
config auto_enable on|off
```

`pz_mod_manager_save_config` exists for the command layer; the UI should not
call it directly.

## Recommended state model

Use the existing `pz_str`/`pz_vec` layer rather than inventing another container.
A single `PzX11Ui` struct passed to every static function replaces the lambda
captures. Suggested fields (names are illustrative):

```c
typedef struct PzX11Ui {
    Display* dpy;
    int screen;
    Window window;
    GC gc;
    Pixmap backbuf;
    Drawable target;              /* backbuf */
    XFontStruct* body;
    XFontStruct* heading;
    Palette pal, light_pal, dark_pal;

    pz_mod_manager* manager;
    pz_str mods_dir;              /* config.user_mods_dir */
    pz_str game_version;          /* copied from manager */

    pz_vec log;                   /* pz_str, cap 300 */
    pz_str input;
    int busy;
    int app_update_available;

    pz_vec selected_ids;          /* uint64_t */
    pz_vec selected_folders;      /* pz_str */
    int confirm_remove;

    pz_str filter_text;
    int filter_mode;
    UiFilterCounts filter_counts;
    int filter_menu_open, filter_menu_hover, filter_focus;
    int sort_mode, list_offset;
    int scroll_drag, scroll_drag_y, scroll_drag_offset;

    pz_vec rows;                  /* UiRow, all tracked/manual rows */
    pz_vec visible;               /* UiRow* or indices */
    int list_dirty;

    int settings_open, settings_dark, settings_auto, settings_field;
    pz_str settings_zomboid, settings_path, settings_root, settings_user;

    int targets_open, target_focus, target_offset;
    pz_vec activation;            /* pz_activation_target_data */
    pz_vec target_mod_ids;        /* pz_str */
    pz_vec row_mod_ids;           /* pz_vec of pz_str per row */
    pz_vec row_enabled;           /* int */

    pz_str dependency_warning;
    pz_str dependency_signature;

    pz_vec info_cache;            /* { pz_str folder; pz_mod_info info; } */
    pz_vec icon_cache;            /* { pz_str folder; Pixmap; unsigned w,h; } */
    pz_vec target_icon_cache;     /* { pz_str path;   Pixmap; unsigned w,h; } */

    pthread_mutex_t pending_mu;
    pz_vec pending;               /* pz_str from the worker */
    int worker_done;
    pthread_t worker;
    int worker_joinable;
    int wake[2];
    int watch_fd;
    int reload_pending;

    /* clipboard, hover, last click, frame_pending, running ... */
} PzX11Ui;
```

`pz_vec` has no element destructor. Write explicit `rows_free`, `infos_free`,
`icons_free`, and `targets_free` helpers and call them on reload and shutdown.

A lower-effort implementation may use fixed-capacity arrays for rows, log
lines, info, and icons (`#define PZ_UI_MAX_ROWS 1024`, etc.) to avoid dynamic
ownership. If so, keep the caps generous and document truncation. The shared
`pz_str`/`pz_vec` path is preferred because the manager already returns them.

## Translation rules

| C++ | C99 replacement |
| --- | --- |
| `std::string` | `pz_str` + `pz_str_assign/append/append_u64/free`; temporary slices use `const char*` + `size_t` |
| `std::vector<T>` | `pz_vec` with `item_size=sizeof(T)`; `pz_vec_push[_copy]`, `pz_vec_at[_const]` |
| `std::optional<T>` | `int` success return + out parameter |
| `std::string_view` | `const char*` + length |
| `auto`, range-for | explicit types and index loops |
| `std::min/max` | local `min_int`/`max_int` helpers |
| `std::to_string` | `pz_str_append_u64` or `snprintf` into a small buffer |
| `std::find` | linear loop; sets/maps are small |
| `pz::flat_map`/`flat_set` | `pz_vec` + linear lookup |
| `pz::thread` | `pthread_create`/`pthread_join` |
| `pz::mutex`/`lock_guard` | `pthread_mutex_t` + explicit lock/unlock |
| `enum class` | `enum` |
| `fs::path` | `pz_path` (`pz_path_assign/join/free`) |
| lambdas | `static` functions taking `PzX11Ui*` |
| `std::fprintf(stderr, ...)` | `fprintf(stderr, ...)` (X11 only; no size concern) |

Do not introduce new C++ headers into the C file. Include `<X11/Xlib.h>`,
`<X11/Xatom.h>`, `<X11/Xutil.h>`, `<X11/keysym.h>`, `<poll.h>`, `<pthread.h>`,
and the core C headers.

## Port order and checkpoints

Each phase must build and pass `ctest` before the next one. Do not delete the
C++ body until the C window opens and the main screen paints.

### Phase 0 — bridge and empty window

1. Add `src/ui/x11/x11_ui_c.h`:
   ```c
   int pz_x11_run_ui(pz_mod_manager* manager);
   ```
2. Replace `x11_ui.cpp` with the 8-line bridge above.
3. Add `x11_ui_c.c` with `pz_x11_run_ui` that opens the display, creates the
   window, prints an error if `XOpenDisplay` fails, and returns 0.
4. Add `src/ui/x11/x11_ui_c.c` to the Linux `pzmodmgr` sources in
   `CMakeLists.txt`.
5. Build and run under Xvfb. Acceptance: an empty `PZ Mod Manager` window
   exists and the CLI still prints `OK: 0 mod(s)`.

### Phase 1 — shared leaves

1. Create `src/ui/glyphs_c.h` from `glyphs.hpp`: one `const char* const
   rows[16]` per glyph, an enum for the 16 glyphs, and a loop replacing
   `glyphs::visit`.
2. Port the model helpers from `ui/ui.hpp`:
   `ascii_lowercase`, `ascii_less`, `format_update_date`,
   `stable_sort_rows`, `join_workshop_ids`, `folder_token`,
   `activation_name`, `activation_count`, `enabled_in`, `mod_filter_counts`,
   `mod_filter_available`, `mod_filter_label`, `dependency_notice`,
   `mod_meta`.
3. Port the drawing primitives from `x11_ui.cpp` lines 90–355: `fill`, `frame`,
   `text`, `text_width`, `baseline_for`, `elide`, `elide_tail`,
   `clean_pasted_text`, `clean_workshop_paste`, `visual_pixel`,
   `draw_button_icon`, `make_icon_image`, `make_icon_pixmap`.
4. Build. Acceptance: no C++ UI logic is used by the C file; no warnings in the
   new C code.

### Phase 2 — palette and main screen

Port constants lines 12–70, `Palette`, `alloc_color`, fonts, back buffer,
`redraw`'s main-screen branch, `draw_input`, `draw_button`, `button_rect`,
`button_enabled`, `button_label`, and the filter menu geometry.

At this phase rows can be empty and selections inert. Acceptance: the full
static layout (brand mark, fields, buttons, activity panel, status) paints under
Xvfb.

### Phase 3 — data, rows, info, icons

1. `rebuild_visible`: `list_all` → rows grouped by workshop ID (manual rows have
   id 0), update/lock flags, filter counts, sort by name/updated, clamp offset.
2. `folder_info`/`get_info`: cache `pz_mod_info` by folder using
   `pz_mod_info_read`.
3. `get_icon`/`get_target_icon`: decode with `pz_mod_icon_load`, upload to a
   pixmap, keep only the pixmap. Free pixmaps on theme/reload/shutdown.
4. Compute `row_mod_ids` from each row's folders and `row_enabled` with
   `enabled_in`.
5. Draw row cards, badges, ON/OFF pills, scrollbar.
6. Acceptance: with a temporary `HOME` containing `Zomboid/mods`, rows and icons
   render; `list` still matches the CLI.

### Phase 4 — commands and worker

1. Port `start_command`, `submit`, `build_command`, `remove_selected`,
   `copy_ids`, and the worker.
2. Use `pthread_create`; the worker calls `pz_command_run` with a `pz_log`
   whose callback locks `pending_mu`, copies the line, and writes one byte to
   `wake[1]`.
3. On `wake[0]`, drain pending lines, join the worker, clear `busy`, refresh
   `app_update_available`, reload installed state if requested, clear caches,
   and repaint.
4. If `pthread_create` fails, clear `busy` and log `ERROR: cannot start worker`.
5. Acceptance: Install/Import/Update/Remove/Check/Self-update run off the UI
   thread; activity lines arrive; the UI never blocks on `poll`.

### Phase 5 — settings and targets

Port `open_settings`, `save_settings`, `open_targets`, target rows, lock
checkbox, Enable/Disable, Share targets, Copy names, and Close. Settings saves
must issue the six config commands and update the palette/watch/reload path on
success. Target writes must go through `set-enabled`/`target-lock` commands, not
direct manager mutation.

### Phase 6 — input, clipboard, watch

Port mouse hover/click/wheel/drag, `XLookupString` keyboard handling, paste via
`XConvertSelection` with UTF8_STRING → XA_STRING fallback, SelectionRequest
replies, `inotify` of `data_dir` for `installed.tsv`, and the daily `check`
after the first Expose.

### Phase 7 — finish

Delete the old C++ body (keep only the bridge), remove `glyphs.hpp` if nothing
else uses it, update `docs/c99-migration.md`, rebuild, run the Xvfb smoke test,
and compare `ldd`/`nm` before and after. The Win32 UI remains C++.

## Constants to copy

From `x11_ui.cpp` lines 12–70, keep these values unchanged:

```
kW=920 kH=720 kMargin=24
kInputX=24 kInputY=84 kInputW=524 kInputH=38
kInstallX=560 kInstallW=212 kImportX=784 kImportW=112
kFilterY=158 kFilterH=24 kFilterW=180
kFilterMenuX=312 kFilterMenuW=170 kFilterMenuRowH=26
kListTop=192 kRowH=64 kRowGap=6 kRows=5 kCardW=872
kScrollX=902 kScrollW=6 kScrollMinThumb=24 kScrollGrab=3
kActionY=554 kActionH=32
kActivityY=602 kActivityH=80 kActivityLines=3
kActivityScrollX=884 kStatusY=702
kSettingsX=130 kSettingsY=140 kSettingsW=660 kSettingsH=500
kSettingsFieldX=162 kSettingsFieldW=596 kSettingsFieldH=38
```

Keep the color triplets and dark-theme triplets from lines 370–445.

## Pitfalls

- **Ownership:** `pz_mod_manager_list_all`, `_activation_targets`, and
  `_missing_requirements` return initialized vectors. Release them with the
  matching `_free_*`; `pz_vec_free` alone leaks `pz_str` elements.
- **Info cache:** `pz_mod_info_read` calls `pz_mod_info_free(out)` before
  writing, so initialize `out` first. The resolved `icon` is an absolute
  `pz_str` path.
- **Paths:** `pz_path` stores UTF-8. Never pass `fs::path` or `wchar_t` into the
  C UI. Use `pz_path_join` for `mods_dir + folder`.
- **Worker:** the log callback runs on the worker. It must not touch `Display`,
  fonts, pixmaps, or the row cache. Lock, copy, write the self-pipe, return.
- **Result messages:** `pz_mm_result.message` is owned; free with
  `pz_mm_result_free`. Do not log the pointer after the free.
- **X11 resources:** `XDestroyImage` is for `XImage`; pixmaps use
  `XFreePixmap`; fonts use `XFreeFont`; the back buffer is a pixmap.
- **Selection/clipboard:** retain the copied text until `SelectionClear`; reply
  to `SelectionRequest` with property `None` on unsupported targets.
- **Fonts:** fall back from the fixed 13px pattern to `"fixed"`. If no font
  loads, close the display and the wake pipe before returning 1.
- **poll:** `watch_fd` may be -1. Keep `nfds` at 2 when it is, and handle
  `EINTR`.
- **Fixed caps:** if you choose fixed arrays, truncate with a NUL and never
  `strcpy`; command lines can exceed 4 KB with many selected IDs.
- **Do not** call `run_command`/`pz_command_run` on the UI thread. The old UI
  avoided it and the AGENTS rule still applies.

## Verification

Build and tests:

```bash
cmake -S . -B /tmp/pzmodmgr-x11-c99 -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/pzmodmgr-x11-c99 -j2
ctest --test-dir /tmp/pzmodmgr-x11-c99 --output-on-failure
```

Headless window check:

```bash
Xvfb :99 -screen 0 1280x800x24 &
DISPLAY=:99 /tmp/pzmodmgr-x11-c99/pzmodmgr list
DISPLAY=:99 /tmp/pzmodmgr-x11-c99/pzmodmgr &
sleep 1
DISPLAY=:99 xdotool search --name "PZ Mod Manager"
DISPLAY=:99 import -window root /tmp/pzmodmgr-x11.png
```

The CLI must still print `OK: 0 mod(s)` with a fresh `HOME`. For a row test,
create `$HOME/Zomboid/mods/Fixture/mod.info` with `name=Fixture` and
`id=fixture`, then start the GUI and confirm the row appears.

Finish with:

```bash
ldd /tmp/pzmodmgr-x11-c99/pzmodmgr
nm -C /tmp/pzmodmgr-x11-c99/pzmodmgr | grep -c 'std::' || true
```

Report the counts honestly; the `ModManager` bridge and Win32 UI still keep
some C++ runtime symbols in the Linux binary until `main.cpp`/the facade move.

## Definition of done

- `src/ui/x11/x11_ui.cpp` is a bridge of at most ~20 lines, or is deleted after
  `main.cpp` moves to C.
- `src/ui/x11/x11_ui_c.c` is C99, compiles with the project's C standard, and
  contains no `std::`, lambdas, classes, or C++ headers.
- The window renders at 920×720 with the same layout, theme toggle, rows,
  filters, settings, targets, clipboard, and activity behavior.
- Commands run on a worker thread; the UI remains responsive.
- All 10 CTest tests pass; the Xvfb smoke test opens the window and exits
  cleanly.
- `docs/c99-migration.md` marks the X11 step and the final C++ removal done.

## Follow-up work

The X11 and Win32 UI ports, the C entry-point conversion, and the test-suite
conversion are finished. `src/main.c` and `src/app/app_c.c` call
`pz_x11_run_ui(pz_mod_manager*)` directly. The repository no longer contains
C++ sources, and CMake enables only C.

Known limitations:

- At 512 mods the C UI uses about 0.7 MB more RSS than the old C++ UI because
  each row keeps fixed inline folder buffers. Total RSS is still under 5 MB.
- Linux stripped size is 194,560 bytes, down from 231,424 before the C entry
  point/facade move (-15.9%).
- ImageMagick `import` cannot capture X11 here; `scrot` was used instead.
- The final pre-C99 and C99 Xvfb screenshots are pixel identical at 1100×900.
