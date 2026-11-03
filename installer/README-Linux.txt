PZ Mod Manager 0.2.0 for Linux
==============================

PZ Mod Manager installs Project Zomboid Steam Workshop items and collections
into your real user mods folder:

    ~/Zomboid/mods

The game client is never modified. The same folder is used by Steam and GOG
installs of Project Zomboid.

Install
-------
Run the self-contained installer:

    sh PZModManager-0.2.0-linux-x86_64.sh

It installs for the current user (no sudo) into:

    ~/.local/share/pzmodmgr

It adds:

    ~/.local/bin/pzmodmgr
    ~/.local/share/applications/pzmodmgr.desktop
    ~/.local/share/icons/hicolor/256x256/apps/pzmodmgr.png

Options:

    --yes             accept the defaults, no prompts
    --desktop         also create ~/Desktop/PZ Mod Manager.desktop
    --no-desktop      do not create a desktop shortcut
    --no-menu         do not create the application-menu entry
    --no-browsers     do not register Firefox/Chrome native hosts
    --prefix=DIR      install into a different directory
    --uninstall       run the installed uninstaller

Uninstall
---------
Run the uninstaller:

    ~/.local/share/pzmodmgr/uninstall.sh

or rerun the original installer with --uninstall. Your mods and settings under
~/Zomboid are kept.

First run
---------
The app downloads Valve's official steamcmd into:

    ~/Zomboid/pzmodmgr/steamcmd

A fresh install seeds `~/Zomboid/pzmodmgr/config.ini` with "anonymous", so
public Workshop items work without opening Settings. Change the Steam username
in Settings for restricted items. No password is stored; steamcmd uses its
cached login.

Downloaded files are moved out of the steam cache into the mods folder, so
they are not stored twice. Existing mod folders are backed up under:

    ~/Zomboid/pzmodmgr/backups

Browser add-ons
---------------
The installer offers to register native messaging hosts for Firefox and Chrome.
Load the extension once:

Firefox:
  about:debugging#/runtime/this-firefox -> Load Temporary Add-on ->
  firefox/manifest.json

Chrome:
  chrome://extensions -> enable Developer mode -> Load unpacked ->
  chrome-extension

The extensions are installed inside the PZ Mod Manager directory. Chrome does
not allow silent sideloading; load the unpacked extension once with **Load
unpacked** on chrome://extensions.

Command line
------------
  pzmodmgr add <workshop url or id>
  pzmodmgr update [id ...]
  pzmodmgr remove <id ...>
  pzmodmgr list
  pzmodmgr config steam_user <name>

A bare Workshop URL or ID also acts as add. Updating uses the Steam Web API to
skip unchanged mods.

License
-------
GPL-3.0-only. See LICENSE.txt.
