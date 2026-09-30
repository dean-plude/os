/*
 * cliptest.exe — the system clipboard across programs
 *
 *   cliptest            run the tests (starts itself to read back)
 *   cliptest read FMT   (child) print the clipboard's text / a format's size
 */
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static int g_pass, g_fail;
static char g_self[MAX_PATH];

static void check(int ok, const char *what)
{
    if (ok) g_pass++;
    else { g_fail++; printf("FAIL: %s (error %lu)\n", what, GetLastError()); }
}

static HGLOBAL global_copy(const void *p, SIZE_T n)
{
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n);
    memcpy(GlobalLock(g), p, n);
    GlobalUnlock(g);
    return g;
}

/* Run "cliptest read @what" and return its output */
static void child(const char *what, char *out, int cap)
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa), 0, TRUE };
    HANDLE r, w;
    CreatePipe(&r, &w, &sa, 0);
    SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0);
    char cl[MAX_PATH + 64];
    snprintf(cl, sizeof(cl), "\"%s\" read %s", g_self, what);
    STARTUPINFOA si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = w;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi;
    out[0] = 0;
    if (!CreateProcessA(0, cl, 0, 0, TRUE, 0, 0, 0, &si, &pi)) { CloseHandle(r); CloseHandle(w); return; }
    CloseHandle(w);
    int n = 0;
    DWORD got;
    while (n < cap - 1 && ReadFile(r, out + n, (DWORD)(cap - 1 - n), &got, 0) && got) n += (int)got;
    out[n] = 0;
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(r);
}

static int read_mode(const char *what)
{
    if (!OpenClipboard(0)) return 1;
    if (!strcmp(what, "text")) {                       /* CF_TEXT, made from Unicode text */
        HANDLE h = GetClipboardData(CF_TEXT);
        if (h) { printf("%s", (char *)GlobalLock(h)); GlobalUnlock(h); }
    } else if (!strcmp(what, "wide")) {
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        if (h) { const WCHAR *w = GlobalLock(h); char a[256]; WideCharToMultiByte(CP_UTF8, 0, w, -1, a, 256, 0, 0); printf("%s", a); }
    } else if (!strcmp(what, "custom")) {
        UINT f = RegisterClipboardFormatA("NovaTest Private");
        HANDLE h = GetClipboardData(f);
        if (h) printf("%s", (char *)GlobalLock(h));
    } else if (!strcmp(what, "hdrop")) {
        HANDLE h = GetClipboardData(CF_HDROP);
        char p[MAX_PATH];
        if (h && DragQueryFileA((HDROP)h, 0, p, MAX_PATH)) printf("%u:%s", DragQueryFileA((HDROP)h, 0xFFFFFFFF, 0, 0), p);
    } else if (!strcmp(what, "dib")) {
        HANDLE h = GetClipboardData(CF_DIB);
        if (h) { BITMAPINFOHEADER *bi = GlobalLock(h); printf("%ldx%ld", bi->biWidth, bi->biHeight); }
    }
    CloseClipboard();
    return 0;
}

static void text_tests(void)
{
    static const WCHAR hello[] = L"h\x00e9llo clipboard";
    check(OpenClipboard(0), "OpenClipboard");
    check(EmptyClipboard(), "EmptyClipboard");
    DWORD seq = GetClipboardSequenceNumber();
    check(SetClipboardData(CF_UNICODETEXT, global_copy(hello, sizeof(hello))) != 0, "SetClipboardData(CF_UNICODETEXT)");
    check(GetClipboardSequenceNumber() != seq, "the sequence number moved");
    CloseClipboard();
    check(IsClipboardFormatAvailable(CF_TEXT), "CF_TEXT is available (converted)");
    int found = 0;
    OpenClipboard(0);
    for (UINT f = EnumClipboardFormats(0); f; f = EnumClipboardFormats(f)) if (f == CF_UNICODETEXT) found++;
    CloseClipboard();
    check(found == 1, "EnumClipboardFormats lists CF_UNICODETEXT");
    char out[512];
    child("text", out, sizeof(out));
    check(!strcmp(out, "h\xc3\xa9llo clipboard"), "another program reads it as CF_TEXT");

    /* the other way: a child sets it? (set here as CF_TEXT, read back wide) */
    OpenClipboard(0);
    EmptyClipboard();
    SetClipboardData(CF_TEXT, global_copy("narrow text", 12));
    CloseClipboard();
    child("wide", out, sizeof(out));
    check(!strcmp(out, "narrow text"), "CF_TEXT read as CF_UNICODETEXT elsewhere");
    OpenClipboard(0);
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    check(h && !wcscmp(GlobalLock(h), L"narrow text"), "and here");
    CloseClipboard();
}

static void format_tests(void)
{
    UINT f = RegisterClipboardFormatA("NovaTest Private");
    OpenClipboard(0);
    EmptyClipboard();
    SetClipboardData(f, global_copy("private-data", 13));
    SetClipboardData(CF_TEXT, global_copy("also text", 10));
    CloseClipboard();
    char out[512];
    child("custom", out, sizeof(out));
    check(!strcmp(out, "private-data"), "a registered format, by name, in another program");
    check(CountClipboardFormats() >= 2, "CountClipboardFormats");

    /* files */
    const char path[] = "C:\\Windows\\win.ini";
    SIZE_T n = sizeof(DROPFILES) + sizeof(path) + 1;
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, n);
    DROPFILES *d = GlobalLock(g);
    d->pFiles = sizeof(DROPFILES);
    memcpy((char *)d + sizeof(DROPFILES), path, sizeof(path));
    GlobalUnlock(g);
    OpenClipboard(0);
    EmptyClipboard();
    SetClipboardData(CF_HDROP, g);
    CloseClipboard();
    child("hdrop", out, sizeof(out));
    check(!strcmp(out, "1:C:\\Windows\\win.ini"), "CF_HDROP in another program (DragQueryFile)");

    /* a bitmap travels as a DIB */
    HDC dc = GetDC(0);
    HBITMAP bmp = CreateCompatibleBitmap(dc, 3, 2);
    ReleaseDC(0, dc);
    OpenClipboard(0);
    EmptyClipboard();
    check(SetClipboardData(CF_BITMAP, bmp) != 0, "SetClipboardData(CF_BITMAP)");
    CloseClipboard();
    check(IsClipboardFormatAvailable(CF_DIB) && IsClipboardFormatAvailable(CF_BITMAP), "CF_DIB and CF_BITMAP available");
    child("dib", out, sizeof(out));
    check(!strcmp(out, "3x2"), "another program reads the bitmap as a 3x2 DIB");
    OpenClipboard(0);
    HBITMAP back = GetClipboardData(CF_BITMAP);
    BITMAP bm = { 0 };
    check(back && GetObjectA(back, sizeof(bm), &bm) && bm.bmWidth == 3 && bm.bmHeight == 2, "CF_BITMAP made back from the DIB");
    CloseClipboard();
}

static void ole_tests(void)
{
    OleInitialize(0);
    OpenClipboard(0);
    EmptyClipboard();
    SetClipboardData(CF_TEXT, global_copy("for ole", 8));
    CloseClipboard();
    IDataObject *obj = 0;
    check(SUCCEEDED(OleGetClipboard(&obj)) && obj, "OleGetClipboard");
    FORMATETC fe = { CF_UNICODETEXT, 0, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM m = { 0 };
    check(obj->lpVtbl->QueryGetData(obj, &fe) == S_OK, "QueryGetData(CF_UNICODETEXT)");
    check(SUCCEEDED(obj->lpVtbl->GetData(obj, &fe, &m)) && !wcscmp(GlobalLock(m.hGlobal), L"for ole"), "GetData");
    ReleaseStgMedium(&m);
    IEnumFORMATETC *en = 0;
    int n = 0;
    FORMATETC f;
    ULONG got;
    if (SUCCEEDED(obj->lpVtbl->EnumFormatEtc(obj, DATADIR_GET, &en)) && en) {
        while (en->lpVtbl->Next(en, 1, &f, &got) == S_OK) n++;
        en->lpVtbl->Release(en);
    }
    check(n >= 2, "EnumFormatEtc lists the formats");
    /* OleSetClipboard with the clipboard's own data object: copies it back */
    check(SUCCEEDED(OleSetClipboard(obj)), "OleSetClipboard");
    check(OleIsCurrentClipboard(obj) == S_OK, "OleIsCurrentClipboard");
    char out[256];
    child("wide", out, sizeof(out));
    check(!strcmp(out, "for ole"), "OleSetClipboard's data in another program");
    obj->lpVtbl->Release(obj);
    OleFlushClipboard();
    OleUninitialize();
}

int main(int argc, char **argv)
{
    GetModuleFileNameA(0, g_self, MAX_PATH);
    if (argc > 2 && !strcmp(argv[1], "read")) return read_mode(argv[2]);
    text_tests();
    format_tests();
    ole_tests();
    printf("cliptest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail != 0;
}
