#!/bin/bash
# Copies GStreamer (from build-gstreamer.sh) into a Wine install and makes it relocatable: libraries in lib/,
# plugins in lib/gstreamer-1.0/, the plugin scanner in libexec/gstreamer-1.0/, every reference @rpath/<name>.
#   bundle-gstreamer.sh <gstreamer prefix> <wine install dir>
# Wine's winegstreamer.so already looks in lib/ (its rpath is @loader_path/../../). OverPass points GStreamer
# at the plugins with GST_PLUGIN_SYSTEM_PATH and GST_PLUGIN_SCANNER.
set -euo pipefail

GST="$(cd "${1:?usage: bundle-gstreamer.sh <gstreamer prefix> <wine install dir>}" && pwd)"
DEST="$(cd "${2:?usage: bundle-gstreamer.sh <gstreamer prefix> <wine install dir>}" && pwd)"
LIB="$DEST/lib"
PLUGINS="$LIB/gstreamer-1.0"
SCANNER_DIR="$DEST/libexec/gstreamer-1.0"
mkdir -p "$PLUGINS" "$SCANNER_DIR"

# Real files only; the unversioned names are symlinks the loader never asks for.
for f in "$GST"/lib/*.dylib; do [ -L "$f" ] || cp "$f" "$LIB/"; done
cp "$GST"/lib/gstreamer-1.0/*.dylib "$PLUGINS/"
cp "$GST/libexec/gstreamer-1.0/gst-plugin-scanner" "$SCANNER_DIR/"
mkdir -p "$DEST/licenses" && cp -R "$GST/share/licenses/." "$DEST/licenses/"
chmod u+w "$LIB"/*.dylib "$PLUGINS"/*.dylib "$SCANNER_DIR/gst-plugin-scanner"

# Every reference to a library we ship (absolute build paths, or a bare name like libvpx's) becomes @rpath.
relink() {
    local file=$1 rpath=$2 dep name
    otool -L "$file" | tail -n +2 | awk '{print $1}' | while read -r dep; do
        name="$(basename "$dep")"
        case "$dep" in /usr/lib/*|/System/*|@rpath/*) continue ;; esac
        [ -f "$LIB/$name" ] && install_name_tool -change "$dep" "@rpath/$name" "$file"
    done
    install_name_tool -add_rpath "$rpath" "$file" 2>/dev/null || true
}
for f in "$GST"/lib/*.dylib; do
    [ -L "$f" ] && continue
    install_name_tool -id "@rpath/$(basename "$f")" "$LIB/$(basename "$f")"
    relink "$LIB/$(basename "$f")" @loader_path
done
for f in "$PLUGINS"/*.dylib; do
    install_name_tool -id "@rpath/gstreamer-1.0/$(basename "$f")" "$f"
    relink "$f" @loader_path/..
done
relink "$SCANNER_DIR/gst-plugin-scanner" @loader_path/../../lib

# Anything still pointing into the build machine means a library was missed.
if otool -L "$LIB"/*.dylib "$PLUGINS"/*.dylib "$SCANNER_DIR/gst-plugin-scanner" | grep -F "$GST"; then
    echo "bundle-gstreamer.sh: references to $GST remain" >&2
    exit 1
fi
echo "GStreamer bundled into $DEST"
