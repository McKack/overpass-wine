#include <windows.h>
#include <stdio.h>
#include <string.h>
/* x87.exe: a loop of x87 fsin/fsqrt/fdiv; prints the time and the result's bits. */
int main(void)
{
    LARGE_INTEGER f, a, b; volatile double x = 0.5, acc = 0; unsigned long long bits; int i;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a);
    for (i = 0; i < 20000000; i++)
    {
        double s, r;
        __asm__ volatile ("fsin" : "=t"(s) : "0"(x + i * 1e-7));
        __asm__ volatile ("fsqrt" : "=t"(r) : "0"(s * s + 1.0));
        acc += s / r;
    }
    QueryPerformanceCounter(&b);
    { double v = acc; memcpy(&bits, &v, 8); }
    { FILE *out = fopen("Z:\\tmp\\x87-result.txt", "a"); fprintf(out ? out : stdout, "%.3f s, result %016llx\n", (double)(b.QuadPart - a.QuadPart) / f.QuadPart, bits); if (out) fclose(out); }
    return 0;
}
