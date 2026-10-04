/* setuptest.exe — what game and application installers need (Inno Setup's,
 * GOG's, NSIS's), 64- and 32-bit.
 *
 * Elevation: the desktop user's token is the limited half of an
 * administrator's (TokenElevationType limited, IsUserAnAdmin false);
 * ShellExecuteEx's "runas" starts a program elevated (TokenElevation,
 * Administrators enabled, high integrity), a program whose manifest asks
 * for administrator starts elevated (setupadmin.exe), and a plain
 * CreateProcess does not.
 *
 * Rich Edit: msftedit.dll's RICHEDIT50W and riched20.dll's RichEdit20W
 * and RichEdit20A take RTF by EM_STREAMIN, EM_SETTEXTEX and WM_SETTEXT
 * (paragraphs, tabs, \'hh, \u, skipped tables and {\*...} groups), and
 * answer EM_GETTEXTEX, EM_GETTEXTLENGTHEX, EM_EXSETSEL/EM_EXGETSEL,
 * EM_GETTEXTRANGE, EM_FINDTEXTEXW and EM_STREAMOUT.
 *
 * Variants: oleaut32's VarAdd, VarSub, VarMul, VarDiv, VarIdiv, VarMod,
 * VarAnd, VarOr, VarXor, VarNeg, VarNot, VarCat and VarCmp (Delphi's
 * variant operators call them).
 *
 * "setuptest bigfile" writes a 300 MB file on C: (installers' data files
 * were cut off at 256 MB), reads its end back and deletes it. */
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>
#include <oleauto.h>

static int pass, fail;
#define CHECK(what, cond) do { if (cond) pass++; else { fail++; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

SHSTDAPI_(BOOL) IsUserAnAdmin(void);
typedef struct {
    DWORD cbSize; ULONG fMask; HWND hwnd; LPCWSTR lpVerb, lpFile, lpParameters, lpDirectory; int nShow;
    HINSTANCE hInstApp; void *lpIDList; LPCWSTR lpClass; HANDLE hkeyClass; DWORD dwHotKey;
    HANDLE hIcon; HANDLE hProcess;
} SEI;
SHSTDAPI_(BOOL) ShellExecuteExW(SEI *info);

/* ---- elevation ------------------------------------------------------ */
static DWORD token_dword(TOKEN_INFORMATION_CLASS c)
{
    HANDLE t;
    DWORD v = 0xFFFF, n;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) return v;
    if (!GetTokenInformation(t, c, &v, sizeof(v), &n)) v = 0xFFFF;
    CloseHandle(t);
    return v;
}

static DWORD integrity(void)
{
    HANDLE t;
    BYTE buf[64];
    DWORD n;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) return 0;
    BOOL ok = GetTokenInformation(t, TokenIntegrityLevel, buf, sizeof(buf), &n);
    CloseHandle(t);
    if (!ok) return 0;
    PSID sid = *(PSID *)buf;                        /* TOKEN_MANDATORY_LABEL: Label.Sid first */
    return *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
}

static BOOL in_admins(void)
{
    SID_IDENTIFIER_AUTHORITY nt = { SECURITY_NT_AUTHORITY };
    PSID sid;
    BOOL m = FALSE;
    if (!AllocateAndInitializeSid(&nt, 2, 32, 544, 0, 0, 0, 0, 0, 0, &sid)) return FALSE;   /* S-1-5-32-544 */
    BOOL ok = CheckTokenMembership(NULL, sid, &m);
    FreeSid(sid);
    return ok && m;
}

/* The child's side: 0 when elevated as @want says */
static int child(int want)
{
    int elevated = token_dword(TokenElevation) == 1 && token_dword(TokenElevationType) == 2 && IsUserAnAdmin() &&
                   in_admins() && integrity() == 0x3000;
    int limited = token_dword(TokenElevation) == 0 && token_dword(TokenElevationType) == 3 && !IsUserAnAdmin() &&
                  !in_admins() && integrity() == 0x2000;
    return want ? !elevated : !limited;
}

static DWORD wait_exit(HANDLE p)
{
    DWORD code = 99;
    if (WaitForSingleObject(p, 60000) == WAIT_OBJECT_0) GetExitCodeProcess(p, &code);
    CloseHandle(p);
    return code;
}

static DWORD run(const WCHAR *cmd)
{
    WCHAR line[MAX_PATH * 2];
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    lstrcpynW(line, cmd, MAX_PATH * 2);
    if (!CreateProcessW(NULL, line, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return 98;
    CloseHandle(pi.hThread);
    return wait_exit(pi.hProcess);
}

static void elevation(void)
{
    WCHAR self[MAX_PATH], dir[MAX_PATH], cmd[MAX_PATH * 2];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    lstrcpyW(dir, self);
    *wcsrchr(dir, L'\\') = 0;
    CHECK("not elevated: TokenElevationType limited", token_dword(TokenElevationType) == 3);
    CHECK("not elevated: TokenElevation 0", token_dword(TokenElevation) == 0);
    CHECK("not elevated: IsUserAnAdmin", !IsUserAnAdmin());
    CHECK("not elevated: Administrators deny-only", !in_admins());
    CHECK("not elevated: medium integrity", integrity() == 0x2000);

    SEI sei;
    memset(&sei, 0, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask = 0x40;                               /* SEE_MASK_NOCLOSEPROCESS */
    sei.lpVerb = L"runas";
    sei.lpFile = self;
    sei.lpParameters = L"child elevated";
    BOOL ok = ShellExecuteExW(&sei);
    CHECK("ShellExecuteEx runas", ok && sei.hProcess);
    if (ok && sei.hProcess) CHECK("runas: the program is elevated", wait_exit(sei.hProcess) == 0);

    swprintf(cmd, MAX_PATH * 2, L"\"%ls\" child limited", self);
    CHECK("CreateProcess: not elevated", run(cmd) == 0);
    swprintf(cmd, MAX_PATH * 2, L"\"%ls\\setupadmin.exe\"", dir);
    CHECK("requireAdministrator in the manifest: elevated", run(cmd) == 0);
}

/* ---- Rich Edit ------------------------------------------------------ */
#define EM_EXGETSEL (WM_USER + 52)
#define EM_EXSETSEL (WM_USER + 55)
#define EM_STREAMIN (WM_USER + 73)
#define EM_STREAMOUT (WM_USER + 74)
#define EM_GETTEXTRANGE (WM_USER + 75)
#define EM_FINDTEXTEXW (WM_USER + 124)
#define EM_FINDTEXTEX (WM_USER + 79)
#define EM_GETTEXTEX (WM_USER + 94)
#define EM_GETTEXTLENGTHEX (WM_USER + 95)
#define EM_SETTEXTEX (WM_USER + 97)
#define EM_SETEVENTMASK (WM_USER + 69)
#define EM_GETEVENTMASK (WM_USER + 59)

#pragma pack(push, 4)                             /* richedit.h packs its structures to 4 bytes (EDITSTREAM's callback is at 12 in 64-bit code) */
typedef DWORD (CALLBACK *ESCB)(DWORD_PTR, LPBYTE, LONG, LONG *);
typedef struct { DWORD_PTR dwCookie; DWORD dwError; ESCB pfnCallback; } EDITSTREAM;
typedef struct { LONG cpMin, cpMax; } CHARRANGE;
typedef struct { CHARRANGE chrg; LPWSTR lpstrText; } TEXTRANGEW;
typedef struct { CHARRANGE chrg; LPCWSTR lpstrText; CHARRANGE chrgText; } FINDTEXTEXW;
typedef struct { DWORD flags; UINT codepage; } SETTEXTEX;
typedef struct { DWORD cb, flags; UINT codepage; LPCSTR lpDefaultChar; LPBOOL lpUsedDefChar; } GETTEXTEX;
typedef struct { DWORD flags; UINT codepage; } GETTEXTLENGTHEX;
#pragma pack(pop)

typedef struct { const char *p; int left; char out[512]; int n; } Buf;

static DWORD CALLBACK reader(DWORD_PTR cookie, LPBYTE b, LONG cb, LONG *got)
{
    Buf *s = (Buf *)cookie;
    LONG n = s->left < cb ? s->left : cb;
    if (n > 7) n = 7;                               /* small pieces: the control must join them */
    memcpy(b, s->p, (size_t)n);
    s->p += n;
    s->left -= n;
    *got = n;
    return 0;
}

static DWORD CALLBACK writer(DWORD_PTR cookie, LPBYTE b, LONG cb, LONG *put)
{
    Buf *s = (Buf *)cookie;
    LONG n = cb < (LONG)sizeof(s->out) - 1 - s->n ? cb : (LONG)sizeof(s->out) - 1 - s->n;
    memcpy(s->out + s->n, b, (size_t)n);
    s->n += n;
    s->out[s->n] = 0;
    *put = n;
    return 0;
}

static const char RTF[] =
    "{\\rtf1\\ansi\\ansicpg1252\\deff0{\\fonttbl{\\f0\\fswiss Arial;}{\\f1 Courier New;}}"
    "{\\colortbl ;\\red255\\green0\\blue0;}{\\*\\generator Msftedit 5.41;}\\viewkind4\\uc1\\pard\\f0\\fs20 "
    "{\\b END USER LICENSE}\\par\n"
    "Caf\\'e9 \\u8364? 5\\tab{\\i done}\\line\n"
    "{\\field{\\*\\fldinst HYPERLINK \"https://example.com\"}{\\fldrslt example.com}}\\par\n"
    "\\{braces\\} and \\\\ backslash\\par\n}";
static const WCHAR TEXT[] = L"END USER LICENSE\r\nCaf\x00e9 \x20ac 5\tdone\r\nexample.com\r\n{braces} and \\ backslash";

static void rich_class(const WCHAR *cls, BOOL ansi)
{
    HWND h = CreateWindowExW(0, cls, L"", WS_CHILD | ES_MULTILINE | ES_READONLY | WS_VSCROLL, 0, 0, 300, 200,
                             HWND_MESSAGE, NULL, NULL, NULL);
    char what[96];
    snprintf(what, sizeof(what), "%ls: created", cls);
    CHECK(what, h != NULL);
    if (!h) return;
    WCHAR got[256];

    Buf in = { RTF, (int)strlen(RTF) };
    EDITSTREAM es = { (DWORD_PTR)&in, 0, reader };
    LRESULT n = SendMessageW(h, EM_STREAMIN, 2 /* SF_RTF */, (LPARAM)&es);
    GetWindowTextW(h, got, 256);
    snprintf(what, sizeof(what), "%ls: EM_STREAMIN RTF", cls);
    CHECK(what, n > 0 && !wcscmp(got, TEXT));
    if (wcscmp(got, TEXT)) printf("  got \"%ls\"\n", got);

    GETTEXTLENGTHEX gl = { 0, 1200 };
    snprintf(what, sizeof(what), "%ls: EM_GETTEXTLENGTHEX (lines as one character)", cls);
    CHECK(what, SendMessageW(h, EM_GETTEXTLENGTHEX, (WPARAM)&gl, 0) == (LRESULT)wcslen(TEXT) - 3);
    GETTEXTEX gt = { sizeof(got), 0, 1200, NULL, NULL };
    SendMessageW(h, EM_GETTEXTEX, (WPARAM)&gt, (LPARAM)got);
    snprintf(what, sizeof(what), "%ls: EM_GETTEXTEX ends lines in \\r", cls);
    CHECK(what, !wcsncmp(got, L"END USER LICENSE\rCaf", 20));

    CHARRANGE cr = { 4, 8 }, back = { 0, 0 };
    SendMessageW(h, EM_EXSETSEL, 0, (LPARAM)&cr);
    SendMessageW(h, EM_EXGETSEL, 0, (LPARAM)&back);
    snprintf(what, sizeof(what), "%ls: EM_EXSETSEL/EM_EXGETSEL", cls);
    CHECK(what, back.cpMin == 4 && back.cpMax == 8);
    char ra[32];
    TEXTRANGEW tr = { { 4, 8 }, ansi ? (LPWSTR)ra : got };
    n = SendMessageW(h, EM_GETTEXTRANGE, 0, (LPARAM)&tr);
    snprintf(what, sizeof(what), "%ls: EM_GETTEXTRANGE", cls);
    CHECK(what, n == 4 && (ansi ? !strcmp(ra, "USER") : !wcscmp(got, L"USER")));

    if (!ansi) {
        FINDTEXTEXW ft = { { 0, -1 }, L"license", { 0, 0 } };
        n = SendMessageW(h, EM_FINDTEXTEXW, 1 /* FR_DOWN */, (LPARAM)&ft);
        snprintf(what, sizeof(what), "%ls: EM_FINDTEXTEXW (any case)", cls);
        CHECK(what, n == 9 && ft.chrgText.cpMin == 9 && ft.chrgText.cpMax == 16);
        n = SendMessageW(h, EM_FINDTEXTEXW, 1 | 4 /* FR_MATCHCASE */, (LPARAM)&ft);
        CHECK("EM_FINDTEXTEXW match case: none", n == -1);
    }

    SETTEXTEX st = { 0, 1200 };
    SendMessageW(h, EM_SETTEXTEX, (WPARAM)&st, (LPARAM)L"{\\rtf1 one\\par two}");
    GetWindowTextW(h, got, 256);
    snprintf(what, sizeof(what), "%ls: EM_SETTEXTEX RTF", cls);
    CHECK(what, !wcscmp(got, L"one\r\ntwo"));
    SetWindowTextW(h, L"{\\rtf1\\ansi plain {\\*\\unknown hidden}text}");
    GetWindowTextW(h, got, 256);
    snprintf(what, sizeof(what), "%ls: WM_SETTEXT RTF", cls);
    CHECK(what, !wcscmp(got, L"plain text"));

    Buf out = { 0 };
    EDITSTREAM eo = { (DWORD_PTR)&out, 0, writer };
    SendMessageW(h, EM_STREAMOUT, 1 /* SF_TEXT */, (LPARAM)&eo);
    snprintf(what, sizeof(what), "%ls: EM_STREAMOUT text", cls);
    CHECK(what, !strcmp(out.out, "plain text"));

    SendMessageW(h, EM_SETEVENTMASK, 0, 0x04000000 /* ENM_LINK */);
    snprintf(what, sizeof(what), "%ls: event mask", cls);
    CHECK(what, SendMessageW(h, EM_GETEVENTMASK, 0, 0) == 0x04000000);
    DestroyWindow(h);
}

static void richedit(void)
{
    CHECK("LoadLibrary msftedit.dll", LoadLibraryW(L"msftedit.dll") != NULL);
    CHECK("LoadLibrary riched20.dll", LoadLibraryW(L"riched20.dll") != NULL);
    rich_class(L"RICHEDIT50W", FALSE);
    rich_class(L"RichEdit20W", FALSE);
    rich_class(L"RichEdit20A", TRUE);
}

/* ---- variants ------------------------------------------------------- */
static VARIANT vi4(LONG v) { VARIANT x; VariantInit(&x); x.vt = VT_I4; x.lVal = v; return x; }
static VARIANT vi2(SHORT v) { VARIANT x; VariantInit(&x); x.vt = VT_I2; x.iVal = v; return x; }
static VARIANT vr8(double v) { VARIANT x; VariantInit(&x); x.vt = VT_R8; x.dblVal = v; return x; }
static VARIANT vbool(BOOL v) { VARIANT x; VariantInit(&x); x.vt = VT_BOOL; x.boolVal = v ? VARIANT_TRUE : VARIANT_FALSE; return x; }
static VARIANT vstr(const WCHAR *s) { VARIANT x; VariantInit(&x); x.vt = VT_BSTR; x.bstrVal = SysAllocString(s); return x; }
static VARIANT vnull(void) { VARIANT x; VariantInit(&x); x.vt = VT_NULL; return x; }

typedef HRESULT (WINAPI *BinOp)(LPVARIANT, LPVARIANT, LPVARIANT);

static void variants(void)
{
    HMODULE oa = LoadLibraryW(L"oleaut32.dll");
    const char *names[] = { "VarAdd", "VarSub", "VarMul", "VarDiv", "VarIdiv", "VarMod", "VarAnd", "VarOr", "VarXor",
                            "VarNeg", "VarNot", "VarCat", "VarCmp" };
    for (int i = 0; i < 13; i++) {
        char what[48];
        snprintf(what, sizeof(what), "oleaut32 exports %s", names[i]);
        CHECK(what, GetProcAddress(oa, names[i]) != NULL);
    }
    VARIANT a, b, r;
    a = vi4(2); b = vi4(3);
    CHECK("VarAdd I4 2 + 3 = I4 5", VarAdd(&a, &b, &r) == S_OK && r.vt == VT_I4 && r.lVal == 5);
    a = vi4(0x7FFFFFFF); b = vi4(1);
    CHECK("VarAdd I4 overflow gives R8", VarAdd(&a, &b, &r) == S_OK && r.vt == VT_R8 && r.dblVal == 2147483648.0);
    a = vi2(200); b = vi2(200);
    CHECK("VarMul I2 200 * 200 = I4 40000", VarMul(&a, &b, &r) == S_OK && r.vt == VT_I4 && r.lVal == 40000);
    a = vstr(L"Gal"); b = vstr(L"axy");
    CHECK("VarAdd strings join", VarAdd(&a, &b, &r) == S_OK && r.vt == VT_BSTR && !wcscmp(r.bstrVal, L"Galaxy"));
    VariantClear(&r); VariantClear(&a); VariantClear(&b);
    a = vstr(L"2"); b = vi2(3);
    CHECK("VarAdd string + I2 = R8 5", VarAdd(&a, &b, &r) == S_OK && r.vt == VT_R8 && r.dblVal == 5.0);
    VariantClear(&a);
    a = vr8(7.5); b = vi4(2);
    CHECK("VarSub R8 7.5 - 2 = 5.5", VarSub(&a, &b, &r) == S_OK && r.vt == VT_R8 && r.dblVal == 5.5);
    a = vi4(7); b = vi4(2);
    CHECK("VarDiv 7 / 2 = R8 3.5", VarDiv(&a, &b, &r) == S_OK && r.vt == VT_R8 && r.dblVal == 3.5);
    CHECK("VarIdiv 7 \\ 2 = 3", VarIdiv(&a, &b, &r) == S_OK && r.vt == VT_I4 && r.lVal == 3);
    a = vi4(-7);
    CHECK("VarMod -7 mod 2 = -1", VarMod(&a, &b, &r) == S_OK && r.vt == VT_I4 && r.lVal == -1);
    b = vi4(0);
    CHECK("VarDiv by zero", VarDiv(&a, &b, &r) == DISP_E_DIVBYZERO);
    a = vbool(TRUE); b = vbool(FALSE);
    CHECK("VarAnd True And False = False", VarAnd(&a, &b, &r) == S_OK && r.vt == VT_BOOL && r.boolVal == VARIANT_FALSE);
    CHECK("VarOr True Or False = True", VarOr(&a, &b, &r) == S_OK && r.vt == VT_BOOL && r.boolVal == VARIANT_TRUE);
    a = vi4(12); b = vi4(10);
    CHECK("VarXor 12 Xor 10 = 6", VarXor(&a, &b, &r) == S_OK && r.vt == VT_I4 && r.lVal == 6);
    a = vnull(); b = vi4(1);
    CHECK("VarAdd Null + 1 = Null", VarAdd(&a, &b, &r) == S_OK && r.vt == VT_NULL);
    b = vbool(FALSE);
    CHECK("VarAnd Null And False = False", VarAnd(&a, &b, &r) == S_OK && r.vt == VT_BOOL && r.boolVal == VARIANT_FALSE);
    a = vi4(5);
    CHECK("VarNeg 5 = -5", VarNeg(&a, &r) == S_OK && r.vt == VT_I4 && r.lVal == -5);
    a = vbool(FALSE);
    CHECK("VarNot False = True", VarNot(&a, &r) == S_OK && r.vt == VT_BOOL && r.boolVal == VARIANT_TRUE);
    a = vi4(42); b = vstr(L"!");
    CHECK("VarCat 42 & \"!\"", VarCat(&a, &b, &r) == S_OK && r.vt == VT_BSTR && !wcscmp(r.bstrVal, L"42!"));
    VariantClear(&r); VariantClear(&b);
    a = vi4(3); b = vr8(3.5);
    CHECK("VarCmp 3 < 3.5", VarCmp(&a, &b, 0x409, 0) == VARCMP_LT);
    a = vstr(L"abc"); b = vstr(L"ABC");
    CHECK("VarCmp strings, ignoring case", VarCmp(&a, &b, 0x409, NORM_IGNORECASE) == VARCMP_EQ);
    CHECK("VarCmp strings, case counts", VarCmp(&a, &b, 0x409, 0) != VARCMP_EQ);
    VariantClear(&a); VariantClear(&b);
    a = vnull(); b = vi4(0);
    CHECK("VarCmp with Null", VarCmp(&a, &b, 0x409, 0) == VARCMP_NULL);
    VARIANT d; VariantInit(&d); d.vt = VT_DATE; d.date = 45000.0;
    b = vi4(1);
    CHECK("VarAdd date + 1 = the next day", VarAdd(&d, &b, &r) == S_OK && r.vt == VT_DATE && r.date == 45001.0);
}

/* ---- fonts a setup program registers -------------------------------- */
int WINAPI AddFontResourceW(LPCWSTR file);
int WINAPI AddFontResourceA(LPCSTR file);

static void fonts(void)
{
    CHECK("AddFontResourceW of a font in the Fonts folder by its file name", AddFontResourceW(L"inter.ttf") > 0);
    CHECK("AddFontResourceA of one by its file name", AddFontResourceA("dejavumono.ttf") > 0);
    CHECK("AddFontResourceW of a full path", AddFontResourceW(L"C:\\Windows\\Fonts\\inter.ttf") > 0);
    CHECK("AddFontResourceW of a missing font fails", AddFontResourceW(L"nosuchfont.ttf") == 0);
}

/* ---- a task dialog with custom buttons (Inno Setup's error prompt) ---- */
#pragma pack(push, 1)
typedef struct { int id; LPCWSTR text; } TdButton;
typedef struct {
    UINT cbSize; HWND parent; HINSTANCE inst; DWORD flags, common;
    LPCWSTR title, icon, main, content;
    UINT nbuttons; const TdButton *buttons; int defbutton;
    UINT nradio; const TdButton *radio; int defradio;
    LPCWSTR verify, expanded, expandctl, collapsectl, footericon, footer;
    void *callback; LONG_PTR data;
    UINT width;
} TdConfig;
#pragma pack(pop)

static int td_buttons;
static WCHAR td_second[64];

/* Finds the dialog, counts its buttons, presses the second one */
static DWORD WINAPI td_presser(void *arg)
{
    (void)arg;
    for (int i = 0; i < 200; i++) {
        HWND d = FindWindowW(NULL, L"setuptest task dialog");
        if (d) {
            HWND b = NULL, second = NULL;
            while ((b = FindWindowExW(d, b, L"BUTTON", NULL)) != NULL)
                if (++td_buttons == 2) second = b;
            if (second) {
                GetWindowTextW(second, td_second, 64);
                PostMessageW(d, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(second), BN_CLICKED), (LPARAM)second);
            }
            return 0;
        }
        Sleep(50);
    }
    return 0;
}

static void taskdialog(void)
{
    typedef HRESULT(WINAPI * Fn)(const TdConfig *, int *, int *, BOOL *);
    Fn fn = (Fn)(void *)GetProcAddress(LoadLibraryW(L"comctl32.dll"), "TaskDialogIndirect");
    CHECK("comctl32 has TaskDialogIndirect", fn != NULL);
    if (!fn) return;
    static const TdButton b[] = { { 101, L"&Retry" }, { 102, L"&Ignore the error and continue" },
                                  { 103, L"&Cancel installation" } };
    TdConfig c;
    memset(&c, 0, sizeof(c));
    c.cbSize = sizeof(c);
    c.flags = 0x10;                                              /* TDF_USE_COMMAND_LINKS */
    c.title = L"setuptest task dialog";
    c.icon = MAKEINTRESOURCEW(-1);
    c.main = L"Select action";
    c.content = L"AddFontResource failed.";
    c.nbuttons = 3;
    c.buttons = b;
    c.defbutton = 101;
    HANDLE t = CreateThread(NULL, 0, td_presser, NULL, 0, NULL);
    int pressed = 0;
    HRESULT hr = fn(&c, &pressed, NULL, NULL);
    WaitForSingleObject(t, 15000);
    CloseHandle(t);
    CHECK("TaskDialogIndirect returns S_OK", hr == S_OK);
    CHECK("its three custom buttons are shown", td_buttons == 3);
    CHECK("the second shows its text", !wcscmp(td_second, L"&Ignore the error and continue"));
    CHECK("pressing it returns its ID", pressed == 102);
}

/* ---- a shortcut's properties (Inno Setup sets its AppUserModelID) ---- */
typedef struct { GUID fmtid; DWORD pid; } PKey;
typedef struct { WORD vt, r1, r2, r3; union { LPWSTR s; short b; BYTE pad[2 * sizeof(void *)]; } u; } PVar;
typedef HRESULT(WINAPI * QiFn)(void *, REFIID, void **);
typedef ULONG(WINAPI * RelFn)(void *);

static void shortcut_props(void)
{
    static const GUID clsid = { 0x00021401, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
    static const GUID iid_sl = { 0x000214F9, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
    static const GUID iid_ps = { 0x886D8EEB, 0x8CF2, 0x4446, { 0x8D, 0x02, 0xCD, 0xBA, 0x1D, 0xBD, 0xCF, 0x99 } };
    static const PKey aumid = { { 0x9F4C2855, 0x9F79, 0x4B39, { 0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3 } }, 5 };
    CoInitialize(NULL);
    void *sl = NULL, *ps = NULL;
    CHECK("CoCreateInstance(CLSID_ShellLink)", CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_SERVER, &iid_sl, &sl) == S_OK && sl);
    if (!sl) return;
    CHECK("the shortcut has IPropertyStore", ((QiFn)(*(void ***)sl)[0])(sl, &iid_ps, &ps) == S_OK && ps);
    if (ps) {
        void **v = *(void ***)ps;
        PVar in, out;
        memset(&in, 0, sizeof(in));
        memset(&out, 0, sizeof(out));
        in.vt = 31;                                                  /* VT_LPWSTR */
        in.u.s = L"NovaOS.SetupTest";
        DWORD n = 0;
        CHECK("SetValue(System.AppUserModel.ID)",
              ((HRESULT(WINAPI *)(void *, const PKey *, const PVar *))v[6])(ps, &aumid, &in) == S_OK);
        CHECK("GetCount is 1", ((HRESULT(WINAPI *)(void *, DWORD *))v[3])(ps, &n) == S_OK && n == 1);
        CHECK("GetValue gives it back",
              ((HRESULT(WINAPI *)(void *, const PKey *, PVar *))v[5])(ps, &aumid, &out) == S_OK && out.vt == 31 &&
              out.u.s && !wcscmp(out.u.s, L"NovaOS.SetupTest"));
        if (out.vt == 31) CoTaskMemFree(out.u.s);
        in.vt = VT_BSTR;                                             /* (as Inno Setup sets it) */
        in.u.s = SysAllocString(L"NovaOS.SetupTest2");
        memset(&out, 0, sizeof(out));
        CHECK("SetValue of a BSTR",
              ((HRESULT(WINAPI *)(void *, const PKey *, const PVar *))v[6])(ps, &aumid, &in) == S_OK);
        SysFreeString(in.u.s);
        CHECK("GetValue gives the BSTR back",
              ((HRESULT(WINAPI *)(void *, const PKey *, PVar *))v[5])(ps, &aumid, &out) == S_OK && out.vt == VT_BSTR &&
              out.u.s && SysStringLen(out.u.s) == 17 && !wcscmp(out.u.s, L"NovaOS.SetupTest2"));
        if (out.vt == VT_BSTR) SysFreeString(out.u.s);
        CHECK("Commit", ((HRESULT(WINAPI *)(void *))v[7])(ps) == S_OK);
        ((RelFn)v[2])(ps);
    }
    ((RelFn)(*(void ***)sl)[2])(sl);
}

/* ---- memory protection (the Visual C++ Redistributable's Burn engine) ---- */
static void protect_memory(void)
{
    typedef BOOL(WINAPI * Fn)(void *, DWORD, DWORD);
    typedef LONG(WINAPI * Rtl)(void *, ULONG, ULONG);
    Fn prot = (Fn)(void *)GetProcAddress(LoadLibraryW(L"crypt32.dll"), "CryptProtectMemory");
    Fn unprot = (Fn)(void *)GetProcAddress(LoadLibraryW(L"crypt32.dll"), "CryptUnprotectMemory");
    Rtl enc = (Rtl)(void *)GetProcAddress(LoadLibraryW(L"advapi32.dll"), "SystemFunction040");
    Rtl dec = (Rtl)(void *)GetProcAddress(LoadLibraryW(L"advapi32.dll"), "SystemFunction041");
    CHECK("CryptProtectMemory and RtlEncryptMemory are exported", prot && unprot && enc && dec);
    if (!prot || !unprot || !enc || !dec) return;
    char buf[32] = "a password, 32 bytes with NUL..", orig[32];
    memcpy(orig, buf, 32);
    CHECK("CryptProtectMemory changes the bytes", prot(buf, 32, 0) && memcmp(buf, orig, 32));
    CHECK("CryptUnprotectMemory restores them", unprot(buf, 32, 0) && !memcmp(buf, orig, 32));
    CHECK("CryptProtectMemory wants blocks of 16", !prot(buf, 20, 0) && GetLastError() == ERROR_INVALID_PARAMETER);
    CHECK("RtlEncryptMemory (cross-process) round trip",
          enc(buf, 16, 1) == 0 && memcmp(buf, orig, 16) && dec(buf, 16, 1) == 0 && !memcmp(buf, orig, 32));
}

/* ---- a file over 256 MB --------------------------------------------- */
static void bigfile(void)
{
    const DWORD chunk = 1 << 20, total = 300;
    BYTE *b = VirtualAlloc(NULL, chunk, MEM_COMMIT, PAGE_READWRITE);
    CreateDirectoryW(L"C:\\Temp", NULL);
    HANDLE f = CreateFileW(L"C:\\Temp\\setuptest-big.bin", GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    CHECK("create the big file", f != INVALID_HANDLE_VALUE && b);
    if (f == INVALID_HANDLE_VALUE || !b) return;
    DWORD i, n;
    for (i = 0; i < total; i++) {
        memset(b, (int)(i & 0xFF), chunk);
        if (!WriteFile(f, b, chunk, &n, NULL) || n != chunk) break;
    }
    CHECK("write 300 MB", i == total);
    LARGE_INTEGER size = { 0 };
    GetFileSizeEx(f, &size);
    CHECK("its size is 300 MB", size.QuadPart == (LONGLONG)total * chunk);
    LARGE_INTEGER at;
    at.QuadPart = (LONGLONG)(total - 1) * chunk + 12345;
    SetFilePointerEx(f, at, NULL, FILE_BEGIN);
    BYTE c = 0;
    CHECK("read its last megabyte back", ReadFile(f, &c, 1, &n, NULL) && n == 1 && c == ((total - 1) & 0xFF));
    CloseHandle(f);
    VirtualFree(b, 0, MEM_RELEASE);
    CHECK("delete it", DeleteFileW(L"C:\\Temp\\setuptest-big.bin"));
}

int main(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "child")) return child(!strcmp(argv[2], "elevated"));
    if (argc > 1 && !strcmp(argv[1], "bigfile")) {
        bigfile();
        printf("setuptest bigfile: %d passed, %d failed\n", pass, fail);
        return fail != 0;
    }
    elevation();
    richedit();
    variants();
    fonts();
    taskdialog();
    shortcut_props();
    protect_memory();
    printf("setuptest: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}
