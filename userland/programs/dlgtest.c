/*
 * dlgtest — the file dialogs and shell items.
 *
 *   dlgtest          checks that need no user: shell items and item arrays,
 *                    the IFileOpenDialog/IFileSaveDialog objects' settings,
 *                    GetOpenFileName's argument checks
 *   dlgtest open     GetOpenFileName (text files), prints the choice
 *   dlgtest multi    GetOpenFileName with several files allowed
 *   dlgtest save     GetSaveFileName (default extension .txt)
 *   dlgtest ifd      IFileOpenDialog, prints GetResult
 *   dlgtest ifdsave  IFileSaveDialog, prints GetResult
 *   dlgtest folder   IFileOpenDialog picking a folder
 */
#include <windows.h>
#include <objbase.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static int pass, fail;
#define CHECK(what, cond) do { if (cond) pass++; else { fail++; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

static const GUID IID_IShellItem_      = { 0x43826D1E, 0xE718, 0x42EE, { 0xBC, 0x55, 0xA1, 0xE2, 0x61, 0xC3, 0x7B, 0xFE } };
static const GUID IID_IShellItemArray_ = { 0xB63EA76D, 0x1F85, 0x456F, { 0xA1, 0x9C, 0x48, 0x15, 0x9E, 0xFA, 0x85, 0x8B } };
static const GUID IID_IFileOpenDialog_ = { 0xD57C7288, 0xD4AD, 0x4768, { 0xBE, 0x02, 0x9D, 0x96, 0x95, 0x32, 0xD9, 0x60 } };
static const GUID IID_IFileSaveDialog_ = { 0x84BCCD23, 0x5FDE, 0x4CDB, { 0xAE, 0xA4, 0xAF, 0x64, 0xB8, 0x3D, 0x78, 0xAB } };
static const GUID IID_IFileDialogCustomize_ = { 0xE6FDD21A, 0x163F, 0x4975, { 0x9C, 0x8C, 0xA6, 0x9F, 0x1B, 0xA3, 0x70, 0x34 } };
static const GUID CLSID_FileOpenDialog_ = { 0xDC1C5A9C, 0xE88A, 0x4DDE, { 0xA5, 0xA1, 0x60, 0xF8, 0x2A, 0x20, 0xAE, 0xF7 } };
static const GUID CLSID_FileSaveDialog_ = { 0xC0B4E2F3, 0xBA21, 0x4773, { 0x8D, 0xBA, 0x33, 0x5E, 0xC9, 0x46, 0xEB, 0x8B } };

/* the interfaces, as plain vtables */
typedef struct IShellItem_ IShellItem_;
typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IShellItem_ *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(IShellItem_ *);
    ULONG (STDMETHODCALLTYPE *Release)(IShellItem_ *);
    HRESULT (STDMETHODCALLTYPE *BindToHandler)(IShellItem_ *, void *, REFGUID, REFIID, void **);
    HRESULT (STDMETHODCALLTYPE *GetParent)(IShellItem_ *, IShellItem_ **);
    HRESULT (STDMETHODCALLTYPE *GetDisplayName)(IShellItem_ *, DWORD, LPWSTR *);
    HRESULT (STDMETHODCALLTYPE *GetAttributes)(IShellItem_ *, DWORD, DWORD *);
    HRESULT (STDMETHODCALLTYPE *Compare)(IShellItem_ *, IShellItem_ *, DWORD, int *);
} IShellItemVtbl_;
struct IShellItem_ { const IShellItemVtbl_ *v; };

typedef struct IShellItemArray_ IShellItemArray_;
typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IShellItemArray_ *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(IShellItemArray_ *);
    ULONG (STDMETHODCALLTYPE *Release)(IShellItemArray_ *);
    void *BindToHandler, *GetPropertyStore, *GetPropertyDescriptionList, *GetAttributes;
    HRESULT (STDMETHODCALLTYPE *GetCount)(IShellItemArray_ *, DWORD *);
    HRESULT (STDMETHODCALLTYPE *GetItemAt)(IShellItemArray_ *, DWORD, IShellItem_ **);
    void *EnumItems;
} IShellItemArrayVtbl_;
struct IShellItemArray_ { const IShellItemArrayVtbl_ *v; };

typedef struct { LPCWSTR pszName, pszSpec; } FILTERSPEC_;
typedef struct IFileDialog_ IFileDialog_;
typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IFileDialog_ *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(IFileDialog_ *);
    ULONG (STDMETHODCALLTYPE *Release)(IFileDialog_ *);
    HRESULT (STDMETHODCALLTYPE *Show)(IFileDialog_ *, HWND);
    HRESULT (STDMETHODCALLTYPE *SetFileTypes)(IFileDialog_ *, UINT, const FILTERSPEC_ *);
    HRESULT (STDMETHODCALLTYPE *SetFileTypeIndex)(IFileDialog_ *, UINT);
    HRESULT (STDMETHODCALLTYPE *GetFileTypeIndex)(IFileDialog_ *, UINT *);
    HRESULT (STDMETHODCALLTYPE *Advise)(IFileDialog_ *, void *, DWORD *);
    HRESULT (STDMETHODCALLTYPE *Unadvise)(IFileDialog_ *, DWORD);
    HRESULT (STDMETHODCALLTYPE *SetOptions)(IFileDialog_ *, DWORD);
    HRESULT (STDMETHODCALLTYPE *GetOptions)(IFileDialog_ *, DWORD *);
    HRESULT (STDMETHODCALLTYPE *SetDefaultFolder)(IFileDialog_ *, IShellItem_ *);
    HRESULT (STDMETHODCALLTYPE *SetFolder)(IFileDialog_ *, IShellItem_ *);
    HRESULT (STDMETHODCALLTYPE *GetFolder)(IFileDialog_ *, IShellItem_ **);
    HRESULT (STDMETHODCALLTYPE *GetCurrentSelection)(IFileDialog_ *, IShellItem_ **);
    HRESULT (STDMETHODCALLTYPE *SetFileName)(IFileDialog_ *, LPCWSTR);
    HRESULT (STDMETHODCALLTYPE *GetFileName)(IFileDialog_ *, LPWSTR *);
    HRESULT (STDMETHODCALLTYPE *SetTitle)(IFileDialog_ *, LPCWSTR);
    HRESULT (STDMETHODCALLTYPE *SetOkButtonLabel)(IFileDialog_ *, LPCWSTR);
    HRESULT (STDMETHODCALLTYPE *SetFileNameLabel)(IFileDialog_ *, LPCWSTR);
    HRESULT (STDMETHODCALLTYPE *GetResult)(IFileDialog_ *, IShellItem_ **);
    HRESULT (STDMETHODCALLTYPE *AddPlace)(IFileDialog_ *, IShellItem_ *, int);
    HRESULT (STDMETHODCALLTYPE *SetDefaultExtension)(IFileDialog_ *, LPCWSTR);
    HRESULT (STDMETHODCALLTYPE *Close)(IFileDialog_ *, HRESULT);
    HRESULT (STDMETHODCALLTYPE *SetClientGuid)(IFileDialog_ *, REFGUID);
    HRESULT (STDMETHODCALLTYPE *ClearClientData)(IFileDialog_ *);
    HRESULT (STDMETHODCALLTYPE *SetFilter)(IFileDialog_ *, void *);
    HRESULT (STDMETHODCALLTYPE *GetResults)(IFileDialog_ *, IShellItemArray_ **);   /* IFileOpenDialog */
    HRESULT (STDMETHODCALLTYPE *GetSelectedItems)(IFileDialog_ *, IShellItemArray_ **);
} IFileDialogVtbl_;
struct IFileDialog_ { const IFileDialogVtbl_ *v; };

typedef struct IFileDialogCustomize_ IFileDialogCustomize_;
typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IFileDialogCustomize_ *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(IFileDialogCustomize_ *);
    ULONG (STDMETHODCALLTYPE *Release)(IFileDialogCustomize_ *);
    void *EnableOpenDropDown, *AddMenu, *AddPushButton, *AddComboBox, *AddRadioButtonList;
    HRESULT (STDMETHODCALLTYPE *AddCheckButton)(IFileDialogCustomize_ *, DWORD, LPCWSTR, BOOL);
    void *AddEditBox, *AddSeparator, *AddText, *SetControlLabel, *GetControlState, *SetControlState,
         *GetEditBoxText, *SetEditBoxText;
    HRESULT (STDMETHODCALLTYPE *GetCheckButtonState)(IFileDialogCustomize_ *, DWORD, BOOL *);
    HRESULT (STDMETHODCALLTYPE *SetCheckButtonState)(IFileDialogCustomize_ *, DWORD, BOOL);
} IFileDialogCustomizeVtbl_;
struct IFileDialogCustomize_ { const IFileDialogCustomizeVtbl_ *v; };

typedef HRESULT (WINAPI *ItemFromName)(PCWSTR, void *, REFIID, void **);
typedef HRESULT (WINAPI *ArrayFromItem)(IShellItem_ *, REFIID, void **);

#define SIGDN_NORMALDISPLAY 0
#define SIGDN_FILESYSPATH   0x80058000
#define SFGAO_FOLDER        0x20000000
#define FOS_PICKFOLDERS     0x20
#define FOS_ALLOWMULTISELECT 0x200

static void print_item(const char *what, IShellItem_ *it)
{
    LPWSTR s = 0;
    if (it && !it->v->GetDisplayName(it, SIGDN_FILESYSPATH, &s)) { printf("%s: %ls\n", what, s); CoTaskMemFree(s); }
    else printf("%s: (none)\n", what);
}

static int show_ifd(BOOL save, DWORD extra)
{
    IFileDialog_ *d = 0;
    HRESULT hr = CoCreateInstance(save ? &CLSID_FileSaveDialog_ : &CLSID_FileOpenDialog_, 0, CLSCTX_INPROC_SERVER,
                                  save ? &IID_IFileSaveDialog_ : &IID_IFileOpenDialog_, (void **)&d);
    if (hr) { printf("CoCreateInstance: 0x%08lx\n", hr); return 1; }
    static const FILTERSPEC_ types[] = { { L"Text files", L"*.txt" }, { L"All files", L"*.*" } };
    if (!(extra & FOS_PICKFOLDERS)) d->v->SetFileTypes(d, 2, types);
    DWORD fos;
    d->v->GetOptions(d, &fos);
    d->v->SetOptions(d, fos | extra);
    if (save) d->v->SetDefaultExtension(d, L"txt");
    hr = d->v->Show(d, 0);
    if (hr) { printf("cancelled (0x%08lx)\n", hr); d->v->Release(d); return 0; }
    IShellItem_ *it = 0;
    d->v->GetResult(d, &it);
    print_item("chosen", it);
    UINT ti = 0;
    d->v->GetFileTypeIndex(d, &ti);
    printf("type %u\n", ti);
    if (it) it->v->Release(it);
    d->v->Release(d);
    return 0;
}

typedef struct {
    DWORD lStructSize; HWND hwndOwner; HINSTANCE hInstance; LPCWSTR lpstrFilter; LPWSTR lpstrCustomFilter;
    DWORD nMaxCustFilter, nFilterIndex; LPWSTR lpstrFile; DWORD nMaxFile; LPWSTR lpstrFileTitle; DWORD nMaxFileTitle;
    LPCWSTR lpstrInitialDir, lpstrTitle; DWORD Flags; WORD nFileOffset, nFileExtension; LPCWSTR lpstrDefExt;
    LPARAM lCustData; void *lpfnHook; LPCWSTR lpTemplateName; void *pvReserved; DWORD dwReserved, FlagsEx;
} OFN_;
typedef BOOL (WINAPI *GetFileName)(OFN_ *);

static int show_ofn(GetFileName fn, DWORD flags, LPCWSTR defext)
{
    static WCHAR file[4096];
    OFN_ o = { sizeof o };
    o.lpstrFilter = L"Text files (*.txt)\0*.txt\0All files (*.*)\0*.*\0";
    o.nFilterIndex = 1;
    o.lpstrFile = file;
    o.nMaxFile = 4096;
    o.Flags = flags;
    o.lpstrDefExt = defext;
    if (!fn(&o)) { printf("cancelled\n"); return 0; }
    if (o.nFileOffset && !file[o.nFileOffset - 1]) {        /* several: the folder, then each name */
        printf("folder: %ls\n", file);
        for (WCHAR *p = file + o.nFileOffset; *p; p += wcslen(p) + 1) printf("file: %ls\n", p);
    } else {
        printf("chosen: %ls (name at %u, extension at %u, type %lu)\n", file, o.nFileOffset, o.nFileExtension, o.nFilterIndex);
    }
    return 0;
}

int main(int argc, char **argv)
{
    CoInitialize(0);
    HMODULE cd = LoadLibraryA("comdlg32.dll");
    GetFileName open = (GetFileName)GetProcAddress(cd, "GetOpenFileNameW");
    GetFileName save = (GetFileName)GetProcAddress(cd, "GetSaveFileNameW");
    if (argc > 1) {
        if (!strcmp(argv[1], "open")) return show_ofn(open, 0x00081800 /* EXPLORER | FILEMUSTEXIST | PATHMUSTEXIST */, 0);
        if (!strcmp(argv[1], "multi")) return show_ofn(open, 0x00081A00 /* + ALLOWMULTISELECT */, 0);
        if (!strcmp(argv[1], "save")) return show_ofn(save, 0x00080002 /* EXPLORER | OVERWRITEPROMPT */, L"txt");
        if (!strcmp(argv[1], "ifd")) return show_ifd(FALSE, 0);
        if (!strcmp(argv[1], "ifdsave")) return show_ifd(TRUE, 0);
        if (!strcmp(argv[1], "folder")) return show_ifd(FALSE, FOS_PICKFOLDERS);
        printf("usage: dlgtest [open|multi|save|ifd|ifdsave|folder]\n");
        return 1;
    }

    /* ---- shell items ---- */
    HMODULE sh = LoadLibraryA("shell32.dll");
    ItemFromName from_name = (ItemFromName)GetProcAddress(sh, "SHCreateItemFromParsingName");
    ArrayFromItem array_from = (ArrayFromItem)GetProcAddress(sh, "SHCreateShellItemArrayFromShellItem");
    IShellItem_ *it = 0, *par = 0;
    CHECK("SHCreateItemFromParsingName", from_name && !from_name(L"C:\\Windows\\System32\\kernel32.dll", 0, &IID_IShellItem_, (void **)&it) && it);
    CHECK("missing file", from_name && from_name(L"C:\\no\\such\\file.txt", 0, &IID_IShellItem_, (void **)&par) && !par);
    if (it) {
        LPWSTR s = 0;
        CHECK("GetDisplayName(FILESYSPATH)", !it->v->GetDisplayName(it, SIGDN_FILESYSPATH, &s) && s && !_wcsicmp(s, L"C:\\Windows\\System32\\kernel32.dll"));
        CoTaskMemFree(s); s = 0;
        CHECK("GetDisplayName(NORMALDISPLAY)", !it->v->GetDisplayName(it, SIGDN_NORMALDISPLAY, &s) && s && !_wcsicmp(s, L"kernel32.dll"));
        CoTaskMemFree(s); s = 0;
        CHECK("GetParent", !it->v->GetParent(it, &par) && par);
        if (par) {
            CHECK("parent path", !par->v->GetDisplayName(par, SIGDN_FILESYSPATH, &s) && s && !_wcsicmp(s, L"C:\\Windows\\System32"));
            CoTaskMemFree(s);
            DWORD a = 0;
            CHECK("folder attribute", !par->v->GetAttributes(par, SFGAO_FOLDER, &a) && a == SFGAO_FOLDER);
            int order = 0;
            CHECK("Compare", par->v->Compare(par, it, 0, &order) == S_FALSE && order != 0);
            par->v->Release(par);
        }
        IShellItemArray_ *arr = 0;
        DWORD n = 0;
        CHECK("item array", array_from && !array_from(it, &IID_IShellItemArray_, (void **)&arr) && arr &&
                            !arr->v->GetCount(arr, &n) && n == 1);
        if (arr) {
            IShellItem_ *one = 0;
            int order = 1;
            CHECK("GetItemAt", !arr->v->GetItemAt(arr, 0, &one) && one && !one->v->Compare(one, it, 0, &order) && !order);
            if (one) one->v->Release(one);
            arr->v->Release(arr);
        }
        it->v->Release(it);
    }

    /* ---- the dialog objects, without showing them ---- */
    IFileDialog_ *d = 0;
    CHECK("CoCreateInstance(FileOpenDialog)",
          !CoCreateInstance(&CLSID_FileOpenDialog_, 0, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog_, (void **)&d) && d);
    if (d) {
        static const FILTERSPEC_ types[] = { { L"Text", L"*.txt" }, { L"C source", L"*.c;*.h" }, { L"All", L"*.*" } };
        UINT ti = 0;
        CHECK("SetFileTypes", !d->v->SetFileTypes(d, 3, types));
        CHECK("SetFileTypeIndex", !d->v->SetFileTypeIndex(d, 2) && !d->v->GetFileTypeIndex(d, &ti) && ti == 2);
        DWORD fos = 0;
        CHECK("default options", !d->v->GetOptions(d, &fos) && (fos & 0x1000 /* FILEMUSTEXIST */));
        CHECK("SetOptions", !d->v->SetOptions(d, fos | FOS_ALLOWMULTISELECT) && !d->v->GetOptions(d, &fos) && (fos & FOS_ALLOWMULTISELECT));
        LPWSTR s = 0;
        CHECK("SetFileName", !d->v->SetFileName(d, L"notes.txt") && !d->v->GetFileName(d, &s) && s && !wcscmp(s, L"notes.txt"));
        CoTaskMemFree(s);
        IShellItem_ *dir = 0, *got = 0;
        if (from_name) from_name(L"C:\\Windows", 0, &IID_IShellItem_, (void **)&dir);
        CHECK("SetFolder/GetFolder", dir && !d->v->SetFolder(d, dir) && !d->v->GetFolder(d, &got) && got);
        if (got) {
            s = 0;
            CHECK("folder kept", !got->v->GetDisplayName(got, SIGDN_FILESYSPATH, &s) && s && !_wcsicmp(s, L"C:\\Windows"));
            CoTaskMemFree(s);
            got->v->Release(got);
        }
        if (dir) dir->v->Release(dir);
        IShellItem_ *r = 0;
        CHECK("no result before Show", d->v->GetResult(d, &r) && !r);
        IFileDialogCustomize_ *cu = 0;
        BOOL b = FALSE;
        CHECK("IFileDialogCustomize", !d->v->QueryInterface(d, &IID_IFileDialogCustomize_, (void **)&cu) && cu &&
                                      !cu->v->AddCheckButton(cu, 7, L"Read only", TRUE) &&
                                      !cu->v->GetCheckButtonState(cu, 7, &b) && b);
        if (cu) cu->v->Release(cu);
        d->v->Release(d);
    }
    IFileDialog_ *sd = 0;
    CHECK("CoCreateInstance(FileSaveDialog)",
          !CoCreateInstance(&CLSID_FileSaveDialog_, 0, CLSCTX_INPROC_SERVER, &IID_IFileSaveDialog_, (void **)&sd) && sd);
    if (sd) {
        DWORD fos = 0;
        CHECK("save options", !sd->v->GetOptions(sd, &fos) && (fos & 0x2 /* OVERWRITEPROMPT */));
        sd->v->Release(sd);
    }

    /* ---- GetOpenFileName's checks ---- */
    DWORD (WINAPI *ext_err)(void) = (void *)GetProcAddress(cd, "CommDlgExtendedError");
    OFN_ bad = { 12 };
    CHECK("bad struct size", open && !open(&bad) && ext_err && ext_err() == 1 /* CDERR_STRUCTSIZE */);

    printf("dlgtest: %d passed, %d failed\n", pass, fail);
    CoUninitialize();
    return fail ? 1 : 0;
}
