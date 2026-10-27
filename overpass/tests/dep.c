#include <windows.h>
#include <stdio.h>
typedef LONG (WINAPI *NQIP)(HANDLE, ULONG, void *, ULONG, ULONG *);
static double now(void) { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (double)c.QuadPart / f.QuadPart; }
int main(int argc, char **argv)
{
    NQIP q = (NQIP)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationProcess");
    ULONG flags = 0; int i, rounds = 20; double t0, t;
    if (argc > 1) LoadLibraryA(argv[1]);   /* a DLL without NX_COMPAT */
    q(GetCurrentProcess(), 34 /* ProcessExecuteFlags */, &flags, sizeof(flags), NULL);
    printf("execute flags 0x%lx (%s)\n", flags, (flags & 2) ? "DEP off" : "DEP on");
    t0 = now();
    for (i = 0; i < rounds; i++)
    {
        SIZE_T size = 64 << 20; char *p = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); SIZE_T o;
        for (o = 0; o < size; o += 4096) p[o] = 1;
        VirtualFree(p, 0, MEM_RELEASE);
    }
    t = now() - t0;
    printf("touched %d x 64 MB in %.3f s\n", rounds, t);
    return 0;
}
