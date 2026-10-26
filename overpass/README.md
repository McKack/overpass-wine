# OverPass Wine

The Wine build used by [OverPass](https://github.com/McKack/OverPass), a macOS app for running Windows games.

## What this is

Three layers, each a separate commit so every change can be traced:

| Layer | Tag | Source |
|---|---|---|
| Upstream Wine 11.0 | `wine-11.0` | https://dl.winehq.org/wine/source/11.0/ |
| CodeWeavers' CrossOver 26.3.0 changes | `crossover-26.3.0` | https://media.codeweavers.com/pub/crossover/source/ (the `wine/` directory, unmodified) |
| OverPass patches | `main` | This repository; listed in [PATCHES.md](PATCHES.md) |

CodeWeavers publishes the Wine part of CrossOver under the LGPL. Their layer adds, among other things, msync (Mach-semaphore synchronization), the `winemac.drv` hooks that D3DMetal and DXMT use, and per-application fixes marked "CW Hack". CrossOver's proprietary parts (its app, its compatibility database, and the `cxcompatdb` module) are not published and are not in this repository.

## Graphics backends

This build can host:
- **DXMT** (open source, Direct3D 10/11 → Metal). OverPass downloads it from https://github.com/3Shain/dxmt.
- **D3DMetal** (Apple, Direct3D 11/12 → Metal). It is not included and can't be redistributed. OverPass imports it from the user's own copy of Apple's Game Porting Toolkit.

OverPass chooses a backend per bottle by setting `OVERPASS_DLL_PATH_PREPEND` (see PATCHES.md).

## Video and audio codecs

Releases include GStreamer for Media Foundation, DirectShow and Windows Media playback (cutscenes, in-game
video, XAudio2's xWMA sounds), built from source by `overpass/build-gstreamer.sh`:
- MP4, AVI, ASF, WAV, Matroska and WebM containers
- H.264, HEVC and AAC through macOS's own VideoToolbox and AudioToolbox
- VP8 and VP9 (libvpx), Opus, Vorbis
- WMA (1, 2, Pro, Lossless, Voice), WMV 1–3, VC-1 and MS-MPEG4 through a minimal LGPL build of FFmpeg that
  holds only these decoders

## Licence

Wine, including CodeWeavers' changes and the OverPass patches, is licensed under the GNU LGPL 2.1 or later. See `COPYING.LIB` and `LICENSE`. Every binary release links back to the exact source commit it was built from.
The bundled GStreamer, GLib and FFmpeg are LGPL too, the other codec libraries BSD or MIT; their licence texts
are in each release's `licenses/` folder.

## Building

On an Intel Mac (or the `macos-15-intel` GitHub runner) with Homebrew:

```sh
brew install bison cmake flex mingw-w64 pkgconf freetype gnutls molten-vk vulkan-loader dylibbundler meson ninja nasm
overpass/build.sh
```

CI builds every push. Tags named `overpass-wine-<version>` publish a release.
