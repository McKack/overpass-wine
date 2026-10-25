# OverPass patches

Every patch on top of `crossover-26.3.0` is listed here with how it was verified.

| # | Patch | Files | Status | Upstream candidate |
|---|---|---|---|---|
| 1 | `OVERPASS_DLL_PATH_PREPEND`: colon-separated directories searched before Wine's own DLL directory, so a bottle can use DXMT or D3DMetal without modifying the Wine install. | `dlls/ntdll/unix/loader.c` | Builds in CI; runtime check pending | No: OverPass-specific. CodeWeavers already exports `prepend_dll_path()` for the same purpose. |
| 2 | Consistent Windows username `overpass` instead of CodeWeavers' `crossover` (CrossOver Hack 12735), so bottles have `C:\users\overpass`. `CX_REPORT_REAL_USERNAME` still reports the real one. | `dlls/advapi32/advapi.c`, `dlls/shell32/shellpath.c` | Builds in CI; new-bottle check pending | No: OverPass-specific. |
| 3 | Compatibility rules: INI files (folders or files listed in `OVERPASS_COMPAT_RULES`), read as they are with no build step, adjust every process created from Windows code (command-line arguments, environment, DLL overrides, graphics backend, starting a different program in place of the one asked for), matched by program path or `SteamGameId`. Open replacement for the per-app behaviour of CrossOver's closed `cxcompatdb.so`; Unix-side settings travel through CodeWeavers' `__CX_UNIX_` promotion. Format documented in the source. | `dlls/kernelbase/overpass_compat.c`, `dlls/kernelbase/process.c` | CI smoke test (env rule reaches a child `cmd.exe`; `replace-path` turns `hostname.exe` into `cmd.exe`) | No: OverPass-specific. |
| 4 | Rosetta code regions without GPTK: when `CX_APPLEGPTK_LIBD3DSHARED_PATH` is unset, ntdll itself tells Rosetta where each native PE module's code is (Rosetta's syscall `0x7000009`, the same call libd3dshared makes), after the same checks: running under Rosetta, Rosetta build ≥ 304. CrossOver gets this in every win64 bottle through libd3dshared; without the patch, OverPass bottles only got it with D3DMetal imported. `OVERPASS_NO_ROSETTA_CODE_REGIONS=1` turns it off, for comparing. | `dlls/ntdll/unix/loader.c` | The new functions, built in a test program with `-Wall -Werror`, register a region on an M4 under Rosetta build 367, and the switch disables them. CI build and a with/without game measurement pending | No: depends on CodeWeavers' CW HACK 22434. |

## Build changes

| Change | Why | Verified |
|---|---|---|
| Build SDL 2.30.12 from source instead of Homebrew's `sdl2` | Homebrew's `sdl2` is `sdl2-compat` (SDL3 underneath). With it, `winedevice.exe` aborts when loading `winebus.sys`: `NSWindow should only be instantiated on the main thread!`. Setting winebus `Enable SDL`=0 stops the crash, which confirms SDL as the cause. CrossOver 26 ships SDL 2.30.12. | Crash reproduced locally with build a8d4038. With CrossOver's SDL 2.30.12 in its place, a prefix boots on an M4 Mac and winebus.sys starts its SDL loop. On the CI runner (no desktop session) the first build with our SDL hung in `wineboot --init`; the smoke test now has time limits and hid traces to show where |
| ccache for clang (unix side), mingw (PE side) and the SDL build; CI keeps the cache between runs | A full build takes 80–112 min on the Intel runner; a small patch should only recompile what changed | Pending: first cached run fills the cache, the second shows the gain |

## Upstreaming policy

Fixes that might help the wider Wine community start here, not as patches sent to WineHQ or CodeWeavers. A fix is proposed upstream only when all of these hold:

1. **Reproducible.** There is a minimal test case, or exact steps with a named application, that show the bug.
2. **Verified.** It has been tested on real hardware, with before/after results recorded here, and doesn't regress the CI smoke test.
3. **Understood.** The change is small, explained, and follows Wine's coding and submission guidelines (https://gitlab.winehq.org/wine/wine/-/wikis/Submitting-Patches).
4. **Reviewed.** A maintainer of this repository has reviewed and approved it.

No automated or bulk submissions. One well-tested patch beats many speculative ones.
