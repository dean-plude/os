/* wvsetuptest.exe — what the WebView2 runtime's own setup (Chromium's
 * setup.exe) needs to unpack its archive and report its crashes:
 *
 *   - a file far larger than 256 MB mapped whole, read-only, through a
 *     duplicate of its handle, as Chromium's lzma_util maps the 728 MB
 *     MSEDGE.7z (base::MemoryMappedFile); the view reads the file's bytes,
 *     and a failed mapping gives a meaningful error, never
 *     ERROR_INVALID_FUNCTION;
 *   - wer.dll's report API (setup.exe delay-loads it): a report is made,
 *     filled in, submitted to nowhere (WerDisabled) and closed, and bad
 *     arguments are refused;
 *   - kernel32's FlsGetValue2 and OOBEComplete, and ntdll's
 *     RtlGetDeviceFamilyInfoEnum (setup.exe stops unless it says "desktop").
 *
 * `wvsetuptest` runs them all; `wvsetuptest small` leaves out the large
 * file (32-bit runs). */
#include <stdio.h>
#include <string.h>
#include <windows.h>

static int pass, fail;
#define CHECK(what, cond) do { if (cond) pass++; else { fail++; printf("FAIL: %s (line %d, error %lu)\n", what, __LINE__, GetLastError()); } } while (0)

#define BIG_MB 300
#define BIG_PATH "C:\\Temp\\wvsetuptest.7z"

static void big_mapping(void)
{
    CreateDirectoryA("C:\\Temp", NULL);
    HANDLE f = CreateFileA(BIG_PATH, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK("CreateFile", f != INVALID_HANDLE_VALUE);
    if (f == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size, at;
    size.QuadPart = (LONGLONG)BIG_MB << 20;
    DWORD n = 0;
    CHECK("write the head", WriteFile(f, "7z\xBC\xAF\x27\x1C", 6, &n, NULL) && n == 6);
    at.QuadPart = size.QuadPart - 4;
    CHECK("seek near the end", SetFilePointerEx(f, at, NULL, FILE_BEGIN));
    CHECK("write the tail", WriteFile(f, "TAIL", 4, &n, NULL) && n == 4);
    LARGE_INTEGER got;
    CHECK("the file's size", GetFileSizeEx(f, &got) && got.QuadPart == size.QuadPart);

    /* base::File::Duplicate, then base::MemoryMappedFile::Initialize (READ_ONLY, whole file) */
    HANDLE dup = NULL;
    CHECK("DuplicateHandle", DuplicateHandle(GetCurrentProcess(), f, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS));
    HANDLE m = CreateFileMappingW(dup, NULL, PAGE_READONLY, 0, 0, NULL);
    CHECK("CreateFileMapping of a 300 MB file", m != NULL);
    const unsigned char *p = m ? MapViewOfFile(m, FILE_MAP_READ, 0, 0, (SIZE_T)size.QuadPart) : NULL;
    CHECK("MapViewOfFile of all of it", p != NULL);
    if (p) {
        CHECK("the view starts with the file's head", memcmp(p, "7z\xBC\xAF\x27\x1C", 6) == 0);
        CHECK("the view ends with the file's tail", memcmp(p + size.QuadPart - 4, "TAIL", 4) == 0);
        CHECK("the hole between reads as zeros", p[size.QuadPart / 2] == 0 && p[4096] == 0);
        MEMORY_BASIC_INFORMATION mbi;
        CHECK("VirtualQuery: a mapped view", VirtualQuery(p, &mbi, sizeof(mbi)) && mbi.Type == MEM_MAPPED);
        CHECK("UnmapViewOfFile", UnmapViewOfFile(p));
    }
    if (m) CloseHandle(m);

    /* A read-only mapping larger than the file cannot extend it */
    SetLastError(0);
    m = CreateFileMappingW(dup, NULL, PAGE_READONLY, 0, (DWORD)size.QuadPart + 65536, NULL);
    CHECK("a read-only mapping larger than the file fails", m == NULL && GetLastError() != ERROR_INVALID_FUNCTION && GetLastError() != 0);
    if (m) CloseHandle(m);
    CloseHandle(dup);
    CloseHandle(f);
    CHECK("DeleteFile", DeleteFileA(BIG_PATH));
}

static void empty_mapping(void)
{
    HANDLE f = CreateFileA("C:\\Temp\\wvsetuptest.empty", GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_FLAG_DELETE_ON_CLOSE, NULL);
    CHECK("CreateFile (empty)", f != INVALID_HANDLE_VALUE);
    SetLastError(0);
    HANDLE m = CreateFileMappingW(f, NULL, PAGE_READONLY, 0, 0, NULL);
    CHECK("an empty file cannot be mapped: ERROR_FILE_INVALID", m == NULL && GetLastError() == 1006);
    if (m) CloseHandle(m);
    CloseHandle(f);
}

/* ---- wer.dll ---- */
typedef HRESULT (WINAPI *WerCreate)(PCWSTR, int, PVOID, HANDLE *);
typedef HRESULT (WINAPI *WerSetParam)(HANDLE, DWORD, PCWSTR, PCWSTR);
typedef HRESULT (WINAPI *WerAddFile)(HANDLE, PCWSTR, int, DWORD);
typedef HRESULT (WINAPI *WerAddDump)(HANDLE, HANDLE, HANDLE, int, PVOID, PVOID, DWORD);
typedef HRESULT (WINAPI *WerSubmit)(HANDLE, int, DWORD, int *);
typedef HRESULT (WINAPI *WerClose)(HANDLE);

static void wer(void)
{
    HMODULE w = LoadLibraryA("wer.dll");
    CHECK("LoadLibrary(wer.dll)", w != NULL);
    if (!w) return;
    WerCreate create = (WerCreate)GetProcAddress(w, "WerReportCreate");
    WerSetParam set = (WerSetParam)GetProcAddress(w, "WerReportSetParameter");
    WerAddFile add_file = (WerAddFile)GetProcAddress(w, "WerReportAddFile");
    WerAddDump add_dump = (WerAddDump)GetProcAddress(w, "WerReportAddDump");
    WerSubmit submit = (WerSubmit)GetProcAddress(w, "WerReportSubmit");
    WerClose close = (WerClose)GetProcAddress(w, "WerReportCloseHandle");
    CHECK("setup.exe's six imports", create && set && add_file && add_dump && submit && close);
    CHECK("WerReportSetUIOption, WerAddExcludedApplication",
          GetProcAddress(w, "WerReportSetUIOption") && GetProcAddress(w, "WerAddExcludedApplication"));
    if (!(create && set && add_file && add_dump && submit && close)) return;

    HANDLE r = NULL;
    CHECK("WerReportCreate without an event type fails", create(NULL, 2, NULL, &r) == E_INVALIDARG && !r);
    CHECK("WerReportCreate", create(L"BEX64", 2 /* WerReportApplicationCrash */, NULL, &r) == S_OK && r);
    CHECK("WerReportSetParameter P0", set(r, 0, L"AppName", L"setup.exe") == S_OK);
    CHECK("WerReportSetParameter P9", set(r, 9, NULL, L"154.0.4258.53") == S_OK);
    CHECK("WerReportSetParameter P10 is refused", set(r, 10, NULL, L"x") == E_INVALIDARG);
    WCHAR self[MAX_PATH];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    CHECK("WerReportAddFile", add_file(r, self, 5 /* WerFileTypeOther */, 0) == S_OK);
    CHECK("WerReportAddFile of a missing file fails", FAILED(add_file(r, L"C:\\no\\such.file", 5, 0)));
    CHECK("WerReportAddDump", add_dump(r, GetCurrentProcess(), GetCurrentThread(), 2 /* WerDumpTypeMiniDump */, NULL, NULL, 0) == S_OK);
    int result = 0;
    CHECK("WerReportSubmit: reporting is turned off", submit(r, 1 /* WerConsentNotAsked */, 0, &result) == S_OK && result == 5 /* WerDisabled */);
    CHECK("WerReportCloseHandle", close(r) == S_OK);
    CHECK("WerReportCloseHandle(NULL) fails", close(NULL) == E_INVALIDARG);
}

static void fls(void)
{
    typedef PVOID (WINAPI *Get2)(DWORD);
    Get2 get2 = (Get2)GetProcAddress(GetModuleHandleA("kernel32.dll"), "FlsGetValue2");
    CHECK("kernel32!FlsGetValue2", get2 != NULL);
    DWORD i = FlsAlloc(NULL);
    CHECK("FlsAlloc", i != FLS_OUT_OF_INDEXES);
    FlsSetValue(i, (PVOID)0x1234);
    SetLastError(77);
    CHECK("FlsGetValue2 reads the slot", get2 && get2(i) == (PVOID)0x1234);
    CHECK("and leaves the last error alone", GetLastError() == 77);
    FlsFree(i);
}

/* What Chromium's setup asks about the machine before it installs */
static void machine(void)
{
    typedef VOID (WINAPI *Family)(ULONGLONG *, ULONG *, ULONG *);
    Family family = (Family)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetDeviceFamilyInfoEnum");
    CHECK("ntdll!RtlGetDeviceFamilyInfoEnum", family != NULL);
    if (family) {
        ULONGLONG uap = 0;
        ULONG fam = 99, form = 99;
        family(&uap, &fam, &form);
        CHECK("a desktop (DEVICEFAMILYINFOENUM_DESKTOP)", fam == 3);
        CHECK("Windows 10.0.19045", uap >> 48 == 10 && (uap >> 32 & 0xFFFF) == 0 && (uap >> 16 & 0xFFFF) == 19045);
        CHECK("a device form", form != 99);
        family(NULL, &fam, NULL);                   /* (each output is optional) */
    }
    typedef BOOL (WINAPI *Oobe)(BOOL *);
    Oobe oobe = (Oobe)GetProcAddress(GetModuleHandleA("kernel32.dll"), "OOBEComplete");
    BOOL done = FALSE;
    CHECK("kernel32!OOBEComplete: first-start setup is done", oobe && oobe(&done) && done);
}

int main(int argc, char **argv)
{
    if (!(argc > 1 && !strcmp(argv[1], "small"))) big_mapping();
    empty_mapping();
    wer();
    fls();
    machine();
    printf("wvsetuptest: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
