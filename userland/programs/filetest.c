/* filetest.exe — Win32 file API self-test on drive C:
 *
 *   filetest            the tests (files, delete on close, registry change events, pending renames)
 *   filetest install    an "installer" that must replace a running program:
 *                       it schedules the replacement for the next boot
 *   filetest installed  after a restart: the replacement happened
 */
#include <stdio.h>
#include <string.h>
#include <windows.h>

#ifndef DELETE
#define DELETE                       0x00010000
#endif
#ifndef FILE_FLAG_BACKUP_SEMANTICS
#define FILE_FLAG_BACKUP_SEMANTICS   0x02000000
#endif
#ifndef FILE_FLAG_OPEN_REPARSE_POINT
#define FILE_FLAG_OPEN_REPARSE_POINT 0x00200000
#endif

static int pass, fail;
#define CHECK(cond) do { if (cond) pass++; else { fail++; printf("FAIL line %d: %s (error %lu)\n", __LINE__, #cond, GetLastError()); } } while (0)

/* RegNotifyChangeKeyValue: an event signalled once, for the changes asked for */
static DWORD WINAPI set_later(LPVOID key)
{
    Sleep(300);
    DWORD v = 2;
    RegSetValueExA((HKEY)key, "v", 0, REG_DWORD, (const BYTE *)&v, 4);
    return 0;
}

static void notify(void)
{
    HKEY k = 0, sub = 0;
    HANDLE ev = CreateEventA(0, FALSE, FALSE, 0);
    RegDeleteTreeA(HKEY_CURRENT_USER, "Software\\NovaFiletest");
    CHECK(!RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\NovaFiletest", 0, 0, 0, KEY_ALL_ACCESS, 0, &k, 0));
    DWORD v = 1;

    CHECK(!RegNotifyChangeKeyValue(k, FALSE, REG_NOTIFY_CHANGE_LAST_SET, ev, TRUE));
    CHECK(WaitForSingleObject(ev, 0) == WAIT_TIMEOUT);              /* nothing yet */
    CHECK(!RegSetValueExA(k, "v", 0, REG_DWORD, (const BYTE *)&v, 4));
    CHECK(WaitForSingleObject(ev, 2000) == WAIT_OBJECT_0);         /* a value set */
    CHECK(!RegSetValueExA(k, "v", 0, REG_DWORD, (const BYTE *)&v, 4));
    CHECK(WaitForSingleObject(ev, 200) == WAIT_TIMEOUT);           /* once: the watch is gone */

    CHECK(!RegNotifyChangeKeyValue(k, FALSE, REG_NOTIFY_CHANGE_NAME, ev, TRUE));
    CHECK(!RegSetValueExA(k, "w", 0, REG_DWORD, (const BYTE *)&v, 4));
    CHECK(WaitForSingleObject(ev, 200) == WAIT_TIMEOUT);           /* a value is not a name change */
    CHECK(!RegCreateKeyExA(k, "Sub", 0, 0, 0, KEY_ALL_ACCESS, 0, &sub, 0));
    CHECK(WaitForSingleObject(ev, 2000) == WAIT_OBJECT_0);         /* a subkey added */

    CHECK(!RegNotifyChangeKeyValue(k, FALSE, REG_NOTIFY_CHANGE_LAST_SET, ev, TRUE));
    CHECK(!RegSetValueExA(sub, "x", 0, REG_DWORD, (const BYTE *)&v, 4));
    CHECK(WaitForSingleObject(ev, 200) == WAIT_TIMEOUT);           /* a subkey's value: not without the subtree */
    CHECK(!RegDeleteValueA(k, "w"));
    CHECK(WaitForSingleObject(ev, 2000) == WAIT_OBJECT_0);         /* a value deleted */
    CHECK(!RegNotifyChangeKeyValue(k, TRUE, REG_NOTIFY_CHANGE_LAST_SET, ev, TRUE));
    CHECK(!RegSetValueExA(sub, "x", 0, REG_DWORD, (const BYTE *)&v, 4));
    CHECK(WaitForSingleObject(ev, 2000) == WAIT_OBJECT_0);         /* with the subtree it counts */

    CHECK(!RegNotifyChangeKeyValue(sub, FALSE, REG_NOTIFY_CHANGE_LAST_SET, ev, TRUE));
    RegCloseKey(sub);
    CHECK(!RegDeleteKeyA(k, "Sub"));
    CHECK(WaitForSingleObject(ev, 2000) == WAIT_OBJECT_0);         /* the watched key deleted */

    /* synchronous: the call returns when another thread sets a value */
    HANDLE t = CreateThread(0, 0, set_later, k, 0, 0);
    DWORD t0 = GetTickCount();
    CHECK(!RegNotifyChangeKeyValue(k, FALSE, REG_NOTIFY_CHANGE_LAST_SET, 0, FALSE));
    CHECK(GetTickCount() - t0 >= 200);
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    CHECK(RegNotifyChangeKeyValue(k, FALSE, REG_NOTIFY_CHANGE_LAST_SET, 0, TRUE) == ERROR_INVALID_PARAMETER);

    RegCloseKey(k);
    RegDeleteTreeA(HKEY_CURRENT_USER, "Software\\NovaFiletest");
    CloseHandle(ev);
}

/* MoveFileEx(MOVEFILE_DELAY_UNTIL_REBOOT): written down as Windows does */
static int pending_count(char *list, DWORD cap)
{
    HKEY k;
    DWORD n = cap, type = 0;
    list[0] = list[1] = 0;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\Session Manager", 0, KEY_READ, &k)) return -1;
    LSTATUS r = RegQueryValueExA(k, "PendingFileRenameOperations", 0, &type, (BYTE *)list, &n);
    RegCloseKey(k);
    if (r) return 0;
    int items = 0;                                                  /* strings, before the list's final NUL */
    for (DWORD i = 0; i + 1 < n; i += (DWORD)strlen(list + i) + 1) items++;
    return items;
}

static void pending(void)
{
    char list[4096];
    HANDLE h = CreateFileA("C:\\Temp\\pending.tmp", GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
    CloseHandle(h);
    CHECK(MoveFileExA("pending.tmp", 0, MOVEFILE_DELAY_UNTIL_REBOOT));
    CHECK(GetFileAttributesA("pending.tmp") != INVALID_FILE_ATTRIBUTES);  /* still there until the restart */
    pending_count(list, sizeof(list));
    CHECK(!strcmp(list, "\\??\\C:\\Temp\\pending.tmp"));        /* a full NT path, then "" */
    CHECK(list[strlen(list) + 1] == 0);
    /* take it back out: the registry value is what the boot reads */
    HKEY k;
    if (!RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\Session Manager", 0, KEY_ALL_ACCESS, &k)) {
        CHECK(!RegDeleteValueA(k, "PendingFileRenameOperations"));
        RegCloseKey(k);
    }
    DeleteFileA("pending.tmp");
}

/* The installer: C:\Temp\app\app.exe runs (a copy of us, waiting), so it
 * can't be replaced now; the new version and the removal of a stale file
 * are scheduled for the next boot */
static int install(void)
{
    char self[MAX_PATH];
    GetModuleFileNameA(0, self, sizeof(self));
    CreateDirectoryA("C:\\Temp", 0);
    CreateDirectoryA("C:\\Temp\\app", 0);
    CHECK(CopyFileA(self, "C:\\Temp\\app\\app.exe", FALSE));
    HANDLE h = CreateFileA("C:\\Temp\\app\\app.new", GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
    DWORD n;
    CHECK(WriteFile(h, "version 2", 9, &n, 0));
    CloseHandle(h);
    h = CreateFileA("C:\\Temp\\app\\stale.dll", GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
    CloseHandle(h);

    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    char cmd[] = "C:\\Temp\\app\\app.exe wait";
    CHECK(CreateProcessA("C:\\Temp\\app\\app.exe", cmd, 0, 0, FALSE, 0, 0, 0, &si, &pi));
    Sleep(500);
    CHECK(!MoveFileExA("C:\\Temp\\app\\app.new", "C:\\Temp\\app\\app.exe", MOVEFILE_REPLACE_EXISTING));   /* in use */
    CHECK(MoveFileExA("C:\\Temp\\app\\app.new", "C:\\Temp\\app\\app.exe", MOVEFILE_REPLACE_EXISTING | MOVEFILE_DELAY_UNTIL_REBOOT));
    CHECK(MoveFileExA("C:\\Temp\\app\\stale.dll", 0, MOVEFILE_DELAY_UNTIL_REBOOT));
    char list[4096];
    CHECK(pending_count(list, sizeof(list)) >= 4);
    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    printf("filetest install: %d passed, %d failed; restart to finish\n", pass, fail);
    return fail ? 1 : 0;
}

static int installed(void)
{
    char buf[64], list[4096];
    DWORD n = 0;
    HANDLE h = CreateFileA("C:\\Temp\\app\\app.exe", GENERIC_READ, 0, 0, OPEN_EXISTING, 0, 0);
    CHECK(h != INVALID_HANDLE_VALUE);
    CHECK(ReadFile(h, buf, sizeof(buf), &n, 0) && n == 9 && !memcmp(buf, "version 2", 9));   /* replaced */
    CloseHandle(h);
    CHECK(GetFileAttributesA("C:\\Temp\\app\\app.new") == INVALID_FILE_ATTRIBUTES);
    CHECK(GetFileAttributesA("C:\\Temp\\app\\stale.dll") == INVALID_FILE_ATTRIBUTES);  /* deleted */
    CHECK(pending_count(list, sizeof(list)) == 0);                   /* and the list is gone */
    DeleteFileA("C:\\Temp\\app\\app.exe");
    RemoveDirectoryA("C:\\Temp\\app");
    printf("filetest installed: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}

/* Delete on close, as installers clean up their temporary files: a file
 * marked for deletion (FILE_FLAG_DELETE_ON_CLOSE, or FileDispositionInfo)
 * goes when its last handle closes, not before; meanwhile it cannot be
 * opened again (ERROR_ACCESS_DENIED), and a running program's file cannot
 * be deleted at all */
static void delete_on_close(const char *self)
{
    DWORD n;
    char buf[8];
    HANDLE h = CreateFileA("doc1.tmp", GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, 0);
    CHECK(h != INVALID_HANDLE_VALUE && WriteFile(h, "x", 1, &n, 0));
    CHECK(GetFileAttributesA("doc1.tmp") != INVALID_FILE_ATTRIBUTES);
    CloseHandle(h);
    CHECK(GetFileAttributesA("doc1.tmp") == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND);

    h = CreateFileA("doc2.tmp", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    0, CREATE_ALWAYS, 0, 0);
    CHECK(h != INVALID_HANDLE_VALUE && WriteFile(h, "data", 4, &n, 0));
    HANDLE d = CreateFileA("doc2.tmp", DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, 0);
    FILE_DISPOSITION_INFO di = { TRUE };
    CHECK(d != INVALID_HANDLE_VALUE && SetFileInformationByHandle(d, FileDispositionInfo, &di, sizeof(di)));
    CloseHandle(d);
    FILE_STANDARD_INFO si;
    CHECK(GetFileInformationByHandleEx(h, FileStandardInfo, &si, sizeof(si)) && si.DeletePending);
    CHECK(CreateFileA("doc2.tmp", GENERIC_READ, 7, 0, OPEN_EXISTING, 0, 0) == INVALID_HANDLE_VALUE &&
          GetLastError() == ERROR_ACCESS_DENIED);               /* (STATUS_DELETE_PENDING) */
    CHECK(SetFilePointer(h, 0, 0, FILE_BEGIN) == 0 && ReadFile(h, buf, 4, &n, 0) && n == 4 && !memcmp(buf, "data", 4));
    CloseHandle(h);                                             /* the last handle: now it goes */
    CHECK(CreateFileA("doc2.tmp", DELETE, 7, 0, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, 0) == INVALID_HANDLE_VALUE &&
          GetLastError() == ERROR_FILE_NOT_FOUND);

    h = CreateFileA("doc3.tmp", GENERIC_WRITE, 7, 0, CREATE_ALWAYS, 0, 0);
    CHECK(h != INVALID_HANDLE_VALUE);
    CHECK(DeleteFileA("doc3.tmp"));                             /* pending while @h is open */
    CHECK(GetFileAttributesA("doc3.tmp") == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_ACCESS_DENIED);
    CloseHandle(h);
    CHECK(GetFileAttributesA("doc3.tmp") == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND);

    h = CreateFileA("doc4.tmp", GENERIC_WRITE, 7, 0, CREATE_ALWAYS, 0, 0);
    d = CreateFileA("doc4.tmp", DELETE, 7, 0, OPEN_EXISTING, 0, 0);
    CHECK(SetFileInformationByHandle(d, FileDispositionInfo, &di, sizeof(di)));
    di = (FILE_DISPOSITION_INFO){ FALSE };                      /* changed its mind */
    CHECK(SetFileInformationByHandle(d, FileDispositionInfo, &di, sizeof(di)));
    CloseHandle(d);
    CloseHandle(h);
    CHECK(GetFileAttributesA("doc4.tmp") != INVALID_FILE_ATTRIBUTES && DeleteFileA("doc4.tmp"));

    CHECK(CreateDirectoryA("docdir", 0));                      /* a directory, the same way */
    d = CreateFileA("docdir", DELETE, 7, 0, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_DELETE_ON_CLOSE, 0);
    CHECK(d != INVALID_HANDLE_VALUE);
    CloseHandle(d);
    CHECK(GetFileAttributesA("docdir") == INVALID_FILE_ATTRIBUTES);

    /* A running program's file: refused while it runs, deleted after */
    CHECK(CopyFileA(self, "docrun.exe", FALSE));
    STARTUPINFOA st = { sizeof(st) };
    PROCESS_INFORMATION pi;
    char cmd[] = "docrun.exe wait";
    CHECK(CreateProcessA("docrun.exe", cmd, 0, 0, FALSE, 0, 0, 0, &st, &pi));
    Sleep(500);
    CHECK(!DeleteFileA("docrun.exe") && GetLastError() == ERROR_ACCESS_DENIED);
    CHECK(GetFileAttributesA("docrun.exe") != INVALID_FILE_ATTRIBUTES);
    TerminateProcess(pi.hProcess, 0);
    WaitForSingleObject(pi.hProcess, 10000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    BOOL gone = FALSE;
    for (int i = 0; i < 50 && !gone; i++) {                     /* (its files are let go just after it ends) */
        gone = DeleteFileA("docrun.exe");
        if (!gone) Sleep(100);
    }
    CHECK(gone && GetFileAttributesA("docrun.exe") == INVALID_FILE_ATTRIBUTES);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "wait")) { Sleep(60000); return 0; }
    if (argc > 1 && !strcmp(argv[1], "install")) return install();
    if (argc > 1 && !strcmp(argv[1], "installed")) return installed();
    DWORD n;
    char buf[256];
    CreateDirectoryA("C:\\Temp", 0);
    CHECK(SetCurrentDirectoryA("C:\\Temp"));
    CHECK(GetCurrentDirectoryA(sizeof(buf), buf) && !strcmp(buf, "C:\\Temp"));

    HANDLE h = CreateFileA("test.txt", GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    CHECK(h != INVALID_HANDLE_VALUE);
    CHECK(WriteFile(h, "Hello, file!\n", 13, &n, 0) && n == 13);
    CHECK(WriteFile(h, "Second line\n", 12, &n, 0) && n == 12);
    CloseHandle(h);

    h = CreateFileA("C:\\Temp\\test.txt", GENERIC_READ, 0, 0, OPEN_EXISTING, 0, 0);
    CHECK(h != INVALID_HANDLE_VALUE);
    CHECK(GetFileSize(h, 0) == 25);
    CHECK(ReadFile(h, buf, 5, &n, 0) && n == 5 && !memcmp(buf, "Hello", 5));
    CHECK(SetFilePointer(h, 7, 0, FILE_BEGIN) == 7);
    CHECK(ReadFile(h, buf, 100, &n, 0) && n == 18 && !memcmp(buf, "file!\nSecond line\n", 18));
    CHECK(ReadFile(h, buf, 100, &n, 0) && n == 0);                 /* EOF */
    CloseHandle(h);

    h = CreateFileA("test.txt", GENERIC_WRITE, 0, 0, OPEN_EXISTING, 0, 0);
    CHECK(SetFilePointer(h, 0, 0, FILE_END) == 25);
    CHECK(WriteFile(h, "tail", 4, &n, 0));
    CHECK(SetFilePointer(h, 5, 0, FILE_BEGIN) == 5 && SetEndOfFile(h));
    CloseHandle(h);
    CHECK((GetFileAttributesA("test.txt") & FILE_ATTRIBUTE_DIRECTORY) == 0);

    FILE *f = fopen("test.txt", "a+");
    CHECK(f != 0);
    fprintf(f, " world %d\n", 42);
    fseek(f, 0, SEEK_SET);
    CHECK(fgets(buf, sizeof(buf), f) && !strcmp(buf, "Hello world 42\n"));
    fclose(f);

    CHECK(CreateFileA("missing.txt", GENERIC_READ, 0, 0, OPEN_EXISTING, 0, 0) == INVALID_HANDLE_VALUE &&
          GetLastError() == ERROR_FILE_NOT_FOUND);
    CHECK(CreateFileA("test.txt", GENERIC_WRITE, 0, 0, CREATE_NEW, 0, 0) == INVALID_HANDLE_VALUE &&
          GetLastError() == ERROR_ALREADY_EXISTS);

    CHECK(CreateDirectoryA("sub", 0));
    CHECK(!CreateDirectoryA("sub", 0) && GetLastError() == ERROR_ALREADY_EXISTS);
    h = CreateFileA("sub\\a.dat", GENERIC_WRITE, 0, 0, CREATE_NEW, 0, 0);
    CHECK(h != INVALID_HANDLE_VALUE);
    CloseHandle(h);
    CHECK(!RemoveDirectoryA("sub") && GetLastError() == ERROR_DIR_NOT_EMPTY);

    WIN32_FIND_DATAA fd;
    int found = 0, count = 0;
    HANDLE fh = FindFirstFileA("C:\\Temp\\*", &fd);
    CHECK(fh != INVALID_HANDLE_VALUE);
    if (fh != INVALID_HANDLE_VALUE) {
        do { count++; if (!strcmp(fd.cFileName, "test.txt") && fd.nFileSizeLow == 15) found = 1; } while (FindNextFileA(fh, &fd));
        FindClose(fh);
    }
    CHECK(found && count == 4);                                 /* ".", "..", test.txt, sub */

    CHECK(DeleteFileA("sub\\a.dat") && RemoveDirectoryA("sub"));
    CHECK(DeleteFileA("test.txt"));
    CHECK(GetFileAttributesA("test.txt") == INVALID_FILE_ATTRIBUTES);

    void *p = VirtualAlloc(0, 1 << 20, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(p != 0);
    if (p) { memset(p, 0xAB, 1 << 20); CHECK(VirtualFree(p, 0, MEM_RELEASE)); }

    char self[MAX_PATH];
    GetModuleFileNameA(0, self, sizeof(self));
    delete_on_close(self);
    notify();
    pending();

    printf("filetest: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
