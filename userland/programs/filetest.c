/* filetest.exe — Win32 file API self-test on drive C: */
#include <stdio.h>
#include <string.h>
#include <windows.h>

static int pass, fail;
#define CHECK(cond) do { if (cond) pass++; else { fail++; printf("FAIL line %d: %s (error %lu)\n", __LINE__, #cond, GetLastError()); } } while (0)

int main(void)
{
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

    printf("filetest: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
