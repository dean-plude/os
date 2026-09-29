/*
 * menu.c — menus: HMENU objects, the menu bar (drawn in the window's
 * frame), popup menus (their own popup windows) and the loop that tracks
 * the mouse and keyboard while a menu is open; menu resources; keyboard
 * accelerators
 */
#include "u32.h"

#define MENU_MAGIC 0x4D454E55u

typedef struct {
    UINT type, state, id;
    HMENU sub;
    HBITMAP checked, unchecked, bmp;
    ULONG_PTR data;
    WCHAR *text;
    RECT rc;                        /* laid out: bar (window coordinates) or popup (client) */
} MItem;

typedef struct Menu {
    DWORD magic;
    int bar;
    MItem *it;
    int n, cap;
    DWORD style;
    UINT maxh;
    HBRUSH bg;
    ULONG_PTR data;
    DWORD help;
    int w, h;                       /* popup size */
    int sel;                        /* highlighted item (-1 none) */
    HWND wnd;                       /* its popup window while open */
    int sysmenu;
} Menu;

Wnd *g_menu_owner;

static Menu *M(HMENU h)
{
    Menu *m = (Menu *)h;
    return m && handle_live(m) && m->magic == MENU_MAGIC ? m : NULL;
}

static HMENU new_menu(int bar)
{
    Menu *m = calloc(1, sizeof(Menu));
    if (!m) return 0;
    m->magic = MENU_MAGIC;
    handle_add(m);
    m->bar = bar;
    m->sel = -1;
    return (HMENU)m;
}

USERAPI HMENU CreateMenu(void) { return new_menu(1); }
USERAPI HMENU CreatePopupMenu(void) { return new_menu(0); }
USERAPI BOOL IsMenu(HMENU h) { return M(h) != NULL; }

static void free_item(MItem *it, int destroy_sub)
{
    free(it->text);
    it->text = NULL;
    if (destroy_sub && it->sub) DestroyMenu(it->sub);
}

USERAPI BOOL DestroyMenu(HMENU h)
{
    Menu *m = M(h);
    if (!m) return FALSE;
    if (m->wnd && IsWindow(m->wnd)) DestroyWindow(m->wnd);
    for (int i = 0; i < m->n; i++) free_item(&m->it[i], 1);
    free(m->it);
    m->magic = 0;
    handle_remove(m);
    free(m);
    return TRUE;
}

/* The item @pos is (by position or command, searching submenus); its menu in *owner */
static MItem *find_item(Menu *m, UINT pos, UINT flags, Menu **owner, int *index)
{
    if (!m) return NULL;
    if (flags & MF_BYPOSITION) {
        if (pos >= (UINT)m->n) return NULL;
        if (owner) *owner = m;
        if (index) *index = (int)pos;
        return &m->it[pos];
    }
    for (int i = 0; i < m->n; i++) {
        if (m->it[i].id == pos && !(m->it[i].sub && !(m->it[i].type & MFT_SEPARATOR) && m->it[i].id == (UINT)(ULONG_PTR)m->it[i].sub)) {
            if (!m->it[i].sub || m->it[i].id == pos) { if (owner) *owner = m; if (index) *index = i; return &m->it[i]; }
        }
    }
    for (int i = 0; i < m->n; i++) {
        if (!m->it[i].sub) continue;
        MItem *r = find_item(M(m->it[i].sub), pos, flags, owner, index);
        if (r) return r;
    }
    return NULL;
}

static MItem *insert_at(Menu *m, int at)
{
    if (m->n == m->cap) {
        int nc = m->cap ? m->cap * 2 : 8;
        MItem *n = realloc(m->it, sizeof(MItem) * nc);
        if (!n) return NULL;
        m->it = n; m->cap = nc;
    }
    if (at < 0 || at > m->n) at = m->n;
    memmove(m->it + at + 1, m->it + at, sizeof(MItem) * (size_t)(m->n - at));
    m->n++;
    MItem *it = &m->it[at];
    memset(it, 0, sizeof(*it));
    return it;
}

/* MF_* flags and the "new item" argument, as AppendMenu takes them */
static void set_from_flags(MItem *it, UINT f, UINT_PTR id, LPCWSTR s)
{
    it->type = 0;
    if (f & MF_POPUP) { it->sub = (HMENU)id; it->id = (UINT)(UINT_PTR)id; }
    else it->id = (UINT)id;
    if (f & MF_SEPARATOR) it->type |= MFT_SEPARATOR;
    else if (f & MF_BITMAP) { it->type |= MFT_BITMAP; it->bmp = (HBITMAP)s; }
    else if (f & MF_OWNERDRAW) { it->type |= MFT_OWNERDRAW; it->data = (ULONG_PTR)s; }
    else { free(it->text); it->text = wstrdup(s ? s : L""); }
    if (f & MF_MENUBARBREAK) it->type |= MFT_MENUBARBREAK;
    if (f & MF_MENUBREAK) it->type |= MFT_MENUBREAK;
    if (f & MF_RIGHTJUSTIFY) it->type |= MFT_RIGHTJUSTIFY;
    it->state = 0;
    if (f & MF_GRAYED) it->state |= MFS_GRAYED;
    if (f & MF_DISABLED) it->state |= MFS_DISABLED;
    if (f & MF_CHECKED) it->state |= MFS_CHECKED;
    if (f & MF_HILITE) it->state |= MFS_HILITE;
    if (f & MF_DEFAULT) it->state |= MFS_DEFAULT;
}

static int is_string_item(UINT f) { return !(f & (MF_SEPARATOR | MF_BITMAP | MF_OWNERDRAW)); }

USERAPI BOOL InsertMenuW(HMENU h, UINT pos, UINT f, UINT_PTR id, LPCWSTR s)
{
    Menu *m = M(h);
    if (!m) return FALSE;
    int at = m->n;
    if (pos != (UINT)-1) {
        Menu *owner = m;
        int idx;
        if (find_item(m, pos, f, &owner, &idx)) { m = owner; at = idx; }
        else if (!(f & MF_BYPOSITION)) at = m->n;
    }
    MItem *it = insert_at(m, at);
    if (!it) return FALSE;
    set_from_flags(it, f, id, s);
    return TRUE;
}

USERAPI BOOL AppendMenuW(HMENU h, UINT f, UINT_PTR id, LPCWSTR s) { return InsertMenuW(h, (UINT)-1, f | MF_BYPOSITION, id, s); }

USERAPI BOOL InsertMenuA(HMENU h, UINT pos, UINT f, UINT_PTR id, LPCSTR s)
{
    if (!is_string_item(f)) return InsertMenuW(h, pos, f, id, (LPCWSTR)s);
    WCHAR *w = a2w(s ? s : "", -1);
    BOOL r = InsertMenuW(h, pos, f, id, w);
    free(w);
    return r;
}

USERAPI BOOL AppendMenuA(HMENU h, UINT f, UINT_PTR id, LPCSTR s) { return InsertMenuA(h, (UINT)-1, f | MF_BYPOSITION, id, s); }

USERAPI BOOL ModifyMenuW(HMENU h, UINT pos, UINT f, UINT_PTR id, LPCWSTR s)
{
    MItem *it = find_item(M(h), pos, f, NULL, NULL);
    if (!it) return FALSE;
    if ((f & MF_POPUP) && it->sub && it->sub != (HMENU)id) DestroyMenu(it->sub);
    if (!(f & MF_POPUP)) it->sub = 0;
    set_from_flags(it, f, id, s);
    return TRUE;
}

USERAPI BOOL ModifyMenuA(HMENU h, UINT pos, UINT f, UINT_PTR id, LPCSTR s)
{
    if (!is_string_item(f)) return ModifyMenuW(h, pos, f, id, (LPCWSTR)s);
    WCHAR *w = a2w(s ? s : "", -1);
    BOOL r = ModifyMenuW(h, pos, f, id, w);
    free(w);
    return r;
}

static BOOL remove_item(HMENU h, UINT pos, UINT f, int destroy)
{
    Menu *owner;
    int idx;
    MItem *it = find_item(M(h), pos, f, &owner, &idx);
    if (!it) return FALSE;
    free_item(it, destroy);
    memmove(owner->it + idx, owner->it + idx + 1, sizeof(MItem) * (size_t)(owner->n - idx - 1));
    owner->n--;
    return TRUE;
}

USERAPI BOOL RemoveMenu(HMENU h, UINT pos, UINT f) { return remove_item(h, pos, f, 0); }
USERAPI BOOL DeleteMenu(HMENU h, UINT pos, UINT f) { return remove_item(h, pos, f, 1); }
USERAPI int GetMenuItemCount(HMENU h) { Menu *m = M(h); return m ? m->n : -1; }

USERAPI UINT GetMenuItemID(HMENU h, int pos)
{
    Menu *m = M(h);
    if (!m || pos < 0 || pos >= m->n) return (UINT)-1;
    return m->it[pos].sub ? (UINT)-1 : m->it[pos].id;
}

USERAPI HMENU GetSubMenu(HMENU h, int pos)
{
    Menu *m = M(h);
    if (!m || pos < 0 || pos >= m->n) return 0;
    return m->it[pos].sub;
}

USERAPI int GetMenuStringW(HMENU h, UINT pos, LPWSTR s, int n, UINT f)
{
    MItem *it = find_item(M(h), pos, f, NULL, NULL);
    if (!it) { if (s && n) s[0] = 0; return 0; }
    const WCHAR *t = it->text ? it->text : L"";
    int len = wlen(t);
    if (!s || n <= 0) return len;
    int k = MIN(len, n - 1);
    memcpy(s, t, 2 * (size_t)k);
    s[k] = 0;
    return k;
}

USERAPI int GetMenuStringA(HMENU h, UINT pos, LPSTR s, int n, UINT f)
{
    MItem *it = find_item(M(h), pos, f, NULL, NULL);
    if (!it) { if (s && n) s[0] = 0; return 0; }
    const WCHAR *t = it->text ? it->text : L"";
    if (!s || n <= 0) return WideCharToMultiByte(CP_ACP, 0, t, -1, NULL, 0, NULL, NULL) - 1;
    int k = WideCharToMultiByte(CP_ACP, 0, t, -1, s, n, NULL, NULL);
    if (k <= 0) { s[n - 1] = 0; return n - 1; }
    return k - 1;
}

USERAPI UINT GetMenuState(HMENU h, UINT pos, UINT f)
{
    MItem *it = find_item(M(h), pos, f, NULL, NULL);
    if (!it) return (UINT)-1;
    UINT r = it->state | (it->type & (MFT_SEPARATOR | MFT_BITMAP | MFT_OWNERDRAW | MFT_MENUBREAK | MFT_MENUBARBREAK));
    if (it->sub) r |= MF_POPUP | ((UINT)GetMenuItemCount(it->sub) << 8);
    return r;
}

USERAPI DWORD CheckMenuItem(HMENU h, UINT id, UINT f)
{
    MItem *it = find_item(M(h), id, f, NULL, NULL);
    if (!it) return (DWORD)-1;
    DWORD old = it->state & MFS_CHECKED;
    if (f & MF_CHECKED) it->state |= MFS_CHECKED; else it->state &= ~MFS_CHECKED;
    return old;
}

USERAPI BOOL EnableMenuItem(HMENU h, UINT id, UINT f)
{
    MItem *it = find_item(M(h), id, f, NULL, NULL);
    if (!it) return -1;
    UINT old = it->state & (MFS_GRAYED | MF_DISABLED);
    it->state &= ~(MFS_GRAYED | MF_DISABLED);
    it->state |= f & (MF_GRAYED | MF_DISABLED);
    return (BOOL)old;
}

USERAPI BOOL CheckMenuRadioItem(HMENU h, UINT first, UINT last, UINT check, UINT f)
{
    Menu *m = M(h);
    if (!m) return FALSE;
    if (f & MF_BYPOSITION) {
        for (UINT i = first; i <= last && i < (UINT)m->n; i++) {
            if (i == check) m->it[i].state |= MFS_CHECKED, m->it[i].type |= MFT_RADIOCHECK;
            else m->it[i].state &= ~MFS_CHECKED;
        }
        return TRUE;
    }
    for (UINT id = first; id <= last; id++) {
        MItem *it = find_item(m, id, 0, NULL, NULL);
        if (!it) continue;
        if (id == check) { it->state |= MFS_CHECKED; it->type |= MFT_RADIOCHECK; }
        else it->state &= ~MFS_CHECKED;
    }
    return TRUE;
}

USERAPI BOOL HiliteMenuItem(HWND h, HMENU m, UINT pos, UINT f)
{
    (void)h;
    MItem *it = find_item(M(m), pos, f, NULL, NULL);
    if (!it) return FALSE;
    if (f & MF_HILITE) it->state |= MFS_HILITE; else it->state &= ~MFS_HILITE;
    return TRUE;
}

USERAPI BOOL SetMenuDefaultItem(HMENU h, UINT item, UINT bypos)
{
    Menu *m = M(h);
    if (!m) return FALSE;
    for (int i = 0; i < m->n; i++) m->it[i].state &= ~MFS_DEFAULT;
    if (item == (UINT)-1) return TRUE;
    MItem *it = find_item(m, item, bypos ? MF_BYPOSITION : 0, NULL, NULL);
    if (!it) return FALSE;
    it->state |= MFS_DEFAULT;
    return TRUE;
}

USERAPI UINT GetMenuDefaultItem(HMENU h, UINT bypos, UINT flags)
{
    (void)flags;
    Menu *m = M(h);
    if (!m) return (UINT)-1;
    for (int i = 0; i < m->n; i++) if (m->it[i].state & MFS_DEFAULT) return bypos ? (UINT)i : m->it[i].id;
    return (UINT)-1;
}

USERAPI BOOL SetMenuItemBitmaps(HMENU h, UINT pos, UINT f, HBITMAP un, HBITMAP ch)
{
    MItem *it = find_item(M(h), pos, f, NULL, NULL);
    if (!it) return FALSE;
    it->unchecked = un; it->checked = ch;
    return TRUE;
}

USERAPI LONG GetMenuCheckMarkDimensions(void) { return MAKELONG(15, 15); }

static void item_info_get(MItem *it, LPMENUITEMINFOW mi, int wide)
{
    UINT mask = mi->fMask;
    if (mask & MIIM_TYPE) { mi->fType = it->type; if (it->type & (MFT_BITMAP | MFT_SEPARATOR | MFT_OWNERDRAW)) {} }
    if (mask & MIIM_FTYPE) mi->fType = it->type;
    if (mask & MIIM_STATE) mi->fState = it->state;
    if (mask & MIIM_ID) mi->wID = it->id;
    if (mask & MIIM_SUBMENU) mi->hSubMenu = it->sub;
    if (mask & MIIM_CHECKMARKS) { mi->hbmpChecked = it->checked; mi->hbmpUnchecked = it->unchecked; }
    if (mask & MIIM_DATA) mi->dwItemData = it->data;
    if (mask & MIIM_BITMAP) mi->hbmpItem = it->bmp;
    if (mask & (MIIM_STRING | MIIM_TYPE)) {
        if (it->type & (MFT_SEPARATOR | MFT_BITMAP | MFT_OWNERDRAW)) {
            if (mask & MIIM_TYPE) mi->dwTypeData = (LPWSTR)(ULONG_PTR)(it->type & MFT_BITMAP ? (ULONG_PTR)it->bmp : it->data);
            if (mask & MIIM_STRING) mi->cch = 0;
        } else {
            const WCHAR *t = it->text ? it->text : L"";
            int len = wlen(t);
            if (wide) {
                if (mi->dwTypeData && mi->cch) {
                    int k = MIN(len, (int)mi->cch - 1);
                    memcpy(mi->dwTypeData, t, 2 * (size_t)k);
                    mi->dwTypeData[k] = 0;
                    mi->cch = (UINT)k;
                } else mi->cch = (UINT)len;
            } else {
                char *a = (char *)mi->dwTypeData;
                int alen = WideCharToMultiByte(CP_ACP, 0, t, len, NULL, 0, NULL, NULL);
                if (a && mi->cch) {
                    int k = WideCharToMultiByte(CP_ACP, 0, t, len, a, (int)mi->cch - 1, NULL, NULL);
                    a[k] = 0;
                    mi->cch = (UINT)k;
                } else mi->cch = (UINT)alen;
            }
        }
    }
}

static void item_info_set(MItem *it, const MENUITEMINFOW *mi, int wide)
{
    UINT mask = mi->fMask;
    if (mask & MIIM_TYPE) {
        it->type = mi->fType;
        if (mi->fType & MFT_BITMAP) it->bmp = (HBITMAP)mi->dwTypeData;
        else if (mi->fType & MFT_OWNERDRAW) it->data = (ULONG_PTR)mi->dwTypeData;
        else if (!(mi->fType & MFT_SEPARATOR)) {
            free(it->text);
            it->text = wide ? wstrdup(mi->dwTypeData) : a2w((const char *)mi->dwTypeData, -1);
        }
    }
    if (mask & MIIM_FTYPE) it->type = (it->type & (MFT_BITMAP | MFT_STRING)) | (mi->fType & ~(MFT_BITMAP));
    if (mask & MIIM_STRING) {
        free(it->text);
        it->text = mi->dwTypeData ? (wide ? wstrdup(mi->dwTypeData) : a2w((const char *)mi->dwTypeData, -1)) : NULL;
        it->type &= ~(MFT_SEPARATOR | MFT_BITMAP);
    }
    if (mask & MIIM_STATE) it->state = mi->fState;
    if (mask & MIIM_ID) it->id = mi->wID;
    if (mask & MIIM_SUBMENU) { it->sub = mi->hSubMenu; if (it->sub && !(mask & MIIM_ID)) it->id = (UINT)(ULONG_PTR)it->sub; }
    if (mask & MIIM_CHECKMARKS) { it->checked = mi->hbmpChecked; it->unchecked = mi->hbmpUnchecked; }
    if (mask & MIIM_DATA) it->data = mi->dwItemData;
    if (mask & MIIM_BITMAP) it->bmp = mi->hbmpItem;
}

USERAPI BOOL GetMenuItemInfoW(HMENU h, UINT item, BOOL bypos, LPMENUITEMINFOW mi)
{
    MItem *it = find_item(M(h), item, bypos ? MF_BYPOSITION : 0, NULL, NULL);
    if (!it || !mi) { SetLastError(ERROR_MENU_ITEM_NOT_FOUND); return FALSE; }
    item_info_get(it, mi, 1);
    return TRUE;
}

USERAPI BOOL GetMenuItemInfoA(HMENU h, UINT item, BOOL bypos, LPMENUITEMINFOA mi)
{
    MItem *it = find_item(M(h), item, bypos ? MF_BYPOSITION : 0, NULL, NULL);
    if (!it || !mi) { SetLastError(ERROR_MENU_ITEM_NOT_FOUND); return FALSE; }
    item_info_get(it, (LPMENUITEMINFOW)mi, 0);
    return TRUE;
}

USERAPI BOOL SetMenuItemInfoW(HMENU h, UINT item, BOOL bypos, LPCMENUITEMINFOW mi)
{
    MItem *it = find_item(M(h), item, bypos ? MF_BYPOSITION : 0, NULL, NULL);
    if (!it || !mi) return FALSE;
    item_info_set(it, mi, 1);
    return TRUE;
}

USERAPI BOOL SetMenuItemInfoA(HMENU h, UINT item, BOOL bypos, const MENUITEMINFOA *mi)
{
    MItem *it = find_item(M(h), item, bypos ? MF_BYPOSITION : 0, NULL, NULL);
    if (!it || !mi) return FALSE;
    item_info_set(it, (const MENUITEMINFOW *)mi, 0);
    return TRUE;
}

static BOOL insert_item_info(HMENU h, UINT item, BOOL bypos, const MENUITEMINFOW *mi, int wide)
{
    Menu *m = M(h);
    if (!m || !mi) return FALSE;
    int at = m->n;
    Menu *owner = m;
    int idx;
    if (find_item(m, item, bypos ? MF_BYPOSITION : 0, &owner, &idx)) { m = owner; at = idx; }
    MItem *it = insert_at(m, at);
    if (!it) return FALSE;
    item_info_set(it, mi, wide);
    return TRUE;
}

USERAPI BOOL InsertMenuItemW(HMENU h, UINT item, BOOL bypos, LPCMENUITEMINFOW mi) { return insert_item_info(h, item, bypos, mi, 1); }
USERAPI BOOL InsertMenuItemA(HMENU h, UINT item, BOOL bypos, const MENUITEMINFOA *mi) { return insert_item_info(h, item, bypos, (const MENUITEMINFOW *)mi, 0); }

USERAPI BOOL SetMenuInfo(HMENU h, LPCMENUINFO mi)
{
    Menu *m = M(h);
    if (!m || !mi) return FALSE;
    if (mi->fMask & MIM_STYLE) m->style = mi->dwStyle;
    if (mi->fMask & MIM_MAXHEIGHT) m->maxh = mi->cyMax;
    if (mi->fMask & MIM_BACKGROUND) m->bg = mi->hbrBack;
    if (mi->fMask & MIM_HELPID) m->help = mi->dwContextHelpID;
    if (mi->fMask & MIM_MENUDATA) m->data = mi->dwMenuData;
    if (mi->fMask & MIM_APPLYTOSUBMENUS)
        for (int i = 0; i < m->n; i++) if (m->it[i].sub) SetMenuInfo(m->it[i].sub, mi);
    return TRUE;
}

USERAPI BOOL GetMenuInfo(HMENU h, LPMENUINFO mi)
{
    Menu *m = M(h);
    if (!m || !mi) return FALSE;
    if (mi->fMask & MIM_STYLE) mi->dwStyle = m->style;
    if (mi->fMask & MIM_MAXHEIGHT) mi->cyMax = m->maxh;
    if (mi->fMask & MIM_BACKGROUND) mi->hbrBack = m->bg;
    if (mi->fMask & MIM_HELPID) mi->dwContextHelpID = m->help;
    if (mi->fMask & MIM_MENUDATA) mi->dwMenuData = m->data;
    return TRUE;
}

USERAPI BOOL SetMenuContextHelpId(HMENU h, DWORD id) { Menu *m = M(h); if (!m) return FALSE; m->help = id; return TRUE; }
USERAPI DWORD GetMenuContextHelpId(HMENU h) { Menu *m = M(h); return m ? m->help : 0; }

/* -----------------------------------------------------------------------
 * Menu bars
 * ----------------------------------------------------------------------- */
USERAPI HMENU GetMenu(HWND h)
{
    Wnd *w = W(h);
    return w && !w->parent ? w->menu : 0;
}

USERAPI BOOL SetMenu(HWND h, HMENU m)
{
    Wnd *w = W(h);
    if (!w || w->parent) return FALSE;
    w->menu = m;
    w->id = (LONG_PTR)m;
    wnd_set_pos(w, 0, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    return TRUE;
}

USERAPI BOOL DrawMenuBar(HWND h)
{
    Wnd *w = W(h);
    if (!w) return FALSE;
    wnd_set_pos(w, 0, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    invalidate_nc(w);
    return TRUE;
}

USERAPI HMENU GetSystemMenu(HWND h, BOOL revert)
{
    Wnd *w = W(h);
    if (!w) return 0;
    if (revert) { if (w->sysmenu) DestroyMenu(w->sysmenu); w->sysmenu = 0; return 0; }
    if (!w->sysmenu) {
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING, SC_RESTORE, L"&Restore");
        AppendMenuW(m, MF_STRING, SC_MOVE, L"&Move");
        AppendMenuW(m, MF_STRING, SC_SIZE, L"&Size");
        AppendMenuW(m, MF_STRING, SC_MINIMIZE, L"Mi&nimize");
        AppendMenuW(m, MF_STRING, SC_MAXIMIZE, L"Ma&ximize");
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING, SC_CLOSE, L"&Close\tAlt+F4");
        Menu *mm = M(m);
        if (mm) mm->sysmenu = 1;
        w->sysmenu = m;
    }
    return w->sysmenu;
}

#define BAR_ITEM_PAD 8

static HFONT menu_font(void) { return gui_font(); }

static int item_text_w(HDC dc, const WCHAR *t, int *accel_w)
{
    if (accel_w) *accel_w = 0;
    if (!t) return 0;
    const WCHAR *tab = t;
    while (*tab && *tab != '\t') tab++;
    RECT r = { 0, 0, 0, 0 };
    DrawTextW(dc, t, (int)(tab - t), &r, DT_CALCRECT | DT_SINGLELINE);
    if (*tab && accel_w) {
        RECT a = { 0, 0, 0, 0 };
        DrawTextW(dc, tab + 1, -1, &a, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
        *accel_w = a.right;
    }
    return r.right;
}

static int bar_row_h(void)
{
    HDC dc = GetDC(NULL);
    HGDIOBJ of = SelectObject(dc, menu_font());
    int h = font_height(dc) + 6;
    SelectObject(dc, of);
    return h < GetSystemMetrics(SM_CYMENU) ? GetSystemMetrics(SM_CYMENU) : h;
}

/* Lay the bar out in a band @width wide; its items' rectangles are relative to the band */
static int bar_layout(Menu *m, int width)
{
    if (!m) return 0;
    HDC dc = GetDC(NULL);
    HGDIOBJ of = SelectObject(dc, menu_font());
    int rh = bar_row_h();
    int x = 0, y = 0;
    for (int i = 0; i < m->n; i++) {
        MItem *it = &m->it[i];
        int iw = (it->type & MFT_SEPARATOR) ? 6 : item_text_w(dc, it->text, NULL) + 2 * BAR_ITEM_PAD;
        if (it->type & MFT_OWNERDRAW) iw = 40;
        if (x > 0 && (x + iw > width || (it->type & (MFT_MENUBREAK | MFT_MENUBARBREAK)))) { x = 0; y += rh; }
        SetRect(&it->rc, x, y, x + iw, y + rh);
        x += iw;
    }
    SelectObject(dc, of);
    return y + rh;
}

int menu_bar_height(Wnd *w, int width)
{
    Menu *m = M(w->menu);
    if (!m) return 0;
    return bar_layout(m, width) + 1;
}

/* Where the bar is (window coordinates) */
static void bar_band(Wnd *w, RECT *r)
{
    RECT c = w->client;
    OffsetRect(&c, -w->rect.left, -w->rect.top);
    int h = menu_bar_height(w, c.right - c.left);
    int top = c.top - h;
    if (w->style & WS_VSCROLL) {}
    SetRect(r, c.left, top, c.right + ((w->style & WS_VSCROLL) ? sb_width() : 0), c.top);
}

static struct {
    int active;
    Wnd *owner;
    Menu *bar;
    int bar_sel;
    int bar_open;
} T;

void menu_bar_draw(Wnd *w, HDC dc, const RECT *r)
{
    Menu *m = M(w->menu);
    fill_rect(dc, r, 0xFFFFFF);
    if (!m) return;
    bar_layout(m, r->right - r->left);
    HGDIOBJ of = SelectObject(dc, menu_font());
    SetBkMode(dc, TRANSPARENT);
    int active = T.active && T.owner == w;
    for (int i = 0; i < m->n; i++) {
        MItem *it = &m->it[i];
        RECT ir = it->rc;
        OffsetRect(&ir, r->left, r->top);
        if (it->type & MFT_SEPARATOR) continue;
        if (active && T.bar_sel == i) fill_rect(dc, &ir, T.bar_open ? 0xF7E4CC : 0xFBF1E5);
        SetTextColor(dc, (it->state & (MFS_GRAYED | MF_DISABLED)) ? sys_color(COLOR_GRAYTEXT) : sys_color(COLOR_MENUTEXT));
        DrawTextW(dc, it->text ? it->text : L"", -1, &ir, DT_SINGLELINE | DT_CENTER | DT_VCENTER | (active ? 0 : DT_HIDEPREFIX));
    }
    RECT line = { r->left, r->bottom - 1, r->right, r->bottom };
    fill_rect(dc, &line, 0xF0F0F0);
    SelectObject(dc, of);
}

/* The bar item at a screen point (-1: none) */
static int bar_hit(Wnd *w, POINT pt)
{
    Menu *m = M(w->menu);
    if (!m) return -1;
    RECT band;
    bar_band(w, &band);
    POINT o;
    wnd_screen_origin(w, 0, &o);
    int x = pt.x - o.x - band.left, y = pt.y - o.y - band.top;
    for (int i = 0; i < m->n; i++) {
        POINT p = { x, y };
        if (PtInRect(&m->it[i].rc, p)) return i;
    }
    return -1;
}

static void bar_item_screen(Wnd *w, int i, RECT *out)
{
    Menu *m = M(w->menu);
    RECT band;
    bar_band(w, &band);
    POINT o;
    wnd_screen_origin(w, 0, &o);
    *out = m->it[i].rc;
    OffsetRect(out, o.x + band.left, o.y + band.top);
}

USERAPI BOOL GetMenuItemRect(HWND h, HMENU hm, UINT item, LPRECT r)
{
    Menu *m = M(hm);
    if (!m || item >= (UINT)m->n || !r) return FALSE;
    Wnd *w = W_quiet(h);
    if (m->bar && w) { bar_item_screen(w, (int)item, r); return TRUE; }
    if (m->wnd) {
        *r = m->it[item].rc;
        POINT o = { 0, 0 };
        ClientToScreen(m->wnd, &o);
        OffsetRect(r, o.x, o.y);
        return TRUE;
    }
    return FALSE;
}

USERAPI int MenuItemFromPoint(HWND h, HMENU hm, POINT pt)
{
    Menu *m = M(hm);
    if (!m) return -1;
    Wnd *w = W_quiet(h);
    if (m->bar && w) return bar_hit(w, pt);
    if (m->wnd) {
        ScreenToClient(m->wnd, &pt);
        for (int i = 0; i < m->n; i++) if (PtInRect(&m->it[i].rc, pt)) return i;
    }
    return -1;
}

USERAPI BOOL GetMenuBarInfo(HWND h, LONG obj, LONG item, PMENUBARINFO mbi)
{
    Wnd *w = W(h);
    if (!w || !mbi) return FALSE;
    Menu *m = M(w->menu);
    if (!m) return FALSE;
    RECT band;
    bar_band(w, &band);
    POINT o;
    wnd_screen_origin(w, 0, &o);
    OffsetRect(&band, o.x, o.y);
    if (item > 0 && item <= m->n) bar_item_screen(w, item - 1, &mbi->rcBar);
    else mbi->rcBar = band;
    mbi->hMenu = w->menu;
    mbi->hwndMenu = 0;
    mbi->fBarFocused = T.active && T.owner == w;
    mbi->fFocused = 0;
    (void)obj;
    return TRUE;
}

/* -----------------------------------------------------------------------
 * Popup menus
 * ----------------------------------------------------------------------- */
#define POPUP_BORDER 1
#define POPUP_PAD 2
#define CHECK_COL 28
#define ARROW_COL 24

static void popup_layout(Menu *m, Wnd *owner)
{
    HDC dc = GetDC(NULL);
    HGDIOBJ of = SelectObject(dc, menu_font());
    int ih = font_height(dc) + 8;
    int textw = 0, accw = 0;
    for (int i = 0; i < m->n; i++) {
        MItem *it = &m->it[i];
        if (it->type & MFT_OWNERDRAW) {
            MEASUREITEMSTRUCT mi = { ODT_MENU, 0, it->id, 0, (UINT)ih, it->data };
            if (owner) send_msg(owner, WM_MEASUREITEM, 0, (LPARAM)&mi);
            if ((int)mi.itemWidth > textw) textw = (int)mi.itemWidth;
            continue;
        }
        if (it->type & MFT_SEPARATOR) continue;
        int aw;
        int tw = item_text_w(dc, it->text, &aw);
        if (tw > textw) textw = tw;
        if (aw > accw) accw = aw;
    }
    int width = CHECK_COL + textw + (accw ? accw + 24 : 0) + ARROW_COL;
    if (width < 120) width = 120;
    int y = POPUP_PAD;
    for (int i = 0; i < m->n; i++) {
        MItem *it = &m->it[i];
        int h = (it->type & MFT_SEPARATOR) ? 9 : ih;
        if (it->type & MFT_OWNERDRAW) {
            MEASUREITEMSTRUCT mi = { ODT_MENU, 0, it->id, 0, (UINT)ih, it->data };
            if (owner) send_msg(owner, WM_MEASUREITEM, 0, (LPARAM)&mi);
            h = (int)mi.itemHeight;
        }
        SetRect(&it->rc, POPUP_BORDER, y, POPUP_BORDER + width, y + h);
        y += h;
    }
    m->w = width + 2 * POPUP_BORDER;
    m->h = y + POPUP_PAD + POPUP_BORDER;
    if (m->maxh && (UINT)m->h > m->maxh) m->h = (int)m->maxh;
    SelectObject(dc, of);
}

static void popup_paint(Menu *m, HDC dc, Wnd *owner)
{
    RECT all = { 0, 0, m->w, m->h };
    fill_rect(dc, &all, 0xF2F2F2);
    frame_rect(dc, &all, 0xCCCCCC);
    HGDIOBJ of = SelectObject(dc, menu_font());
    SetBkMode(dc, TRANSPARENT);
    for (int i = 0; i < m->n; i++) {
        MItem *it = &m->it[i];
        RECT r = it->rc;
        int dis = (it->state & (MFS_GRAYED | MF_DISABLED)) != 0;
        if (it->type & MFT_SEPARATOR) {
            RECT l = { r.left + CHECK_COL, r.top + 4, r.right - 2, r.top + 5 };
            fill_rect(dc, &l, 0xD7D7D7);
            continue;
        }
        if (it->type & MFT_OWNERDRAW) {
            DRAWITEMSTRUCT di;
            memset(&di, 0, sizeof(di));
            di.CtlType = ODT_MENU; di.itemID = it->id; di.itemAction = ODA_DRAWENTIRE;
            di.itemState = (i == m->sel ? ODS_SELECTED : 0) | (dis ? ODS_GRAYED | ODS_DISABLED : 0) | ((it->state & MFS_CHECKED) ? ODS_CHECKED : 0);
            di.hwndItem = (HWND)(ULONG_PTR)m; di.hDC = dc; di.rcItem = r; di.itemData = it->data;
            if (owner) send_msg(owner, WM_DRAWITEM, 0, (LPARAM)&di);
            continue;
        }
        if (i == m->sel) {
            RECT hr = r;
            InflateRect(&hr, -2, 0);
            fill_rect(dc, &hr, dis ? 0xE6E6E6 : 0xF7E4CC);
        }
        COLORREF tc = dis ? 0x6D6D6D : 0x000000;
        SetTextColor(dc, tc);
        if (it->state & MFS_CHECKED) {
            RECT cr = { r.left + 6, r.top + (r.bottom - r.top - 14) / 2, r.left + 20, 0 };
            cr.bottom = cr.top + 14;
            if (it->type & MFT_RADIOCHECK) {
                RECT d = { cr.left + 4, cr.top + 4, cr.left + 10, cr.top + 10 };
                HBRUSH b = CreateSolidBrush(tc);
                HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, GetStockObject(NULL_PEN));
                Ellipse(dc, d.left, d.top, d.right + 1, d.bottom + 1);
                SelectObject(dc, ob); SelectObject(dc, op);
                DeleteObject(b);
            } else draw_check_mark(dc, cr.left, cr.top, 14, tc);
        } else if (it->bmp && (ULONG_PTR)it->bmp > 11) {
            BITMAP bm;
            if (GetObjectW(it->bmp, sizeof(bm), &bm)) {
                HDC md = CreateCompatibleDC(dc);
                HGDIOBJ o = SelectObject(md, it->bmp);
                int by = r.top + (r.bottom - r.top - bm.bmHeight) / 2;
                if (bm.bmBitsPixel == 32) {
                    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
                    AlphaBlend(dc, r.left + 6, by, bm.bmWidth, bm.bmHeight, md, 0, 0, bm.bmWidth, bm.bmHeight, bf);
                } else BitBlt(dc, r.left + 6, by, bm.bmWidth, bm.bmHeight, md, 0, 0, SRCCOPY);
                SelectObject(md, o);
                DeleteDC(md);
            }
        }
        const WCHAR *t = it->text ? it->text : L"";
        const WCHAR *tab = t;
        while (*tab && *tab != '\t') tab++;
        RECT tr = { r.left + CHECK_COL, r.top, r.right - ARROW_COL, r.bottom };
        HGDIOBJ bf = NULL;
        if (it->state & MFS_DEFAULT) bf = SelectObject(dc, gui_font_bold());
        DrawTextW(dc, t, (int)(tab - t), &tr, DT_SINGLELINE | DT_VCENTER | DT_HIDEPREFIX);
        if (*tab) DrawTextW(dc, tab + 1, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
        if (bf) SelectObject(dc, bf);
        if (it->sub) {
            RECT ar = { r.right - ARROW_COL, r.top, r.right - 6, r.bottom };
            draw_arrow(dc, &ar, 3, tc);
        }
    }
    SelectObject(dc, of);
}

LRESULT CALLBACK MenuWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    Wnd *w = W_quiet(h);
    if (!w) return 0;
    switch (msg) {
    case WM_NCCREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        w->ctl = cs ? cs->lpCreateParams : NULL;
        return TRUE;
    }
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        Menu *m = M((HMENU)w->ctl);
        if (dc && m) popup_paint(m, dc, T.owner);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_NCDESTROY: {
        Menu *m = M((HMENU)w->ctl);
        if (m && m->wnd == h) m->wnd = 0;
        return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* -----------------------------------------------------------------------
 * Tracking
 * ----------------------------------------------------------------------- */
#define MAX_LEVELS 8
static struct { Menu *m; HWND wnd; } L[MAX_LEVELS];
static int g_depth;
static UINT g_flags;
static int g_result, g_done;
static int g_notify = 1;

static void send_owner(UINT msg, WPARAM wp, LPARAM lp)
{
    if (T.owner && W_quiet(T.owner->h) && g_notify) send_msg(T.owner, msg, wp, lp);
}

static void menuselect(Menu *m, int i)
{
    if (!m) { send_owner(WM_MENUSELECT, MAKEWPARAM(0, 0xFFFF), 0); return; }
    if (i < 0 || i >= m->n) return;
    MItem *it = &m->it[i];
    UINT f = it->state | (it->sub ? MF_POPUP : 0) | ((it->type & MFT_SEPARATOR) ? MF_SEPARATOR : 0) | (m->sysmenu ? MF_SYSMENU : 0) | MF_HILITE;
    send_owner(WM_MENUSELECT, MAKEWPARAM(it->sub ? (UINT)i : it->id, f), (LPARAM)m);
}

static void repaint_popup(Menu *m)
{
    if (m && m->wnd) { InvalidateRect(m->wnd, NULL, FALSE); UpdateWindow(m->wnd); }
}

static void redraw_bar(void)
{
    if (!T.owner || !T.bar) return;
    nc_paint(T.owner);
    present(top_of(T.owner));
}

static void close_levels(int keep)
{
    while (g_depth > keep) {
        g_depth--;
        Menu *m = L[g_depth].m;
        HWND wnd = L[g_depth].wnd;
        if (m) { m->sel = -1; m->wnd = 0; send_owner(WM_UNINITMENUPOPUP, (WPARAM)m, 0); }
        if (wnd && IsWindow(wnd)) DestroyWindow(wnd);
    }
    if (g_depth > 0 && L[g_depth - 1].wnd) SetCapture(L[g_depth - 1].wnd);
}

/* Open @hm as a popup at (x, y) screen; @ex: a rectangle not to cover (the parent item) */
static int open_popup(HMENU hm, int x, int y, UINT align, const RECT *ex, int index)
{
    Menu *m = M(hm);
    if (!m || g_depth >= MAX_LEVELS) return 0;
    send_owner(WM_INITMENUPOPUP, (WPARAM)hm, MAKELPARAM(index, m->sysmenu));
    if (!M(hm)) return 0;
    popup_layout(m, T.owner);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    if (align & TPM_RIGHTALIGN) x -= m->w;
    else if (align & TPM_CENTERALIGN) x -= m->w / 2;
    if (align & TPM_BOTTOMALIGN) y -= m->h;
    else if (align & TPM_VCENTERALIGN) y -= m->h / 2;
    if (x + m->w > sw) x = ex && g_depth ? ex->left - m->w + 3 : sw - m->w;
    if (x < 0) x = 0;
    if (y + m->h > sh) y = ex && !g_depth ? ex->top - m->h : sh - m->h;
    if (y < 0) y = 0;
    m->sel = -1;
    HWND wnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"#32768", NULL, WS_POPUP,
                               x, y, m->w, m->h, T.owner ? T.owner->h : 0, 0, 0, (LPVOID)hm);
    if (!wnd) return 0;
    m->wnd = wnd;
    L[g_depth].m = m; L[g_depth].wnd = wnd;
    g_depth++;
    ShowWindow(wnd, SW_SHOWNA);
    UpdateWindow(wnd);
    SetCapture(wnd);
    return 1;
}

static void select_in(int level, int i)
{
    Menu *m = L[level].m;
    if (!m || m->sel == i) return;
    m->sel = i;
    repaint_popup(m);
    menuselect(m, i);
}

static void open_sub(int level, int i)
{
    Menu *m = L[level].m;
    if (!m || i < 0 || i >= m->n || !m->it[i].sub) return;
    if (m->it[i].state & (MFS_GRAYED | MF_DISABLED)) return;
    close_levels(level + 1);
    RECT r = m->it[i].rc;
    POINT o = { 0, 0 };
    ClientToScreen(L[level].wnd, &o);
    OffsetRect(&r, o.x, o.y);
    RECT ex = { o.x, r.top, o.x + m->w, r.bottom };
    open_popup(m->it[i].sub, o.x + m->w - 3, r.top - POPUP_PAD - 1, 0, &ex, i);
}

static void bar_open(int i, int select_first)
{
    Menu *b = T.bar;
    if (!b || i < 0 || i >= b->n) return;
    close_levels(0);
    T.bar_sel = i;
    MItem *it = &b->it[i];
    T.bar_open = it->sub != 0 && !(it->state & (MFS_GRAYED | MF_DISABLED));
    redraw_bar();
    menuselect(b, i);
    if (T.bar_open) {
        RECT r;
        bar_item_screen(T.owner, i, &r);
        open_popup(it->sub, r.left, r.bottom, 0, &r, i);
        if (select_first && g_depth) {
            Menu *m = L[0].m;
            for (int k = 0; k < m->n; k++) if (!(m->it[k].type & MFT_SEPARATOR)) { select_in(0, k); break; }
        }
    }
}

static void choose(Menu *m, int i)
{
    MItem *it = &m->it[i];
    if (it->type & MFT_SEPARATOR) return;
    if (it->state & (MFS_GRAYED | MF_DISABLED)) return;
    if (it->sub) return;
    g_result = (int)it->id;
    g_done = m->sysmenu ? 2 : 1;
}

/* Which open popup (and item) is at a screen point */
static int hit_popup(POINT pt, int *item)
{
    for (int l = g_depth - 1; l >= 0; l--) {
        RECT r;
        if (!L[l].wnd || !GetWindowRect(L[l].wnd, &r) || !PtInRect(&r, pt)) continue;
        POINT c = { pt.x - r.left, pt.y - r.top };
        *item = -1;
        Menu *m = L[l].m;
        for (int i = 0; i < m->n; i++) if (PtInRect(&m->it[i].rc, c)) { *item = i; break; }
        return l;
    }
    return -1;
}

static int mnemonic(Menu *m, WCHAR c)
{
    WCHAR up = (WCHAR)(ULONG_PTR)CharUpperW((LPWSTR)(ULONG_PTR)c);
    for (int i = 0; i < m->n; i++) {
        const WCHAR *t = m->it[i].text;
        if (!t) continue;
        for (const WCHAR *p = t; *p; p++) {
            if (*p == '&' && p[1] && p[1] != '&') {
                if ((WCHAR)(ULONG_PTR)CharUpperW((LPWSTR)(ULONG_PTR)p[1]) == up) return i;
                break;
            }
            if (*p == '&' && p[1] == '&') p++;
        }
    }
    return -1;
}

static int next_item(Menu *m, int from, int dir)
{
    if (!m->n) return -1;
    int i = from;
    for (int k = 0; k < m->n; k++) {
        i = i < 0 ? (dir > 0 ? 0 : m->n - 1) : (i + dir + m->n) % m->n;
        if (!(m->it[i].type & MFT_SEPARATOR)) return i;
    }
    return -1;
}

static void key(WPARAM vk)
{
    int top = g_depth - 1;
    Menu *m = top >= 0 ? L[top].m : NULL;
    switch (vk) {
    case VK_ESCAPE:
        if (g_depth > 1 || (g_depth == 1 && !T.bar)) { close_levels(g_depth - 1); if (!g_depth && !T.bar) g_done = -1; return; }
        if (g_depth == 1 && T.bar) { close_levels(0); T.bar_open = 0; redraw_bar(); return; }
        g_done = -1;
        return;
    case VK_MENU: case VK_F10: g_done = -1; return;
    case VK_UP: case VK_DOWN:
        if (!m) { if (T.bar && T.bar_sel >= 0) bar_open(T.bar_sel, 1); return; }
        select_in(top, next_item(m, m->sel, vk == VK_DOWN ? 1 : -1));
        return;
    case VK_RIGHT:
        if (m && m->sel >= 0 && m->it[m->sel].sub) { open_sub(top, m->sel); if (g_depth > top + 1) select_in(top + 1, next_item(L[top + 1].m, -1, 1)); return; }
        if (T.bar) { int n = T.bar->n; int i = (T.bar_sel + 1) % n; if (T.bar_open || g_depth) bar_open(i, 1); else { T.bar_sel = i; redraw_bar(); } }
        return;
    case VK_LEFT:
        if (g_depth > 1) { close_levels(g_depth - 1); return; }
        if (T.bar) { int n = T.bar->n; int i = (T.bar_sel - 1 + n) % n; if (T.bar_open || g_depth) bar_open(i, 1); else { T.bar_sel = i; redraw_bar(); } }
        return;
    case VK_RETURN:
        if (m && m->sel >= 0) {
            if (m->it[m->sel].sub) { open_sub(top, m->sel); if (g_depth > top + 1) select_in(top + 1, next_item(L[top + 1].m, -1, 1)); }
            else choose(m, m->sel);
        } else if (T.bar && T.bar_sel >= 0) bar_open(T.bar_sel, 1);
        return;
    }
}

static void character(WCHAR c)
{
    int top = g_depth - 1;
    Menu *m = top >= 0 ? L[top].m : T.bar;
    if (!m) return;
    int i = mnemonic(m, c);
    if (i < 0) {
        LRESULT r = 0;
        if (T.owner) r = send_msg(T.owner, WM_MENUCHAR, MAKEWPARAM(c, m->bar ? MF_POPUP : MF_POPUP), (LPARAM)m);
        if (HIWORD(r) == MNC_EXECUTE) i = LOWORD(r);
        else if (HIWORD(r) == MNC_SELECT) { if (top >= 0) select_in(top, LOWORD(r)); return; }
        else if (HIWORD(r) == MNC_CLOSE) { g_done = -1; return; }
        else { MessageBeep(0); return; }
    }
    if (top < 0) { bar_open(i, 1); return; }
    select_in(top, i);
    if (m->it[i].sub) { open_sub(top, i); if (g_depth > top + 1) select_in(top + 1, next_item(L[top + 1].m, -1, 1)); }
    else choose(m, i);
}

static void mouse(UINT msg, POINT pt, int *moved_in)
{
    int item = -1;
    int lvl = hit_popup(pt, &item);
    if (lvl >= 0) {
        if (msg == WM_MOUSEMOVE) *moved_in = 1;
        if (item >= 0) {
            if (L[lvl].m->sel != item) {
                close_levels(lvl + 1);
                select_in(lvl, item);
                if (L[lvl].m->it[item].sub) open_sub(lvl, item);
            } else if (msg == WM_MOUSEMOVE && L[lvl].m->it[item].sub && g_depth == lvl + 1) open_sub(lvl, item);
        }
        if ((msg == WM_LBUTTONUP || msg == WM_RBUTTONUP) && item >= 0 && (*moved_in || msg == WM_LBUTTONUP)) {
            if (!L[lvl].m->it[item].sub) choose(L[lvl].m, item);
        }
        return;
    }
    if (T.bar && T.owner) {
        int b = bar_hit(T.owner, pt);
        if (b >= 0) {
            if (msg == WM_LBUTTONDOWN) {
                if (b == T.bar_sel && T.bar_open) { g_done = -1; return; }
                bar_open(b, 0);
                return;
            }
            if (msg == WM_MOUSEMOVE && b != T.bar_sel && (T.bar_open || g_depth)) bar_open(b, 0);
            return;
        }
    }
    if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_NCLBUTTONDOWN) { g_done = -1; return; }
    /* over nothing: in the deepest popup nothing is selected */
    if (msg == WM_MOUSEMOVE && g_depth && L[g_depth - 1].m->sel >= 0 && !L[g_depth - 1].m->it[L[g_depth - 1].m->sel].sub)
        select_in(g_depth - 1, -1);
}

/* The loop; returns the chosen command (0: none) */
static int track(Wnd *owner, Menu *bar, HMENU popup, int x, int y, UINT flags, int bar_item, int key_mode, const RECT *ex)
{
    if (T.active) return 0;
    HWND oh = owner ? owner->h : 0;
    memset(&T, 0, sizeof(T));
    T.active = 1;
    T.owner = owner;
    T.bar = bar;
    T.bar_sel = -1;
    g_menu_owner = owner;
    g_depth = 0;
    g_flags = flags;
    g_result = 0;
    g_done = 0;
    g_notify = !(flags & TPM_NONOTIFY);
    HWND prev_capture = GetCapture();
    if (prev_capture) ReleaseCapture();
    send_owner(WM_ENTERMENULOOP, bar ? FALSE : TRUE, 0);
    if (bar) send_owner(WM_INITMENU, (WPARAM)bar, 0);
    else send_owner(WM_INITMENU, (WPARAM)popup, 0);
    if (bar) {
        if (key_mode) { T.bar_sel = bar_item >= 0 ? bar_item : 0; redraw_bar(); if (bar_item >= 0) bar_open(bar_item, 1); }
        else bar_open(bar_item, 0);
    } else open_popup(popup, x, y, flags, ex, 0);
    int moved_in = 0;
    while (!g_done) {
        if (owner && !W_quiet(oh)) { g_done = -1; break; }
        if (!bar && !g_depth) { g_done = -1; break; }
        MSG m;
        if (!pump_one(&m, 0, 0, 0, PM_REMOVE, 1, INFINITE)) continue;
        if (m.message == WM_QUIT) { PostQuitMessage((int)m.wParam); g_done = -1; break; }
        if (CallMsgFilterW(&m, MSGF_MENU)) continue;
        if (m.message >= WM_MOUSEFIRST && m.message <= WM_MOUSELAST) {
            POINT pt = { (short)LOWORD(m.lParam), (short)HIWORD(m.lParam) };
            if (m.message != WM_MOUSEWHEEL) ClientToScreen(m.hwnd, &pt);
            mouse(m.message, pt, &moved_in);
            continue;
        }
        if (m.message >= WM_NCMOUSEMOVE && m.message <= WM_NCMBUTTONDBLCLK) {
            POINT pt = { (short)LOWORD(m.lParam), (short)HIWORD(m.lParam) };
            mouse(m.message - WM_NCMOUSEMOVE + WM_MOUSEMOVE, pt, &moved_in);
            continue;
        }
        if (m.message == WM_KEYDOWN || m.message == WM_SYSKEYDOWN) {
            TranslateMessage(&m);
            key(m.wParam);
            continue;
        }
        if (m.message == WM_CHAR || m.message == WM_SYSCHAR) { if (m.wParam > ' ') character((WCHAR)m.wParam); continue; }
        if (m.message == WM_KEYUP || m.message == WM_SYSKEYUP) {
            if ((m.wParam == VK_MENU || m.wParam == VK_F10) && m.message == WM_SYSKEYUP && key_mode && !g_depth && T.bar_open == 0) {}
            continue;
        }
        DispatchMessageW(&m);
    }
    close_levels(0);
    if (GetCapture()) ReleaseCapture();
    T.bar_sel = -1;
    T.bar_open = 0;
    T.active = 0;
    if (bar && owner && W_quiet(oh)) redraw_bar();
    menuselect(NULL, 0);
    send_owner(WM_EXITMENULOOP, bar ? FALSE : TRUE, 0);
    g_menu_owner = NULL;
    T.owner = NULL;
    T.bar = NULL;
    int r = g_done > 0 ? g_result : 0;
    if (g_done == 2) r = -r - 1;                            /* a system command */
    return r;
}

void menu_track_bar(Wnd *w, POINT pt, int key)
{
    Menu *bar = M(w->menu);
    if (!bar || !bar->n) return;
    int item = 0;
    int key_mode = 0;
    if (key == 0) item = bar_hit(w, pt);
    else if (key < 0) { key_mode = 1; item = -1; }
    else {
        key_mode = 1;
        item = mnemonic(bar, (WCHAR)key);
        if (item < 0) { MessageBeep(0); return; }
    }
    if (!key_mode && item < 0) return;
    int cmd = track(w, bar, 0, 0, 0, 0, item, key_mode, NULL);
    if (cmd > 0 && W_quiet(w->h)) PostMessageW(w->h, WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
    else if (cmd < 0 && W_quiet(w->h)) PostMessageW(w->h, WM_SYSCOMMAND, (WPARAM)(-cmd - 1), 0);
}

void menu_cancel(void) { if (T.active) g_done = -1; }

USERAPI BOOL EndMenu(void) { menu_cancel(); return TRUE; }

USERAPI BOOL TrackPopupMenuEx(HMENU hm, UINT flags, int x, int y, HWND h, LPTPMPARAMS p)
{
    Menu *m = M(hm);
    Wnd *w = W_quiet(h);
    if (!m) { SetLastError(ERROR_INVALID_MENU_HANDLE); return FALSE; }
    if (T.active) return FALSE;
    const RECT *ex = p ? &p->rcExclude : NULL;
    int cmd = track(w, NULL, hm, x, y, flags, -1, 0, ex);
    if (cmd < 0) cmd = -cmd - 1;
    if (flags & TPM_RETURNCMD) return cmd;
    if (cmd && w) PostMessageW(h, m->sysmenu ? WM_SYSCOMMAND : WM_COMMAND, (WPARAM)(m->sysmenu ? cmd : MAKEWPARAM(cmd, 0)), 0);
    return TRUE;
}

USERAPI BOOL TrackPopupMenu(HMENU hm, UINT flags, int x, int y, int r, HWND h, const RECT *rc)
{
    (void)r; (void)rc;
    return TrackPopupMenuEx(hm, flags, x, y, h, NULL);
}

USERAPI UINT GetMenuBarHeight_(HWND h) { Wnd *w = W(h); return w ? (UINT)menu_bar_height(w, w->rect.right - w->rect.left) : 0; }

int menu_translate_sys(Wnd *w, UINT msg, WPARAM wp) { (void)w; (void)msg; (void)wp; return 0; }

/* -----------------------------------------------------------------------
 * Menu resources
 * ----------------------------------------------------------------------- */
static const BYTE *parse_menu(HMENU hm, const BYTE *p, const BYTE *end)
{
    for (;;) {
        if (p + 2 > end) return end;
        WORD flags = *(const WORD *)p; p += 2;
        WORD id = 0;
        if (!(flags & MF_POPUP)) { id = *(const WORD *)p; p += 2; }
        const WCHAR *text = (const WCHAR *)p;
        while (*(const WCHAR *)p) p += 2;
        p += 2;
        if (flags & MF_POPUP) {
            HMENU sub = CreatePopupMenu();
            p = parse_menu(sub, p, end);
            AppendMenuW(hm, (flags & ~MF_END) | MF_POPUP | MF_STRING, (UINT_PTR)sub, text);
        } else if (!id && !*text && !(flags & ~MF_END)) {
            AppendMenuW(hm, MF_SEPARATOR, 0, NULL);
        } else {
            AppendMenuW(hm, (flags & ~MF_END), id, text);
        }
        if (flags & MF_END) return p;
    }
}

static const BYTE *parse_menuex(HMENU hm, const BYTE *p, const BYTE *end)
{
    for (;;) {
        if (p + 14 > end) return end;
        DWORD type = *(const DWORD *)p, state = *(const DWORD *)(p + 4), id = *(const DWORD *)(p + 8);
        WORD res = *(const WORD *)(p + 12);
        p += 14;
        const WCHAR *text = (const WCHAR *)p;
        while (*(const WCHAR *)p) p += 2;
        p += 2;
        p = (const BYTE *)(((ULONG_PTR)p + 3) & ~(ULONG_PTR)3);
        MENUITEMINFOW mi;
        memset(&mi, 0, sizeof(mi));
        mi.cbSize = sizeof(mi);
        mi.fMask = MIIM_ID | MIIM_STATE | MIIM_FTYPE | ((type & MFT_SEPARATOR) ? 0 : MIIM_STRING);
        mi.fType = type; mi.fState = state; mi.wID = id; mi.dwTypeData = (LPWSTR)text;
        if (res & 1) {
            p += 4;                                        /* help id */
            HMENU sub = CreatePopupMenu();
            p = parse_menuex(sub, p, end);
            mi.fMask |= MIIM_SUBMENU;
            mi.hSubMenu = sub;
        }
        InsertMenuItemW(hm, (UINT)GetMenuItemCount(hm), TRUE, &mi);
        if (res & 0x80) return p;
    }
}

USERAPI HMENU LoadMenuIndirectW(const void *tmpl)
{
    const BYTE *p = tmpl;
    if (!p) return 0;
    WORD ver = *(const WORD *)p, off = *(const WORD *)(p + 2);
    HMENU m = CreateMenu();
    if (ver == 0) parse_menu(m, p + 4 + off, p + 0x100000);
    else if (ver == 1) parse_menuex(m, p + 4 + off, p + 0x100000);
    return m;
}

USERAPI HMENU LoadMenuIndirectA(const void *tmpl) { return LoadMenuIndirectW(tmpl); }

USERAPI HMENU LoadMenuW(HINSTANCE inst, LPCWSTR name)
{
    DWORD size;
    const void *p = find_res(inst, name, RT_MENU, &size);
    if (!p) { SetLastError(ERROR_RESOURCE_NAME_NOT_FOUND); return 0; }
    return LoadMenuIndirectW(p);
}

USERAPI HMENU LoadMenuA(HINSTANCE inst, LPCSTR name)
{
    if ((ULONG_PTR)name < 0x10000) return LoadMenuW(inst, (LPCWSTR)name);
    WCHAR *w = a2w(name, -1);
    HMENU m = LoadMenuW(inst, w);
    free(w);
    return m;
}

/* -----------------------------------------------------------------------
 * Accelerators
 * ----------------------------------------------------------------------- */
#define ACC_MAGIC 0x41434345u
typedef struct { DWORD magic; int n; ACCEL a[1]; } AccTable;

static AccTable *ACC(HACCEL h)
{
    AccTable *t = (AccTable *)h;
    return t && handle_live(t) && t->magic == ACC_MAGIC ? t : NULL;
}

USERAPI HACCEL CreateAcceleratorTableW(LPACCEL a, int n)
{
    if (n <= 0 || !a) return 0;
    AccTable *t = malloc(sizeof(AccTable) + sizeof(ACCEL) * (size_t)n);
    if (!t) return 0;
    t->magic = ACC_MAGIC;
    handle_add(t);
    t->n = n;
    memcpy(t->a, a, sizeof(ACCEL) * (size_t)n);
    return (HACCEL)t;
}

USERAPI HACCEL CreateAcceleratorTableA(LPACCEL a, int n) { return CreateAcceleratorTableW(a, n); }
USERAPI BOOL DestroyAcceleratorTable(HACCEL h) { AccTable *t = ACC(h); if (!t) return FALSE; t->magic = 0; handle_remove(t); free(t); return TRUE; }

USERAPI int CopyAcceleratorTableW(HACCEL h, LPACCEL a, int n)
{
    AccTable *t = ACC(h);
    if (!t) return 0;
    if (!a) return t->n;
    int k = MIN(n, t->n);
    memcpy(a, t->a, sizeof(ACCEL) * (size_t)k);
    return k;
}
USERAPI int CopyAcceleratorTableA(HACCEL h, LPACCEL a, int n) { return CopyAcceleratorTableW(h, a, n); }

USERAPI HACCEL LoadAcceleratorsW(HINSTANCE inst, LPCWSTR name)
{
    DWORD size;
    const BYTE *p = find_res(inst, name, RT_ACCELERATOR, &size);
    if (!p) return 0;
    int n = (int)(size / 8);
    ACCEL *a = malloc(sizeof(ACCEL) * (size_t)(n ? n : 1));
    if (!a) return 0;
    int k = 0;
    for (int i = 0; i < n; i++) {
        WORD f = *(const WORD *)(p + i * 8), key = *(const WORD *)(p + i * 8 + 2), id = *(const WORD *)(p + i * 8 + 4);
        a[k].fVirt = (BYTE)(f & 0x7F); a[k].key = key; a[k].cmd = id;
        k++;
        if (f & 0x80) break;
    }
    HACCEL h = CreateAcceleratorTableW(a, k);
    free(a);
    return h;
}

USERAPI HACCEL LoadAcceleratorsA(HINSTANCE inst, LPCSTR name)
{
    if ((ULONG_PTR)name < 0x10000) return LoadAcceleratorsW(inst, (LPCWSTR)name);
    WCHAR *w = a2w(name, -1);
    HACCEL r = LoadAcceleratorsW(inst, w);
    free(w);
    return r;
}

USERAPI int TranslateAcceleratorW(HWND h, HACCEL ha, LPMSG m)
{
    AccTable *t = ACC(ha);
    if (!t || !m || !h) return 0;
    if (m->message != WM_KEYDOWN && m->message != WM_SYSKEYDOWN && m->message != WM_CHAR && m->message != WM_SYSCHAR) return 0;
    int shift = GetKeyState(VK_SHIFT) < 0, ctrl = GetKeyState(VK_CONTROL) < 0, alt = GetKeyState(VK_MENU) < 0;
    for (int i = 0; i < t->n; i++) {
        ACCEL *a = &t->a[i];
        int match = 0;
        if (a->fVirt & FVIRTKEY) {
            if ((m->message == WM_KEYDOWN || m->message == WM_SYSKEYDOWN) && a->key == (WORD)m->wParam &&
                !!(a->fVirt & FSHIFT) == shift && !!(a->fVirt & FCONTROL) == ctrl && !!(a->fVirt & FALT) == alt) match = 1;
        } else {
            if ((m->message == WM_CHAR || m->message == WM_SYSCHAR) && a->key == (WORD)m->wParam &&
                (!(a->fVirt & FALT) || m->message == WM_SYSCHAR)) match = 1;
        }
        if (!match) continue;
        Wnd *w = W_quiet(h);
        if (!w) return 0;
        Wnd *top = top_of(w);
        if (top->style & WS_DISABLED) return 1;
        MItem *it = top->menu ? find_item(M(top->menu), a->cmd, 0, NULL, NULL) : NULL;
        if (it) {
            send_msg(top, WM_INITMENU, (WPARAM)top->menu, 0);
            it = top->menu ? find_item(M(top->menu), a->cmd, 0, NULL, NULL) : NULL;
            if (it && (it->state & (MFS_GRAYED | MF_DISABLED))) return 1;
        }
        SendMessageW(h, WM_COMMAND, MAKEWPARAM(a->cmd, 1), 0);
        return 1;
    }
    return 0;
}

USERAPI int TranslateAcceleratorA(HWND h, HACCEL ha, LPMSG m) { return TranslateAcceleratorW(h, ha, m); }
