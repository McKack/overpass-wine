#!/bin/bash
# Builds DXVK for the "DXVK (Vulkan via MoltenVK)" graphics choice: CodeWeavers' fork of DXVK 1.10.3 from the
# CrossOver source release (zlib licence), as CrossOver ships it: d3d9, d3d10, d3d10_1, d3d10core and d3d11
# marked as Wine builtin DLLs, without DXVK's dxgi (Wine's own dxgi creates DXVK swap chains through
# IWineDXGISwapChainFactory). Only mingw-w64, meson and glslang are needed; the DLLs are the same on any Mac.
#   build-dxvk.sh <work dir> <install prefix>     (installs <prefix>/x86_64-windows and <prefix>/i386-windows)
#
# Our patches, inline so that changing one rebuilds DXVK (build.sh rebuilds when this script changes):
#  - mingw-w64 14 declares D3DDEVINFO_RESOURCEMANAGER and ID3D10StateBlock's GUID itself, and GCC 16 no longer
#    includes <cstdint> along the way (-include cstdint).
#  - MoltenVK has no geometry shaders and no cull distances: the D3D11 device is created without them (DXVK
#    leaves the geometry stage out then), so feature level 11_0 works; a game's geometry shader fails to compile.
#    CodeWeavers' MoltenVK reports geometryShader without supporting it instead.
set -euo pipefail

CX_VERSION=26.3.0
CX_SHA256=ac99c8ca4b3848f3e81784135f023df266b61c2345726ea55a50b3e030dd6872
WORK="${1:?usage: build-dxvk.sh <work dir> <install prefix>}"
PREFIX="${2:?usage: build-dxvk.sh <work dir> <install prefix>}"
unset PKG_CONFIG_PATH CPPFLAGS CFLAGS LDFLAGS
mkdir -p "$WORK" "$PREFIX"
WORK="$(cd "$WORK" && pwd)" PREFIX="$(cd "$PREFIX" && pwd)"
for tool in meson ninja glslangValidator x86_64-w64-mingw32-g++ i686-w64-mingw32-g++; do
    command -v "$tool" >/dev/null || { echo "build-dxvk.sh: $tool is missing (brew install meson ninja glslang mingw-w64)" >&2; exit 1; }
done

TARBALL="$WORK/crossover-sources-$CX_VERSION.tar.gz"
if [ ! -f "$TARBALL" ] || ! echo "$CX_SHA256  $TARBALL" | shasum -a 256 -c --status; then
    curl -fsSL -o "$TARBALL" "https://media.codeweavers.com/pub/crossover/source/crossover-sources-$CX_VERSION.tar.gz"
    echo "$CX_SHA256  $TARBALL" | shasum -a 256 -c
fi
rm -rf "$WORK/sources/dxvk"
tar -xzf "$TARBALL" -C "$WORK" sources/dxvk
SRC="$WORK/sources/dxvk"

patch -d "$SRC" -p1 <<'PATCH'
--- a/src/d3d9/d3d9_include.h
+++ b/src/d3d9/d3d9_include.h
@@ -79,10 +79,13 @@
 
 // MinGW headers are broken. Who'dve guessed?
 #ifndef _MSC_VER
+// mingw-w64 14 defines the real struct.
+#if !defined(__MINGW64_VERSION_MAJOR) || __MINGW64_VERSION_MAJOR < 14
 typedef struct _D3DDEVINFO_RESOURCEMANAGER
 {
   char dummy;
 } D3DDEVINFO_RESOURCEMANAGER, * LPD3DDEVINFO_RESOURCEMANAGER;
+#endif
 
 #ifndef __WINE__
 extern "C" WINUSERAPI WINBOOL WINAPI SetProcessDPIAware(VOID);
--- a/src/d3d10/d3d10_interfaces.h
+++ b/src/d3d10/d3d10_interfaces.h
@@ -4,6 +4,7 @@
 
 #ifdef _MSC_VER
 struct __declspec(uuid("0803425a-57f5-4dd6-9465-a87570834a08")) ID3D10StateBlock;
-#else
+#elif !defined(__MINGW64_VERSION_MAJOR) || __MINGW64_VERSION_MAJOR < 14
+// mingw-w64 14 declares it in d3d10effect.h.
 __CRT_UUID_DECL(ID3D10StateBlock, 0x0803425a,0x57f5,0x4dd6,0x94,0x65,0xa8,0x75,0x70,0x83,0x4a,0x08);
 #endif
--- a/src/d3d11/d3d11_device.cpp
+++ b/src/d3d11/d3d11_device.cpp
@@ -1948,7 +1948,8 @@
     DxvkDeviceFeatures supported = adapter->features();
     DxvkDeviceFeatures enabled   = {};
 
-    enabled.core.features.geometryShader                          = VK_TRUE;
+    // MoltenVK has no geometry shaders: create the device without them; a geometry shader then fails to compile.
+    enabled.core.features.geometryShader                          = supported.core.features.geometryShader;
     enabled.core.features.robustBufferAccess                      = VK_TRUE;
     enabled.core.features.shaderStorageImageWriteWithoutFormat    = VK_TRUE;
     enabled.core.features.depthBounds                             = supported.core.features.depthBounds;
@@ -1981,7 +1982,7 @@
       enabled.core.features.sampleRateShading                     = VK_TRUE;
       enabled.core.features.samplerAnisotropy                     = supported.core.features.samplerAnisotropy;
       enabled.core.features.shaderClipDistance                    = VK_TRUE;
-      enabled.core.features.shaderCullDistance                    = VK_TRUE;
+      enabled.core.features.shaderCullDistance                    = supported.core.features.shaderCullDistance;
       enabled.core.features.textureCompressionBC                  = VK_TRUE;
       enabled.extDepthClipEnable.depthClipEnable                  = supported.extDepthClipEnable.depthClipEnable;
       enabled.extHostQueryReset.hostQueryReset                    = VK_TRUE;
PATCH

cat > "$WORK/cross64.txt" <<'CROSS'
[binaries]
c = 'x86_64-w64-mingw32-gcc'
cpp = 'x86_64-w64-mingw32-g++'
ar = 'x86_64-w64-mingw32-ar'
strip = 'x86_64-w64-mingw32-strip'
windres = 'x86_64-w64-mingw32-windres'

[properties]
needs_exe_wrapper = true

[host_machine]
system = 'windows'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
CROSS
sed -e 's/x86_64-w64-mingw32/i686-w64-mingw32/' -e "s/cpu_family = 'x86_64'/cpu_family = 'x86'/" \
    -e "s/cpu = 'x86_64'/cpu = 'i686'/" "$WORK/cross64.txt" > "$WORK/cross32.txt"

for arch in 64:x86_64-windows 32:i386-windows; do
    bits="${arch%%:*}" dest="$PREFIX/${arch#*:}"
    rm -rf "$WORK/build$bits" "$dest"
    CXXFLAGS="-include cstdint" meson setup --cross-file "$WORK/cross$bits.txt" --buildtype release --strip \
        -Denable_tests=false -Denable_dxgi=false "$WORK/build$bits" "$SRC"
    ninja -C "$WORK/build$bits"
    mkdir -p "$dest"
    for dll in d3d9 d3d10 d3d10_1 d3d10core d3d11; do
        file="$(find "$WORK/build$bits/src" -name "$dll.dll" | head -1)"
        cp "$file" "$dest/"
        # What `winebuild --builtin` does: Wine loads a DLL from its own DLL path only with this mark.
        printf 'Wine builtin DLL\0' | dd of="$dest/$dll.dll" bs=1 seek=64 conv=notrunc 2>/dev/null
    done
done
mkdir -p "$PREFIX/licenses" && cp "$SRC/LICENSE" "$PREFIX/licenses/dxvk.txt"
