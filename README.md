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

The GUI has a Workshop URL/ID field with **Add / Install**, a multi-select list
of installed mods with details, **Update selected**, **Remove selected**,
**Update all**, and **Select all** actions, plus **Settings** for the steamcmd
path and Steam username. Win32 uses Ctrl/Shift-click and double-click to update;
X11 toggles rows, confirms a removal with a second click, and scrolls the list
with the mouse wheel. The CLI mode still accepts all commands. No password is
stored: run steamcmd manually once so its login is cached.

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
./scripts/package_windows.sh
```

Linux distribution (single self-extracting `.sh` with a `.desktop` entry):

```bash
./scripts/package_linux.sh
./dist/PZModManager-0.2.0-linux-x86_64.sh
```

## License

GPL-3.0-only. See `LICENSE`.

