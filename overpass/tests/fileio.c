/* OverPass Wine: file and memory calls from the 11.0-10 fix batch. Exits 0 when every check passes.
 *  - LockFileEx granted at once on a handle tied to a completion port queues a packet (RDR2 streaming),
 *    and none with FILE_SKIP_COMPLETION_PORT_ON_SUCCESS
 *  - \\.\GLOBALROOT\??\C:\... and \GLOBALROOT\??\C:\... open the file (Arxan, Forza Horizon 5)
 *  - FileAllocationInformation: below the end of file truncates, past it succeeds and keeps the size
 *  - WriteProcessMemory into a child's ntdll code (hook engines, Trackmania)
 *  - QueryWorkingSetEx: a touched page is Valid, an untouched one isn't
 *   x86_64-w64-mingw32-gcc -O1 fileio.c -o fileio.exe -lntdll -lpsapi */
#include <windows.h>
#include <winternl.h>
#include <psapi.h>
#include <stdio.h>

static int failures;

static void check(BOOL ok, const char *what)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

static void lock_completion(const WCHAR *path, BOOL skip_on_success)
{
    HANDLE file, port;
    OVERLAPPED ov = {0}, *got = NULL;
    DWORD bytes;
    ULONG_PTR key;
    BOOL ok;

    file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_OVERLAPPED, NULL);
    port = CreateIoCompletionPort(file, NULL, 42, 0);
    if (skip_on_success) SetFileCompletionNotificationModes(file, FILE_SKIP_COMPLETION_PORT_ON_SUCCESS);
    ok = LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK, 0, 10, 0, &ov);
    check(ok, "LockFileEx granted at once");
    ok = GetQueuedCompletionStatus(port, &bytes, &key, &got, 1000);
    if (skip_on_success)
        check(!ok && !got, "no completion packet with FILE_SKIP_COMPLETION_PORT_ON_SUCCESS");
    else
        check(ok && got == &ov && key == 42, "completion packet for a lock granted at once");
    UnlockFile(file, 0, 0, 10, 0);
    CloseHandle(port);
    CloseHandle(file);
    DeleteFileW(path);
}

static void globalroot(void)
{
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attr;
    IO_STATUS_BLOCK io;
    HANDLE file;
    NTSTATUS status;

    file = CreateFileW(L"\\\\.\\GLOBALROOT\\??\\C:\\windows\\win.ini", GENERIC_READ, FILE_SHARE_READ, NULL,
                       OPEN_EXISTING, 0, NULL);
    check(file != INVALID_HANDLE_VALUE, "CreateFile \\\\.\\GLOBALROOT\\??\\C:\\windows\\win.ini");
    CloseHandle(file);

    RtlInitUnicodeString(&name, L"\\GLOBALROOT\\??\\C:\\windows\\win.ini");
    InitializeObjectAttributes(&attr, &name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    status = NtCreateFile(&file, GENERIC_READ | SYNCHRONIZE, &attr, &io, NULL, 0, FILE_SHARE_READ, FILE_OPEN,
                          FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
    printf("     NtCreateFile \\GLOBALROOT\\??\\...: %#lx\n", status);
    check(!status, "NtCreateFile \\GLOBALROOT\\??\\C:\\windows\\win.ini");
    if (!status) NtClose(file);
}

static void allocation(const WCHAR *path)
{
    FILE_ALLOCATION_INFO info;
    LARGE_INTEGER size;
    DWORD written;
    char data[100] = {0};
    HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);

    WriteFile(file, data, sizeof(data), &written, NULL);
    info.AllocationSize.QuadPart = 1 << 20;
    check(SetFileInformationByHandle(file, FileAllocationInfo, &info, sizeof(info)), "FileAllocationInfo 1 MB");
    GetFileSizeEx(file, &size);
    check(size.QuadPart == 100, "size unchanged after reserving past the end");
    info.AllocationSize.QuadPart = 10;
    check(SetFileInformationByHandle(file, FileAllocationInfo, &info, sizeof(info)), "FileAllocationInfo 10 bytes");
    GetFileSizeEx(file, &size);
    check(size.QuadPart == 10, "truncated to 10 bytes");
    CloseHandle(file);
    DeleteFileW(path);
}

static void write_code(void)
{
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    WCHAR cmd[] = L"C:\\windows\\system32\\cmd.exe /c exit";
    BYTE *target = (BYTE *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtProtectVirtualMemory");
    BYTE original[16], patched[16], back[16];
    MEMORY_BASIC_INFORMATION mbi;
    SIZE_T done = 0;
    BOOL ok;

    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi))
    {
        check(FALSE, "start a suspended child");
        return;
    }
    VirtualQueryEx(pi.hProcess, target, &mbi, sizeof(mbi));
    printf("     child's NtProtectVirtualMemory page protection %#lx\n", mbi.Protect);
    ReadProcessMemory(pi.hProcess, target, original, sizeof(original), &done);
    memcpy(patched, original, sizeof(patched));
    patched[0] = 0xe9;  /* a hook engine's jump */
    ok = WriteProcessMemory(pi.hProcess, target, patched, sizeof(patched), &done);
    printf("     WriteProcessMemory: %d, error %lu\n", ok, ok ? 0 : GetLastError());
    ReadProcessMemory(pi.hProcess, target, back, sizeof(back), &done);
    check(ok && !memcmp(back, patched, sizeof(back)), "WriteProcessMemory into a child's ntdll code");
    VirtualQueryEx(pi.hProcess, target, &mbi, sizeof(mbi));
    printf("     protection afterwards %#lx\n", mbi.Protect);
    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

static void working_set(void)
{
    PSAPI_WORKING_SET_EX_INFORMATION info[2];
    BYTE *mem = VirtualAlloc(NULL, 0x20000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    mem[0] = 1;
    info[0].VirtualAddress = mem;
    info[1].VirtualAddress = mem + 0x10000;
    check(QueryWorkingSetEx(GetCurrentProcess(), info, sizeof(info)), "QueryWorkingSetEx");
    printf("     touched: valid %u protection %#x; untouched: valid %u\n", (unsigned)info[0].VirtualAttributes.Valid,
           (unsigned)info[0].VirtualAttributes.Win32Protection, (unsigned)info[1].VirtualAttributes.Valid);
    check(info[0].VirtualAttributes.Valid && info[0].VirtualAttributes.Win32Protection == PAGE_READWRITE,
          "touched page is valid and read-write");
    check(!info[1].VirtualAttributes.Valid, "untouched page is not valid");
    VirtualFree(mem, 0, MEM_RELEASE);
}

int main(void)
{
    WCHAR dir[MAX_PATH], path[MAX_PATH];

    GetTempPathW(MAX_PATH, dir);
    swprintf(path, MAX_PATH, L"%lsoverpass-fileio.tmp", dir);
    lock_completion(path, FALSE);
    lock_completion(path, TRUE);
    globalroot();
    allocation(path);
    write_code();
    working_set();
    printf("%d failure(s)\n", failures);
    return failures != 0;
}
