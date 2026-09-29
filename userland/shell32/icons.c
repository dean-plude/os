/*
 * icons.c — the system image lists: small (16x16) and large (32x32) icons
 * for folders, drives and the common file types, drawn here, which
 * SHGetFileInfo(SHGFI_SYSICONINDEX) and SHGetImageList hand to programs
 * (file managers show them in their list views).
 */

#define NOVA_BUILD_SHELL32
#include <windows.h>
#include <commctrl.h>

enum { IC_FILE, IC_FOLDER, IC_EXE, IC_TEXT, IC_ARCHIVE, IC_IMAGE, IC_DLL, IC_DRIVE, IC_COUNT };

static HIMAGELIST g_list[2];            /* large, small */

/* ---- a tiny painter on a 16-unit grid, scaled to the icon size ---- */
typedef struct { DWORD *px; int size; } Canvas;

static void fill(Canvas *c, int x0, int y0, int x1, int y1, DWORD argb)
{
    int k = c->size / 16;
    for (int y = y0 * k; y < y1 * k && y < c->size; y++)
        for (int x = x0 * k; x < x1 * k && x < c->size; x++)
            if (x >= 0 && y >= 0) c->px[y * c->size + x] = argb;
}

static void box(Canvas *c, int x0, int y0, int x1, int y1, DWORD border, DWORD inside)
{
    int k = c->size / 16;
    fill(c, x0, y0, x1, y1, border);
    for (int y = y0 * k + 1; y < y1 * k - 1; y++)
        for (int x = x0 * k + 1; x < x1 * k - 1; x++)
            c->px[y * c->size + x] = inside;
}

static void page(Canvas *c, DWORD tint)
{
    box(c, 3, 1, 13, 15, 0xFF8C939Bu, tint);
    box(c, 9, 1, 13, 5, 0xFF8C939Bu, 0xFFE3E7EBu);           /* the folded corner */
}

static void draw(Canvas *c, int kind)
{
    switch (kind) {
    case IC_FOLDER:
        fill(c, 1, 3, 7, 5, 0xFFD9A21Fu);
        fill(c, 1, 4, 15, 14, 0xFFD9A21Fu);
        fill(c, 1, 6, 15, 14, 0xFFFFC94Au);
        break;
    case IC_DRIVE:
        box(c, 1, 5, 15, 12, 0xFF5F6368u, 0xFFBDC1C6u);
        fill(c, 2, 9, 14, 11, 0xFF9AA0A6u);
        fill(c, 12, 7, 13, 8, 0xFF34A853u);
        break;
    case IC_EXE:
        box(c, 1, 2, 15, 14, 0xFF2B6CD4u, 0xFFFFFFFFu);
        fill(c, 1, 2, 15, 5, 0xFF2B6CD4u);
        fill(c, 3, 7, 9, 8, 0xFF9AA0A6u);
        fill(c, 3, 9, 12, 10, 0xFF9AA0A6u);
        fill(c, 3, 11, 7, 12, 0xFF9AA0A6u);
        break;
    case IC_TEXT:
        page(c, 0xFFFFFFFFu);
        for (int y = 6; y <= 12; y += 2) fill(c, 5, y, y == 12 ? 9 : 11, y + 1, 0xFF9AA0A6u);
        break;
    case IC_ARCHIVE:
        page(c, 0xFFFFF4D6u);
        fill(c, 7, 2, 9, 14, 0xFF6B5B3Eu);
        for (int y = 3; y < 13; y += 2) fill(c, 7, y, 8, y + 1, 0xFFE0C27Au);
        fill(c, 6, 11, 10, 14, 0xFF6B5B3Eu);
        break;
    case IC_IMAGE:
        box(c, 1, 3, 15, 13, 0xFF8C939Bu, 0xFFBFE3FFu);
        fill(c, 2, 9, 14, 12, 0xFF5CB85Cu);
        fill(c, 10, 5, 12, 7, 0xFFFFC107u);
        break;
    case IC_DLL:
        page(c, 0xFFFFFFFFu);
        box(c, 5, 7, 11, 13, 0xFF6A4FB6u, 0xFFD9CCF5u);
        break;
    default:
        page(c, 0xFFFFFFFFu);
        break;
    }
}

static HIMAGELIST build(int size)
{
    HIMAGELIST il = ImageList_Create(size, size, ILC_COLOR32 | ILC_MASK, IC_COUNT, 8);
    if (!il) return NULL;
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = size * IC_COUNT;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void *bits = NULL;
    HBITMAP bm = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bm || !bits) { ImageList_Destroy(il); return NULL; }
    DWORD *strip = bits;
    DWORD *one = LocalAlloc(LMEM_ZEROINIT, (SIZE_T)size * size * 4);
    for (int k = 0; one && k < IC_COUNT; k++) {
        ZeroMemory(one, (SIZE_T)size * size * 4);
        Canvas c = { one, size };
        draw(&c, k);
        for (int y = 0; y < size; y++)
            for (int x = 0; x < size; x++) strip[y * size * IC_COUNT + k * size + x] = one[y * size + x];
    }
    if (one) LocalFree(one);
    ImageList_Add(il, bm, NULL);
    DeleteObject(bm);
    return il;
}

HIMAGELIST sys_image_list(int small)
{
    int k = small ? 1 : 0;
    if (!g_list[k]) {
        HIMAGELIST l = build(small ? 16 : 32);
        if (l && InterlockedCompareExchangePointer((PVOID *)&g_list[k], l, NULL)) ImageList_Destroy(l);
    }
    return g_list[k];
}

static int ends_with_i(const WCHAR *s, const WCHAR *ext)
{
    int n = 0, m = 0;
    while (s[n]) n++;
    while (ext[m]) m++;
    if (m > n) return 0;
    for (int i = 0; i < m; i++) {
        WCHAR a = s[n - m + i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a = (WCHAR)(a + 32);
        if (a != b) return 0;
    }
    return 1;
}

int sys_icon_index(const WCHAR *path, DWORD attrs)
{
    if (path && path[0] && path[1] == ':' && (!path[2] || ((path[2] == '\\' || path[2] == '/') && !path[3]))) return IC_DRIVE;
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) return IC_FOLDER;
    if (!path) return IC_FILE;
    static const WCHAR *const exe[] = { L".exe", L".com", L".bat", L".cmd", L".msi", L".scr" };
    static const WCHAR *const text[] = { L".txt", L".log", L".ini", L".md", L".c", L".h", L".cpp", L".hpp", L".py", L".js",
                                         L".json", L".xml", L".html", L".htm", L".css", L".cfg", L".csv", L".rtf" };
    static const WCHAR *const arc[] = { L".7z", L".zip", L".rar", L".tar", L".gz", L".tgz", L".xz", L".txz", L".bz2", L".tbz2",
                                        L".cab", L".iso", L".wim", L".arj", L".lzma", L".zst", L".lz", L".z", L".cpio", L".rpm",
                                        L".deb", L".dmg", L".vhd", L".vhdx", L".chm", L".001" };
    static const WCHAR *const img[] = { L".png", L".jpg", L".jpeg", L".gif", L".bmp", L".ico", L".webp", L".tif", L".tiff" };
    static const WCHAR *const dll[] = { L".dll", L".sys", L".ocx", L".drv", L".cpl" };
    for (unsigned i = 0; i < sizeof exe / sizeof *exe; i++) if (ends_with_i(path, exe[i])) return IC_EXE;
    for (unsigned i = 0; i < sizeof text / sizeof *text; i++) if (ends_with_i(path, text[i])) return IC_TEXT;
    for (unsigned i = 0; i < sizeof arc / sizeof *arc; i++) if (ends_with_i(path, arc[i])) return IC_ARCHIVE;
    for (unsigned i = 0; i < sizeof img / sizeof *img; i++) if (ends_with_i(path, img[i])) return IC_IMAGE;
    for (unsigned i = 0; i < sizeof dll / sizeof *dll; i++) if (ends_with_i(path, dll[i])) return IC_DLL;
    return IC_FILE;
}

/* SHGetImageList: the list itself stands for its IImageList */
SHSTDAPI_(HRESULT) SHGetImageList(int which, REFIID riid, void **out)
{
    (void)riid;
    if (!out) return E_POINTER;
    HIMAGELIST l = sys_image_list(which == 1 /* SHIL_SMALL */ || which == 3 /* SHIL_SYSSMALL */);
    *out = l;
    return l ? S_OK : E_FAIL;
}

SHSTDAPI_(BOOL) Shell_GetImageLists(HIMAGELIST *large, HIMAGELIST *small)
{
    if (large) *large = sys_image_list(0);
    if (small) *small = sys_image_list(1);
    return TRUE;
}

SHSTDAPI_(int) Shell_GetCachedImageIndexW(LPCWSTR path, int index, UINT flags)
{
    (void)index; (void)flags;
    return sys_icon_index(path, GetFileAttributesW(path));
}
