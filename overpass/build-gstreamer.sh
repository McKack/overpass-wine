#!/bin/bash
# Builds the GStreamer that winegstreamer uses for game video (Media Foundation, DirectShow, Windows Media):
# GStreamer's own sources, every dependency from its meson subprojects (nothing from Homebrew), x86_64,
# only the plugins games need. The same on Intel (CI) and Apple silicon Macs, where it's a cross build.
#   build-gstreamer.sh <work dir> <install prefix>
#
# Plugins: what CrossOver 26 ships (MP4, AVI, ASF and WAV demuxers, audio and video parsers, H.264, HEVC and
# AAC through Apple's VideoToolbox and AudioToolbox, conversion and scaling, deinterlacing), plus Matroska/WebM
# with VP8/VP9 (libvpx), Opus and Vorbis, which UE4/UE5 cutscenes use and CrossOver lacks.
set -euo pipefail

GST_VERSION=1.28.8
VPX_VERSION=1.17.0
WORK="${1:?usage: build-gstreamer.sh <work dir> <install prefix>}"
PREFIX="${2:?usage: build-gstreamer.sh <work dir> <install prefix>}"
MIN_MACOS="${MACOSX_DEPLOYMENT_TARGET:-11.0}"
# Wine's build exports Homebrew's paths; none of them may reach this build.
unset PKG_CONFIG_PATH CPPFLAGS CFLAGS LDFLAGS
mkdir -p "$WORK" "$PREFIX"
WORK="$(cd "$WORK" && pwd)" PREFIX="$(cd "$PREFIX" && pwd)"

[ -d "$WORK/gstreamer" ] ||
    git clone --depth 1 --branch "$GST_VERSION" https://gitlab.freedesktop.org/gstreamer/gstreamer.git "$WORK/gstreamer"

# libvpx with its own configure script: the meson port in GStreamer's subprojects leaves the VP9 decoder out.
if [ ! -f "$WORK/vpx/lib/pkgconfig/vpx.pc" ]; then
    [ -d "$WORK/libvpx" ] ||
        git clone --depth 1 --branch "v$VPX_VERSION" https://chromium.googlesource.com/webm/libvpx "$WORK/libvpx"
    rm -rf "$WORK/vpx-build" && mkdir -p "$WORK/vpx-build"
    (cd "$WORK/vpx-build" &&
        CFLAGS="-mmacosx-version-min=$MIN_MACOS" LDFLAGS="-mmacosx-version-min=$MIN_MACOS" \
        "$WORK/libvpx/configure" --target=x86_64-darwin20-gcc --prefix="$WORK/vpx" \
            --enable-shared --disable-static --disable-vp8-encoder --disable-vp9-encoder --enable-vp9-highbitdepth \
            --disable-examples --disable-tools --disable-docs --disable-unit-tests &&
        make -j"$(sysctl -n hw.ncpu)" && make install)
fi

# The pkg-config path holds only libvpx, so the host's libraries stay out: everything else comes from a
# subproject.
ARCH="'-arch', 'x86_64', '-mmacosx-version-min=$MIN_MACOS'"
cat > "$WORK/x86_64-darwin.ini" <<EOF
[binaries]
c = 'clang'
cpp = 'clang++'
objc = 'clang'
objcpp = 'clang++'
ar = 'ar'
strip = 'strip'
pkg-config = 'pkg-config'
nasm = 'nasm'

[built-in options]
c_args = [$ARCH]
cpp_args = [$ARCH]
objc_args = [$ARCH]
objcpp_args = [$ARCH]
c_link_args = [$ARCH]
cpp_link_args = [$ARCH]
objc_link_args = [$ARCH]
objcpp_link_args = [$ARCH]

[properties]
pkg_config_libdir = ['$WORK/vpx/lib/pkgconfig']
needs_exe_wrapper = false

[host_machine]
system = 'darwin'
subsystem = 'macos'
kernel = 'xnu'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
EOF

enable() { local module=$1; shift; for p in "$@"; do printf -- "-D%s:%s=enabled " "$module" "$p"; done; }

rm -rf "$WORK/build"
# shellcheck disable=SC2046
meson setup "$WORK/build" "$WORK/gstreamer" --cross-file "$WORK/x86_64-darwin.ini" \
    --prefix "$PREFIX" --libdir lib --buildtype release --strip --default-library shared \
    -Dauto_features=disabled \
    -Dbase=enabled -Dgood=enabled -Dbad=enabled -Dugly=enabled -Dorc=enabled -Dtools=enabled \
    -Dlibav=disabled -Ddevtools=disabled -Dges=disabled -Drtsp_server=disabled -Dpython=disabled \
    -Dgst-examples=disabled -Dtls=disabled -Dlibnice=disabled -Dtests=disabled -Dexamples=disabled \
    -Ddoc=disabled -Dintrospection=disabled -Dnls=disabled -Dgpl=disabled \
    $(enable gst-plugins-base app audioconvert audioresample playback typefind videoconvertscale pbtypes \
        ogg opus vorbis gl) \
    -Dgst-plugins-base:gl_api=opengl -Dgst-plugins-base:gl_platform=cgl -Dgst-plugins-base:gl_winsys=cocoa \
    $(enable gst-plugins-good audioparsers avi deinterlace id3demux isomp4 matroska videofilter wavparse vpx \
        osxaudio) \
    $(enable gst-plugins-bad applemedia videoparsers gl) \
    $(enable gst-plugins-ugly asfdemux)
meson compile -C "$WORK/build"
meson install -C "$WORK/build" --quiet
cp -a "$WORK"/vpx/lib/libvpx*.dylib "$PREFIX/lib/"

# Licence texts of everything linked into what ships (LGPL for GStreamer, GLib and libintl; BSD and MIT for the rest).
S="$WORK/gstreamer/subprojects"
for dir in "$S"/gstreamer "$S"/gst-plugins-{base,good,bad,ugly} "$S"/glib-* "$S"/libffi-* "$S"/proxy-libintl-* \
           "$S"/orc "$S"/pcre2-* "$S"/libogg-* "$S"/libvorbis-* "$S"/opus-* "$WORK/libvpx"; do
    [ -d "$dir" ] || continue
    mkdir -p "$PREFIX/share/licenses/$(basename "$dir")"
    for f in COPYING LICENSE LICENCE.md LICENSES PATENTS; do
        [ -e "$dir/$f" ] && cp -R "$dir/$f" "$PREFIX/share/licenses/$(basename "$dir")/"
    done
done
echo "GStreamer $GST_VERSION installed to $PREFIX"
