# pzmodmgr

See AGENTS.md for architecture, rules, and the TODO list.

Quick start:

cmake -B build && cmake --build build
./build/pzmodmgr config steam_user <steam_login_name>
./build/pzmodmgr add <workshop item or collection url/id>

The first add downloads the built-in steamcmd; no separate install is needed.
`steam_user` can be `anonymous` for public Workshop items. To use an existing
steamcmd instead, set it with `config steamcmd_exe /path/to/steamcmd.sh`.

A bare Workshop URL or ID can also be pasted without `add`. A collection
installs every child item it contains.

The GUI is a native utility on both platforms: an accent-orange install action,
a card list with each mod's `icon=` PNG, author and `modversion`, Ctrl/Shift
multi-select, double-click to update, and a compact activity log. The list can be
filtered by name/author/version/folder, restricted to mods with an available
update, and sorted by name or last update. `Check` refreshes update availability
through the Steam Web API; `UPDATE` badges mark the results. Installs and updates
are skipped when `versionMin`/`versionMax` target a different major Project
Zomboid build (41, 42, 43), so a newer Workshop item cannot replace a working mod
with one that does not support the installed game. X11 adds Ctrl+V / Shift+Insert
/ middle-click paste; Win32 keeps the native edit and list controls. The UI is
drawn with Win32 and Xlib rather than a cross-platform toolkit, keeping the
downloads small and dependency-free; `third_party/stb_image.h` is compiled in
only to decode mod icons. The CLI mode still accepts all commands. No password
is stored: run steamcmd manually once so its login is cached.

Normal runs use the real `~/Zomboid/mods` folder (`%USERPROFILE%\Zomboid\mods`
on Windows). steamcmd is built in: on first use the app downloads Valve's
official steamcmd into `~/Zomboid/pzmodmgr/steamcmd` (Windows:
`%USERPROFILE%\Zomboid\pzmodmgr\steamcmd`). Settings can point at an existing
installation; a system install is only a fallback when the download fails.
Update checks and the steamcmd download use `curl` on PATH (bundled with
Windows 10+; install curl on Linux).

Firefox right-click add-on:

```bash
cmake --build build --target pzmodmgr_native_host
sh scripts/install_firefox_host.sh
# then load firefox/manifest.json from about:debugging
```

See `firefox/README.md` for native-host registration details.

Tests (Linux; use temp Zomboid dirs + fake steamcmd/curl, no Steam account needed):

ctest --test-dir build --output-on-failure

Windows distribution (portable ZIP + setup EXE):

```bash
UPX_BIN=/path/to/upx ./scripts/package_windows.sh   # optional compression
```

The setup reuses a registered previous install directory, so a newer setup
upgrades in place instead of creating a second copy. The Linux installer follows
the existing `~/.local/bin/pzmodmgr` symlink for the same behavior.

Linux distribution (single self-extracting `.sh` with a `.desktop` entry):

```bash
./scripts/package_linux.sh
./dist/PZModManager-0.2.0-linux-x86_64.sh
```

## License

GPL-3.0-only. See `LICENSE`.

