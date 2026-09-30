/* shmtest.exe — named shared memory between two processes: the parent
 * creates a mapping, the child it starts opens it by name, reads the
 * parent's message and writes an answer back */
#include <stdio.h>
#include <string.h>
#include <windows.h>

#define NAME "Local\\NovaShmTest"
#define FNAME "Local\\NovaShmFile"
#define FPATH "C:\\Temp\\shmtest.bin"

static int child(void)
{
    HANDLE m = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, NAME);
    if (!m) { printf("child: OpenFileMapping failed (%lu)\n", GetLastError()); return 2; }
    char *p = MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!p) { printf("child: MapViewOfFile failed (%lu)\n", GetLastError()); return 3; }
    printf("child: read \"%s\"\n", p);
    strcpy(p + 2048, "pong from the child");
    p[8191] = 'Z';                                  /* the second page too */
    UnmapViewOfFile(p);
    CloseHandle(m);
    /* the file-backed mapping: change the file through it */
    HANDLE fm = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, FNAME);
    char *q = fm ? MapViewOfFile(fm, FILE_MAP_ALL_ACCESS, 0, 0, 0) : NULL;
    if (!q) { printf("child: file mapping open failed (%lu)\n", GetLastError()); return 4; }
    printf("child: file view says \"%s\"\n", q);
    memcpy(q, "child wrote", 11);
    UnmapViewOfFile(q);
    CloseHandle(fm);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "child")) return child();

    HANDLE m = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 8192, NAME);
    if (!m) { printf("CreateFileMapping failed (%lu)\n", GetLastError()); return 1; }
    char *p = MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!p) { printf("MapViewOfFile failed (%lu)\n", GetLastError()); return 1; }
    int zero = 1;
    for (int i = 0; i < 8192; i++) if (p[i]) zero = 0;
    printf("new mapping is zeroed: %s\n", zero ? "yes" : "NO");

    /* a second create of the same name opens it */
    HANDLE again = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 8192, NAME);
    printf("second create: %s\n", again && GetLastError() == ERROR_ALREADY_EXISTS ? "already exists (ok)" : "WRONG");
    char *q = MapViewOfFile(again, FILE_MAP_READ, 0, 0, 0);
    strcpy(p, "ping from the parent");
    printf("second view sees: \"%s\"\n", q ? q : "(no view)");

    /* a file-backed mapping shared by name */
    CreateDirectoryA("C:\\Temp", NULL);
    HANDLE f = CreateFileA(FPATH, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD wr;
    WriteFile(f, "file content here", 17, &wr, NULL);
    HANDLE fm = CreateFileMappingA(f, NULL, PAGE_READWRITE, 0, 0, FNAME);
    char *fv = fm ? MapViewOfFile(fm, FILE_MAP_ALL_ACCESS, 0, 0, 0) : NULL;
    printf("file view: \"%s\"\n", fv ? fv : "(none)");

    char cmd[MAX_PATH + 16];
    GetModuleFileNameA(NULL, cmd, MAX_PATH);
    strcat(cmd, " child");
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) { printf("CreateProcess failed (%lu)\n", GetLastError()); return 1; }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 99;
    GetExitCodeProcess(pi.hProcess, &code);
    printf("child exited with %lu\n", code);
    printf("parent reads: \"%s\", last byte '%c'\n", p + 2048, p[8191]);
    int ok = code == 0 && !strcmp(p + 2048, "pong from the child") && p[8191] == 'Z';
    printf("file view after child: \"%s\"\n", fv ? fv : "(none)");
    if (!fv || memcmp(fv, "child wrote", 11)) ok = 0;
    UnmapViewOfFile(fv);
    CloseHandle(fm);
    char rb[32] = { 0 };
    SetFilePointer(f, 0, NULL, FILE_BEGIN);
    ReadFile(f, rb, 17, &wr, NULL);
    CloseHandle(f);
    DeleteFileA(FPATH);
    printf("file on disk: \"%s\"\n", rb);
    if (memcmp(rb, "child wrote", 11)) ok = 0;

    UnmapViewOfFile(q);
    UnmapViewOfFile(p);
    CloseHandle(again);
    CloseHandle(m);
    /* the name is gone with its last handle */
    HANDLE gone = OpenFileMappingA(FILE_MAP_READ, FALSE, NAME);
    printf("after closing: %s\n", gone ? "STILL OPEN" : "gone (ok)");
    if (gone) ok = 0;
    printf("shmtest: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
