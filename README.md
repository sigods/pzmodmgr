# pzmodmgr

See AGENTS.md for architecture, rules, and the TODO list.

Quick start:

cmake -B build && cmake --build build
./build/pzmodmgr config steam_user <steam_login_name>
./build/pzmodmgr config zomboid_dir /path/to/Zomboid   # optional
./build/pzmodmgr add <workshop item or collection url/id>...
./build/pzmodmgr config steamcmd_root /path/to/steamcmd   # for import
./build/pzmodmgr import

The first add downloads the built-in steamcmd; no separate install is needed.
`steam_user` can be `anonymous` for public Workshop items. To use an existing
steamcmd instead, set it with `config steamcmd_exe /path/to/steamcmd.sh`.

A bare Workshop URL or ID can also be pasted without `add`. Several URLs or IDs
can be supplied at once, separated by spaces or newlines. A collection installs
every child item it contains.

The GUI is a native utility on both platforms: an accent-orange install action,
a card list with each mod's `icon=` PNG, folder, enabled `ON`/`OFF` state,
click-to-toggle selection (Shift-click spans a range), double-click to update,
and a compact activity log. The list can be
filtered by name or folder, and filtered from a dropdown to Updates, Locked, or
Nontracked mods; an option only appears while at least one matching mod exists.
The list can be sorted by name or last update. Settings also switches the UI
between light and dark themes; the choice is stored in `config.ini`. `Check`
refreshes update availability
through the Steam Web API; `UPDATE` badges mark the results. On GUI start,
`Check` runs once when the last mod or app-release check is more than a day old,
even with no mods installed. It also checks the latest GitHub release, so
`Update app` is enabled and green only when a newer version exists. Reopening
the GUI within a day does not check again; the manual `Check` button always
works. Checks query up to four items at a time, while steamcmd downloads remain
serial. An open window watches `installed.tsv` with OS file notifications, so
installs from the CLI or the browser native host appear immediately without
polling. Game-compatible variants are preferred; an older-build mod is still
installed with an activity warning instead of being blocked. X11 adds Ctrl+V / Shift+Insert
/ middle-click paste; Win32 keeps the native edit and list controls. The UI is
drawn with Win32 and Xlib rather than a cross-platform toolkit, keeping the
downloads small and dependency-free; `third_party/stb_image.h` is compiled in
only to decode mod icons. The CLI mode still accepts all commands. `Enable...` changes
`mods/default.txt`, a save's `mods.txt`, or a server INI. The block files use
plain Mod IDs on both builds; B42's leading backslash is only for a server
INI's `Mods=` line. Multi-folder Workshop items enable only the newest
game-compatible variant. Manual mods that are not
in `installed.tsv` are listed too and can be enabled or disabled by folder.
Each target has a `Lock updates` switch, off by default, including
`mods/default.txt`, saves, and server INIs. When it is on, items enabled in that
target are pinned: they are not checked for updates, are removed from
`updates.tsv`, and `Update` / `Update all` skip them. The `Lock` / `Unlock`
action stores a separate manual lock as the sixth `installed.tsv` column and
survives restarts. The seventh column records whether a copy came from
SteamCMD import. Turning the target switch off releases its automatic lock;
removing a server's `Mods=` entry also stops it from applying. Settings stores a
SteamCMD download root for the built-in `Import` action. With an empty input it
reads `steamapps/workshop/appworkshop_108600.acf` when present, otherwise the
numeric folders under `steamapps/workshop/content/108600/`. With Workshop URLs
or IDs in the input, it imports only those items. That path never invokes
steamcmd. If the download root is unavailable or the item has no content
folder, Import asks the public Steam Web API for the Workshop title (no API key
or login), matches it case-insensitively against the folder name or `name=` in
`mod.info`, and tracks only one exact untracked match without moving or
rewriting it. Ambiguous title matches are skipped. With an empty input it
still scans the configured download root. Only items with a readable
`mod.info` are copied into `~/Zomboid/mods`; already tracked or installed IDs are
skipped, and directory names still present in an emptied SteamCMD content folder
are matched against `Zomboid/mods` so manually copied mods are not duplicated.
The SteamCMD tree is strictly read-only: import never moves, rewrites, or
deletes anything there, including when an imported copy is later removed.
Imported records keep the ACF or Steam API `timeupdated` value and are marked
`[imported]`.
`Share server`
copies the server's
enabled Workshop IDs as a space-separated list that can be pasted into `Add` on
another instance. To pin those same items there, run `pzmodmgr lock <ids>` or
prefix the pasted list with `lock`; installed folder or mod names are accepted
too (`lock "My Mod"`). Removing an item first
disables its Mod IDs in `mods/default.txt`, every save's `mods.txt`, and every
server `.ini`; removal stops if one of those files cannot be written. No password
is stored: run steamcmd manually once so its login is cached.

Normal runs use the real `~/Zomboid/mods` folder (`%USERPROFILE%\Zomboid\mods`
on Windows). For a custom Zomboid location (for example a game launched with
`-cachedir=`), pass `--zomboid-dir=/absolute/path` to the Linux installer or
run the Windows setup with `/ZOMBOIDDIR=C:\absolute\path`; it can also be
changed later in Settings. Installed builds read and write `config.ini` next to
the executable, so no bootstrap config is created in the default Zomboid folder.
The manager then uses that folder's `mods` and `pzmodmgr` subfolders;
`installed.tsv`, `updates.tsv`, downloads, the temporary copy used during a replacement,
`last_check`, `last_app_check`, and the cached app release state follow the
configured folder. The old copy is deleted after a successful swap, so updates
do not leave a growing backup pile. Source/portable builds without an adjacent `config.ini` keep the old
`~/Zomboid/pzmodmgr/config.ini` fallback. steamcmd is built in: on first use the app downloads Valve's
official steamcmd into `<zomboid_dir>/pzmodmgr/steamcmd`. Settings can point at
an existing installation; a system install is only a fallback when the download
fails. Update checks and the steamcmd download use `curl` on PATH (bundled with
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
UPX_BIN=/path/to/upx ./scripts/package_windows.sh   # optional pzmodmgr.exe + setup compression
```

The setup reuses a registered previous install directory, so a newer setup
upgrades in place instead of creating a second copy. The Linux installer follows
the existing `~/.local/bin/pzmodmgr` symlink for the same behavior. The Windows
package script prefers the POSIX-threaded MinGW compiler
(`x86_64-w64-mingw32-g++-posix`); on Debian/Ubuntu install
`g++-mingw-w64-x86-64-posix`. The release workflow also installs `upx-ucl`,
packs `pzmodmgr.exe` and the setup executable, and verifies the release
artifacts and checksums before creating the release.

An installed or portable copy can update itself from the latest GitHub release.
The app checks daily with the mod updates; `Update app` is enabled and green
only when a newer release is available. Use it in either native UI, or run:

```bash
./pzmodmgr self-update
```

A completed update removes its staged release immediately; abandoned release
folders and crashed download caches are also pruned on startup. Manager-created
backups older than a week and downloads older than a day are removed. A staged
update whose helper is still running is always preserved.

Releases are published by `.github/workflows/release.yml`. Push a `v*` tag that
matches the CMake project version; the workflow builds and attaches the Linux
`.sh`/`.tar.gz` and Windows `.zip`/setup artifacts plus `SHA256SUMS`.

To publish a pre-release without making it the updater's latest release, set the
repository variable `PRERELEASE=true` before pushing the tag and clear it after
the workflow. GitHub's `/releases/latest` endpoint excludes pre-releases.

The release archive is checked against GitHub's SHA-256 digest when one is
available. A detached helper then waits for pzmodmgr to close, swaps the whole
installation folder, keeps `config.ini` and uninstall metadata, and restarts
the new version. The helper needs write access beside the executable for the
temporary sibling folders used during the swap.

Linux distribution (single self-extracting `.sh` with a `.desktop` entry):

```bash
./scripts/package_linux.sh
./dist/PZModManager-0.3.0-linux-x86_64.sh
```

GCC/Clang Release builds (Linux and the MinGW Windows build) default to size
optimization (`-Os`) with function/data sections and section GC. Configure with
`-DPZMODMGR_SIZE_OPT=OFF` to use the normal Release optimization flags for a
performance build.

## License

GPL-3.0-only. See `LICENSE`.

