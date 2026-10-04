/* ramdisktest.exe — files on drive C: take the memory their contents need.
 *
 * Drive C: is kept in memory, so its free space is the machine's free
 * memory (GetDiskFreeSpaceEx), and a file's buffer is RAM for as long as the
 * file exists.  The checks:
 *   - a file written by appending (its buffer grows as it goes) takes about
 *     its size once it is closed, not the doubled buffer it grew into;
 *   - SetEndOfFile takes the size asked for, not the next power of two
 *     (a 72 MiB file once needed 128 MiB in one piece; 7-Zip sets the
 *     length of each file before unpacking it);
 *   - two files appended in turn (so neither can grow where it is), a file
 *     made shorter and longer again, and appending to a closed file keep
 *     their contents;
 *   - deleting the files gives the memory back. */
#include <stdio.h>
#include <string.h>
#include <windows.h>

static int pass, fail;
#define CHECK(what, cond) do { if (cond) pass++; else { fail++; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

#define MiB (1024ull * 1024ull)
#define CHUNK (64 * 1024)
#define SLACK (6 * MiB)           /* other programs and the kernel allocate meanwhile */

static unsigned char buf[CHUNK];

static unsigned long long free_bytes(void)
{
    ULARGE_INTEGER avail, total, free;
    if (!GetDiskFreeSpaceExW(L"C:\\", &avail, &total, &free)) return 0;
    return free.QuadPart;
}

static unsigned char pattern(int file, unsigned long long off) { return (unsigned char)(file * 37 + off / 4093 + off); }

static void fill(int file, unsigned long long off, DWORD n)
{
    for (DWORD i = 0; i < n; i++) buf[i] = pattern(file, off + i);
}

static HANDLE create(const WCHAR *name, DWORD disp)
{
    return CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, disp, FILE_ATTRIBUTE_NORMAL, NULL);
}

static BOOL append(HANDLE h, int file, unsigned long long off, DWORD n)
{
    DWORD got = 0;
    fill(file, off, n);
    return WriteFile(h, buf, n, &got, NULL) && got == n;
}

/* The file's contents are pattern @file for @len bytes (zeros from @zero_from to @zero_to) */
static BOOL verify(const WCHAR *name, int file, unsigned long long len, unsigned long long zero_from,
                   unsigned long long zero_to)
{
    HANDLE h = CreateFileW(name, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    LARGE_INTEGER size;
    BOOL ok = GetFileSizeEx(h, &size) && (unsigned long long)size.QuadPart == len;
    for (unsigned long long off = 0; ok && off < len; off += CHUNK) {
        DWORD n = len - off < CHUNK ? (DWORD)(len - off) : CHUNK, got = 0;
        ok = ReadFile(h, buf, n, &got, NULL) && got == n;
        for (DWORD i = 0; ok && i < n; i++) {
            unsigned long long at = off + i;
            unsigned char want = at >= zero_from && at < zero_to ? 0 : pattern(file, at);
            if (buf[i] != want) { printf("  %ls: byte %llu is %u, not %u\n", name, at, buf[i], want); ok = FALSE; }
        }
    }
    CloseHandle(h);
    return ok;
}

int main(void)
{
    const WCHAR *a = L"C:\\Temp\\ramdisktest-a.bin", *b = L"C:\\Temp\\ramdisktest-b.bin", *c = L"C:\\Temp\\ramdisktest-c.bin";
    CreateDirectoryW(L"C:\\Temp", NULL);
    DeleteFileW(a); DeleteFileW(b); DeleteFileW(c);
    unsigned long long start = free_bytes();
    CHECK("C: reports its free space", start > 200 * MiB);

    /* 1. 33 MiB appended in 64 KiB writes: the buffer may grow to 64 MiB
     *    while the file is open, the file takes 33 MiB once closed */
    unsigned long long alen = 33 * MiB;
    HANDLE h = create(a, CREATE_ALWAYS);
    CHECK("create the appended file", h != INVALID_HANDLE_VALUE);
    BOOL ok = TRUE;
    for (unsigned long long off = 0; ok && off < alen; off += CHUNK) ok = append(h, 1, off, CHUNK);
    CHECK("append 33 MiB", ok);
    CloseHandle(h);
    unsigned long long taken = start - free_bytes();
    printf("ramdisktest: 33 MiB appended, closed: %llu KiB taken\n", taken / 1024);
    CHECK("a closed appended file takes its size, not its grown buffer", taken < alen + SLACK);
    CHECK("the appended file reads back", verify(a, 1, alen, 0, 0));

    /* 2. SetEndOfFile to 72 MiB takes 72 MiB, zeros; then written to */
    unsigned long long before = free_bytes(), clen = 72 * MiB;
    h = create(c, CREATE_ALWAYS);
    LARGE_INTEGER pos = { .QuadPart = (LONGLONG)clen };
    ok = h != INVALID_HANDLE_VALUE && SetFilePointerEx(h, pos, NULL, FILE_BEGIN) && SetEndOfFile(h);
    CHECK("set the length of a new file to 72 MiB", ok);
    taken = before - free_bytes();
    printf("ramdisktest: length set to 72 MiB: %llu KiB taken\n", taken / 1024);
    CHECK("SetEndOfFile takes the length asked for", taken < clen + SLACK);
    pos.QuadPart = 0;
    ok = SetFilePointerEx(h, pos, NULL, FILE_BEGIN);
    for (unsigned long long off = 0; ok && off < 8 * MiB; off += CHUNK) ok = append(h, 3, off, CHUNK);
    CHECK("write into the file whose length was set", ok);
    CloseHandle(h);
    CHECK("a set length keeps its zeros past what was written", verify(c, 3, clen, 8 * MiB, clen));
    DeleteFileW(c);

    /* 3. Two files appended in turn (neither can grow where it is), then made
     *    shorter and longer again, then appended to after being closed */
    HANDLE ha = create(b, CREATE_ALWAYS);
    h = create(c, CREATE_ALWAYS);
    ok = ha != INVALID_HANDLE_VALUE && h != INVALID_HANDLE_VALUE;
    unsigned long long blen = 5 * MiB + 1234;
    for (unsigned long long off = 0; ok && off < blen; off += CHUNK) {
        DWORD n = blen - off < CHUNK ? (DWORD)(blen - off) : CHUNK;
        ok = append(ha, 2, off, n) && append(h, 4, off, n);
    }
    CHECK("append to two files in turn", ok);
    unsigned long long cut = 3 * MiB + 77, grow = 4 * MiB;
    pos.QuadPart = (LONGLONG)cut;
    ok = SetFilePointerEx(h, pos, NULL, FILE_BEGIN) && SetEndOfFile(h);
    pos.QuadPart = (LONGLONG)grow;
    ok = ok && SetFilePointerEx(h, pos, NULL, FILE_BEGIN) && SetEndOfFile(h);
    CHECK("make a file shorter and longer again", ok);
    CloseHandle(ha);
    CloseHandle(h);
    CHECK("the first file of the pair reads back", verify(b, 2, blen, 0, 0));
    CHECK("the shortened file reads back, zeros where it grew", verify(c, 4, grow, cut, grow));
    h = CreateFileW(b, FILE_APPEND_DATA, 0, NULL, OPEN_EXISTING, 0, NULL);
    ok = h != INVALID_HANDLE_VALUE;
    for (unsigned long long off = blen; ok && off < blen + 3 * MiB; off += CHUNK) ok = append(h, 2, off, CHUNK);
    CloseHandle(h);
    CHECK("append to a closed file", ok);
    CHECK("the file appended to again reads back", verify(b, 2, blen + 3 * MiB, 0, 0));

    /* 4. Deleting them gives the memory back */
    CHECK("delete the files", DeleteFileW(a) && DeleteFileW(b) && DeleteFileW(c));
    unsigned long long end = free_bytes();
    printf("ramdisktest: after deleting: %lld KiB taken\n", (long long)(start - end) / 1024);
    CHECK("deleted files give their memory back", end + SLACK > start);

    printf("ramdisktest: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
