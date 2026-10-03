/*
 * kbd.c — keyboard layouts: what a key types (ToUnicode, ToAscii), virtual
 * keys and scan codes (MapVirtualKey, VkKeyScan), key names, and the
 * layout in effect (GetKeyboardLayout, LoadKeyboardLayout,
 * ActivateKeyboardLayout, SPI_GET/SETDEFAULTINPUTLANG)
 *
 * The layouts are kbdlayouts.h, the tables the kernel types with too
 * (kernel/wm/kbdlayout.c).  The one the user chose is published by the
 * kernel in KUSER_SHARED_DATA (its HKL at 0x7FFE0F00, a NovaOS field), so
 * a change in Settings reaches running programs at their next key.  A
 * program's ActivateKeyboardLayout chooses another for itself.
 */
#include "u32.h"
#include <wctype.h>
#include <kbdlayouts.h>

#define KUSD_KBD_HKL ((volatile const DWORD *)(ULONG_PTR)0x7FFE0F00)
#define PRELOAD_KEY  L"Keyboard Layout\\Preload"

static DWORD g_own;                    /* ActivateKeyboardLayout's choice (0: the user's) */
static WCHAR g_dead;                   /* a dead key's accent, waiting for its letter */

static const KbdLayout *by_hkl(DWORD hkl)
{
    for (int i = 0; i < KBD_NLAYOUTS; i++)
        if (kbd_layouts[i].hkl == hkl) return &kbd_layouts[i];
    return NULL;
}

static const KbdLayout *current(void)
{
    const KbdLayout *l = g_own ? by_hkl(g_own) : NULL;
    if (!l) l = by_hkl(*KUSD_KBD_HKL);
    return l ? l : &kbd_layouts[0];
}

static const KbdLayout *layout_of(HKL h)
{
    const KbdLayout *l = h ? by_hkl((DWORD)(ULONG_PTR)h) : NULL;
    return l ? l : current();
}

static HKL hkl_of(const KbdLayout *l) { return (HKL)(LONG_PTR)(LONG)l->hkl; }   /* (sign-extended, as Windows') */

static const KbdLayout *by_klid(const WCHAR *id)
{
    for (int i = 0; id && i < KBD_NLAYOUTS; i++) {
        int k = 0;
        while (id[k] && kbd_layouts[i].klid[k] && towupper(id[k]) == (WCHAR)kbd_layouts[i].klid[k]) k++;
        if (!id[k] && !kbd_layouts[i].klid[k]) return &kbd_layouts[i];
    }
    return NULL;
}

static const KbdKey *key_by_sc(const KbdLayout *l, UINT sc)
{
    for (int i = 0; i < l->nkeys; i++) if (l->keys[i].sc == sc) return &l->keys[i];
    return NULL;
}

static const KbdKey *key_by_vk(const KbdLayout *l, UINT vk)
{
    for (int i = 0; i < l->nkeys; i++) if (l->keys[i].vk == vk) return &l->keys[i];
    return NULL;
}

/* The keys no layout changes: VK -> set-1 scan code */
static const BYTE g_vk_to_sc[256] = {
    [0x1B] = 0x01, [0x08] = 0x0E, [0x09] = 0x0F, [0x0D] = 0x1C, [0x11] = 0x1D, [0x10] = 0x2A, [0x6A] = 0x37,
    [0x12] = 0x38, [0x20] = 0x39, [0x14] = 0x3A, [0x70] = 0x3B, [0x71] = 0x3C, [0x72] = 0x3D, [0x73] = 0x3E,
    [0x74] = 0x3F, [0x75] = 0x40, [0x76] = 0x41, [0x77] = 0x42, [0x78] = 0x43, [0x79] = 0x44, [0x90] = 0x45,
    [0x91] = 0x46, [0x24] = 0x47, [0x26] = 0x48, [0x21] = 0x49, [0x6D] = 0x4A, [0x25] = 0x4B, [0x0C] = 0x4C,
    [0x27] = 0x4D, [0x6B] = 0x4E, [0x23] = 0x4F, [0x28] = 0x50, [0x22] = 0x51, [0x2D] = 0x52, [0x2E] = 0x53,
    [0x7A] = 0x57, [0x7B] = 0x58, [0xA0] = 0x2A, [0xA1] = 0x36, [0xA2] = 0x1D, [0xA3] = 0x1D, [0xA4] = 0x38,
    [0xA5] = 0x38, [0x5B] = 0x5B, [0x5C] = 0x5C, [0x5D] = 0x5D,
};

/* ... and what they type */
static WCHAR fixed_char(UINT vk)
{
    switch (vk) {
    case 0x20: return ' ';
    case 0x0D: return '\r';
    case 0x09: return '\t';
    case 0x08: return '\b';
    case 0x1B: return 0x1B;
    case 0x6A: return '*';
    case 0x6B: return '+';
    case 0x6D: return '-';
    case 0x6E: return '.';
    case 0x6F: return '/';
    }
    if (vk >= 0x60 && vk <= 0x69) return (WCHAR)('0' + vk - 0x60);
    return 0;
}

/* What @k types at @level with Caps Lock @caps; *dead: an accent */
static WCHAR key_char(const KbdKey *k, int level, BOOL caps, BOOL *dead)
{
    if (caps && (k->flags & KBD_CAPS) && level < 2) level ^= 1;
    if (dead) *dead = (k->flags & KBD_DEAD(level)) != 0;
    return k->ch[level];
}

static WCHAR compose(WCHAR dead, WCHAR base)
{
    for (int i = 0; i < KBD_NCOMPOSE; i++)
        if (kbd_compose[i].dead == dead && kbd_compose[i].base == base) return kbd_compose[i].out;
    return 0;
}

/* -----------------------------------------------------------------------
 * The layout in effect
 * ----------------------------------------------------------------------- */
USERAPI HKL GetKeyboardLayout(DWORD tid) { (void)tid; return hkl_of(current()); }
USERAPI int GetKeyboardLayoutList(int n, HKL *list)
{
    if (n >= 1 && list) list[0] = GetKeyboardLayout(0);
    return 1;
}
USERAPI BOOL GetKeyboardLayoutNameW(LPWSTR name)
{
    if (!name) return FALSE;
    const char *s = current()->klid;
    for (int i = 0; i < KL_NAMELENGTH; i++) name[i] = (WCHAR)s[i];
    return TRUE;
}
USERAPI BOOL GetKeyboardLayoutNameA(LPSTR name)
{
    if (!name) return FALSE;
    memcpy(name, current()->klid, KL_NAMELENGTH);
    return TRUE;
}
USERAPI HKL ActivateKeyboardLayout(HKL h, UINT f)
{
    (void)f;
    HKL prev = GetKeyboardLayout(0);
    if ((ULONG_PTR)h == HKL_PREV || (ULONG_PTR)h == HKL_NEXT) return prev;   /* (one layout loaded) */
    const KbdLayout *l = by_hkl((DWORD)(ULONG_PTR)h);
    if (!l) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    g_own = l->hkl;
    return prev;
}
USERAPI HKL LoadKeyboardLayoutW(LPCWSTR id, UINT f)
{
    const KbdLayout *l = by_klid(id);
    if (!l) return GetKeyboardLayout(0);                    /* (Windows: the one in effect) */
    if (f & KLF_ACTIVATE) g_own = l->hkl;
    return hkl_of(l);
}
USERAPI HKL LoadKeyboardLayoutA(LPCSTR id, UINT f)
{
    WCHAR w[KL_NAMELENGTH + 1];
    int i = 0;
    for (; id && id[i] && i < KL_NAMELENGTH; i++) w[i] = (WCHAR)(BYTE)id[i];
    w[i] = 0;
    return LoadKeyboardLayoutW(w, f);
}
USERAPI BOOL UnloadKeyboardLayout(HKL h) { (void)h; return TRUE; }

/* SPI_SETDEFAULTINPUTLANG: the user's layout (HKCU\Keyboard Layout\Preload
 * "1"), which the kernel follows: the desktop and every program */
BOOL kbd_set_default(HKL hkl)
{
    const KbdLayout *l = by_hkl((DWORD)(ULONG_PTR)hkl);
    if (!l) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, PRELOAD_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL)) return FALSE;
    WCHAR v[KL_NAMELENGTH];
    for (int i = 0; i < KL_NAMELENGTH; i++) v[i] = (WCHAR)l->klid[i];
    LSTATUS r = RegSetValueExW(k, L"1", 0, REG_SZ, (const BYTE *)v, sizeof(v));
    RegCloseKey(k);
    g_own = 0;
    return r == 0;
}

/* -----------------------------------------------------------------------
 * Virtual keys and scan codes
 * ----------------------------------------------------------------------- */
static UINT vk_to_sc(const KbdLayout *l, UINT vk)
{
    const KbdKey *k = key_by_vk(l, vk & 0xFF);
    if (k) return k->sc;
    UINT sc = g_vk_to_sc[vk & 0xFF];
    return sc && !key_by_sc(l, sc) ? sc : 0;
}

static UINT sc_to_vk(const KbdLayout *l, UINT sc)
{
    const KbdKey *k = key_by_sc(l, sc & 0xFF);
    if (k) return k->vk;
    for (UINT vk = 1; vk < 256; vk++) if (g_vk_to_sc[vk] == (sc & 0xFF)) return vk;
    return 0;
}

static UINT map_vk(const KbdLayout *l, UINT code, UINT type)
{
    switch (type) {
    case MAPVK_VK_TO_VSC: return vk_to_sc(l, code);
    case MAPVK_VSC_TO_VK: case MAPVK_VSC_TO_VK_EX: return sc_to_vk(l, code);
    case MAPVK_VK_TO_CHAR: {                                /* unshifted, letters as capitals; a dead key: bit 31 */
        const KbdKey *k = key_by_vk(l, code & 0xFF);
        BOOL dead = FALSE;
        WCHAR c = k ? key_char(k, 0, FALSE, &dead) : fixed_char(code);
        if (c >= 'a' && c <= 'z') c -= 32;
        return c | (dead ? 0x80000000u : 0);
    }
    }
    return 0;
}

USERAPI UINT MapVirtualKeyW(UINT code, UINT type) { return map_vk(current(), code, type); }
USERAPI UINT MapVirtualKeyA(UINT code, UINT type) { return map_vk(current(), code, type); }
USERAPI UINT MapVirtualKeyExW(UINT code, UINT type, HKL hkl) { return map_vk(layout_of(hkl), code, type); }
USERAPI UINT MapVirtualKeyExA(UINT code, UINT type, HKL hkl) { return map_vk(layout_of(hkl), code, type); }

/* VkKeyScan: the key typing @c, with its shift state in the high byte
 * (1 Shift, 2 Ctrl, 4 Alt: AltGr is Ctrl+Alt) */
static SHORT vk_scan(const KbdLayout *l, WCHAR c)
{
    static const BYTE mods[4] = { 0, 1, 6, 7 };
    for (int level = 0; level < 4; level++)
        for (int i = 0; i < l->nkeys; i++)
            if (l->keys[i].ch[level] == c && !(l->keys[i].flags & KBD_DEAD(level)))
                return (SHORT)(l->keys[i].vk | mods[level] << 8);
    for (UINT vk = 1; vk < 256; vk++)
        if (!key_by_vk(l, vk) && fixed_char(vk) == c) return (SHORT)vk;
    return -1;
}
USERAPI SHORT VkKeyScanW(WCHAR c) { return vk_scan(current(), c); }
USERAPI SHORT VkKeyScanA(CHAR c) { return vk_scan(current(), (WCHAR)(BYTE)c); }
USERAPI SHORT VkKeyScanExW(WCHAR c, HKL hkl) { return vk_scan(layout_of(hkl), c); }
USERAPI SHORT VkKeyScanExA(CHAR c, HKL hkl) { return vk_scan(layout_of(hkl), (WCHAR)(BYTE)c); }

/* -----------------------------------------------------------------------
 * What a key types
 * ----------------------------------------------------------------------- */
/* flags bit 2 (Windows 10 1607): leave the keyboard state (a waiting dead
 * key) as it is */
USERAPI int ToUnicodeEx(UINT vk, UINT sc, const BYTE *keys, LPWSTR out, int n, UINT flags, HKL hkl)
{
    if (n < 1 || !out) return 0;
    if (sc & 0x8000) return 0;                              /* KF_UP in the scan code: a release types nothing */
    const KbdLayout *l = layout_of(hkl);
    BOOL shift = keys && (keys[VK_SHIFT] & 0x80), caps = keys && (keys[VK_CAPITAL] & 1);
    BOOL ctrl = keys && (keys[VK_CONTROL] & 0x80), alt = keys && (keys[VK_MENU] & 0x80);
    BOOL altgr = l->altgr && ctrl && alt, keep = (flags & 4) != 0;
    const KbdKey *k = key_by_vk(l, vk & 0xFF);
    WCHAR c, typed[2];
    int count = 1;
    if (k) {
        BOOL dead = FALSE;
        c = key_char(k, (shift ? 1 : 0) + (altgr ? 2 : 0), caps, &dead);
        if (ctrl && !altgr) {                               /* Ctrl+letter: control characters */
            if (alt) return 0;
            if (vk >= 'A' && vk <= 'Z') c = (WCHAR)(vk - 'A' + 1);
            else if (c == '[') c = 0x1B; else if (c == ']') c = 0x1D; else if (c == '\\') c = 0x1C;
            else return 0;
            dead = FALSE;
        }
        if (!c) return 0;
        if (dead) {
            if (g_dead == c) {                              /* the accent twice: the accent itself */
                if (!keep) g_dead = 0;
                out[0] = c;
                if (n > 1) out[1] = 0;
                return 1;
            }
            if (!keep) g_dead = c;
            out[0] = c;
            if (n > 1) out[1] = 0;
            return -1;
        }
    } else {
        if (alt && ctrl) return 0;
        c = fixed_char(vk & 0xFF);
        if (!c) return 0;
        if (c != ' ' || !g_dead) goto done;                 /* (an accent, space: the accent) */
        c = g_dead;
        if (!keep) g_dead = 0;
        goto done;
    }
    if (g_dead) {
        WCHAR a = compose(g_dead, c);
        if (a) c = a;
        else { typed[0] = g_dead; typed[1] = c; count = 2; }   /* no such letter: the accent, then the key */
        if (!keep) g_dead = 0;
    }
done:
    if (count == 2) {
        int m = n < 2 ? n : 2;
        memcpy(out, typed, m * sizeof(WCHAR));
        if (n > 2) out[2] = 0;
        return m;
    }
    out[0] = c;
    if (n > 1) out[1] = 0;
    return 1;
}
USERAPI int ToUnicode(UINT vk, UINT sc, const BYTE *keys, LPWSTR out, int n, UINT flags)
{
    return ToUnicodeEx(vk, sc, keys, out, n, flags, NULL);
}
USERAPI int ToAsciiEx(UINT vk, UINT sc, const BYTE *keys, LPWORD out, UINT flags, HKL hkl)
{
    WCHAR w[3];
    int r = ToUnicodeEx(vk, sc, keys, w, 3, flags, hkl);
    if (r && out) {
        BYTE *b = (BYTE *)out;                              /* one or two characters, a byte each */
        b[0] = (BYTE)w[0];
        if (r == 2) b[1] = (BYTE)w[1];
    }
    return r;
}
USERAPI int ToAscii(UINT vk, UINT sc, const BYTE *keys, LPWORD out, UINT flags)
{
    return ToAsciiEx(vk, sc, keys, out, flags, NULL);
}

/* -----------------------------------------------------------------------
 * Key names
 * ----------------------------------------------------------------------- */
USERAPI int GetKeyNameTextW(LONG lp, LPWSTR buf, int n)
{
    if (!buf || n < 1) return 0;
    const KbdLayout *l = current();
    UINT sc = (UINT)(lp >> 16) & 0xFF, vk = sc_to_vk(l, sc);
    const KbdKey *k = key_by_sc(l, sc);
    WCHAR name[16];
    const char *s = NULL;
    name[0] = name[1] = 0;
    if (k) {
        name[0] = towupper(k->ch[0]);
    } else if (vk == 0x20) s = "Space";
    else if (vk == 0x0D) s = "Enter";
    else if (vk == 0x1B) s = "Esc";
    else if (vk == 0x08) s = "Backspace";
    else if (vk == 0x09) s = "Tab";
    else if (vk == 0x10) s = "Shift";
    else if (vk == 0x11) s = "Ctrl";
    else if (vk == 0x12) s = "Alt";
    else if (vk == 0x14) s = "Caps Lock";
    else if (vk >= 0x70 && vk <= 0x7B) {
        name[0] = 'F';
        int f = (int)vk - 0x6F;
        if (f >= 10) { name[1] = '1'; name[2] = (WCHAR)('0' + f - 10); name[3] = 0; }
        else { name[1] = (WCHAR)('0' + f); name[2] = 0; }
    } else if (fixed_char(vk) > ' ' && fixed_char(vk) < 0x7F) name[0] = fixed_char(vk);
    if (s) { int i = 0; for (; s[i] && i < 15; i++) name[i] = (WCHAR)s[i]; name[i] = 0; }
    if (!name[0]) return 0;
    int len = (int)wcslen(name);
    if (len > n - 1) len = n - 1;
    memcpy(buf, name, len * sizeof(WCHAR));
    buf[len] = 0;
    return len;
}
USERAPI int GetKeyNameTextA(LONG lp, LPSTR buf, int n)
{
    WCHAR w[16];
    if (!buf || n < 1 || !GetKeyNameTextW(lp, w, 16)) return 0;
    int r = WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, n, NULL, NULL);
    return r > 0 ? r - 1 : 0;
}
