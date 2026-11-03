# AGENTS.md

Instructions for AI coding agents working in this repository.

## Project

`pzmodmgr` is a native C++17 mod installer and manager for Project Zomboid.

- Downloads Steam Workshop items with a built-in `steamcmd` (app 108600), bootstrapped from Valve into `~/Zomboid/pzmodmgr/steamcmd` on first use.
- Copies the mod files into the user mods folder `~/Zomboid/mods` (Windows: `%USERPROFILE%\Zomboid\mods`).
- Works for GOG and Steam installs, because both use the same `Zomboid` user folder. The game client is never touched.
- Native UIs: Win32 (Windows) and Xlib (Linux/X11). A CLI mode runs the same commands.
- Current input is a pasted Workshop URL or numeric ID (bare input means `add`). Collections are expanded via the Steam Web API and every child item is installed. Search is not implemented and is out of scope for now.
- Steam Web API calls use the system `curl` binary; no libcurl link dependency.

## Layout

- `CMakeLists.txt`: builds `pzcore` (static lib) and `pzmodmgr` (exe). Selects the UI per platform.
- `src/core/`: platform-neutral logic. No UI headers, no OS-specific UI calls.
- `src/core/steamcmd.cpp`: the only place that spawns processes; also bootstraps the built-in steamcmd.
- `src/native/native_host.cpp`: Firefox native messaging stdio host; stdout is protocol-only.
- `firefox/`, `scripts/install_firefox_host.sh`: right-click add-on and its registration.
- `src/core/mod_manager.cpp`: install, remove, update, the `installed.tsv` database, and backups.
- `src/core/commands.cpp`: the command parser shared by CLI and both UIs.
- `src/ui/win32/`, `src/ui/x11/`: UI code only. They call `run_command`, never core internals directly.

## Build and run

```bash
cmake -B build && cmake --build build
./build/pzmodmgr config steam_user <steam_login_name>
./build/pzmodmgr add https://steamcommunity.com/sharedfiles/filedetails/?id=123456789

The first add downloads the built-in steamcmd into ~/Zomboid/pzmodmgr/steamcmd.
`anonymous` works for public Workshop items. Set `steamcmd_exe` to use an
existing installation instead.

Windows: use MinGW-w64 or MSVC. The built-in copy goes to
%USERPROFILE%\Zomboid\pzmodmgr\steamcmd and mods to
%USERPROFILE%\Zomboid\mods.
Rules for agents

    Keep core free of UI code. Anything the user can do goes through run_command.
    Keep stdout clean in the native host: only framed JSON messages, logs go in the response.
    Never store, log, or pass a Steam password. Authentication relies on steamcmd's cached login. Do not add +login user pass.
    Never delete a folder in ~/Zomboid/mods that the manager did not install (check installed.tsv first). Back up before replacing, as install() does now.
    Never write outside ~/Zomboid (the mods folder and ~/Zomboid/pzmodmgr) and the user-set steamcmd path. The built-in steamcmd lives in ~/Zomboid/pzmodmgr/steamcmd.
    UI actions that call steamcmd must not block the UI thread. Use a worker thread and marshal logs back (PostMessage on Win32, a self-pipe on X11).
    Quote all paths and arguments when building shell commands. Do not concatenate raw user input into a command without the quote() helper.
    Keep the code C++17 and dependency-free beyond the OS SDK and Xlib.
    Do not invent steamcmd output formats. Verify the content path against a real download before changing steamcmd.cpp.
    Add a test for any change to parse_workshop_id, discover_mod_roots, or read_mod_info.

Open TODO (priority order)

    Steam Guard: detect the prompt and tell the user to run steamcmd manually once.
    Support custom Zomboid locations (-cachedir=).
    Add fixture-based tests for parse_workshop_id, discover_mod_roots, and read_mod_info.
    Optional: hide the console on Windows (-mwindows or /SUBSYSTEM:WINDOWS).

Style

    namespace pz, 4-space indent, #pragma once headers.
    Return Result{ok, message} from commands. Do not throw across UI boundaries.
    Log user-facing progress through the LogFn callback.
    