#!/bin/sh
# Build the Linux distribution and the single-file self-extracting installer.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VERSION=$(sed -n 's/^project(pzmodmgr VERSION \([0-9][0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt" | head -1)
[ -n "$VERSION" ] || { echo "error: could not read project version" >&2; exit 1; }

BUILD=${BUILD_DIR:-"$ROOT/build"}
OUT=${DIST_DIR:-"$ROOT/dist"}
NAME="PZModManager-$VERSION-linux-x86_64"
STAGE="$OUT/$NAME"
WORK="$OUT/.packaging-linux"

jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)

echo "==> building Linux binaries"
cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$BUILD" -j "$jobs"

echo "==> staging Linux files"
rm -rf "$STAGE" "$WORK"
mkdir -p "$STAGE" "$WORK"
cmake --install "$BUILD" --prefix "$STAGE" >/dev/null
cp -R "$ROOT/firefox" "$STAGE/firefox"
cp -R "$ROOT/chrome-extension" "$STAGE/chrome-extension"
rm -f "$STAGE/firefox/README.md" "$STAGE/chrome-extension/README.md"
cp "$ROOT/src/ui/win32/pzmodmgr.png" "$STAGE/pzmodmgr.png"
cp "$ROOT/installer/README-Linux.txt" "$STAGE/README.txt"
cp "$ROOT/LICENSE" "$STAGE/LICENSE.txt"
rm -f "$STAGE/README.md" "$STAGE/LICENSE"

tar -czf "$WORK/payload.tar.gz" -C "$STAGE" .
sed "s/@VERSION@/$VERSION/g" "$ROOT/installer/linux-installer.sh" > "$WORK/installer.sh"
cat "$WORK/payload.tar.gz" >> "$WORK/installer.sh"
chmod 755 "$WORK/installer.sh"
cp "$WORK/installer.sh" "$OUT/$NAME.sh"

tar -czf "$OUT/$NAME.tar.gz" -C "$OUT" "$NAME"
( cd "$OUT" && sha256sum "$NAME.sh" "$NAME.tar.gz" > SHA256SUMS )

echo
echo "Distribution ready:"
ls -lh "$OUT/$NAME.sh" "$OUT/$NAME.tar.gz" "$OUT/SHA256SUMS"
