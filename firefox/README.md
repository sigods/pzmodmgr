# Firefox add-on

Right-click a Project Zomboid Workshop item or collection and choose
**Add or update in PZ Mod Manager**. The add-on sends the URL to the native
host, which installs a new item or checks an installed one for an update. It
installs into the real `~/Zomboid/mods` folder (or
`%USERPROFILE%\Zomboid\mods` on Windows). The action is always present on
Workshop pages; the host decides whether to add or update, so the menu never
shows a stale state.

## Linux setup

1. Build the host:

   ```bash
   cmake -B build && cmake --build build --target pzmodmgr_native_host
   ```

2. Register it for Firefox (writes only the native-host manifest):

   ```bash
   sh scripts/install_firefox_host.sh
   ```

   It writes `~/.mozilla/native-messaging-hosts/pzmodmgr.json` and points it at
   the built host. Rerun it if you move the binary.

3. Load the add-on temporarily: open `about:debugging#/runtime/this-firefox`,
   click **Load Temporary Add-on**, and pick `firefox/manifest.json`.

4. Make sure the app is configured: open the GUI **Settings** and set the
   steamcmd executable and Steam username. The native host reads
   `~/Zomboid/pzmodmgr/config.ini`; no password is stored.

Leave the steamcmd path empty to use the built-in copy. The app/host downloads
Valve's official steamcmd into `~/Zomboid/pzmodmgr/steamcmd` on first use, or
uses a system installation as a fallback.

## Windows

Build `pzmodmgr_native_host.exe`, write the same manifest to a stable location,
and register it under
`HKCU\Software\Mozilla\NativeMessagingHosts\pzmodmgr` with the manifest path as
the default value. The manifest path must be absolute:

```json
{
  "name": "pzmodmgr",
  "description": "Project Zomboid Mod Manager native host",
  "path": "C:\\path\\to\\pzmodmgr_native_host.exe",
  "type": "stdio",
  "allowed_extensions": ["pzmodmgr@local"]
}
```

The add-on itself is loaded the same way on both platforms.

## Notes

- The extension ID is `pzmodmgr@local`; it must match `allowed_extensions`.
- Native messaging requires the host manifest to be installed before Firefox
  starts the add-on.
- A single Workshop add runs synchronously in the host. The browser stays
  usable; the notification appears when the install finishes.
- Large collections can take a long time because every child item is downloaded
  sequentially. The manager's GUI shows progress if you run it there instead.
