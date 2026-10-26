#!/bin/bash
# Builds the libraries Wine links or loads, x86_64, from their own sources (nothing from Homebrew, which no longer
# supports Intel): FreeType, GnuTLS (with GMP and Nettle), SDL 2, libinotify, the Vulkan loader, and MoltenVK
# from its release. The same on Intel (CI) and Apple silicon Macs, where it's a cross build.
#   build-deps.sh <work dir> <install prefix>
#
# The set follows CrossOver 26: FreeType without PNG or Brotli, GnuTLS with its own copies of libtasn1 and
# libunistring and without p11-kit or IDN, so only GMP and Nettle come along.
set -euo pipefail

GMP_VERSION=6.3.0
NETTLE_VERSION=4.0
GNUTLS_VERSION=3.8.13
FREETYPE_VERSION=2.15.0
# SDL 2.30, the version CrossOver ships. Homebrew's sdl2 is now sdl2-compat, a shim over SDL3 that makes
# winebus.sys (game controllers) abort with "NSWindow should only be instantiated on the main thread".
SDL_VERSION=2.30.12
INOTIFY_VERSION=20240724
VULKAN_VERSION=1.4.363.0
MOLTENVK_VERSION=1.4.2

WORK="${1:?usage: build-deps.sh <work dir> <install prefix>}"
PREFIX="${2:?usage: build-deps.sh <work dir> <install prefix>}"
MIN_MACOS="${MACOSX_DEPLOYMENT_TARGET:-11.0}"
JOBS="$(sysctl -n hw.ncpu)"
# Wine's build exports Homebrew's paths; none of them may reach this build.
unset PKG_CONFIG_PATH CPPFLAGS CFLAGS LDFLAGS
mkdir -p "$WORK" "$PREFIX"
WORK="$(cd "$WORK" && pwd)" PREFIX="$(cd "$PREFIX" && pwd)"
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"

export CC="clang -arch x86_64" CXX="clang++ -arch x86_64"
export CFLAGS="-O2 -mmacosx-version-min=$MIN_MACOS" CXXFLAGS="-O2 -mmacosx-version-min=$MIN_MACOS"
export CPPFLAGS="-I$PREFIX/include" LDFLAGS="-L$PREFIX/lib -mmacosx-version-min=$MIN_MACOS"
AUTOTOOLS=(--host=x86_64-apple-darwin --prefix="$PREFIX" --enable-shared --disable-static)
CMAKE=(-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_INSTALL_NAME_DIR="$PREFIX/lib"
       -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN_MACOS" -DCMAKE_PREFIX_PATH="$PREFIX"
       -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_IGNORE_PREFIX_PATH="/opt/homebrew;/usr/local"
       -DBUILD_SHARED_LIBS=ON)
if command -v ccache >/dev/null; then CMAKE+=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache); fi

# fetch <url> <dir name>: unpacks a source tarball into $WORK once.
fetch() { [ -d "$WORK/$2" ] || curl -fsSL "$1" | tar -x -C "$WORK"; }
# licence <name> <files...>: licence texts travel with the binaries.
licence() { local name=$1; shift; mkdir -p "$PREFIX/share/licenses/$name"; cp "$@" "$PREFIX/share/licenses/$name/"; }

# GMP and Nettle with run-time CPU detection (--enable-fat): a build tuned to the build machine's CPU could use
# instructions Rosetta doesn't have.
fetch "https://ftpmirror.gnu.org/gmp/gmp-$GMP_VERSION.tar.xz" "gmp-$GMP_VERSION"
(cd "$WORK/gmp-$GMP_VERSION" && ./configure "${AUTOTOOLS[@]}" --enable-fat && make -j"$JOBS" && make install)
licence gmp "$WORK/gmp-$GMP_VERSION"/{COPYING.LESSERv3,COPYINGv2,COPYINGv3}

fetch "https://ftpmirror.gnu.org/nettle/nettle-$NETTLE_VERSION.tar.gz" "nettle-$NETTLE_VERSION"
(cd "$WORK/nettle-$NETTLE_VERSION" &&
    ./configure "${AUTOTOOLS[@]}" --enable-fat --disable-documentation --disable-openssl --libdir="$PREFIX/lib" &&
    make -j"$JOBS" && make install)
licence nettle "$WORK/nettle-$NETTLE_VERSION"/{COPYING.LESSERv3,COPYINGv2,COPYINGv3}

fetch "https://www.gnupg.org/ftp/gcrypt/gnutls/v${GNUTLS_VERSION%.*}/gnutls-$GNUTLS_VERSION.tar.xz" "gnutls-$GNUTLS_VERSION"
(cd "$WORK/gnutls-$GNUTLS_VERSION" &&
    ./configure "${AUTOTOOLS[@]}" --with-included-libtasn1 --with-included-unistring --without-p11-kit \
        --without-idn --without-brotli --without-zstd --without-tpm --without-tpm2 --disable-libdane \
        --disable-cxx --disable-nls --disable-guile --disable-doc --disable-manpages --disable-tools \
        --disable-tests --disable-full-test-suite &&
    make -j"$JOBS" && make install)
licence gnutls "$WORK/gnutls-$GNUTLS_VERSION"/{COPYING,COPYING.LESSERv2}

fetch "https://downloads.sourceforge.net/project/freetype/freetype2/$FREETYPE_VERSION/freetype-$FREETYPE_VERSION.tar.xz" \
    "freetype-$FREETYPE_VERSION"
rm -rf "$WORK/freetype-build"
cmake -S "$WORK/freetype-$FREETYPE_VERSION" -B "$WORK/freetype-build" "${CMAKE[@]}" \
    -DFT_DISABLE_PNG=TRUE -DFT_DISABLE_BROTLI=TRUE -DFT_DISABLE_HARFBUZZ=TRUE
cmake --build "$WORK/freetype-build" -j"$JOBS" && cmake --install "$WORK/freetype-build"
licence freetype "$WORK/freetype-$FREETYPE_VERSION"/{LICENSE.TXT,docs/FTL.TXT}

fetch "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL2-$SDL_VERSION.tar.gz" "SDL2-$SDL_VERSION"
rm -rf "$WORK/sdl2-build"
cmake -S "$WORK/SDL2-$SDL_VERSION" -B "$WORK/sdl2-build" "${CMAKE[@]}" -DSDL_STATIC=OFF -DSDL_TEST=OFF
cmake --build "$WORK/sdl2-build" -j"$JOBS" && cmake --install "$WORK/sdl2-build"
licence sdl2 "$WORK/SDL2-$SDL_VERSION/LICENSE.txt"

# libinotify (inotify on kqueue, MIT), as CrossOver ships it: wineserver uses it to tell programs that files
# in a folder changed (FindFirstChangeNotification, ReadDirectoryChangesW). Launchers wait on this.
fetch "https://github.com/libinotify-kqueue/libinotify-kqueue/releases/download/$INOTIFY_VERSION/libinotify-$INOTIFY_VERSION.tar.gz" \
    "libinotify-$INOTIFY_VERSION"
# fdclosedir() is in the SDK but only on macOS 26.4 and later.
(cd "$WORK/libinotify-$INOTIFY_VERSION" && ./configure "${AUTOTOOLS[@]}" ac_cv_func_fdclosedir=no && make -j"$JOBS" && make install)
licence libinotify "$WORK/libinotify-$INOTIFY_VERSION/LICENSE"

fetch "https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-$VULKAN_VERSION.tar.gz" \
    "Vulkan-Headers-vulkan-sdk-$VULKAN_VERSION"
rm -rf "$WORK/vulkan-headers-build"
cmake -S "$WORK/Vulkan-Headers-vulkan-sdk-$VULKAN_VERSION" -B "$WORK/vulkan-headers-build" "${CMAKE[@]}"
cmake --install "$WORK/vulkan-headers-build"
fetch "https://github.com/KhronosGroup/Vulkan-Loader/archive/refs/tags/vulkan-sdk-$VULKAN_VERSION.tar.gz" \
    "Vulkan-Loader-vulkan-sdk-$VULKAN_VERSION"
rm -rf "$WORK/vulkan-loader-build"
cmake -S "$WORK/Vulkan-Loader-vulkan-sdk-$VULKAN_VERSION" -B "$WORK/vulkan-loader-build" "${CMAKE[@]}" \
    -DBUILD_TESTS=OFF
cmake --build "$WORK/vulkan-loader-build" -j"$JOBS" && cmake --install "$WORK/vulkan-loader-build"
licence vulkan-loader "$WORK/Vulkan-Loader-vulkan-sdk-$VULKAN_VERSION/LICENSE.txt"

# MoltenVK from its release (building it needs Xcode and a long list of shader compilers), x86_64 only.
# The release needs macOS 12.
[ -d "$WORK/MoltenVK" ] ||
    curl -fsSL "https://github.com/KhronosGroup/MoltenVK/releases/download/v$MOLTENVK_VERSION/MoltenVK-macos.tar" |
    tar -x -C "$WORK"
lipo -thin x86_64 "$WORK/MoltenVK/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib" -output "$PREFIX/lib/libMoltenVK.dylib"
licence moltenvk "$WORK/MoltenVK/LICENSE"
