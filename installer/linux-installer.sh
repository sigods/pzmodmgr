#!/bin/sh
# PZ Mod Manager @VERSION@ Linux installer (self-contained).
set -eu

VERSION="@VERSION@"
APP="PZ Mod Manager"
ASSUME_YES=0
MODE=install
WANT_DESKTOP=no
WANT_MENU=yes
WANT_BROWSERS=yes
DESKTOP_OPTION=0
MENU_OPTION=0
BROWSERS_OPTION=0
PREFIX=
ZOMBOID_DIR=${PZMODMGR_ZOMBOID_DIR:-}
ZOMBOID_DIR_OPTION=0

for arg in "$@"; do
    case "$arg" in
        --yes|-y) ASSUME_YES=1 ;;
        --desktop) WANT_DESKTOP=yes; DESKTOP_OPTION=1 ;;
        --no-desktop) WANT_DESKTOP=no; DESKTOP_OPTION=1 ;;
        --no-menu) WANT_MENU=no; MENU_OPTION=1 ;;
        --no-browsers) WANT_BROWSERS=no; BROWSERS_OPTION=1 ;;
        --uninstall) MODE=uninstall ;;
        --prefix=*) PREFIX=${arg#--prefix=} ;;
        --zomboid-dir=*) ZOMBOID_DIR=${arg#--zomboid-dir=}; ZOMBOID_DIR_OPTION=1 ;;
        -h|--help) MODE=help ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

case "$(basename "$0")" in uninstall.sh) MODE=uninstall ;; esac

DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}
CONFIG_HOME=${XDG_CONFIG_HOME:-$HOME/.config}
BIN_DIR=$HOME/.local/bin

# Prefer the folder of an existing per-user install, so running a newer
# installer upgrades it in place even when it was installed with --prefix.
if [ -z "$PREFIX" ] && [ -L "$BIN_DIR/pzmodmgr" ]; then
    existing=$(readlink -f "$BIN_DIR/pzmodmgr" 2>/dev/null || true)
    if [ -n "$existing" ] && [ -x "$existing" ] && [ -f "$(dirname "$existing")/uninstall.sh" ]; then
        INSTALL_DIR=$(dirname "$existing")
    fi
fi
INSTALL_DIR=${INSTALL_DIR:-${PREFIX:-$DATA_HOME/pzmodmgr}}
INSTALL_DIR=${INSTALL_DIR%/}
APPLICATIONS=$DATA_HOME/applications
ICON_DIR=$DATA_HOME/icons/hicolor/256x256/apps
DESKTOP_DIR=$HOME/Desktop
if command -v xdg-user-dir >/dev/null 2>&1; then
    d=$(xdg-user-dir DESKTOP 2>/dev/null || true)
    [ -n "$d" ] && DESKTOP_DIR=$d
fi

config_value() {
    [ -n "$1" ] && [ -f "$1" ] || return 0
    sed -n "s/^$2=//p" "$1" | tr -d '\r' | tail -n 1
}

SOURCE_CONFIG=
if [ -f "$INSTALL_DIR/config.ini" ]; then
    SOURCE_CONFIG=$INSTALL_DIR/config.ini
elif [ -f "$HOME/Zomboid/pzmodmgr/config.ini" ]; then
    SOURCE_CONFIG=$HOME/Zomboid/pzmodmgr/config.ini
fi
if [ -z "$ZOMBOID_DIR" ] && [ -n "$SOURCE_CONFIG" ]; then
    ZOMBOID_DIR=$(config_value "$SOURCE_CONFIG" zomboid_dir)
fi
[ -n "$ZOMBOID_DIR" ] || ZOMBOID_DIR=$HOME/Zomboid

case "$MODE" in
    help)
        cat <<EOF
$APP $VERSION installer

  --yes            accept defaults
  --desktop        create a desktop shortcut too
  --no-desktop     do not create a desktop shortcut
  --no-menu        do not add the application-menu entry
  --no-browsers    do not register Firefox/Chrome native hosts
  --prefix=DIR     install elsewhere
  --zomboid-dir=DIR  Zomboid cache folder (default: ~/Zomboid)
  --uninstall      remove the installed copy
EOF
        exit 0
        ;;
    uninstall)
        if [ -x "$INSTALL_DIR/uninstall.sh" ]; then
            exec "$INSTALL_DIR/uninstall.sh"
        fi
        echo "error: not installed in $INSTALL_DIR" >&2
        exit 1
        ;;
esac

case "$INSTALL_DIR" in
    /|"$HOME"|"$HOME/Zomboid"|"$HOME/Zomboid/"*)
        echo "error: choose a dedicated application folder" >&2; exit 1 ;;
esac

say() { printf '%s\n' "$*"; }

ask() {
    _prompt=$1; _default=$2
    if [ "$ASSUME_YES" = 1 ]; then return 0; fi
    printf '%s ' "$_prompt"
    if ! read -r _answer; then _answer=; fi
    case "$_answer" in
        "") [ "$_default" = yes ] ;;
        [yY]*) return 0 ;;
        *) return 1 ;;
    esac
}

if [ "$ASSUME_YES" = 0 ]; then
    say "$APP $VERSION"
    say "Installs for the current user into $INSTALL_DIR."
    say "Mods are installed into ~/Zomboid/mods. No sudo is required."
fi

# Explicit options always win; --yes only accepts the defaults. This matters
# for unattended installs such as --yes --no-browsers --no-menu.
if [ "$DESKTOP_OPTION" = 0 ] && [ "$ASSUME_YES" = 0 ]; then
    if ask "Create a desktop shortcut? [y/N]" no; then WANT_DESKTOP=yes; fi
fi
if [ "$MENU_OPTION" = 0 ] && [ "$ASSUME_YES" = 0 ]; then
    if ! ask "Add PZ Mod Manager to the application menu? [Y/n]" yes; then WANT_MENU=no; fi
fi
if [ "$BROWSERS_OPTION" = 0 ] && [ "$ASSUME_YES" = 0 ]; then
    if ! ask "Register Firefox and Chrome add-on hosts? [Y/n]" yes; then WANT_BROWSERS=no; fi
fi

if [ "$ASSUME_YES" = 0 ] && [ "$ZOMBOID_DIR_OPTION" = 0 ] && [ -z "${PZMODMGR_ZOMBOID_DIR:-}" ]; then
    say "Zomboid cache folder: $ZOMBOID_DIR"
    printf 'Change it? [Enter to keep]: '
    if read -r _zomboid; then
        [ -n "$_zomboid" ] && ZOMBOID_DIR=$_zomboid
    fi
fi
case "$ZOMBOID_DIR" in
    "~") ZOMBOID_DIR=$HOME ;;
    "~/"*) ZOMBOID_DIR=$HOME/${ZOMBOID_DIR#~/} ;;
esac
case "$ZOMBOID_DIR" in
    /*) ;;
    *) echo "error: Zomboid cache folder must be an absolute path: $ZOMBOID_DIR" >&2; exit 2 ;;
esac
[ "$ZOMBOID_DIR" != / ] || { echo "error: refusing to use / as the Zomboid cache folder" >&2; exit 2; }

TMP=$(mktemp -d "${TMPDIR:-/tmp}/pzmodmgr.XXXXXX")
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
PAYLOAD_LINE=$(awk '/^__PZMODMGR_PAYLOAD__$/ { print NR + 1; exit }' "$0")
[ -n "$PAYLOAD_LINE" ] || { echo "error: missing payload" >&2; exit 1; }
tail -n +"$PAYLOAD_LINE" "$0" | tar -xzf - -C "$TMP"

say "Installing files..."
mkdir -p "$INSTALL_DIR"
cp -R "$TMP"/. "$INSTALL_DIR"/

steamcmd_exe=$(config_value "$SOURCE_CONFIG" steamcmd_exe)
steam_user=$(config_value "$SOURCE_CONFIG" steam_user)
dark_mode=$(config_value "$SOURCE_CONFIG" dark_mode)
[ -n "$steam_user" ] || steam_user=anonymous
case "$dark_mode" in
    1|on|true|yes|dark) dark_mode=1 ;;
    *) dark_mode=0 ;;
esac
config_tmp=$(mktemp "$INSTALL_DIR/.config.ini.XXXXXX")
printf 'steamcmd_exe=%s\nsteam_user=%s\nzomboid_dir=%s\ndark_mode=%s\n' \
    "$steamcmd_exe" "$steam_user" "$ZOMBOID_DIR" "$dark_mode" > "$config_tmp"
chmod 644 "$config_tmp"
mv -f "$config_tmp" "$INSTALL_DIR/config.ini"
chmod 755 "$INSTALL_DIR/pzmodmgr" "$INSTALL_DIR/pzmodmgr_native_host"
mkdir -p "$BIN_DIR"
ln -sfn "$INSTALL_DIR/pzmodmgr" "$BIN_DIR/pzmodmgr"

if [ "$WANT_MENU" = yes ]; then
    mkdir -p "$APPLICATIONS" "$ICON_DIR"
    cp "$INSTALL_DIR/pzmodmgr.png" "$ICON_DIR/pzmodmgr.png"
    cat > "$APPLICATIONS/pzmodmgr.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=$APP
Comment=Install Project Zomboid Workshop mods
Exec="$INSTALL_DIR/pzmodmgr"
TryExec=$INSTALL_DIR/pzmodmgr
Path=$INSTALL_DIR
Icon=pzmodmgr
Terminal=false
Categories=Game;Utility;
Keywords=Project Zomboid;Workshop;Mods;
EOF
    chmod 644 "$APPLICATIONS/pzmodmgr.desktop"
    command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$APPLICATIONS" >/dev/null 2>&1 || true
fi

if [ "$WANT_DESKTOP" = yes ] && [ -f "$APPLICATIONS/pzmodmgr.desktop" ]; then
    mkdir -p "$DESKTOP_DIR"
    cp "$APPLICATIONS/pzmodmgr.desktop" "$DESKTOP_DIR/PZ Mod Manager.desktop"
    chmod 755 "$DESKTOP_DIR/PZ Mod Manager.desktop"
    command -v gio >/dev/null 2>&1 && gio set "$DESKTOP_DIR/PZ Mod Manager.desktop" metadata::trusted true 2>/dev/null || true
fi

json_escape() { printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'; }

write_host() {
    _dir=$1; _json=$2
    mkdir -p "$_dir"
    printf '%s\n' "$_json" > "$_dir/pzmodmgr.json"
}

if [ "$WANT_BROWSERS" = yes ]; then
    host=$(json_escape "$INSTALL_DIR/pzmodmgr_native_host")
    write_host "$HOME/.mozilla/native-messaging-hosts" "{
  \"name\": \"pzmodmgr\",
  \"description\": \"Project Zomboid Mod Manager native host\",
  \"path\": \"$host\",
  \"type\": \"stdio\",
  \"allowed_extensions\": [\"pzmodmgr@local\"]
}"
    chrome_json="{
  \"name\": \"pzmodmgr\",
  \"description\": \"Project Zomboid Mod Manager native host\",
  \"path\": \"$host\",
  \"type\": \"stdio\",
  \"allowed_origins\": [\"chrome-extension://medndgoffhfpiabahjpdkfihnhdcmhoo/\"]
}"
    for dir in "$CONFIG_HOME/google-chrome" "$CONFIG_HOME/chromium" \
               "$CONFIG_HOME/BraveSoftware/Brave-Browser" "$CONFIG_HOME/microsoft-edge"; do
        if [ -d "$dir" ]; then
            write_host "$dir/NativeMessagingHosts" "$chrome_json"
        fi
    done
fi

cat > "$INSTALL_DIR/uninstall.sh" <<'EOF'
#!/bin/sh
set -eu
INSTALL_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BIN_DIR=$HOME/.local/bin
DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}
CONFIG_HOME=${XDG_CONFIG_HOME:-$HOME/.config}
DESKTOP_DIR=$HOME/Desktop
command -v xdg-user-dir >/dev/null 2>&1 && DESKTOP_DIR=$(xdg-user-dir DESKTOP 2>/dev/null || echo "$HOME/Desktop")
if [ -L "$BIN_DIR/pzmodmgr" ] && [ "$(readlink "$BIN_DIR/pzmodmgr")" = "$INSTALL_DIR/pzmodmgr" ]; then
    rm -f "$BIN_DIR/pzmodmgr"
fi
rm -f "$DATA_HOME/applications/pzmodmgr.desktop"
rm -f "$DATA_HOME/icons/hicolor/256x256/apps/pzmodmgr.png"
rm -f "$DESKTOP_DIR/PZ Mod Manager.desktop"
rm -f "$HOME/.mozilla/native-messaging-hosts/pzmodmgr.json"
for d in google-chrome chromium BraveSoftware/Brave-Browser microsoft-edge; do
    rm -f "$CONFIG_HOME/$d/NativeMessagingHosts/pzmodmgr.json"
done
rm -rf "$INSTALL_DIR"
echo "PZ Mod Manager was uninstalled. Your ~/Zomboid mods and settings were kept."
EOF
chmod 755 "$INSTALL_DIR/uninstall.sh"

say ""
say "Installed $APP $VERSION to $INSTALL_DIR"
say "Zomboid cache folder: $ZOMBOID_DIR"
say "Config: $INSTALL_DIR/config.ini"
say "Launch: $BIN_DIR/pzmodmgr"
say "Uninstall: $INSTALL_DIR/uninstall.sh"
say "No password is stored. The first add downloads steamcmd into the Zomboid folder."
exit 0

__PZMODMGR_PAYLOAD__
