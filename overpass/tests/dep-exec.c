#include <windows.h>
#include <stdio.h>
static LONG WINAPI handler(EXCEPTION_POINTERS *e) { printf("exception 0x%lx\n", e->ExceptionRecord->ExceptionCode); fflush(stdout); ExitProcess(1); }
int main(void)
{
    static const unsigned char code[] = { 0xb8, 42, 0, 0, 0, 0xc3 }; /* mov eax, 42; ret */
    unsigned char *p = HeapAlloc(GetProcessHeap(), 0, 64);
    SetUnhandledExceptionFilter(handler);
    memcpy(p, code, sizeof(code));
    printf("data code returned %d\n", ((int (*)(void))p)());
    return 0;
}
