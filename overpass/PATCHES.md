# OverPass patches

Every patch on top of `crossover-26.3.0` is listed here with how it was verified.

| # | Patch | Files | Status | Upstream candidate |
|---|---|---|---|---|
| 1 | `OVERPASS_DLL_PATH_PREPEND`: colon-separated directories searched before Wine's own DLL directory, so a bottle can use DXMT or D3DMetal without modifying the Wine install. | `dlls/ntdll/unix/loader.c` | Builds in CI; runtime check pending | No: OverPass-specific. CodeWeavers already exports `prepend_dll_path()` for the same purpose. |
| 2 | Consistent Windows username `overpass` instead of CodeWeavers' `crossover` (CrossOver Hack 12735), so bottles have `C:\users\overpass`. `CX_REPORT_REAL_USERNAME` still reports the real one. | `dlls/advapi32/advapi.c`, `dlls/shell32/shellpath.c` | Builds in CI; new-bottle check pending | No: OverPass-specific. |
| 3 | Compatibility rules: INI files (folders or files listed in `OVERPASS_COMPAT_RULES`), read as they are with no build step, adjust every process created from Windows code (command-line arguments, environment, DLL overrides, graphics backend), matched by program path or `SteamGameId`. Open replacement for the per-app behaviour of CrossOver's closed `cxcompatdb.so`; Unix-side settings travel through CodeWeavers' `__CX_UNIX_` promotion. Format documented in the source. | `dlls/kernelbase/overpass_compat.c`, `dlls/kernelbase/process.c` | CI smoke test (env rule reaches a child `cmd.exe`) | No: OverPass-specific. |

## Build changes

| Change | Why | Verified |
|---|---|---|
| Build SDL 2.30.12 from source instead of Homebrew's `sdl2` | Homebrew's `sdl2` is `sdl2-compat` (SDL3 underneath). With it, `winedevice.exe` aborts when loading `winebus.sys`: `NSWindow should only be instantiated on the main thread!`. Setting winebus `Enable SDL`=0 stops the crash, which confirms SDL as the cause. CrossOver 26 ships SDL 2.30.12. | Crash reproduced locally with build a8d4038; fix checked by the CI smoke test |

## Upstreaming policy

Fixes that might help the wider Wine community start here, not as patches sent to WineHQ or CodeWeavers. A fix is proposed upstream only when all of these hold:

1. **Reproducible.** There is a minimal test case, or exact steps with a named application, that show the bug.
2. **Verified.** It has been tested on real hardware, with before/after results recorded here, and doesn't regress the CI smoke test.
3. **Understood.** The change is small, explained, and follows Wine's coding and submission guidelines (https://gitlab.winehq.org/wine/wine/-/wikis/Submitting-Patches).
4. **Reviewed.** A maintainer of this repository has reviewed and approved it.

No automated or bulk submissions. One well-tested patch beats many speculative ones.
