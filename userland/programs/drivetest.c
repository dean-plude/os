/* drivetest.exe — drives beyond C: (read-only NTFS volumes on disk)
 *
 * Expects the disk scripts/make-ntfs-disk.sh makes, attached as drive D:.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* (not in our windows.h yet) */
#ifndef DRIVE_FIXED
#define DRIVE_NO_ROOT_DIR 1
#define DRIVE_FIXED 3
UINT WINAPI GetDriveTypeA(LPCSTR root);
BOOL WINAPI GetVolumeInformationA(LPCSTR root, LPSTR name, DWORD nn, LPDWORD serial, LPDWORD maxlen, LPDWORD flags,
                                  LPSTR fs, DWORD nfs);
#endif
#ifndef ERROR_WRITE_PROTECT
#define ERROR_WRITE_PROTECT 19
#endif
#ifndef FILE_READ_ONLY_VOLUME
#define FILE_READ_ONLY_VOLUME 0x00080000
#endif

static int pass, fail;
#define CHECK(what, cond) do { if (cond) pass++; else { fail++; printf("FAIL: %s (error %lu)\n", what, GetLastError()); } } while (0)

/* The bytes of data.bin: a pattern the script writes too */
static unsigned char pattern(unsigned i) { return (unsigned char)(i * 7 + (i >> 9)); }

static char *read_all(const char *path, DWORD *n)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    DWORD size = GetFileSize(h, 0), got = 0;
    char *b = malloc(size + 1);
    if (b && (!ReadFile(h, b, size, &got, 0) || got != size)) { free(b); b = NULL; }
    CloseHandle(h);
    if (b) { b[size] = 0; *n = size; }
    return b;
}

int main(void)
{
    DWORD drives = GetLogicalDrives();
    CHECK("GetLogicalDrives has C: and D:", (drives & 0xC) == 0xC);
    char list[128];
    DWORD n = GetLogicalDriveStringsA(sizeof(list), list);
    CHECK("GetLogicalDriveStrings lists D:\\", n >= 8 && !strcmp(list + 4, "D:\\"));
    CHECK("GetDriveType(D:) is fixed", GetDriveTypeA("D:\\") == DRIVE_FIXED);
    CHECK("GetDriveType(Q:) has no root", GetDriveTypeA("Q:\\") == DRIVE_NO_ROOT_DIR);

    char label[64], fs[32];
    DWORD serial, maxlen, flags;
    BOOL ok = GetVolumeInformationA("D:\\", label, sizeof(label), &serial, &maxlen, &flags, fs, sizeof(fs));
    CHECK("GetVolumeInformation(D:)", ok);
    CHECK("D: is labelled NOVATEST", ok && !strcmp(label, "NOVATEST"));
    CHECK("D: is NTFS", ok && !strcmp(fs, "NTFS"));
    CHECK("D: is read-only", ok && (flags & FILE_READ_ONLY_VOLUME));
    ULARGE_INTEGER avail, total, freeb;
    CHECK("GetDiskFreeSpaceEx(D:)", GetDiskFreeSpaceExA("D:\\", &avail, &total, &freeb) && total.QuadPart > (100ull << 20));

    char *s = read_all("D:\\hello.txt", &n);
    CHECK("read D:\\hello.txt", s && !strcmp(s, "Hello from NTFS\r\n"));
    free(s);
    s = read_all("d:\\DIR\\nested\\Data.bin", &n);           /* names match whatever the case */
    ok = s && n == 300000;
    for (DWORD i = 0; ok && i < n; i++) ok = (unsigned char)s[i] == pattern(i);
    CHECK("read D:\\Dir\\Nested\\data.bin", ok);
    free(s);
    s = read_all("D:\\Compressed\\words.txt", &n);
    ok = s && n == 600000;
    for (DWORD i = 0; ok && i < n; i++) ok = s[i] == "kernel ntfs "[i % 12];
    CHECK("read a compressed file", ok);
    free(s);
    s = read_all("D:\\Unicode \xc3\xa9t\xc3\xa9\\caf\xc3\xa9.txt", &n);
    CHECK("read a file with accented names", s && !strcmp(s, "accents\n"));
    free(s);

    WIN32_FIND_DATAA fd;
    HANDLE f = FindFirstFileA("D:\\Many\\*", &fd);
    int count = 0;
    if (f != INVALID_HANDLE_VALUE) {
        do if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..")) count++; while (FindNextFileA(f, &fd));
        FindClose(f);
    }
    CHECK("list 2000 files in D:\\Many", count == 2000);
    f = FindFirstFileA("D:\\*", &fd);
    ok = f != INVALID_HANDLE_VALUE;
    BOOL meta = FALSE;
    if (ok) { do if (fd.cFileName[0] == '$') meta = TRUE; while (FindNextFileA(f, &fd)); FindClose(f); }
    CHECK("the root lists no metafiles ($MFT ...)", ok && !meta);
    DWORD attr = GetFileAttributesA("D:\\hello.txt");
    CHECK("files on D: are read-only", attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_READONLY));
    CHECK("D:\\Dir is a directory", GetFileAttributesA("D:\\Dir") & FILE_ATTRIBUTE_DIRECTORY);

    HANDLE h = CreateFileA("D:\\new.txt", GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
    CHECK("creating a file on D: fails", h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_WRITE_PROTECT);
    h = CreateFileA("D:\\hello.txt", GENERIC_WRITE, 0, 0, OPEN_EXISTING, 0, 0);
    CHECK("writing a file on D: fails", h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_WRITE_PROTECT);
    CHECK("deleting a file on D: fails", !DeleteFileA("D:\\hello.txt") && GetFileAttributesA("D:\\hello.txt") != INVALID_FILE_ATTRIBUTES);
    CHECK("making a folder on D: fails", !CreateDirectoryA("D:\\NewDir", 0));

    CHECK("SetCurrentDirectory(D:\\Dir)", SetCurrentDirectoryA("D:\\Dir"));
    s = read_all("Nested\\data.bin", &n);
    CHECK("a relative path on D:", s && n == 300000);
    free(s);
    s = read_all("\\hello.txt", &n);
    CHECK("a rooted path is on the current drive", s && !strcmp(s, "Hello from NTFS\r\n"));
    free(s);
    char cwd[MAX_PATH];
    GetCurrentDirectoryA(sizeof(cwd), cwd);
    CHECK("GetCurrentDirectory is D:\\Dir", !_stricmp(cwd, "D:\\Dir"));
    SetCurrentDirectoryA("C:\\");

    HMODULE m = LoadLibraryA("D:\\Bin\\testdll.dll");
    CHECK("LoadLibrary from D:", m != NULL);
    int (*add)(int, int) = m ? (int (*)(int, int))GetProcAddress(m, "testdll_add") : NULL;
    CHECK("call into a DLL loaded from D:", add && add(20, 22) == 42);

    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    char cmd[] = "D:\\Bin\\hello.exe";
    ok = CreateProcessA(0, cmd, 0, 0, FALSE, 0, 0, 0, &si, &pi);
    DWORD code = 99;
    if (ok) { WaitForSingleObject(pi.hProcess, 10000); GetExitCodeProcess(pi.hProcess, &code); CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
    CHECK("run a program from D:", ok && code == 0);

    printf("drivetest: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
