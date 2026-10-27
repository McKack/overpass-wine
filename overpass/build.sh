#!/bin/bash
# Builds and packages OverPass Wine for x86_64 macOS: on an Intel Mac, or on Apple silicon with
# `arch -x86_64 overpass/build.sh`. Build tools come from Homebrew (bison, flex, mingw-w64, cmake, meson, ninja,
# nasm, pkgconf, dylibbundler, ccache); the libraries Wine uses are built here (build-deps.sh, build-gstreamer.sh).
# Output: overpass-wine-<version>-x86_64.tar.xz containing bin/, lib/, share/ (same layout OverPass expects).
set -euo pipefail

SRC="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="${OVERPASS_WINE_VERSION:-dev}"
BUILD="$SRC/build-overpass"
DEST="$BUILD/stage/overpass-wine-$VERSION"
JOBS="$(sysctl -n hw.ncpu)"

BREW="$(brew --prefix)"
export PATH="$BREW/opt/bison/bin:$BREW/opt/flex/bin:$PATH"
export MACOSX_DEPLOYMENT_TARGET=11.0

rm -rf "$BUILD"
mkdir -p "$BUILD/obj" "$DEST"

# With ccache installed, compiles that didn't change are reused (CI keeps its cache between runs).
# Wine builds unix code with clang and PE code with mingw, so both are wrapped.
WINE_CC=()
if command -v ccache >/dev/null; then
    export CCACHE_BASEDIR="$SRC" CCACHE_COMPILERCHECK=content CCACHE_NOHASHDIR=1
    WINE_CC=(CC="ccache clang" x86_64_CC="ccache x86_64-w64-mingw32-gcc" i386_CC="ccache i686-w64-mingw32-gcc")
    ccache -z >/dev/null
fi

# prebuilt <name>: runs overpass/build-<name>.sh into build-<name>/prefix, outside $BUILD, unless the stamp there
# shows the same script already built it. These take longer than Wine and rarely change; CI caches the folders.
prebuilt() {
    local script="$SRC/overpass/build-$1.sh" root="$SRC/build-$1" stamp
    stamp="$(shasum -a 256 "$script" | cut -d' ' -f1) $MACOSX_DEPLOYMENT_TARGET"
    if [ "$(cat "$root/stamp" 2>/dev/null)" != "$stamp" ]; then
        rm -rf "$root"
        "$script" "$root/work" "$root/prefix"
        rm -rf "$root/work"
        echo "$stamp" > "$root/stamp"
    fi
}
# FreeType, GnuTLS, SDL 2, libinotify, Vulkan loader, MoltenVK; then GStreamer for game video (winegstreamer).
prebuilt deps
prebuilt gstreamer
DEPS_ROOT="$SRC/build-deps" GST_ROOT="$SRC/build-gstreamer"
DEPS_PREFIX="$DEPS_ROOT/prefix" GST_PREFIX="$GST_ROOT/prefix"
# FreeType's pkg-config file requires zlib, which macOS ships (in the SDK) without one.
mkdir -p "$BUILD/pkgconfig"
printf 'Name: zlib\nDescription: zlib from the macOS SDK\nVersion: 1.2.12\nLibs: -lz\nCflags:\n' \
    > "$BUILD/pkgconfig/zlib.pc"
export PKG_CONFIG_LIBDIR="$DEPS_PREFIX/lib/pkgconfig:$GST_PREFIX/lib/pkgconfig:$BUILD/pkgconfig"
export CPPFLAGS="-I$DEPS_PREFIX/include" LDFLAGS="-L$DEPS_PREFIX/lib"

cd "$BUILD/obj"

# No --with-opengl: Wine 11 makes EGL a hard requirement then, but winemac.drv does OpenGL through CGL.
"$SRC/configure" \
    --prefix="$DEST" \
    --enable-archs=i386,x86_64 \
    --disable-tests \
    --with-coreaudio --with-freetype --with-gnutls --with-gstreamer --with-inotify --with-mingw \
    --with-pthread --with-sdl --with-unwind --with-vulkan \
    --without-alsa --without-capi --without-dbus --without-fontconfig --without-gettextpo \
    --without-gphoto --without-gssapi --without-krb5 --without-netapi \
    --without-oss --without-pulse --without-sane --without-udev --without-usb \
    --without-v4l2 --without-wayland --without-x \
    ${WINE_CC[@]+"${WINE_CC[@]}"}

make -j"$JOBS"
if command -v ccache >/dev/null; then ccache -s; fi
make install-lib

# Wine dlopen()s these by leaf name; macOS resolves leaf names through the caller's LC_RPATH,
# so ship them in lib/ and point the binaries there.
LIB="$DEST/lib"
for f in \
    "$DEPS_PREFIX/lib/libfreetype.6.dylib" \
    "$DEPS_PREFIX/lib/libgnutls.30.dylib" \
    "$DEPS_PREFIX/lib/libSDL2-2.0.0.dylib" \
    "$DEPS_PREFIX/lib/libinotify.0.dylib" \
    "$DEPS_PREFIX/lib/libMoltenVK.dylib" \
    "$DEPS_PREFIX/lib/libvulkan.1.dylib"; do
    cp -L "$f" "$LIB/"
done
chmod u+w "$LIB"/*.dylib
for f in "$LIB"/*.dylib; do
    install_name_tool -id "@rpath/$(basename "$f")" "$f"
done
# Copy transitive dependencies next to them and rewrite references to @loader_path.
dylibbundler -of -b -d "$LIB" -p @loader_path/ $(for f in "$LIB"/*.dylib; do printf -- "-x %s " "$f"; done)

for f in "$LIB"/wine/x86_64-unix/*.so; do
    install_name_tool -add_rpath @loader_path/../../ "$f" 2>/dev/null || true
done
for f in "$DEST"/bin/wine "$DEST"/bin/wineserver; do
    install_name_tool -add_rpath @loader_path/../lib "$f" 2>/dev/null || true
done

# GStreamer goes in after dylibbundler, already relocatable.
"$SRC/overpass/bundle-gstreamer.sh" "$GST_PREFIX" "$DEST"

# Libraries built here are linked by their build path (wineserver: libinotify; winegstreamer.so: GStreamer).
for f in "$DEST"/bin/wine "$DEST"/bin/wineserver "$LIB"/wine/x86_64-unix/*.so; do
    otool -L "$f" | tail -n +2 | awk '{print $1}' | { grep -e "^$BUILD/" -e "^$DEPS_ROOT/" -e "^$GST_ROOT/" || true; } | while read -r dep; do
        install_name_tool -change "$dep" "@rpath/$(basename "$dep")" "$f"
    done
done
# configure also puts build folders on their search paths (winegstreamer.so: GStreamer's lib/), which would win
# over the bundled copies wherever that folder exists.
for f in "$DEST"/bin/wine "$DEST"/bin/wineserver "$LIB"/wine/x86_64-unix/*.so; do
    otool -l "$f" | awk '/LC_RPATH/ {getline; getline; print $2}' |
        { grep -e "^$BUILD/" -e "^$DEPS_ROOT/" -e "^$GST_ROOT/" || true; } | while read -r path; do
        install_name_tool -delete_rpath "$path" "$f"
    done
done
if { otool -X -L "$DEST"/bin/* "$LIB"/wine/x86_64-unix/*.so "$LIB"/*.dylib
     otool -l "$DEST"/bin/* "$LIB"/wine/x86_64-unix/*.so "$LIB"/*.dylib | awk '/LC_RPATH/ {getline; getline; print $2}'
   } 2>/dev/null | grep -F -e "$BUILD/" -e "$DEPS_ROOT/" -e "$GST_ROOT/"; then
    echo "build.sh: binaries still reference a build folder" >&2
    exit 1
fi

# Licence and provenance travel with the binaries (LGPL-2.1 §6).
cp "$SRC/COPYING.LIB" "$SRC/LICENSE" "$SRC/AUTHORS" "$DEST/"
mkdir -p "$DEST/licenses" && cp -R "$DEPS_PREFIX/share/licenses/." "$DEST/licenses/"
cp "$SRC/dlls/libnghttp2/COPYING" "$DEST/licenses/nghttp2.txt"
cp "$SRC/overpass/README.md" "$DEST/OVERPASS-WINE.md"
# What this build supports, for OverPass and component manifests (see overpass-components' SOCKET.md).
mkdir -p "$DEST/share/overpass" && cp "$SRC/overpass/features.json" "$DEST/share/overpass/"
git -C "$SRC" rev-parse HEAD > "$DEST/SOURCE-COMMIT" 2>/dev/null || true

cd "$BUILD/stage"
tar -cJf "$SRC/overpass-wine-$VERSION-x86_64.tar.xz" "overpass-wine-$VERSION"
echo "Packaged $SRC/overpass-wine-$VERSION-x86_64.tar.xz"
# The tarball holds everything the stage did.
rm -rf "$BUILD/stage"
# On a maintainer's Mac, OverPass's clean-up script clears older tarballs, temp files and caches.
CLEAN="${OVERPASS_CLEAN_SCRIPT:-$HOME/Dev/OverPass/src/scripts/clean.sh}"
if [ -x "$CLEAN" ]; then "$CLEAN" --after-build || true; fi
