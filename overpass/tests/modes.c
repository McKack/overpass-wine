/* OverPass Wine: window modes and resolutions through DXMT. Draws a test pattern (red frame, green top-left and
 * magenta bottom-right squares, a grid every 100 pixels) and steps through window mode changes; a screenshot of
 * each phase shows whether the picture fills the screen and is scaled, not cropped or shifted.
 *   modes.exe <scenario> <w> <h> [12]
 * Scenarios: win, bl (borderless over the monitor), fs (DXGI exclusive fullscreen), cds (ChangeDisplaySettings,
 * then a popup over the new monitor rect), fs2bl (fs, then back to borderless keeping w x h), bl2fs.
 * w/h 0 = follow the client size; "12" uses D3D12 instead of D3D11. Each phase prints the window and swapchain
 * state and writes its number to C:\phase.txt (C:\phase.txt.start once the first frame is up), so a script can
 * bring the window forward and run `screencapture` at the right moments.
 * MODES_HOLD=<ms> (default 2500) is how long each phase stays up after its number is written.
 *   x86_64-w64-mingw32-gcc -O1 -static modes.c -o modes.exe -ld3d12 -ld3d11 -ldxgi -luuid */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <stdio.h>
#include <stdlib.h>

static HWND hwnd;
static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;
static IDXGISwapChain *sc;
static UINT bw, bh, want_w, want_h;
static ID3D11Texture2D *pattern;
static UINT pw, ph;
static int d12;
static ID3D12Device *dev12;
static ID3D12CommandQueue *queue;
static ID3D12CommandAllocator *alloc;
static ID3D12GraphicsCommandList *list;
static ID3D12Fence *fence;
static UINT64 fence_value;
static ID3D12Resource *upload;
static UINT upload_pitch;

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_SIZE && sc && !want_w)
    {
        UINT w = LOWORD(lp), hh = HIWORD(lp);
        if (w && hh && (w != bw || hh != bh))
        {
            HRESULT hr = IDXGISwapChain_ResizeBuffers(sc, 0, w, hh, DXGI_FORMAT_UNKNOWN, 0);
            printf("  WM_SIZE %ux%u ResizeBuffers %#lx\n", w, hh, hr);
            bw = w; bh = hh;
        }
    }
    if (msg == WM_CLOSE) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, msg, wp, lp);
}

static void fill_pattern(UINT *px, UINT w, UINT h, UINT pitch)
{
    UINT x, y;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
        {
            UINT c = 0xff102040; /* dark blue */
            if (x % 100 == 0 || y % 100 == 0) c = 0xffc0c0c0;           /* grid every 100 px */
            if ((x > w / 2 - 40 && x < w / 2 + 40 && y > h / 2 - 4 && y < h / 2 + 4) ||
                (y > h / 2 - 40 && y < h / 2 + 40 && x > w / 2 - 4 && x < w / 2 + 4)) c = 0xffffff00; /* centre */
            if (x < 16 || y < 16 || x >= w - 16 || y >= h - 16) c = 0xffff0000; /* red frame */
            if (x < 64 && y < 64) c = 0xff00ff00;                       /* green top-left */
            if (x >= w - 64 && y >= h - 64) c = 0xffff00ff;             /* magenta bottom-right */
            px[y * pitch + x] = c;
        }
}

static void make_pattern12(UINT w, UINT h)
{
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC rd = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, 0, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                              D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
    void *map;
    if (upload && pw == w && ph == h) return;
    if (upload) ID3D12Resource_Release(upload);
    upload_pitch = (w * 4 + 255) & ~255u;
    rd.Width = (UINT64)upload_pitch * h;
    ID3D12Device_CreateCommittedResource(dev12, &hp, 0, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                                         &IID_ID3D12Resource, (void **)&upload);
    ID3D12Resource_Map(upload, 0, NULL, &map);
    fill_pattern(map, w, h, upload_pitch / 4);
    ID3D12Resource_Unmap(upload, 0, NULL);
    pw = w; ph = h;
}

static void barrier(ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b = {D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
    b.Transition.pResource = r; b.Transition.Subresource = 0;
    b.Transition.StateBefore = from; b.Transition.StateAfter = to;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
}

static void draw12(UINT w, UINT h)
{
    ID3D12Resource *bb;
    D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
    UINT index = IDXGISwapChain3_GetCurrentBackBufferIndex((IDXGISwapChain3 *)sc);
    HANDLE ev;
    make_pattern12(w, h);
    if (FAILED(IDXGISwapChain_GetBuffer(sc, index, &IID_ID3D12Resource, (void **)&bb))) return;
    ID3D12CommandAllocator_Reset(alloc);
    ID3D12GraphicsCommandList_Reset(list, alloc, NULL);
    barrier(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    dst.pResource = bb; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = upload; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    src.PlacedFootprint.Footprint.Width = w; src.PlacedFootprint.Footprint.Height = h;
    src.PlacedFootprint.Footprint.Depth = 1; src.PlacedFootprint.Footprint.RowPitch = upload_pitch;
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &dst, 0, 0, 0, &src, NULL);
    barrier(bb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    ID3D12GraphicsCommandList_Close(list);
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&list);
    ID3D12Resource_Release(bb);
    IDXGISwapChain_Present(sc, 1, 0);
    ID3D12CommandQueue_Signal(queue, fence, ++fence_value);
    ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    ID3D12Fence_SetEventOnCompletion(fence, fence_value, ev);
    WaitForSingleObject(ev, 2000);
    CloseHandle(ev);
}

static void make_pattern(UINT w, UINT h)
{
    D3D11_TEXTURE2D_DESC d = {w, h, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, 0, 0, 0};
    D3D11_SUBRESOURCE_DATA init;
    UINT *px;
    if (pattern && pw == w && ph == h) return;
    if (pattern) ID3D11Texture2D_Release(pattern);
    px = malloc(w * h * 4);
    fill_pattern(px, w, h, w);
    init.pSysMem = px; init.SysMemPitch = w * 4; init.SysMemSlicePitch = 0;
    ID3D11Device_CreateTexture2D(dev, &d, &init, &pattern);
    pw = w; ph = h;
    free(px);
}

static void report(const char *what)
{
    RECT wr, cr; MONITORINFO mi = {sizeof(mi)}; DEVMODEW dm = {.dmSize = sizeof(dm)};
    DXGI_SWAP_CHAIN_DESC sd; BOOL fs = FALSE;
    GetWindowRect(hwnd, &wr); GetClientRect(hwnd, &cr);
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
    EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm);
    IDXGISwapChain_GetDesc(sc, &sd);
    IDXGISwapChain_GetFullscreenState(sc, &fs, NULL);
    printf("[%s] screen %dx%d mode %lux%lu monitor (%ld,%ld)-(%ld,%ld) window (%ld,%ld)-(%ld,%ld) client %ldx%ld "
           "buffer %ux%u fullscreen %d style %#lx\n", what, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
           dm.dmPelsWidth, dm.dmPelsHeight, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom,
           wr.left, wr.top, wr.right, wr.bottom, cr.right, cr.bottom, sd.BufferDesc.Width, sd.BufferDesc.Height, fs,
           GetWindowLongW(hwnd, GWL_STYLE));
    fflush(stdout);
}

static void frames(int ms)
{
    DWORD end = GetTickCount() + ms; MSG m;
    while (GetTickCount() < end)
    {
        ID3D11Texture2D *bb;
        DXGI_SWAP_CHAIN_DESC sd;
        while (PeekMessageW(&m, 0, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        IDXGISwapChain_GetDesc(sc, &sd);
        if (d12) { draw12(sd.BufferDesc.Width, sd.BufferDesc.Height); continue; }
        make_pattern(sd.BufferDesc.Width, sd.BufferDesc.Height);
        if (SUCCEEDED(IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&bb)))
        {
            ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)bb, (ID3D11Resource *)pattern);
            ID3D11Texture2D_Release(bb);
        }
        IDXGISwapChain_Present(sc, 1, 0);
    }
}

static int phase_no;
static void phase(const char *what)
{
    FILE *f;
    frames(1500);
    report(what);
    if ((f = fopen("C:\\phase.txt", "w"))) { fprintf(f, "%d %s\n", ++phase_no, what); fclose(f); }
    /* MODES_HOLD=<ms> keeps each phase up longer, for switching to another app and back during it. */
    frames(getenv("MODES_HOLD") ? atoi(getenv("MODES_HOLD")) : 2500);
}

static void borderless(void)
{
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
    SetWindowLongW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

static void resize_buffers(UINT w, UINT h)
{
    HRESULT hr = IDXGISwapChain_ResizeBuffers(sc, 0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    printf("  ResizeBuffers %ux%u %#lx\n", w, h, hr);
    bw = w; bh = h;
}

static void fullscreen(UINT w, UINT h)
{
    DXGI_MODE_DESC md = {w, h, {0, 0}, DXGI_FORMAT_UNKNOWN};
    HRESULT hr = IDXGISwapChain_ResizeTarget(sc, &md);
    printf("  ResizeTarget %ux%u %#lx\n", w, h, hr);
    hr = IDXGISwapChain_SetFullscreenState(sc, TRUE, NULL);
    printf("  SetFullscreenState(TRUE) %#lx\n", hr);
    resize_buffers(w, h);
}

int main(int argc, char **argv)
{
    const char *sc_name = argc > 1 ? argv[1] : "bl";
    WNDCLASSW wc = {0, wndproc, 0, 0, GetModuleHandleW(NULL), 0, LoadCursorW(0, (LPCWSTR)IDC_ARROW), 0, 0, L"modes"};
    DXGI_SWAP_CHAIN_DESC sd = {0};
    HRESULT hr;

    want_w = argc > 3 ? atoi(argv[2]) : 0;
    want_h = argc > 3 ? atoi(argv[3]) : 0;
    SetProcessDPIAware();
    RegisterClassW(&wc);
    hwnd = CreateWindowW(L"modes", L"modes", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 800, 450, 0, 0, wc.hInstance, 0);

    if (!strcmp(sc_name, "cds"))
    {
        DEVMODEW dm = {.dmSize = sizeof(dm), .dmPelsWidth = want_w, .dmPelsHeight = want_h,
                       .dmFields = DM_PELSWIDTH | DM_PELSHEIGHT};
        LONG r = ChangeDisplaySettingsW(&dm, CDS_FULLSCREEN);
        printf("  ChangeDisplaySettings %ux%u -> %ld\n", want_w, want_h, r);
    }

    sd.BufferDesc.Width = want_w ? want_w : 800;
    sd.BufferDesc.Height = want_h ? want_h : 450;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.OutputWindow = hwnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    d12 = argc > 4 && !strcmp(argv[4], "12");
    if (d12)
    {
        D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT};
        IDXGIFactory4 *f4;
        hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev12);
        if (SUCCEEDED(hr)) hr = ID3D12Device_CreateCommandQueue(dev12, &qd, &IID_ID3D12CommandQueue, (void **)&queue);
        if (SUCCEEDED(hr)) hr = CreateDXGIFactory1(&IID_IDXGIFactory4, (void **)&f4);
        if (SUCCEEDED(hr)) hr = IDXGIFactory4_CreateSwapChain(f4, (IUnknown *)queue, &sd, &sc);
        if (SUCCEEDED(hr))
        {
            IDXGISwapChain *s3;
            IDXGISwapChain_QueryInterface(sc, &IID_IDXGISwapChain3, (void **)&s3);
            IDXGISwapChain_Release(sc); sc = s3;
            ID3D12Device_CreateCommandAllocator(dev12, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator,
                                                (void **)&alloc);
            ID3D12Device_CreateCommandList(dev12, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL,
                                           &IID_ID3D12GraphicsCommandList, (void **)&list);
            ID3D12GraphicsCommandList_Close(list);
            ID3D12Device_CreateFence(dev12, 0, 0, &IID_ID3D12Fence, (void **)&fence);
        }
    }
    else
        hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &sd,
                                           &sc, &dev, NULL, &ctx);
    if (FAILED(hr)) { printf("device %#lx\n", hr); return 1; }
    bw = sd.BufferDesc.Width; bh = sd.BufferDesc.Height;
    {
        IDXGIFactory *f; IDXGISwapChain_GetParent(sc, &IID_IDXGIFactory, (void **)&f);
        IDXGIFactory_MakeWindowAssociation(f, hwnd, DXGI_MWA_NO_ALT_ENTER); IDXGIFactory_Release(f);
    }
    report("start");
    { FILE *f = fopen("C:\\phase.txt.start", "w"); if (f) fclose(f); }
    frames(1500);

    if (!strcmp(sc_name, "win"))
    {
        RECT r = {0, 0, want_w, want_h};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(hwnd, 0, 50, 50, r.right - r.left, r.bottom - r.top, SWP_NOZORDER);
        phase("windowed");
    }
    else if (!strcmp(sc_name, "bl") || !strcmp(sc_name, "cds"))
    {
        borderless();
        if (!want_w) phase("borderless, buffer follows client");
        else { resize_buffers(want_w, want_h); phase("borderless, fixed buffer"); }
    }
    else if (!strcmp(sc_name, "fs"))
    {
        fullscreen(want_w, want_h);
        phase("fullscreen");
        IDXGISwapChain_SetFullscreenState(sc, FALSE, NULL);
        phase("left fullscreen");
    }
    else if (!strcmp(sc_name, "fs2bl"))
    {
        fullscreen(want_w, want_h);
        phase("fullscreen");
        hr = IDXGISwapChain_SetFullscreenState(sc, FALSE, NULL);
        printf("  SetFullscreenState(FALSE) %#lx\n", hr);
        borderless();
        resize_buffers(want_w, want_h);
        phase("borderless after fullscreen, fixed buffer");
        {
            RECT cr; GetClientRect(hwnd, &cr);
            resize_buffers(cr.right, cr.bottom);
        }
        phase("borderless after fullscreen, buffer = client");
    }
    else if (!strcmp(sc_name, "bl2fs"))
    {
        borderless();
        resize_buffers(want_w, want_h);
        phase("borderless, fixed buffer");
        fullscreen(want_w, want_h);
        phase("fullscreen after borderless");
    }
    if (!strcmp(sc_name, "cds")) ChangeDisplaySettingsW(NULL, 0);
    IDXGISwapChain_SetFullscreenState(sc, FALSE, NULL);
    report("end");
    return 0;
}
