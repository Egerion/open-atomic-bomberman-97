#!/usr/bin/env bash
# Build a self-contained, double-clickable "Open Bomberman.app" on macOS.
#
# Run this ON YOUR OWN Mac (needs the Xcode command-line tools + cmake).
# SDL3 is fetched and STATICALLY linked, so the app is a single self-contained
# binary — no dylib bundling, nothing to install alongside it.
#
#   scripts/package_macos.sh /path/to/your/BOMBRMAN
#
# The argument is YOUR OWN Atomic Bomberman install directory (the folder that
# contains DATA/). Its contents are copied into the .app's Resources so the app
# runs fully on its own when double-clicked. Use only game data you own.
#
# Omit the argument to build the .app WITHOUT data (it will show a "no install"
# message until you drop a DATA/ folder into Contents/Resources/ yourself).
#
# Output: dist/Open Bomberman.app
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build/macos-app"
APP="$ROOT/dist/Open Bomberman.app"
GAME_DATA="${1:-}"

if ! command -v cmake >/dev/null 2>&1; then
  echo "error: cmake not found. Install it (e.g. 'brew install cmake') and retry." >&2
  exit 1
fi
if [ "$(uname)" != "Darwin" ]; then
  echo "warning: this script assembles a macOS .app and is meant to run on a Mac." >&2
fi

echo ">> Configuring (static SDL3 via FetchContent, Release)…"
# Native host arch (arm64 on Apple Silicon, x86_64 on Intel). For a universal
# binary add: -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
cmake -S "$ROOT" -B "$BUILD" \
  -DCMAKE_BUILD_TYPE=Release \
  -DBOMBER_FETCH_SDL3=ON \
  -DSDL_SHARED=OFF -DSDL_STATIC=ON

echo ">> Building bomber_game…"
cmake --build "$BUILD" --target bomber_game -j

BIN="$BUILD/OPEN-BM95"
[ -x "$BIN" ] || { echo "error: build did not produce $BIN" >&2; exit 1; }

echo ">> Assembling ${APP}…"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"

# The real engine binary.
cp "$BIN" "$APP/Contents/MacOS/bomber_game"

# Launcher = the bundle's executable. It resolves the bundle's own Resources
# dir at runtime (so the app keeps working if you move it) and passes it as the
# game_dir, then forwards any extra args. This is why double-clicking finds the
# bundled game data with no environment setup.
cat > "$APP/Contents/MacOS/open-bomberman" <<'LAUNCH'
#!/bin/bash
HERE="$(cd "$(dirname "$0")" && pwd)"
RES="$(cd "$HERE/.." && pwd)/Resources"
exec "$HERE/bomber_game" "$RES" "$@"
LAUNCH
chmod +x "$APP/Contents/MacOS/open-bomberman" "$APP/Contents/MacOS/bomber_game"

cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key>            <string>Open Bomberman</string>
  <key>CFBundleDisplayName</key>     <string>Open Bomberman</string>
  <key>CFBundleIdentifier</key>      <string>org.openbomberman.game</string>
  <key>CFBundleVersion</key>         <string>0.1.0</string>
  <key>CFBundleShortVersionString</key><string>0.1.0</string>
  <key>CFBundlePackageType</key>     <string>APPL</string>
  <key>CFBundleExecutable</key>      <string>open-bomberman</string>
  <key>NSHighResolutionCapable</key> <true/>
  <key>LSMinimumSystemVersion</key>  <string>11.0</string>
</dict>
</plist>
PLIST
printf 'APPL????' > "$APP/Contents/PkgInfo"

# Bundle your own game data, if provided.
#
# Only the original game's runtime assets go in — never the RE working
# material (IDA databases, decompiler output, the EXE itself, logs) that may
# be sitting next to it in a dev copy of the install dir. CLAUDE.md's rule
# ("NEVER commit exe-derived material... those stay in the BOMBRMAN folder")
# applies just as much to a shipped .app as to a git commit.
if [ -n "$GAME_DATA" ]; then
  if [ ! -d "$GAME_DATA/DATA" ] && [ ! -d "$GAME_DATA/data" ]; then
    echo "warning: '$GAME_DATA' has no DATA/ subfolder — is it your BOMBRMAN dir?" >&2
  fi
  echo ">> Copying game data from ${GAME_DATA} into the app…"
  rsync -a \
    --exclude='*.idb' \
    --exclude='*.EXE' --exclude='*.exe' \
    --exclude='pseudo.c' \
    --exclude='decompile_all.py' --exclude='decompile.bat' --exclude='decompile_done.txt' \
    --exclude='idalog.txt' --exclude='*.log' \
    --exclude='WINEREG/' \
    --exclude='bomber_hd_toolkit/' --exclude='bomber_hd_toolkit.zip' \
    --exclude='TOOLS/' \
    --exclude='.DS_Store' \
    "$GAME_DATA"/ "$APP/Contents/Resources/"
fi

echo ""
echo "Done: $APP"
if [ -z "$GAME_DATA" ]; then
  echo "No data bundled — copy your DATA/ folder into:"
  echo "  $APP/Contents/Resources/"
fi
echo ""
echo "First launch (unsigned app): right-click the app -> Open, or run once:"
echo "  xattr -dr com.apple.quarantine \"$APP\""
