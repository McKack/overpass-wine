# OverPass patches

Every patch on top of `crossover-26.3.0` is listed here with how it was verified.

| # | Patch | Files | Status | Upstream candidate |
|---|---|---|---|---|
| 1 | `OVERPASS_DLL_PATH_PREPEND`: colon-separated directories searched before Wine's own DLL directory, so a bottle can use DXMT or D3DMetal without modifying the Wine install. | `dlls/ntdll/unix/loader.c` | Builds in CI; runtime check pending | No: OverPass-specific. CodeWeavers already exports `prepend_dll_path()` for the same purpose. |

## Upstreaming policy

Fixes that might help the wider Wine community start here, not as patches sent to WineHQ or CodeWeavers. A fix is proposed upstream only when all of these hold:

1. **Reproducible.** There is a minimal test case, or exact steps with a named application, that show the bug.
2. **Verified.** It has been tested on real hardware, with before/after results recorded here, and doesn't regress the CI smoke test.
3. **Understood.** The change is small, explained, and follows Wine's coding and submission guidelines (https://gitlab.winehq.org/wine/wine/-/wikis/Submitting-Patches).
4. **Reviewed.** A maintainer of this repository has reviewed and approved it.

No automated or bulk submissions. One well-tested patch beats many speculative ones.
