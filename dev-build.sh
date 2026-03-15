#!/bin/bash
# Build and install BambuStudio Dev locally
set -e

usage() {
    echo "Usage: ./dev-build.sh [command]"
    echo ""
    echo "Commands:"
    echo "  (none)     incremental build + install"
    echo "  clean      reconfigure cmake + full build + install"
    echo "  build      build only, don't install"
    echo "  nuke       delete build dir + reconfigure + full build + install"
    echo "  configure  cmake configure only (no build)"
    echo "  help       show this help"
    exit 0
}

[ "$1" = "help" ] || [ "$1" = "--help" ] || [ "$1" = "-h" ] && usage

CMD="${1#--}"  # strip -- prefix if present (--clean → clean)

# Backup BambuStudioInternal data dir before build
INTERNAL_DIR="$HOME/Library/Application Support/BambuStudioInternal"
BACKUP_DIR="$HOME/.3d-engineer/bambu-config-backup/BambuStudioInternal"
if [ -d "$INTERNAL_DIR" ]; then
    mkdir -p "$BACKUP_DIR"
    rsync -a --delete \
        --exclude 'log/' \
        --exclude 'cache/' \
        "$INTERNAL_DIR/" "$BACKUP_DIR/"
    echo "Backed up BambuStudioInternal"
fi

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
DEPS="$PROJECT_DIR/../BambuStudio_dep/usr/local"
if [ ! -d "$DEPS" ] && [ -d "$PROJECT_DIR/../BambuStudio_dep/destdir/usr/local" ]; then
    DEPS="$PROJECT_DIR/../BambuStudio_dep/destdir/usr/local"
fi
APP_NAME="BambuStudio Dev"
APP_DST="/Applications/$APP_NAME.app"

# Nuke: delete build dir entirely
if [ "$CMD" = "nuke" ]; then
    echo "Nuking build directory..."
    rm -rf "$BUILD_DIR"
fi

# Configure (clean, nuke, or configure)
if [ "$CMD" = "clean" ] || [ "$CMD" = "nuke" ] || [ "$CMD" = "configure" ]; then
    echo "Configuring..."
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
    [ "$CMD" = "configure" ] && { echo "✅ Configure complete."; exit 0; }
fi

# Build
echo "Building..."
cd "$BUILD_DIR"
cmake --build . --target BambuStudio --config Release -j$(sysctl -n hw.ncpu) 2>&1 | tee "$PROJECT_DIR/build.log"
if [ ${PIPESTATUS[0]} -ne 0 ]; then
    echo "❌ Build failed. See build.log"
    exit 1
fi

[ "$CMD" = "build" ] && { echo "✅ Build complete (skipping install)."; exit 0; }

# Install
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

# Orange icon for dev build
for icon in IconDev.icns; do
    [ -f "$PROJECT_DIR/resources/images/$icon" ] && \
        cp "$PROJECT_DIR/resources/images/$icon" "$APP_DST/Contents/Resources/Icon.icns"
done
[ -f "$PROJECT_DIR/resources/images/BambuStudioDev-mac_256px.ico" ] && \
    cp "$PROJECT_DIR/resources/images/BambuStudioDev-mac_256px.ico" "$APP_DST/Contents/Resources/images/BambuStudio-mac_256px.ico"

echo ""
echo "✅ Installed: $APP_DST"
echo "   Data dir:  ~/Library/Application Support/BambuStudioInternal/"
echo "   Presets:   ~/Library/Application Support/BambuStudioInternal/user/1615318752/"
