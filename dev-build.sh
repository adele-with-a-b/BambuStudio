#!/bin/bash
# Build and install BambuStudio Dev locally
# Usage: ./dev-build.sh          # incremental build + install
#        ./dev-build.sh clean    # reconfigure + full build + install
#        ./dev-build.sh build    # build only, don't install

set -e

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
DEPS="$PROJECT_DIR/../BambuStudio_dep/usr/local"
APP_NAME="BambuStudio Dev"
APP_DST="/Applications/$APP_NAME.app"

if [ "$1" = "clean" ]; then
    echo "Reconfiguring..."
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    cmake "$PROJECT_DIR" \
        -DCMAKE_PREFIX_PATH="$DEPS" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_MACOSX_RPATH=ON \
        -DCMAKE_MACOSX_BUNDLE=ON \
        -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DBBL_RELEASE_TO_PUBLIC=0 \
        -DBBL_INTERNAL_TESTING=1
fi

echo "Building..."
cd "$BUILD_DIR"
cmake --build . --target BambuStudio --config Release -j10

if [ "$1" = "build" ]; then
    echo "Build complete (skipping install)."
    exit 0
fi

echo "Installing to $APP_DST..."
APP_SRC="$BUILD_DIR/src/BambuStudio.app"

# Fix resources symlink if present
resources_path=$(readlink "$APP_SRC/Contents/Resources" 2>/dev/null || true)
if [ -L "$APP_SRC/Contents/Resources" ] && [ -n "$resources_path" ]; then
    rm "$APP_SRC/Contents/Resources"
    cp -R "$resources_path" "$APP_SRC/Contents/Resources"
fi

rm -rf "$APP_DST"
cp -R "$APP_SRC" "$APP_DST"

# Use orange icon for dev build
if [ -f "$PROJECT_DIR/resources/images/IconDev.icns" ]; then
    cp "$PROJECT_DIR/resources/images/IconDev.icns" "$APP_DST/Contents/Resources/Icon.icns"
fi
if [ -f "$PROJECT_DIR/resources/images/BambuStudioDev-mac_256px.ico" ]; then
    cp "$PROJECT_DIR/resources/images/BambuStudioDev-mac_256px.ico" "$APP_DST/Contents/Resources/images/BambuStudio-mac_256px.ico"
fi

echo ""
echo "✓ Installed: $APP_DST"
echo "  Data dir:  ~/Library/Application Support/BambuStudioInternal/"
echo "  Presets:   ~/Library/Application Support/BambuStudioInternal/user/1615318752/"
