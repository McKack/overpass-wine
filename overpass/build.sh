#!/bin/bash
# Builds and packages OverPass Wine on an x86_64 macOS host with Homebrew.
# Output: overpass-wine-<version>-x86_64.tar.xz containing bin/, lib/, share/ (same layout OverPass expects).
set -euo pipefail

SRC="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="${OVERPASS_WINE_VERSION:-dev}"
BUILD="$SRC/build-overpass"
DEST="$BUILD/stage/overpass-wine-$VERSION"
JOBS="$(sysctl -n hw.ncpu)"

BREW="$(brew --prefix)"
export PATH="$BREW/opt/bison/bin:$BREW/opt/flex/bin:$PATH"
export PKG_CONFIG_PATH="$BREW/lib/pkgconfig:$BREW/share/pkgconfig"
export CPPFLAGS="-I$BREW/include"
export LDFLAGS="-L$BREW/lib"
export MACOSX_DEPLOYMENT_TARGET=11.0

rm -rf "$BUILD"
mkdir -p "$BUILD/obj" "$DEST"
cd "$BUILD/obj"

# No --with-opengl: Wine 11 makes EGL a hard requirement then, but winemac.drv does OpenGL through CGL.
"$SRC/configure" \
    --prefix="$DEST" \
    --enable-archs=i386,x86_64 \
    --disable-tests \
    --with-coreaudio --with-freetype --with-gnutls --with-mingw \
    --with-pthread --with-sdl --with-unwind --with-vulkan \
    --without-alsa --without-capi --without-dbus --without-fontconfig --without-gettextpo \
    --without-gphoto --without-gssapi --without-gstreamer --without-krb5 --without-netapi \
    --without-oss --without-pulse --without-sane --without-udev --without-usb \
    --without-v4l2 --without-wayland --without-x

make -j"$JOBS"
make install-lib

# Wine dlopen()s these by leaf name; macOS resolves leaf names through the caller's LC_RPATH,
# so ship them in lib/ and point the binaries there.
LIB="$DEST/lib"
for f in \
    "$BREW/opt/freetype/lib/libfreetype.6.dylib" \
    "$BREW/opt/gnutls/lib/libgnutls.30.dylib" \
    "$BREW/opt/sdl2/lib/libSDL2-2.0.0.dylib" \
    "$BREW/opt/molten-vk/lib/libMoltenVK.dylib" \
    "$BREW/opt/vulkan-loader/lib/libvulkan.1.dylib"; do
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

# Licence and provenance travel with the binaries (LGPL-2.1 §6).
cp "$SRC/COPYING.LIB" "$SRC/LICENSE" "$SRC/AUTHORS" "$DEST/"
cp "$SRC/overpass/README.md" "$DEST/OVERPASS-WINE.md"
git -C "$SRC" rev-parse HEAD > "$DEST/SOURCE-COMMIT" 2>/dev/null || true

cd "$BUILD/stage"
tar -cJf "$SRC/overpass-wine-$VERSION-x86_64.tar.xz" "overpass-wine-$VERSION"
echo "Packaged $SRC/overpass-wine-$VERSION-x86_64.tar.xz"
