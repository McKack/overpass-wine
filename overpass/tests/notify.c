/* OverPass Wine: folder change notifications, which wineserver gets from libinotify. A second thread creates
 * files in a temporary folder while the main thread waits, first through FindFirstChangeNotification, then
 * through ReadDirectoryChangesW, which must also name the file. Exits 0 when both arrive within 5 seconds.
 *   x86_64-w64-mingw32-gcc -O1 notify.c -o notify.exe */
#include <windows.h>
#include <stdio.h>
#include <wchar.h>

static WCHAR dir[MAX_PATH];

static DWORD WINAPI create_file(void *name)
{
    WCHAR path[MAX_PATH];
    Sleep(500);
    swprintf(path, MAX_PATH, L"%ls\\%ls", dir, (WCHAR *)name);
    CloseHandle(CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL));
    return 0;
}

static void clean(const WCHAR *name)
{
    WCHAR path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%ls\\%ls", dir, name);
    DeleteFileW(path);
}

int main(void)
{
    int failures = 0;
    GetTempPathW(MAX_PATH, dir);
    wcscat(dir, L"overpass-notify");
    CreateDirectoryW(dir, NULL);
    clean(L"first.txt");
    clean(L"second.txt");

    HANDLE change = FindFirstChangeNotificationW(dir, FALSE, FILE_NOTIFY_CHANGE_FILE_NAME);
    CloseHandle(CreateThread(NULL, 0, create_file, L"first.txt", 0, NULL));
    DWORD waited = WaitForSingleObject(change, 5000);
    printf("%s FindFirstChangeNotification\n", waited == WAIT_OBJECT_0 ? "PASS" : "FAIL");
    failures += waited != WAIT_OBJECT_0;
    FindCloseChangeNotification(change);

    HANDLE folder = CreateFileW(dir, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
    DWORD buffer[1024], got = 0;
    OVERLAPPED ov = { .hEvent = CreateEventW(NULL, TRUE, FALSE, NULL) };
    ReadDirectoryChangesW(folder, buffer, sizeof(buffer), FALSE, FILE_NOTIFY_CHANGE_FILE_NAME, NULL, &ov, NULL);
    CloseHandle(CreateThread(NULL, 0, create_file, L"second.txt", 0, NULL));
    waited = WaitForSingleObject(ov.hEvent, 5000);
    BOOL named = FALSE;
    if (waited == WAIT_OBJECT_0 && GetOverlappedResult(folder, &ov, &got, FALSE) && got)
    {
        FILE_NOTIFY_INFORMATION *info = (FILE_NOTIFY_INFORMATION *)buffer;
        named = info->FileNameLength == wcslen(L"second.txt") * sizeof(WCHAR) &&
                !wcsncmp(info->FileName, L"second.txt", info->FileNameLength / sizeof(WCHAR));
    }
    printf("%s ReadDirectoryChangesW names the new file\n", named ? "PASS" : "FAIL");
    failures += !named;
    CancelIo(folder);
    CloseHandle(folder);

    clean(L"first.txt");
    clean(L"second.txt");
    RemoveDirectoryW(dir);
    return failures;
}
