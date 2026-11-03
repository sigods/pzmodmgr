#!/bin/sh
# Test double for steamcmd. It emulates the layout that
# `+force_install_dir <dir> +workshop_download_item 108600 <id>` produces,
# without a Steam account or network access.
#
# Item id 555 simulates a failed download (non-zero exit, no content).
set -u

install_dir=
appid=
item=

while [ $# -gt 0 ]; do
    case "$1" in
        +force_install_dir)
            shift
            [ $# -gt 0 ] && install_dir=$1
            ;;
        +workshop_download_item)
            shift
            [ $# -gt 0 ] && appid=$1
            shift
            [ $# -gt 0 ] && item=$1
            ;;
    esac
    shift
done

echo "[  0%] Checking for available updates..."
echo "[----] Verifying installation..."

if [ -z "$install_dir" ] || [ -z "$appid" ] || [ -z "$item" ]; then
    echo "ERROR! Invalid command line"
    exit 2
fi

if [ -n "${FAKE_STEAMCMD_LOG:-}" ]; then
    printf '%s\n' "$item" >> "$FAKE_STEAMCMD_LOG"
fi

if [ "$item" = "555" ]; then
    echo "ERROR! Failed to install workshop item $item"
    exit 5
fi

content="$install_dir/steamapps/workshop/content/$appid/$item"
mod=FixtureMod
if [ -n "${FAKE_STEAMCMD_PER_ITEM:-}" ]; then
    mod="FixtureMod$item"
fi
mkdir -p "$content/mods/$mod"
cat > "$content/mods/$mod/mod.info" <<EOF
id=$mod
name=Fixture Mod $item
EOF

echo "Success. Downloaded item $item to \"$content\" (0 bytes)"
echo "Logged in OK."
exit 0
