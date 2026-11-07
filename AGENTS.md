# AGENTS.md

Instructions for AI coding agents working in this repository.

## Project

`pzmodmgr` is a native C++17 mod installer and manager for Project Zomboid.

- Downloads Steam Workshop items with a built-in `steamcmd` (app 108600), bootstrapped from Valve into `~/Zomboid/pzmodmgr/steamcmd` on first use.
- Copies the mod files into the user mods folder `~/Zomboid/mods` (Windows: `%USERPROFILE%\Zomboid\mods`), or `<zomboid_dir>/mods` when a custom base folder is configured.
- Works for GOG and Steam installs, because both use the same `Zomboid` user folder. The game client is never touched.
- Native UIs: Win32 (Windows) and Xlib (Linux/X11). A CLI mode runs the same commands.
- Current input is a pasted Workshop URL or numeric ID (bare input means `add`). Collections are expanded via the Steam Web API and every child item is installed. Local filtering of installed mods is implemented; Workshop search is out of scope for now.
- Update checks use the Steam Web API. Installed cards show author and `modversion`; `versionMin`/`versionMax` are treated as major game builds (41/42/43) when deciding whether an install/update is compatible.
- A custom Zomboid base folder can be selected at install time (Linux `--zomboid-dir`; Windows `/ZOMBOIDDIR=`) or in both Settings dialogs. Installed builds read/write `config.ini` next to the executable; source/portable builds without that file use the old `~/Zomboid/pzmodmgr/config.ini` fallback. State follows `<zomboid_dir>/pzmodmgr`.
- GUI startup runs one update check when mods are installed and `last_check` is over 24 hours old. Checks use up to four parallel Steam API lookups; steamcmd downloads stay serial.
- Open GUI windows watch `installed.tsv` with inotify (X11) or `FindFirstChangeNotification` (Win32) and reload external changes, such as browser-extension installs, without polling.
- Steam Web API calls use the system `curl` binary; no libcurl link dependency.

## Layout

- `CMakeLists.txt`: builds `pzcore` (static lib) and `pzmodmgr` (exe). Selects the UI per platform.
- `src/core/`: platform-neutral logic. No UI headers, no OS-specific UI calls.
- `src/core/steamcmd.cpp`: the only place that spawns processes; also bootstraps the built-in steamcmd.
- `src/native/native_host.cpp`: Firefox native messaging stdio host; stdout is protocol-only.
- `firefox/`, `scripts/install_firefox_host.sh`: right-click add-on and its registration.
- `src/core/mod_manager.cpp`: install, remove, update, the `installed.tsv` database, and the temporary swap copy.
- `src/core/commands.cpp`: the command parser shared by CLI and both UIs.
- `src/ui/win32/`, `src/ui/x11/`: UI code only. They call `run_command`, never core internals directly.
- `src/ui/win32/win32_entry.cpp`: the Windows GUI entry point (`wWinMain`). Decodes the UTF-16 command line, re-attaches a console for CLI mode, then calls `pz::run_app` from `src/main.cpp`.
- `src/ui/mod_icon.cpp`, `third_party/stb_image.h`: shared icon decoding for the two native UIs.
- `src/core/mod_info.cpp`: resolves the `icon=` PNG and version bounds from B41/B42 mod layouts.

## Build and run

```bash
cmake -B build && cmake --build build
./build/pzmodmgr config steam_user <steam_login_name>
./build/pzmodmgr config zomboid_dir /absolute/Zomboid   # optional
./build/pzmodmgr add https://steamcommunity.com/sharedfiles/filedetails/?id=123456789
```

```
The first add downloads the built-in steamcmd into <zomboid_dir>/pzmodmgr/steamcmd.
`anonymous` works for public Workshop items. Set `steamcmd_exe` to use an
existing installation instead.

Windows: use MinGW-w64 or MSVC. The built-in copy goes to
%USERPROFILE%\Zomboid\pzmodmgr\steamcmd and mods to
%USERPROFILE%\Zomboid\mods.

On Windows `pzmodmgr.exe` is a GUI-subsystem program, so starting it from
Explorer or a shortcut opens the manager window alone. CLI runs re-attach to the
console of the shell that started them (or allocate one when there is none), and
leave redirected output alone.

Rules for agents

    Keep core free of UI code. Anything the user can do goes through run_command.
    Keep stdout clean in the native host: only framed JSON messages, logs go in the response.
    Keep pzmodmgr a GUI-subsystem program on Windows (WIN32_EXECUTABLE on MSVC/MinGW). Do not add a console subsystem or a second console-only target; its entry point is wWinMain in src/ui/win32/win32_entry.cpp, which calls pz::run_app from main.cpp. pzmodmgr_native_host must stay console-subsystem.
    Never store, log, or pass a Steam password. Authentication relies on steamcmd's cached login. Do not add +login user pass.
    Never delete a folder in the mods folder (default ~/Zomboid/mods, or <zomboid_dir>/mods) that the manager did not install (check installed.tsv first). Move the old folder aside for the swap, restore it on failure, and delete it after success; do not retain persistent backups. A folder adopted from a manual install is recorded as external and is left in place on remove.
    Never write outside the Zomboid folder (default ~/Zomboid, or the user-set zomboid_dir), the user-set steamcmd path, and the installed config.ini next to the executable. The built-in steamcmd lives in <zomboid_dir>/pzmodmgr/steamcmd. Do not create a bootstrap config in the default Zomboid folder when an installed adjacent config.ini exists.
    UI actions that call steamcmd must not block the UI thread. Use a worker thread and marshal logs back (PostMessage on Win32, a self-pipe on X11).
    Quote all paths and arguments when building shell commands. Do not concatenate raw user input into a command without the quote() helper.
    Keep the code C++17 and dependency-free beyond the OS SDK, Xlib, and the vendored
    `stb_image` decoder used for mod icons.
    Do not invent steamcmd output formats. Verify the content path against a real download before changing steamcmd.cpp.
    Add a test for any change to parse_workshop_id, discover_mod_roots, or read_mod_info.

Open TODO (priority order)

    Verify the steamcmd force_install_dir layout and content path on a real install.
    Optional: improve diagnostics for users who deliberately configure a named Steam account (anonymous covers public Workshop items).

Style

    namespace pz, 4-space indent, #pragma once headers.
    Return Result{ok, message} from commands. Do not throw across UI boundaries.
    Log user-facing progress through the LogFn callback.
    