/* d3d9.exe: clears to a colour, draws a fixed-function triangle, reads the back buffer back and checks pixels. */
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
struct vtx { float x, y, z, rhw; DWORD c; };
int main(void)
{
    HWND hwnd = CreateWindowA("STATIC", "d3d9 test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 640, 480, 0, 0, 0, 0);
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp = {0}; IDirect3DDevice9 *dev; IDirect3DSurface9 *bb, *sys; D3DLOCKED_RECT lr;
    D3DADAPTER_IDENTIFIER9 id; HRESULT hr; int i, ok = 1;
    struct vtx tri[3] = { {320, 100, 0.5f, 1, 0xff00ff00}, {520, 380, 0.5f, 1, 0xff00ff00}, {120, 380, 0.5f, 1, 0xff00ff00} };
    if (!d3d) { printf("FAIL Direct3DCreate9\n"); return 1; }
    IDirect3D9_GetAdapterIdentifier(d3d, 0, 0, &id);
    printf("adapter: %s (%s)\n", id.Description, id.Driver);
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = 640; pp.BackBufferHeight = 480; pp.hDeviceWindow = hwnd;
    hr = IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr)) { printf("FAIL CreateDevice 0x%lx\n", hr); return 1; }
    for (i = 0; i < 60; i++)
    {
        IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xff2040c0, 1.0f, 0);
        IDirect3DDevice9_BeginScene(dev);
        IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
        IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, tri, sizeof(tri[0]));
        IDirect3DDevice9_EndScene(dev);
        if (i < 59) IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
    }
    IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    IDirect3DDevice9_CreateOffscreenPlainSurface(dev, 640, 480, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL);
    hr = IDirect3DDevice9_GetRenderTargetData(dev, bb, sys);
    if (FAILED(hr)) { printf("FAIL GetRenderTargetData 0x%lx\n", hr); return 1; }
    IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY);
    {
        DWORD corner = ((DWORD *)lr.pBits)[10 * lr.Pitch / 4 + 10] & 0xffffff;
        DWORD centre = ((DWORD *)((char *)lr.pBits + 300 * lr.Pitch))[320] & 0xffffff;
        printf("corner %06lx (want 2040c0), triangle %06lx (want 00ff00)\n", corner, centre);
        ok = corner == 0x2040c0 && centre == 0x00ff00;
    }
    IDirect3DSurface9_UnlockRect(sys);
    printf(ok ? "PASS\n" : "FAIL pixels\n");
    return !ok;
}
