# Chrome extension

Right-click a Project Zomboid Workshop item or collection and choose
**Add to PZ Mod Manager**. The extension sends the page URL to the same native
host as the Firefox add-on, which installs into the real
`~/Zomboid/mods` folder (or `%USERPROFILE%\Zomboid\mods` on Windows).

## Load the extension

1. Open `chrome://extensions`.
2. Enable **Developer mode**.
3. Click **Load unpacked** and select this folder.

The extension ID is pinned by the `key` in `manifest.json`:
`medndgoffhfpiabahjpdkfihnhdcmhoo`.

## Native host

The Windows setup and the Linux `.sh` installer register the native messaging
host for Chrome (and Chromium on Linux) when the Chrome option is selected.
The host manifest is `pzmodmgr-chrome.json` with:

```json
"allowed_origins": ["chrome-extension://medndgoffhfpiabahjpdkfihnhdcmhoo/"]
```

On Windows the registration is under
`HKCU\Software\Google\Chrome\NativeMessagingHosts\pzmodmgr` and points at the
installed `pzmodmgr-chrome.json`. On Linux it is written to
`~/.config/google-chrome/NativeMessagingHosts/pzmodmgr.json` (and Chromium's
equivalent when present).

Chrome does not allow silent sideloading on Windows. Load the unpacked
extension once as above; the native host is then ready. For an enterprise
deployment, package the extension and distribute it through Chrome policy.
