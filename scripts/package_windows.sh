#!/bin/sh
# Build the Windows distribution on Linux with MinGW-w64.
#
# Outputs:
#   dist/PZModManager-<version>-win64-setup.exe
#   dist/PZModManager-<version>-win64.zip
#   dist/SHA256SUMS
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VERSION=$(sed -n 's/^project(pzmodmgr VERSION \([0-9][0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt" | head -1)
[ -n "$VERSION" ] || { echo "error: could not read project version" >&2; exit 1; }

BUILD=${BUILD_DIR:-"$ROOT/build-win"}
OUT=${DIST_DIR:-"$ROOT/dist"}
NAME="PZModManager-$VERSION-win64"
STAGE="$OUT/$NAME"
WORK="$BUILD/packaging"
PAYLOAD="$WORK/payload"

for tool in cmake x86_64-w64-mingw32-g++ x86_64-w64-mingw32-windres zip; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "error: $tool is required (install MinGW-w64, cmake and zip)" >&2
        exit 1
    }
done

jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)

echo "==> configuring $BUILD"
cmake -S "$ROOT" -B "$BUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/toolchain-mingw.cmake" \
    -DCMAKE_BUILD_TYPE=Release >/dev/null

echo "==> building Windows binaries"
cmake --build "$BUILD" --config Release --target pzmodmgr pzmodmgr_native_host -j "$jobs"

echo "==> staging portable files"
rm -rf "$STAGE" "$WORK"
mkdir -p "$STAGE" "$WORK" "$PAYLOAD"
cmake --install "$BUILD" --prefix "$STAGE" >/dev/null

cp -R "$ROOT/firefox" "$STAGE/firefox"
cp -R "$ROOT/chrome-extension" "$STAGE/chrome-extension"
rm -f "$STAGE/firefox/README.md" "$STAGE/chrome-extension/README.md"

# Use Windows-friendly line endings for documentation.
crlf() {
    awk '{ sub(/\r$/, ""); printf "%s\r\n", $0 }' "$1" > "$2"
}
crlf "$ROOT/installer/README-Windows.txt" "$STAGE/README.txt"
crlf "$ROOT/LICENSE" "$STAGE/LICENSE.txt"
rm -f "$STAGE/README.md" "$STAGE/LICENSE"

strip_tool=x86_64-w64-mingw32-strip
if command -v "$strip_tool" >/dev/null 2>&1; then
    "$strip_tool" --strip-unneeded "$STAGE/pzmodmgr.exe" "$STAGE/pzmodmgr_native_host.exe"
fi

# The distributed executables must not need MinGW runtime DLLs.
for exe in pzmodmgr.exe pzmodmgr_native_host.exe; do
    if objdump -p "$STAGE/$exe" | grep -Eq 'libstdc\+\+|libgcc|winpthread'; then
        echo "error: $exe still depends on a MinGW runtime DLL" >&2
        exit 1
    fi
done

# Optional: pack the main executable with UPX (the same size trick used by
# many small Windows tools). Set UPX_BIN=/path/to/upx to enable it. The
# native host is left unpacked because browsers may inspect it.
if [ -n "${UPX_BIN:-}" ] && [ -x "$UPX_BIN" ]; then
    echo "==> packing pzmodmgr.exe with UPX"
    "$UPX_BIN" --best --lzma "$STAGE/pzmodmgr.exe"
fi

echo "==> building portable zip"
rm -f "$OUT/$NAME.zip"
( cd "$OUT" && zip -q -r -9 "$NAME.zip" "$NAME" )

echo "==> building small uninstaller"
x86_64-w64-mingw32-windres -I "$ROOT/installer" \
    "$ROOT/installer/uninstaller.rc" -O coff -o "$WORK/uninstaller-res.o"
x86_64-w64-mingw32-g++ -std=c++17 -Os -municode -mwindows \
    -static -static-libgcc -static-libstdc++ -fno-exceptions -fno-rtti \
    -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0601 -DWINVER=0x0601 \
    "$ROOT/installer/uninstaller.cpp" "$WORK/uninstaller-res.o" \
    -o "$STAGE/uninstall.exe" \
    -lshell32 -lshlwapi -ladvapi32 -luser32
if command -v "$strip_tool" >/dev/null 2>&1; then
    "$strip_tool" --strip-unneeded "$STAGE/uninstall.exe"
fi

# Refresh the portable zip with the uninstaller included.
rm -f "$OUT/$NAME.zip"
( cd "$OUT" && zip -q -r -9 "$NAME.zip" "$NAME" )

echo "==> building setup executable"
cp -R "$STAGE"/. "$PAYLOAD"/

cat > "$WORK/payload.rc" <<EOF
#include <windows.h>
2001 RCDATA "$PAYLOAD/pzmodmgr.exe"
2002 RCDATA "$PAYLOAD/pzmodmgr_native_host.exe"
2003 RCDATA "$PAYLOAD/README.txt"
2004 RCDATA "$PAYLOAD/LICENSE.txt"
2005 RCDATA "$PAYLOAD/firefox/manifest.json"
2006 RCDATA "$PAYLOAD/firefox/background.js"
2009 RCDATA "$PAYLOAD/chrome-extension/manifest.json"
2010 RCDATA "$PAYLOAD/chrome-extension/background.js"
2012 RCDATA "$PAYLOAD/chrome-extension/icons/icon16.png"
2013 RCDATA "$PAYLOAD/chrome-extension/icons/icon32.png"
2014 RCDATA "$PAYLOAD/chrome-extension/icons/icon48.png"
2015 RCDATA "$PAYLOAD/chrome-extension/icons/icon128.png"
2016 RCDATA "$PAYLOAD/uninstall.exe"
EOF

x86_64-w64-mingw32-windres -I "$ROOT/installer" \
    "$ROOT/installer/installer.rc" -O coff -o "$WORK/installer-res.o"
x86_64-w64-mingw32-windres "$WORK/payload.rc" -O coff -o "$WORK/payload-res.o"

x86_64-w64-mingw32-g++ -std=c++17 -O2 -municode -mwindows \
    -static -static-libgcc -static-libstdc++ \
    -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0601 -DWINVER=0x0601 \
    -DPZ_VERSION="L\"$VERSION\"" \
    -I"$ROOT/installer" \
    "$ROOT/installer/installer.cpp" \
    "$WORK/installer-res.o" "$WORK/payload-res.o" \
    -o "$OUT/$NAME-setup.exe" \
    -lcomctl32 -lshell32 -lshlwapi -lole32 -luuid -ladvapi32 \
    -luser32 -lgdi32 -lmsimg32

if command -v "$strip_tool" >/dev/null 2>&1; then
    "$strip_tool" --strip-unneeded "$OUT/$NAME-setup.exe"
fi
if objdump -p "$OUT/$NAME-setup.exe" | grep -Eq 'libstdc\+\+|libgcc|winpthread'; then
    echo "error: setup executable still depends on a MinGW runtime DLL" >&2
    exit 1
fi

( cd "$OUT" && sha256sum "$NAME-setup.exe" "$NAME.zip" > SHA256SUMS )

echo
echo "Distribution ready:"
ls -lh "$OUT/$NAME-setup.exe" "$OUT/$NAME.zip" "$OUT/SHA256SUMS"
echo
echo "Portable folder: $STAGE"
