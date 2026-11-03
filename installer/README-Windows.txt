PZ Mod Manager 0.2.0 for Windows
================================

PZ Mod Manager installs Project Zomboid Steam Workshop items and collections
into your real user mods folder:

    %USERPROFILE%\Zomboid\mods

The game client is never modified. The same folder is used by Steam and GOG
installs of Project Zomboid.

Installer
---------
Run PZModManager-0.2.0-win64-setup.exe. It installs for the current Windows
user, so no administrator rights are needed. The default location is:

    %LOCALAPPDATA%\Programs\PZModManager

Options:
  * Start Menu shortcut
  * Desktop shortcut
  * Firefox right-click add-on host
  * Chrome right-click add-on host
  * Launch PZ Mod Manager

Uninstall from Settings > Apps > Installed apps > PZ Mod Manager. Your mods
and settings under %USERPROFILE%\Zomboid are kept.

Portable build
--------------
Extract the ZIP and run pzmodmgr.exe. The portable build does not register
browser native hosts; use the installer if you want the right-click add-ons.

First run
---------
The app downloads Valve's official steamcmd into:

    %USERPROFILE%\Zomboid\pzmodmgr\steamcmd

A fresh install seeds `%USERPROFILE%\Zomboid\pzmodmgr\config.ini` with
"anonymous", so public Workshop items work without opening Settings. Change
the Steam username in Settings for restricted items. No password is stored;
steamcmd uses its cached login. If a password or Steam Guard prompt is needed,
run steamcmd manually once, then try again.

Downloaded files are moved out of the steam cache into the mods folder, so
they are not stored twice. Existing mod folders are backed up under:

    %USERPROFILE%\Zomboid\pzmodmgr\backups

Browser add-ons
---------------
The installer registers the native messaging host for Firefox and Chrome.
Load the extension once:

Firefox:
  about:debugging#/runtime/this-firefox -> Load Temporary Add-on ->
  firefox\manifest.json

Chrome:
  chrome://extensions -> enable Developer mode -> Load unpacked ->
  chrome-extension

Chrome does not allow silent sideloading on Windows. The setup registers the
native host, but Chrome still requires the one-time **Load unpacked** action;
there is no way to install an unpacked extension from an installer. The
extension ID is pinned to medndgoffhfpiabahjpdkfihnhdcmhoo by its manifest key.

Command line
------------
  pzmodmgr.exe add <workshop url or id>
  pzmodmgr.exe update [id ...]
  pzmodmgr.exe remove <id ...>
  pzmodmgr.exe list
  pzmodmgr.exe config steam_user <name>

A bare Workshop URL or ID also acts as add. Updating uses the Steam Web API to
skip unchanged mods. API calls and the one-time steamcmd download use curl.exe
from Windows 10 or later.

Code signing
------------
These builds are not code-signed. Windows SmartScreen may show a warning for
an unsigned executable. Verify SHA256SUMS if you downloaded the release from a
mirror.

License
-------
GPL-3.0-only. See LICENSE.txt.
