#!/bin/sh
# Registers the pzmodmgr native messaging host for Firefox on Linux.
# Override the destination for testing with PZMODMGR_NATIVE_DIR.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
HOST=${1:-"$ROOT/build/pzmodmgr_native_host"}

if [ ! -x "$HOST" ]; then
    echo "error: $HOST is missing or not executable" >&2
    echo "build it first: cmake -B build && cmake --build build --target pzmodmgr_native_host" >&2
    exit 1
fi

HOST=$(realpath "$HOST")
DEST=${PZMODMGR_NATIVE_DIR:-"$HOME/.mozilla/native-messaging-hosts"}
mkdir -p "$DEST"

cat > "$DEST/pzmodmgr.json" <<EOF
{
  "name": "pzmodmgr",
  "description": "Project Zomboid Mod Manager native host",
  "path": "$HOST",
  "type": "stdio",
  "allowed_extensions": ["pzmodmgr@local"]
}
EOF

chmod 644 "$DEST/pzmodmgr.json"
echo "registered $DEST/pzmodmgr.json -> $HOST"
