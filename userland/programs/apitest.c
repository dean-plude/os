/* apitest.exe — the standard DLLs: kernel32 extras, advapi32, bcrypt,
 * shlwapi, shell32, psapi, version, user32/gdi32 helpers */
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>
#include <winternl.h>

static int pass, fail;
#define CHECK(what, cond) do { if (cond) pass++; else { fail++; printf("FAIL: %s (line %d, error %lu)\n", what, __LINE__, GetLastError()); } } while (0)

/* imports by name, so this program also shows GetProcAddress across DLLs */
static FARPROC fn(const char *dll, const char *name)
{
    HMODULE m = LoadLibraryA(dll);
    return m ? GetProcAddress(m, name) : 0;
}

static void hex(const BYTE *b, int n, char *out) { for (int i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", b[i]); }

/* fibers: two switches back and forth, with data on each stack */
typedef VOID (WINAPI *fiber_fn)(LPVOID);
static VOID (WINAPI *g_switch)(LPVOID);
static LPVOID g_main_fiber;
static int g_fiber_steps;
static VOID WINAPI fiber_proc(LPVOID p)
{
    volatile double x = 1.5;
    g_fiber_steps += (int)(INT_PTR)p;
    g_switch(g_main_fiber);
    g_fiber_steps += x == 1.5 ? 10 : 1000;
    g_switch(g_main_fiber);
}

static volatile LONG g_flag;
static DWORD WINAPI waker(LPVOID p) { (void)p; Sleep(50); g_flag = 1; WakeByAddressAll((PVOID)&g_flag); return 0; }

int main(int argc, char **argv)
{
    char buf[512];
    WCHAR w[MAX_PATH];

    if (argc > 1 && !strcmp(argv[1], "child")) return 42;   /* for the CreateProcess test */

    /* ---- bcrypt: SHA-256 and HMAC ---- */
    NTSTATUS (WINAPI *hash)(void *, PUCHAR, ULONG, PUCHAR, ULONG, PUCHAR, ULONG) = (void *)fn("bcrypt.dll", "BCryptHash");
    BYTE d[32];
    CHECK("BCryptHash", hash && !hash((void *)0x41 /* BCRYPT_SHA256_ALG_HANDLE */, 0, 0, (PUCHAR)"abc", 3, d, 32));
    hex(d, 32, buf);
    CHECK("sha256(abc)", !strcmp(buf, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK("HMAC", hash && !hash((void *)0xB1, (PUCHAR)"key", 3, (PUCHAR)"The quick brown fox jumps over the lazy dog", 43, d, 32));
    hex(d, 32, buf);
    CHECK("hmac-sha256", !strcmp(buf, "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8"));

    /* ---- advapi32 ---- */
    HANDLE tok;
    CHECK("OpenProcessToken", OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok));
    BYTE tu[128];
    DWORD n;
    CHECK("GetTokenInformation", GetTokenInformation(tok, TokenUser, tu, sizeof(tu), &n));
    LPSTR sid;
    CHECK("ConvertSidToStringSid", ConvertSidToStringSidA(((TOKEN_USER *)tu)->User.Sid, &sid) && !strncmp(sid, "S-1-5-21-", 9));
    CloseHandle(tok);
    DWORD un = sizeof(buf);
    CHECK("GetUserName", GetUserNameA(buf, &un) && un > 1);
    BYTE r1[16] = { 0 }, r2[16] = { 0 };
    CHECK("RtlGenRandom", SystemFunction036(r1, 16) && SystemFunction036(r2, 16) && memcmp(r1, r2, 16));

    /* ---- shlwapi ---- */
    BOOL (WINAPI *combine)(LPSTR, LPCSTR, LPCSTR) = (void *)fn("shlwapi.dll", "PathCombineA");
    BOOL (WINAPI *relative)(LPCSTR) = (void *)fn("shlwapi.dll", "PathIsRelativeA");
    LPSTR (WINAPI *ext)(LPCSTR) = (void *)fn("shlwapi.dll", "PathFindExtensionA");
    int (WINAPI *logical)(LPCWSTR, LPCWSTR) = (void *)fn("shlwapi.dll", "StrCmpLogicalW");
    CHECK("PathCombine", combine && combine(buf, "C:\\a\\b", "..\\c.txt") && !strcmp(buf, "C:\\a\\c.txt"));
    CHECK("PathIsRelative", relative && relative("x\\y") && !relative("C:\\x"));
    CHECK("PathFindExtension", ext && !strcmp(ext("C:\\dir.v1\\file.tar.gz"), ".gz"));
    CHECK("StrCmpLogical", logical && logical(L"file2", L"file10") < 0);

    /* ---- shell32 ---- */
    HRESULT (WINAPI *known)(const GUID *, DWORD, HANDLE, LPWSTR *) = (void *)fn("shell32.dll", "SHGetKnownFolderPath");
    LPWSTR *(WINAPI *argvw)(LPCWSTR, int *) = (void *)fn("shell32.dll", "CommandLineToArgvW");
    static const GUID docs = { 0xFDD39AD0, 0x238F, 0x46AF, { 0xAD, 0xB4, 0x6C, 0x85, 0x48, 0x03, 0x69, 0xC7 } };
    LPWSTR path = 0;
    CHECK("SHGetKnownFolderPath", known && !known(&docs, 0, 0, &path) && path && !wcscmp(path, L"C:\\Documents"));
    int ac = 0;
    LPWSTR *av = argvw ? argvw(L"prog \"a b\" c\\\"d", &ac) : 0;
    CHECK("CommandLineToArgvW", av && ac == 3 && !wcscmp(av[1], L"a b") && !wcscmp(av[2], L"c\"d"));

    /* ---- psapi and version ---- */
    BOOL (WINAPI *enum_mods)(HANDLE, HMODULE *, DWORD, LPDWORD) = (void *)fn("psapi.dll", "EnumProcessModules");
    HMODULE mods[64];
    CHECK("EnumProcessModules", enum_mods && enum_mods(GetCurrentProcess(), mods, sizeof(mods), &n) && n >= 4 * sizeof(HMODULE));
    CHECK("GetModuleFileName(DLL)", GetModuleFileNameA(GetModuleHandleA("kernel32.dll"), buf, sizeof(buf)) &&
                                    !strcmp(buf, sizeof(void *) == 8 ? "C:\\Windows\\System32\\kernel32.dll"
                                                                           : "C:\\Windows\\SysWOW64\\kernel32.dll"));
    DWORD (WINAPI *vsize)(LPCSTR, LPDWORD) = (void *)fn("version.dll", "GetFileVersionInfoSizeA");
    CHECK("no version resource", vsize && !vsize("C:\\Windows\\System32\\kernel32.dll", &n));

    /* ---- kernel32: file mapping, rename, attributes ---- */
    HANDLE f = CreateFileA("C:\\Temp\\map.bin", GENERIC_READ | GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
    if (f == INVALID_HANDLE_VALUE) { CreateDirectoryA("C:\\Temp", 0); f = CreateFileA("C:\\Temp\\map.bin", GENERIC_READ | GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0); }
    DWORD wr;
    WriteFile(f, "hello mapping", 13, &wr, 0);
    HANDLE map = CreateFileMappingA(f, 0, PAGE_READWRITE, 0, 0, 0);
    char *v = map ? MapViewOfFile(map, FILE_MAP_WRITE, 0, 0, 0) : 0;
    CHECK("MapViewOfFile", v && !memcmp(v, "hello mapping", 13));
    if (v) { v[0] = 'J'; UnmapViewOfFile(v); }
    CloseHandle(map);
    SetFilePointer(f, 0, 0, FILE_BEGIN);
    char rb[16] = { 0 };
    ReadFile(f, rb, 13, &wr, 0);
    CHECK("mapping written back", !memcmp(rb, "Jello mapping", 13));
    CloseHandle(f);
    DeleteFileA("C:\\Temp\\map2.bin");
    CHECK("MoveFileEx", MoveFileExA("C:\\Temp\\map.bin", "C:\\Temp\\map2.bin", 0) &&
                        GetFileAttributesA("C:\\Temp\\map.bin") == INVALID_FILE_ATTRIBUTES &&
                        GetFileAttributesA("C:\\Temp\\map2.bin") != INVALID_FILE_ATTRIBUTES);
    WIN32_FILE_ATTRIBUTE_DATA fad;
    CHECK("GetFileAttributesEx", GetFileAttributesExA("C:\\Temp\\map2.bin", GetFileExInfoStandard, &fad) && fad.nFileSizeLow == 13);
    DeleteFileA("C:\\Temp\\map2.bin");

    /* ---- completion ports, WaitOnAddress, timers ---- */
    HANDLE port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, 0, 0, 1);
    PostQueuedCompletionStatus(port, 7, 99, 0);
    ULONG_PTR key = 0;
    LPOVERLAPPED ov;
    CHECK("IOCP", GetQueuedCompletionStatus(port, &n, &key, &ov, 1000) && n == 7 && key == 99);
    CHECK("IOCP timeout", !GetQueuedCompletionStatus(port, &n, &key, &ov, 10) && GetLastError() == WAIT_TIMEOUT);
    CloseHandle(port);
    LONG zero = 0;
    HANDLE t = CreateThread(0, 0, waker, 0, 0, 0);
    CHECK("WaitOnAddress", WaitOnAddress(&g_flag, &zero, sizeof(zero), 2000) && g_flag == 1);
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    HANDLE timer = CreateWaitableTimerW(0, TRUE, 0);
    LARGE_INTEGER due;
    due.QuadPart = -300000;                                 /* 30 ms */
    ULONGLONG t0 = GetTickCount64();
    CHECK("waitable timer", SetWaitableTimer(timer, &due, 0, 0, 0, FALSE) && WaitForSingleObject(timer, 2000) == WAIT_OBJECT_0 &&
                            GetTickCount64() - t0 >= 20);
    CloseHandle(timer);

    /* ---- processes ---- */
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    char cmd[] = "apitest.exe child";
    DWORD code = 0;
    CHECK("CreateProcess", CreateProcessA(0, cmd, 0, 0, FALSE, 0, 0, 0, &si, &pi) &&
                           WaitForSingleObject(pi.hProcess, 5000) == WAIT_OBJECT_0 &&
                           GetExitCodeProcess(pi.hProcess, &code) && code == 42);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    /* ---- messages and NLS ---- */
    CHECK("FormatMessage", FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, 0, 2, 0, buf, sizeof(buf), 0) &&
                           !strncmp(buf, "The system cannot find the file specified.", 42));
    DWORD_PTR args[] = { (DWORD_PTR)"world", 7 };
    CHECK("FormatMessage inserts", FormatMessageA(FORMAT_MESSAGE_FROM_STRING | FORMAT_MESSAGE_ARGUMENT_ARRAY, "hello %1 %2!d!", 0, 0,
                                                  buf, sizeof(buf), (va_list *)args) && !strcmp(buf, "hello world 7"));
    CHECK("CompareStringOrdinal", CompareStringOrdinal(L"Abc", -1, L"aBC", -1, TRUE) == CSTR_EQUAL);
    CHECK("LCMapString", LCMapStringW(0x409, LCMAP_UPPERCASE, L"straße", -1, w, MAX_PATH) && w[0] == 'S' && w[4] == 0xDF);

    /* ---- user32/gdi32 without a window ---- */
    RECT a = { 0, 0, 10, 10 }, b = { 5, 5, 20, 20 }, c;
    CHECK("IntersectRect", IntersectRect(&c, &a, &b) && c.left == 5 && c.right == 10);
    sprintf(buf, "%d", 0);
    wsprintfA(buf, "%s-%d", "x", 5);
    CHECK("wsprintf", !strcmp(buf, "x-5"));
    HDC mem = CreateCompatibleDC(0);
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = 8; bi.bmiHeader.biHeight = -8; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    DWORD *px = 0;
    HBITMAP dib = CreateDIBSection(mem, &bi, 0, (void **)&px, 0, 0);
    SelectObject(mem, dib);
    SetPixel(mem, 2, 3, RGB(255, 0, 0));
    CHECK("DIB section BGRA", px && (px[3 * 8 + 2] & 0xFFFFFF) == 0xFF0000 && GetPixel(mem, 2, 3) == RGB(255, 0, 0));
    DeleteDC(mem);
    DeleteObject(dib);

    /* ---- registry ---- */
    HKEY rk, rk2;
    DWORD disp = 0, type = 0, dv = 0;
    n = sizeof(buf);
    CHECK("default key", !RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "ProductName",
                                       RRF_RT_REG_SZ, &type, buf, &n) && type == REG_SZ && n > 1);
    RegDeleteTreeA(HKEY_CURRENT_USER, "Software\\NovaTest");
    CHECK("RegCreateKeyEx", !RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\NovaTest\\Sub", 0, 0, 0, KEY_ALL_ACCESS, 0, &rk, &disp) &&
                            disp == REG_CREATED_NEW_KEY);
    dv = 0x12345678;
    CHECK("RegSetValueEx", !RegSetValueExA(rk, "Num", 0, REG_DWORD, (BYTE *)&dv, 4) &&
                           !RegSetValueExW(rk, L"Str", 0, REG_SZ, (const BYTE *)L"héllo", 12));
    dv = 0; n = 4;
    CHECK("RegQueryValueEx", !RegQueryValueExA(rk, "num", 0, &type, (BYTE *)&dv, &n) && type == REG_DWORD && dv == 0x12345678);
    n = sizeof(buf);
    CHECK("RegQueryValueExA utf-8", !RegQueryValueExA(rk, "Str", 0, &type, (BYTE *)buf, &n) && !strcmp(buf, "h\xc3\xa9llo"));
    n = 2;
    CHECK("ERROR_MORE_DATA", RegQueryValueExW(rk, L"Str", 0, 0, (BYTE *)w, &n) == ERROR_MORE_DATA && n == 12);
    RegCloseKey(rk);
    CHECK("RegOpenKeyEx", !RegOpenKeyExA(HKEY_CURRENT_USER, "SOFTWARE\\novatest", 0, KEY_READ, &rk2));
    n = sizeof(buf);
    CHECK("RegEnumKeyEx", !RegEnumKeyExA(rk2, 0, buf, &n, 0, 0, 0, 0) && !strcmp(buf, "Sub") &&
                          RegEnumKeyExA(rk2, 1, buf, &n, 0, 0, 0, 0) == ERROR_NO_MORE_ITEMS);
    DWORD nsub = 0, nval = 9;
    CHECK("RegQueryInfoKey", !RegQueryInfoKeyA(rk2, 0, 0, 0, &nsub, 0, 0, &nval, 0, 0, 0, 0) && nsub == 1 && nval == 0);
    RegCloseKey(rk2);
    typedef DWORD (WINAPI *shget_t)(HKEY, LPCSTR, LPCSTR, LPDWORD, LPVOID, LPDWORD);
    shget_t shget = (shget_t)fn("shlwapi.dll", "SHGetValueA");
    dv = 0; n = 4;
    CHECK("SHGetValue", shget && !shget(HKEY_CURRENT_USER, "Software\\NovaTest\\Sub", "Num", &type, &dv, &n) && dv == 0x12345678);
    typedef LSTATUS (WINAPI *regopen_t)(HKEY, LPCSTR, DWORD, REGSAM, PHKEY);
    regopen_t adv_open = (regopen_t)fn("advapi32.dll", "RegOpenKeyExA");
    CHECK("advapi32 forwarder", adv_open && !adv_open(HKEY_CURRENT_USER, "Software\\NovaTest\\Sub", 0, KEY_READ, &rk) && !RegCloseKey(rk));
    CHECK("RegDeleteTree", !RegDeleteTreeA(HKEY_CURRENT_USER, "Software\\NovaTest") &&
                           RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\NovaTest", 0, KEY_READ, &rk) == ERROR_FILE_NOT_FOUND);

    /* ---- fibers ---- */
    LPVOID (WINAPI *conv)(LPVOID) = (void *)fn("kernel32.dll", "ConvertThreadToFiber");
    LPVOID (WINAPI *mk)(SIZE_T, fiber_fn, LPVOID) = (void *)fn("kernel32.dll", "CreateFiber");
    BOOL (WINAPI *isf)(void) = (void *)fn("kernel32.dll", "IsThreadAFiber");
    VOID (WINAPI *del)(LPVOID) = (void *)fn("kernel32.dll", "DeleteFiber");
    g_switch = (void *)fn("kernel32.dll", "SwitchToFiber");
    CHECK("fiber APIs", conv && mk && isf && del && g_switch && !isf());
    if (conv && mk && isf && del && g_switch) {
        g_main_fiber = conv((LPVOID)7);
        LPVOID f = mk(64 * 1024, fiber_proc, (LPVOID)3);
        volatile int local = 99;
        g_switch(f);
        CHECK("SwitchToFiber", g_fiber_steps == 3 && local == 99 && isf());
        g_switch(f);
        CHECK("fiber resumes", g_fiber_steps == 13 && local == 99);
        CHECK("GetFiberData", *(LPVOID *)g_main_fiber == (LPVOID)7);
        del(f);
    }

    CHECK("deliberate failure to prove the CI gate (revert me)", 0);
    printf("apitest: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}
