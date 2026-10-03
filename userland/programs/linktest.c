/* linktest.exe — hard links (CreateHardLink) self-test
 *
 *   linktest [DIR]         the tests, in DIR (C:\LinkTest); leaves kept-a.txt and
 *                          kept-b.txt there, two names of one file
 *   linktest restarted     after a restart: they are still one file
 */
#include <stdio.h>
#include <string.h>
#include <windows.h>

#ifndef FILE_SUPPORTS_HARD_LINKS
#define FILE_SUPPORTS_HARD_LINKS 0x00400000
#endif

static int pass, fail;
#define CHECK(cond) do { if (cond) pass++; else { fail++; printf("FAIL line %d: %s (error %lu)\n", __LINE__, #cond, GetLastError()); } } while (0)

static const char *g_dir = "C:\\LinkTest";

/* The link count and identity of @name, by a fresh handle */
static BOOL info(const char *name, BY_HANDLE_FILE_INFORMATION *bi)
{
    HANDLE h = CreateFileA(name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0, OPEN_EXISTING, 0, 0);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    BOOL ok = GetFileInformationByHandle(h, bi);
    CloseHandle(h);
    return ok;
}

static int links(const char *name)
{
    BY_HANDLE_FILE_INFORMATION bi;
    return info(name, &bi) ? (int)bi.nNumberOfLinks : -1;
}

static int same_file(const char *a, const char *b)
{
    BY_HANDLE_FILE_INFORMATION x, y;
    if (!info(a, &x) || !info(b, &y)) return 0;
    return x.nFileIndexLow == y.nFileIndexLow && x.nFileIndexHigh == y.nFileIndexHigh &&
           x.dwVolumeSerialNumber == y.dwVolumeSerialNumber;
}

static int read_all(const char *name, char *buf, DWORD cap)
{
    HANDLE h = CreateFileA(name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
    if (h == INVALID_HANDLE_VALUE) return -1;
    DWORD n = 0;
    BOOL ok = ReadFile(h, buf, cap - 1, &n, 0);
    CloseHandle(h);
    if (!ok) return -1;
    buf[n] = '\0';
    return (int)n;
}

static BOOL write_all(const char *name, const char *text, DWORD how)
{
    HANDLE h = CreateFileA(name, GENERIC_WRITE, FILE_SHARE_READ, 0, how, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    DWORD n;
    if (how == OPEN_EXISTING) SetFilePointer(h, 0, 0, FILE_END);
    BOOL ok = WriteFile(h, text, (DWORD)strlen(text), &n, 0) && n == strlen(text);
    CloseHandle(h);
    return ok;
}

static void clean(void)
{
    const char *names[] = { "a.txt", "b.txt", "sub\\c.txt", "sub\\d.txt", "e.txt", "kept-a.txt", "kept-b.txt" };
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        SetFileAttributesA(names[i], FILE_ATTRIBUTE_NORMAL);
        DeleteFileA(names[i]);
    }
    RemoveDirectoryA("sub");
}

static int restarted(void)
{
    CHECK(SetCurrentDirectoryA(g_dir));
    char buf[256];
    CHECK(links("kept-a.txt") == 2 && links("kept-b.txt") == 2);
    CHECK(same_file("kept-a.txt", "kept-b.txt"));
    CHECK(read_all("kept-b.txt", buf, sizeof(buf)) > 0 && !strcmp(buf, "Kept by linktest\r\n"));
    CHECK(write_all("kept-a.txt", "and again\r\n", OPEN_EXISTING));
    CHECK(read_all("kept-b.txt", buf, sizeof(buf)) > 0 && !strcmp(buf, "Kept by linktest\r\nand again\r\n"));
    printf("linktest restarted: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "restarted")) return restarted();
    if (argc > 1) g_dir = argv[1];
    char buf[256], root[4] = "C:\\";
    DWORD n, flags = 0;
    if (g_dir[1] == ':') root[0] = g_dir[0];

    CHECK(GetVolumeInformationA(root, 0, 0, 0, 0, &flags, 0, 0) && (flags & FILE_SUPPORTS_HARD_LINKS));

    CreateDirectoryA(g_dir, 0);
    CHECK(SetCurrentDirectoryA(g_dir));
    clean();

    /* two names, one file */
    CHECK(write_all("a.txt", "hello", CREATE_ALWAYS));
    CHECK(links("a.txt") == 1);
    CHECK(CreateHardLinkA("b.txt", "a.txt", 0));
    CHECK(links("a.txt") == 2 && links("b.txt") == 2);
    CHECK(same_file("a.txt", "b.txt"));
    CHECK(read_all("b.txt", buf, sizeof(buf)) == 5 && !strcmp(buf, "hello"));

    /* a write by one name is read by the other, at once */
    CHECK(write_all("b.txt", " world", OPEN_EXISTING));
    CHECK(read_all("a.txt", buf, sizeof(buf)) == 11 && !strcmp(buf, "hello world"));
    HANDLE ha = CreateFileA("a.txt", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
    CHECK(ha != INVALID_HANDLE_VALUE);
    CHECK(GetFileSize(ha, 0) == 11);
    CHECK(write_all("b.txt", "!", OPEN_EXISTING));
    CHECK(GetFileSize(ha, 0) == 12);                            /* (an open handle sees the other name's write) */
    CHECK(SetFilePointer(ha, 11, 0, FILE_BEGIN) == 11 && ReadFile(ha, buf, 1, &n, 0) && n == 1 && buf[0] == '!');
    CloseHandle(ha);

    /* attributes are the file's, not the name's */
    CHECK(SetFileAttributesA("a.txt", FILE_ATTRIBUTE_READONLY));
    CHECK(GetFileAttributesA("b.txt") & FILE_ATTRIBUTE_READONLY);
    CHECK(SetFileAttributesA("b.txt", FILE_ATTRIBUTE_NORMAL));
    CHECK(!(GetFileAttributesA("a.txt") & FILE_ATTRIBUTE_READONLY));

    /* both names are listed */
    WIN32_FIND_DATAA fd;
    int seen_a = 0, seen_b = 0, count = 0;
    HANDLE fh = FindFirstFileA("*", &fd);
    CHECK(fh != INVALID_HANDLE_VALUE);
    if (fh != INVALID_HANDLE_VALUE) {
        do {
            count++;
            if (!strcmp(fd.cFileName, "a.txt") && fd.nFileSizeLow == 12) seen_a = 1;
            if (!strcmp(fd.cFileName, "b.txt") && fd.nFileSizeLow == 12) seen_b = 1;
        } while (FindNextFileA(fh, &fd));
        FindClose(fh);
    }
    CHECK(seen_a && seen_b && count == 4);                      /* ".", "..", a.txt, b.txt */

    /* what is refused: a taken name, a directory, a name on another drive */
    CHECK(!CreateHardLinkA("b.txt", "a.txt", 0) && GetLastError() == ERROR_ALREADY_EXISTS);
    CHECK(CreateDirectoryA("sub", 0));
    CHECK(!CreateHardLinkA("e.txt", "sub", 0));
    CHECK(!CreateHardLinkA("e.txt", "missing.txt", 0) && GetLastError() == ERROR_FILE_NOT_FOUND);
    CHECK(!CreateHardLinkA(root[0] == 'C' ? "Z:\\e.txt" : "C:\\LinkTest\\e.txt", "a.txt", 0));
    CHECK(GetFileAttributesA("e.txt") == INVALID_FILE_ATTRIBUTES);

    /* a name in another folder, renamed there */
    CHECK(CreateHardLinkA("sub\\c.txt", "a.txt", 0));
    CHECK(links("a.txt") == 3);
    CHECK(MoveFileA("sub\\c.txt", "sub\\d.txt"));
    CHECK(links("b.txt") == 3 && same_file("sub\\d.txt", "b.txt"));
    CHECK(read_all("sub\\d.txt", buf, sizeof(buf)) == 12 && !strcmp(buf, "hello world!"));

    /* deleting a name leaves the file to the others; the last name takes it */
    CHECK(DeleteFileA("a.txt"));
    CHECK(GetFileAttributesA("a.txt") == INVALID_FILE_ATTRIBUTES);
    CHECK(links("b.txt") == 2);
    CHECK(read_all("sub\\d.txt", buf, sizeof(buf)) == 12 && !strcmp(buf, "hello world!"));
    HANDLE hb = CreateFileA("b.txt", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0, OPEN_EXISTING, 0, 0);
    CHECK(hb != INVALID_HANDLE_VALUE);
    CHECK(DeleteFileA("sub\\d.txt"));                            /* (while the file is open by another name) */
    BY_HANDLE_FILE_INFORMATION bi;
    CHECK(GetFileInformationByHandle(hb, &bi) && bi.nNumberOfLinks == 1);
    CHECK(SetFilePointer(hb, 0, 0, FILE_BEGIN) == 0 && ReadFile(hb, buf, 12, &n, 0) && n == 12 && !memcmp(buf, "hello world!", 12));
    CloseHandle(hb);
    CHECK(RemoveDirectoryA("sub"));
    CHECK(DeleteFileA("b.txt"));

    /* a new file over a name that is a link, and a link replacing a file */
    CHECK(write_all("a.txt", "one", CREATE_ALWAYS) && CreateHardLinkA("b.txt", "a.txt", 0));
    CHECK(write_all("b.txt", "two", CREATE_ALWAYS));            /* (truncates the file, by either name) */
    CHECK(read_all("a.txt", buf, sizeof(buf)) == 3 && !strcmp(buf, "two"));
    CHECK(write_all("e.txt", "three", CREATE_ALWAYS));
    CHECK(MoveFileExA("e.txt", "a.txt", MOVEFILE_REPLACE_EXISTING));   /* a.txt is e's file now; b.txt keeps "two" */
    CHECK(read_all("a.txt", buf, sizeof(buf)) == 5 && !strcmp(buf, "three"));
    CHECK(read_all("b.txt", buf, sizeof(buf)) == 3 && !strcmp(buf, "two") && links("b.txt") == 1);
    CHECK(DeleteFileA("a.txt") && DeleteFileA("b.txt"));

    /* kept for "linktest restarted" */
    CHECK(write_all("kept-a.txt", "Kept by linktest\r\n", CREATE_ALWAYS));
    CHECK(CreateHardLinkA("kept-b.txt", "kept-a.txt", 0));
    CHECK(links("kept-b.txt") == 2);

    printf("linktest: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
