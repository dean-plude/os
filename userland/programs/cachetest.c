/* cachetest.exe — drive C:'s saved files let go of when memory runs short.
 *
 * Drive C: lives in memory and is saved to the data disk.  The contents of
 * a saved file that nothing holds can be let go of and are read back from
 * the disk when wanted, as Windows drops a file's cached pages; the same
 * happens to what NovaOS restores at boot, which is read only once wanted.
 * NtSetSystemInformation(SystemMemoryListInformation) asks for both steps,
 * as RAMMap does: MemoryFlushModifiedList saves, MemoryPurgeStandbyList
 * lets go.  The checks:
 *   - letting go gives the memory back (C:'s free space) and the
 *     files read back as they were, also through a mapped view;
 *   - opening a file for its details only reads nothing back;
 *   - a file renamed and one hard-linked while only on the disk, then
 *     saved and let go of again, read back as they were (a file is read
 *     back before it is saved under its new name);
 *   - one emptied while only on the disk is empty;
 *   - "cachetest after" (after a restart): the files, restored without
 *     being read, read back as they were; then they are deleted. */
#include <stdio.h>
#include <string.h>
#include <windows.h>

static int pass, fail;
#define CHECK(what, cond) do { if (cond) pass++; else { fail++; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

#define MiB (1024u * 1024u)
#define DIR_ L"C:\\Temp\\cachetest"

#ifndef FILE_READ_ATTRIBUTES
#define FILE_READ_ATTRIBUTES 0x0080
#endif

typedef LONG (*SetSysInfo)(ULONG, PVOID, ULONG);
static SetSysInfo set_sys_info;

static unsigned char pattern(int file, DWORD off) { return (unsigned char)(file * 53 + off / 4099 + off); }

static BOOL make(const WCHAR *name, int file, DWORD len)
{
    static unsigned char b[64 * 1024];
    HANDLE h = CreateFileW(name, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    BOOL ok = TRUE;
    for (DWORD off = 0; off < len && ok; off += sizeof(b)) {
        DWORD n = len - off < sizeof(b) ? len - off : sizeof(b), got = 0;
        for (DWORD i = 0; i < n; i++) b[i] = pattern(file, off + i);
        ok = WriteFile(h, b, n, &got, NULL) && got == n;
    }
    CloseHandle(h);
    return ok;
}

static BOOL same(const unsigned char *p, int file, DWORD len)
{
    for (DWORD i = 0; i < len; i++) if (p[i] != pattern(file, i)) return FALSE;
    return TRUE;
}

/* @name holds pattern @file, @len bytes (read with ReadFile) */
static BOOL verify(const WCHAR *name, int file, DWORD len)
{
    HANDLE h = CreateFileW(name, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    unsigned char *p = VirtualAlloc(NULL, len + 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    DWORD got = 0;
    BOOL ok = p && ReadFile(h, p, len + 4096, &got, NULL) && got == len && same(p, file, len);
    if (p) VirtualFree(p, 0, MEM_RELEASE);
    CloseHandle(h);
    return ok;
}

/* The same through a mapped view */
static BOOL verify_mapped(const WCHAR *name, int file, DWORD len)
{
    HANDLE h = CreateFileW(name, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    HANDLE m = CreateFileMappingW(h, NULL, PAGE_READONLY, 0, 0, NULL);
    const unsigned char *v = m ? MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0) : NULL;
    BOOL ok = v && same(v, file, len);
    if (v) UnmapViewOfFile(v);
    if (m) CloseHandle(m);
    CloseHandle(h);
    return ok;
}

static LONG memory_list(ULONG cmd) { return set_sys_info(80, &cmd, sizeof(cmd)); }   /* SystemMemoryListInformation */

/* Free memory: drive C:'s free space (GlobalMemoryStatusEx's figure is
 * updated every tick) */
static unsigned long long avail(void)
{
    ULARGE_INTEGER a, total, free;
    return GetDiskFreeSpaceExW(L"C:\\", &a, &total, &free) ? free.QuadPart : 0;
}

static unsigned long long size_of(const WCHAR *name, DWORD access)
{
    HANDLE h = CreateFileW(name, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    LARGE_INTEGER s = { 0 };
    if (h == INVALID_HANDLE_VALUE || !GetFileSizeEx(h, &s)) s.QuadPart = -1;
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    return (unsigned long long)s.QuadPart;
}

static void before_restart(void)
{
    CreateDirectoryW(L"C:\\Temp", NULL);
    CreateDirectoryW(DIR_, NULL);
    CHECK("an unknown memory list command is refused", memory_list(9) == (LONG)0xC000000D);
    CHECK("write a.bin (3 MiB)", make(DIR_ L"\\a.bin", 1, 3 * MiB));
    CHECK("write b.bin (1 MiB)", make(DIR_ L"\\b.bin", 2, 1 * MiB));
    CHECK("write c.bin (2 MiB)", make(DIR_ L"\\c.bin", 3, 2 * MiB));
    CHECK("write d.bin (1 MiB)", make(DIR_ L"\\d.bin", 4, 1 * MiB));
    CHECK("write small.txt (1 KiB)", make(DIR_ L"\\small.txt", 5, 1024));
    CHECK("MemoryFlushModifiedList saves them", memory_list(3) == 0);

    unsigned long long a0 = avail();
    CHECK("MemoryPurgeStandbyList", memory_list(4) == 0);
    unsigned long long a1 = avail();
    printf("  free memory %llu MiB before letting go, %llu MiB after\n", a0 / MiB, a1 / MiB);
    CHECK("letting go gives their memory back (6 of their 7 MiB at least)", a1 >= a0 + 6ull * MiB);

    CHECK("a.bin's size, opened for its details", size_of(DIR_ L"\\a.bin", FILE_READ_ATTRIBUTES) == 3 * MiB);
    CHECK("which read nothing back", avail() + 2ull * MiB >= a1);

    CHECK("a.bin reads back as it was", verify(DIR_ L"\\a.bin", 1, 3 * MiB));
    CHECK("small.txt was kept", verify(DIR_ L"\\small.txt", 5, 1024));
    unsigned long long a2 = avail();
    CHECK("reading a.bin back takes its memory again", a2 + 2ull * MiB <= a1);

    CHECK("let go again", memory_list(4) == 0);
    CHECK("a.bin reads back through a mapped view", verify_mapped(DIR_ L"\\a.bin", 1, 3 * MiB));

    CHECK("let go again", memory_list(4) == 0);
    CHECK("rename b.bin to b2.bin while it is only on the disk", MoveFileW(DIR_ L"\\b.bin", DIR_ L"\\b2.bin"));
    CHECK("hard-link c.bin as c2.bin while it is only on the disk", CreateHardLinkW(DIR_ L"\\c2.bin", DIR_ L"\\c.bin", NULL));
    HANDLE h = CreateFileW(DIR_ L"\\d.bin", GENERIC_WRITE, 0, NULL, TRUNCATE_EXISTING, 0, NULL);
    CHECK("empty d.bin while it is only on the disk", h != INVALID_HANDLE_VALUE);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    CHECK("save", memory_list(3) == 0);
    CHECK("let go again", memory_list(4) == 0);
    CHECK("b2.bin reads back as b.bin was", verify(DIR_ L"\\b2.bin", 2, 1 * MiB));
    CHECK("b.bin is gone", GetFileAttributesW(DIR_ L"\\b.bin") == INVALID_FILE_ATTRIBUTES);
    CHECK("c.bin reads back as it was", verify(DIR_ L"\\c.bin", 3, 2 * MiB));
    CHECK("c2.bin too", verify(DIR_ L"\\c2.bin", 3, 2 * MiB));
    CHECK("d.bin is empty", size_of(DIR_ L"\\d.bin", GENERIC_READ) == 0);
    CHECK("save before the restart", memory_list(3) == 0);
}

static void after_restart(void)
{
    CHECK("a.bin reads back as it was", verify(DIR_ L"\\a.bin", 1, 3 * MiB));
    CHECK("b2.bin too", verify(DIR_ L"\\b2.bin", 2, 1 * MiB));
    CHECK("c.bin too", verify(DIR_ L"\\c.bin", 3, 2 * MiB));
    CHECK("c2.bin too", verify(DIR_ L"\\c2.bin", 3, 2 * MiB));
    CHECK("small.txt too", verify(DIR_ L"\\small.txt", 5, 1024));
    CHECK("d.bin is empty", size_of(DIR_ L"\\d.bin", GENERIC_READ) == 0);
    const char *names[] = { "a.bin", "b2.bin", "c.bin", "c2.bin", "d.bin", "small.txt" };
    for (int i = 0; i < 6; i++) {
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "C:\\Temp\\cachetest\\%s", names[i]);
        DeleteFileA(path);
    }
    CHECK("clean up", RemoveDirectoryW(DIR_));
}

int main(int argc, char **argv)
{
    set_sys_info = (SetSysInfo)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetSystemInformation");
    if (!set_sys_info) { printf("FAIL: no NtSetSystemInformation\n"); return 1; }
    if (argc > 1 && !strcmp(argv[1], "after")) after_restart();
    else before_restart();
    printf("cachetest: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
