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
# Deps can live at several historical locations. Check each in order until
# we find a usable install prefix with include/ and lib/ populated.
DEPS_CANDIDATES=(
    "$PROJECT_DIR/../BambuStudio_dep/usr/local"
    "$PROJECT_DIR/../BambuStudio_dep/destdir/usr/local"
    "$PROJECT_DIR/deps_build/usr/local"
    "$PROJECT_DIR/deps_build/destdir/usr/local"
)
DEPS=""
for candidate in "${DEPS_CANDIDATES[@]}"; do
    if [ -d "$candidate/include" ] && [ -d "$candidate/lib" ]; then
        DEPS="$candidate"
        break
    fi
done
if [ -z "$DEPS" ]; then
    echo "ERROR: could not locate deps install prefix. Searched:"
    printf '  %s\n' "${DEPS_CANDIDATES[@]}"
    exit 1
fi
echo "Using DEPS=$DEPS"
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
    # Force the SDK to Xcode's full SDK rather than letting cmake autodetect
    # via xcrun, which on this machine returns the Command Line Tools SDK
    # by default (the two often drift versions). Mismatched SDK + clang
    # cause libc++ #include_next failures ("<cstddef> tried including
    # <stddef.h> but didn't find libc++'s <stddef.h>") because cmake's
    # IMPLICIT_INCLUDE_DIRECTORIES probe records CLT paths but isysroot
    # then points at Xcode's, breaking the strip-implicit-dirs logic.
    XCODE_SDK="$(xcrun --sdk macosx --show-sdk-path)"
    cmake "$PROJECT_DIR" \
        -DCMAKE_PREFIX_PATH="$DEPS" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_MACOSX_RPATH=ON \
        -DCMAKE_MACOSX_BUNDLE=ON \
        -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_SYSROOT="$XCODE_SDK" \
        -DBBL_RELEASE_TO_PUBLIC=0 \
        -DBBL_INTERNAL_TESTING=1
    [ "$CMD" = "configure" ] && { echo "✅ Configure complete."; exit 0; }
fi

# Clear log before build so progress bar doesn't read configure output
> "$PROJECT_DIR/build.log"

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

# Resources handling. The cmake build (src/CMakeLists.txt) drops a symlink
# from $APP_SRC/Contents/Resources into $PROJECT_DIR/resources. The first
# install run breaks the symlink and replaces it with a `cp -R` copy --
# that's what makes the installed .app self-contained. But it also means
# subsequent installs never refresh resources unless we explicitly rsync
# from the project tree. Without this rsync, new SVGs / images / data
# files added between builds silently never make it into /Applications,
# and the running app crashes at first reference (see the launch-crash
# investigation around 2026-05-17 where new share*.svg files were missing
# from the bundle and the app aborted in WebViewPanel ctor).
#
# Always pull the live project resources into the build's .app first,
# then rm+cp into /Applications. rsync --delete keeps stale files from
# accumulating across renames in the project tree.
RESOURCES_SRC="$PROJECT_DIR/resources"
APP_RES="$APP_SRC/Contents/Resources"
if [ -L "$APP_RES" ]; then
    # First install: replace the symlink with a fresh copy.
    rm "$APP_RES"
    cp -R "$RESOURCES_SRC" "$APP_RES"
elif [ -d "$APP_RES" ]; then
    # Subsequent installs: refresh from the project tree.
    rsync -a --delete "$RESOURCES_SRC/" "$APP_RES/"
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

# Code-sign with a stable self-signed identity so macOS TCC permission
# grants persist across rebuilds. Without this, every recompile produces
# a new ad-hoc signature, TCC sees a "different" app, and the user has
# to re-grant Files/Folders/Camera/Network permissions on every build.
#
# The identity below is a self-signed code-signing cert in the login
# keychain. Setup is one-time per machine: Keychain Access -> Certificate
# Assistant -> Create a Certificate, name "BambuStudioDev", self-signed
# root, Code Signing type. After creation, mark Always Trust under the
# certificate's Trust settings so it shows up in the codesigning identity
# list. Then `security find-identity -v -p codesigning` will list it.
#
# The signing must happen AFTER the icon swap above; rewriting Icon.icns
# invalidates any prior signature and codesign --force regenerates it
# over the final bundle contents.
# Note: NOT using --options runtime (Hardened Runtime). Hardened Runtime
# enforces Library Validation, which requires every loaded dylib to have a
# Team ID matching the main binary's Team ID. Our self-signed cert has no
# Team ID, while Homebrew dylibs (zstd, etc.) are signed by Homebrew with
# their own Team ID -- mismatch -> dyld refuses to load -> app crashes at
# launch ("mapping process and mapped file (non-platform) have different
# Team IDs"). Hardened Runtime's main purpose is notarization, which we
# don't need for a local dev build, so we leave it off.
SIGN_IDENTITY="14F9F6CEB1DC42167435386D87623D555E50DAEE"
if security find-identity -v -p codesigning | grep -q "$SIGN_IDENTITY"; then
    codesign --force --deep --sign "$SIGN_IDENTITY" "$APP_DST" 2>&1 | sed 's/^/  codesign: /'
    echo "  codesign: signed with BambuStudioDev identity"
else
    echo "  codesign: BambuStudioDev cert not found, falling back to ad-hoc (TCC grants will reset on each build)"
    codesign --force --deep --sign - "$APP_DST" 2>&1 | sed 's/^/  codesign: /'
fi

echo ""
echo "✅ Installed: $APP_DST"
echo "   Data dir:  ~/Library/Application Support/BambuStudioInternal/"
echo "   Presets:   ~/Library/Application Support/BambuStudioInternal/user/1615318752/"

# Ensure network plugin is available (needed for login)
INTERNAL_PLUGINS="$HOME/Library/Application Support/BambuStudioInternal/plugins"
if [ ! -f "$INTERNAL_PLUGINS/libbambu_networking.dylib" ]; then
    echo "→ Network plugin missing — searching for a copy..."
    FOUND=""
    for src in \
        "$HOME/Library/Application Support/BambuStudioBeta/plugins" \
        "$HOME/Library/Application Support/BambuStudio/plugins" \
        "/Applications/BambuStudio.app/Contents/Frameworks/plugins" \
        "/Applications/BambuStudio Beta.app/Contents/Frameworks/plugins"; do
        if [ -f "$src/libbambu_networking.dylib" ]; then
            FOUND="$src"
            break
        fi
    done
    if [ -n "$FOUND" ]; then
        mkdir -p "$INTERNAL_PLUGINS"
        cp -R "$FOUND/"* "$INTERNAL_PLUGINS/"
        echo "  ✅ Network plugin copied from $(basename "$(dirname "$FOUND")")"
    else
        echo "  ⚠️  No network plugin found. Install BambuStudio Beta first, then re-run."
        echo "     Download: https://bambulab.com/en/download/studio"
    fi
fi

# Ensure network plugin is available (copy from Beta if missing)
INTERNAL_PLUGINS="$HOME/Library/Application Support/BambuStudioInternal/plugins"
BETA_PLUGINS="$HOME/Library/Application Support/BambuStudioBeta/plugins"
if [ ! -f "$INTERNAL_PLUGINS/libbambu_networking.dylib" ] && [ -f "$BETA_PLUGINS/libbambu_networking.dylib" ]; then
    echo "→ Copying network plugin from Beta..."
    mkdir -p "$INTERNAL_PLUGINS"
    cp -R "$BETA_PLUGINS/"* "$INTERNAL_PLUGINS/"
    echo "  ✅ Network plugin installed"
fi
