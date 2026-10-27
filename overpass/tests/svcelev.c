/* svcelev: checks that services run elevated. "svcelev install" registers itself as a service, starts it and
 * prints what the service saw; the service writes its token's elevation to C:\svcelev.txt. Also tries what
 * GOG Galaxy's service does: create a key under HKLM\SOFTWARE\Khronos\Vulkan\ImplicitLayers. */
#include <windows.h>
#include <stdio.h>

static SERVICE_STATUS_HANDLE status_handle;
static SERVICE_STATUS status = {SERVICE_WIN32_OWN_PROCESS};

static void report(void)
{
    TOKEN_ELEVATION elevation = {0};
    TOKEN_ELEVATION_TYPE type = 0;
    HANDLE token;
    DWORD size, err;
    HKEY key;
    FILE *f;

    OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token);
    GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    GetTokenInformation(token, TokenElevationType, &type, sizeof(type), &size);
    err = RegCreateKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Khronos\\Vulkan\\ImplicitLayers", 0, NULL, 0,
                          KEY_SET_VALUE, NULL, &key, NULL);
    if (!err) RegCloseKey(key);
    if ((f = fopen("C:\\svcelev.txt", "w")))
    {
        fprintf(f, "elevated %lu type %d hklm-vulkan-key %lu\n", elevation.TokenIsElevated, type, err);
        fclose(f);
    }
}

static DWORD WINAPI handler(DWORD control, DWORD type, void *data, void *context)
{
    if (control == SERVICE_CONTROL_STOP)
    {
        status.dwCurrentState = SERVICE_STOPPED;
        SetServiceStatus(status_handle, &status);
    }
    return NO_ERROR;
}

static void WINAPI service_main(DWORD argc, char **argv)
{
    status_handle = RegisterServiceCtrlHandlerExA("svcelev", handler, NULL);
    status.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(status_handle, &status);
    report();
    status.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(status_handle, &status);
}

int main(int argc, char **argv)
{
    SERVICE_TABLE_ENTRYA table[] = {{(char *)"svcelev", service_main}, {NULL, NULL}};
    char path[MAX_PATH], line[128] = "";
    SC_HANDLE scm, service;
    FILE *f;
    int i;

    if (argc < 2 || strcmp(argv[1], "install")) return !StartServiceCtrlDispatcherA(table);

    GetModuleFileNameA(NULL, path, sizeof(path));
    DeleteFileA("C:\\svcelev.txt");
    scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!(service = OpenServiceA(scm, "svcelev", SERVICE_ALL_ACCESS)))
        service = CreateServiceA(scm, "svcelev", "svcelev", SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                                 SERVICE_DEMAND_START, SERVICE_ERROR_IGNORE, path, NULL, NULL, NULL, NULL, NULL);
    if (!service || !StartServiceA(service, 0, NULL))
    {
        printf("FAIL: couldn't start the service (%lu)\n", GetLastError());
        return 1;
    }
    for (i = 0; i < 100 && !(f = fopen("C:\\svcelev.txt", "r")); i++) Sleep(100);
    if (f) { fgets(line, sizeof(line), f); fclose(f); }
    DeleteService(service);
    printf("service: %s", *line ? line : "no report\n");
    return strncmp(line, "elevated 1", 10) != 0;
}
