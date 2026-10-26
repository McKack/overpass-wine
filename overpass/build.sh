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

# With ccache installed (CI keeps its cache between runs), compiles that didn't change are reused.
# Wine builds unix code with clang and PE code with mingw, so both are wrapped.
CMAKE_LAUNCHER=()
WINE_CC=()
if command -v ccache >/dev/null; then
    export CCACHE_BASEDIR="$SRC" CCACHE_COMPILERCHECK=content CCACHE_NOHASHDIR=1
    CMAKE_LAUNCHER=(-DCMAKE_C_COMPILER_LAUNCHER=ccache)
    WINE_CC=(CC="ccache clang" x86_64_CC="ccache x86_64-w64-mingw32-gcc" i386_CC="ccache i686-w64-mingw32-gcc")
    ccache -z >/dev/null
fi

# SDL 2.30, the version CrossOver ships. Homebrew's sdl2 is now sdl2-compat, a shim over SDL3 that makes
# winebus.sys (game controllers) abort with "NSWindow should only be instantiated on the main thread".
SDL_VERSION=2.30.12
SDL_PREFIX="$BUILD/sdl2"
curl -fsSL "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL2-$SDL_VERSION.tar.gz" | tar -xz -C "$BUILD"
cmake -S "$BUILD/SDL2-$SDL_VERSION" -B "$BUILD/sdl2-build" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$SDL_PREFIX" \
    -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOSX_DEPLOYMENT_TARGET" \
    -DSDL_STATIC=OFF -DSDL_TEST=OFF ${CMAKE_LAUNCHER[@]+"${CMAKE_LAUNCHER[@]}"}
cmake --build "$BUILD/sdl2-build" -j"$JOBS"
cmake --install "$BUILD/sdl2-build"
export PKG_CONFIG_PATH="$SDL_PREFIX/lib/pkgconfig:$PKG_CONFIG_PATH"

# libinotify (inotify on kqueue, MIT), as CrossOver ships it: wineserver uses it to tell programs that files
# in a folder changed (FindFirstChangeNotification, ReadDirectoryChangesW). Launchers wait on this.
INOTIFY_VERSION=20240724
INOTIFY_PREFIX="$BUILD/inotify"
curl -fsSL "https://github.com/libinotify-kqueue/libinotify-kqueue/releases/download/$INOTIFY_VERSION/libinotify-$INOTIFY_VERSION.tar.gz" | tar -xz -C "$BUILD"
(cd "$BUILD/libinotify-$INOTIFY_VERSION" &&
    ./configure --prefix="$INOTIFY_PREFIX" --disable-static --enable-shared &&
    make -j"$JOBS" && make install)
export PKG_CONFIG_PATH="$INOTIFY_PREFIX/lib/pkgconfig:$PKG_CONFIG_PATH"

# GStreamer for game video (winegstreamer); see build-gstreamer.sh for what's in it. It lives outside $BUILD and
# is reused while build-gstreamer.sh is unchanged (CI caches the folder): it takes longer to build than Wine.
GST_ROOT="$SRC/build-gstreamer"
GST_PREFIX="$GST_ROOT/prefix"
GST_STAMP="$(shasum -a 256 "$SRC/overpass/build-gstreamer.sh" | cut -d' ' -f1) $MACOSX_DEPLOYMENT_TARGET"
if [ "$(cat "$GST_ROOT/stamp" 2>/dev/null)" != "$GST_STAMP" ]; then
    rm -rf "$GST_ROOT"
    "$SRC/overpass/build-gstreamer.sh" "$GST_ROOT/work" "$GST_PREFIX"
    rm -rf "$GST_ROOT/work"
    echo "$GST_STAMP" > "$GST_ROOT/stamp"
fi
export PKG_CONFIG_PATH="$GST_PREFIX/lib/pkgconfig:$PKG_CONFIG_PATH"

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
    "$BREW/opt/freetype/lib/libfreetype.6.dylib" \
    "$BREW/opt/gnutls/lib/libgnutls.30.dylib" \
    "$SDL_PREFIX/lib/libSDL2-2.0.0.dylib" \
    "$INOTIFY_PREFIX/lib/libinotify.0.dylib" \
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

# GStreamer goes in after dylibbundler, already relocatable.
"$SRC/overpass/bundle-gstreamer.sh" "$GST_PREFIX" "$DEST"

# Libraries built here are linked by their build path (wineserver: libinotify; winegstreamer.so: GStreamer).
for f in "$DEST"/bin/wine "$DEST"/bin/wineserver "$LIB"/wine/x86_64-unix/*.so; do
    otool -L "$f" | tail -n +2 | awk '{print $1}' | { grep -e "^$BUILD/" -e "^$GST_ROOT/" || true; } | while read -r dep; do
        install_name_tool -change "$dep" "@rpath/$(basename "$dep")" "$f"
    done
done
if otool -L "$DEST"/bin/* "$LIB"/wine/x86_64-unix/*.so "$LIB"/*.dylib 2>/dev/null | grep -F -e "$BUILD/" -e "$GST_ROOT/"; then
    echo "build.sh: binaries still reference $BUILD or $GST_ROOT" >&2
    exit 1
fi

# Licence and provenance travel with the binaries (LGPL-2.1 §6).
cp "$SRC/COPYING.LIB" "$SRC/LICENSE" "$SRC/AUTHORS" "$DEST/"
mkdir -p "$DEST/licenses/libinotify" && cp "$BUILD/libinotify-$INOTIFY_VERSION/LICENSE" "$DEST/licenses/libinotify/"
cp "$SRC/overpass/README.md" "$DEST/OVERPASS-WINE.md"
git -C "$SRC" rev-parse HEAD > "$DEST/SOURCE-COMMIT" 2>/dev/null || true

cd "$BUILD/stage"
tar -cJf "$SRC/overpass-wine-$VERSION-x86_64.tar.xz" "overpass-wine-$VERSION"
echo "Packaged $SRC/overpass-wine-$VERSION-x86_64.tar.xz"
