/* OverPass Wine: modifier keys after another application had the keyboard. Opens a window and prints, every
 * 250 ms and on focus changes, whether Windows thinks Alt (Command) is down. Drive it from the Mac: hold
 * Command in the window, switch to another app, let go there, come back: Alt must read as up again.
 *   x86_64-w64-mingw32-gcc -O1 modkeys.c -o modkeys.exe -mwindows -luser32 */
#include <windows.h>
#include <stdio.h>

static void report(const char *when)
{
    printf("%lu %s: lmenu %d menu %d\n", GetTickCount(), when, (GetAsyncKeyState(VK_LMENU) & 0x8000) != 0,
           (GetKeyState(VK_MENU) & 0x8000) != 0);
    fflush(stdout);
}

static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_SETFOCUS: report("focus"); break;
    case WM_KILLFOCUS: report("lost focus"); break;
    case WM_TIMER: report("tick"); break;
    case WM_SYSKEYDOWN: case WM_KEYDOWN: case WM_SYSKEYUP: case WM_KEYUP:
        printf("%lu key msg %#x vkey %#lx\n", GetTickCount(), msg, (unsigned long)wp); fflush(stdout); break;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int main(void)
{
    WNDCLASSW wc = {0};
    MSG msg;
    HWND hwnd;

    wc.lpfnWndProc = proc;
    wc.lpszClassName = L"modkeys";
    RegisterClassW(&wc);
    hwnd = CreateWindowW(L"modkeys", L"modkeys", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 400, 300,
                         NULL, NULL, NULL, NULL);
    SetTimer(hwnd, 1, 250, NULL);
    SetTimer(hwnd, 2, 20000, NULL);
    while (GetMessageW(&msg, NULL, 0, 0))
    {
        if (msg.message == WM_TIMER && msg.wParam == 2) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
