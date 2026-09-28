/*
 * disktest.exe write | verify — checks that drive C: survives a restart.
 *
 * "write" builds C:\DiskTest: 150 files of assorted sizes (up to 3 MiB)
 * in nested folders with long names, then renames a folder, deletes some
 * files and rewrites others.  After a restart, "verify" checks that
 * exactly the expected files are there with the expected contents.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define NFILES 150

static unsigned size_of(int i) { return i % 50 == 7 ? 3u * 1024 * 1024 + (unsigned)i : (unsigned)(i * 997 % 70000); }

/* file i's folder and name; @renamed: after folder "Group 2" became "Group Two (renamed)" */
static void path_of(int i, char *out, int renamed)
{
    int g = i % 5;
    const char *dir = g == 2 && renamed ? "Group Two (renamed)" : NULL;
    char gname[32];
    if (!dir) { sprintf(gname, "Group %d", g); dir = gname; }
    sprintf(out, "C:\\DiskTest\\%s\\Sub %d\\file number %03d with a long name.bin", dir, i % 3, i);
}

static unsigned char byte_at(int i, unsigned off, int gen) { return (unsigned char)(off * 31 + (unsigned)i * 7 + (unsigned)gen * 13); }

static int deleted(int i) { return i % 11 == 3; }
static int rewritten(int i) { return i % 13 == 5 && !deleted(i); }

static int write_file(const char *path, int i, int gen)
{
    unsigned n = size_of(i);
    unsigned char *buf = malloc(n ? n : 1);
    for (unsigned k = 0; k < n; k++) buf[k] = byte_at(i, k, gen);
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
    DWORD w = 0;
    int ok = h != INVALID_HANDLE_VALUE && WriteFile(h, buf, n, &w, 0) && w == n;
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    free(buf);
    if (!ok) printf("write failed: %s (%lu)\n", path, GetLastError());
    return ok;
}

static void mkdirs(const char *file)
{
    char p[MAX_PATH];
    strcpy(p, file);
    for (char *s = p + 3; *s; s++)
        if (*s == '\\') { *s = 0; CreateDirectoryA(p, 0); *s = '\\'; }
}

int main(int argc, char **argv)
{
    char path[MAX_PATH], path2[MAX_PATH];
    if (argc > 1 && !strcmp(argv[1], "write")) {
        int ok = 1;
        for (int i = 0; i < NFILES; i++) {
            path_of(i, path, 0);
            mkdirs(path);
            ok &= write_file(path, i, 0);
        }
        if (!MoveFileA("C:\\DiskTest\\Group 2", "C:\\DiskTest\\Group Two (renamed)")) { printf("rename failed\n"); ok = 0; }
        for (int i = 0; i < NFILES; i++) {
            path_of(i, path, 1);
            if (deleted(i) && !DeleteFileA(path)) { printf("delete failed: %s\n", path); ok = 0; }
            if (rewritten(i)) ok &= write_file(path, i, 1);
        }
        CreateDirectoryA("C:\\DiskTest\\Empty folder", 0);
        CreateDirectoryA("C:\\DiskTest\\Gone", 0);
        path_of(0, path2, 0);
        strcpy(path2, "C:\\DiskTest\\Gone\\x.txt");
        write_file(path2, 1, 0);
        DeleteFileA(path2);
        RemoveDirectoryA("C:\\DiskTest\\Gone");
        printf("disktest write: %s\n", ok ? "done" : "FAILED");
        return !ok;
    }
    if (argc > 1 && !strcmp(argv[1], "verify")) {
        int good = 0, bad = 0;
        for (int i = 0; i < NFILES; i++) {
            path_of(i, path, 1);
            HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
            if (deleted(i)) {
                if (h != INVALID_HANDLE_VALUE) { printf("still there: %s\n", path); bad++; CloseHandle(h); } else good++;
                continue;
            }
            if (h == INVALID_HANDLE_VALUE) { printf("missing: %s\n", path); bad++; continue; }
            unsigned n = size_of(i);
            unsigned char *buf = malloc(n + 1);
            DWORD r = 0;
            ReadFile(h, buf, n + 1, &r, 0);
            CloseHandle(h);
            int gen = rewritten(i), same = r == n;
            for (unsigned k = 0; same && k < n; k++) same = buf[k] == byte_at(i, k, gen);
            free(buf);
            if (same) good++; else { printf("wrong contents: %s (%lu bytes)\n", path, r); bad++; }
        }
        DWORD a = GetFileAttributesA("C:\\DiskTest\\Group 2");
        if (a != INVALID_FILE_ATTRIBUTES) { printf("old folder name still there\n"); bad++; }
        a = GetFileAttributesA("C:\\DiskTest\\Empty folder");
        if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) { printf("empty folder missing\n"); bad++; }
        if (GetFileAttributesA("C:\\DiskTest\\Gone") != INVALID_FILE_ATTRIBUTES) { printf("removed folder came back\n"); bad++; }
        printf("disktest verify: %d ok, %d wrong\n", good, bad);
        return bad != 0;
    }
    printf("usage: disktest write | verify\n");
    return 1;
}
