/*
 * clipboard.c — the system clipboard
 *
 * One clipboard for every program (through NtNovaClipboard, which user32
 * uses) and the built-in apps.  Each format is a copy of its bytes: text
 * is kept as the program gave it, and CF_TEXT / CF_OEMTEXT (UTF-8, the
 * ANSI code page here) and CF_UNICODETEXT are converted into each other
 * when asked for.  Formats a program registered are known by name, since
 * their numbers differ from program to program.
 */

#include "clipboard.h"
#include "../ke/spinlock.h"
#include "../mm/vmm.h"
#include "../lib/string.h"

#define MAX_FMTS 24

typedef struct { UINT32 fmt; char name[CLIP_NAME_MAX]; UINT8 *data; UINT32 size; } Fmt;

static Fmt       g_fmt[MAX_FMTS];
static int       g_n;
static UINT32    g_seq = 1;
static UINT64    g_owner;
static KSpinLock g_lock = KSPINLOCK_INIT;

static bool same(const Fmt *f, UINT32 fmt, const char *name)
{
    if (fmt < 0xC000) return f->fmt == fmt;
    return f->fmt >= 0xC000 && name && !strcmp(f->name, name);
}

void ClipEmpty(UINT64 owner)
{
    UINT8 *old[MAX_FMTS];
    int n;
    spin_lock(&g_lock);
    n = g_n;
    for (int i = 0; i < n; i++) old[i] = g_fmt[i].data;
    g_n = 0;
    g_seq++;
    g_owner = owner;
    spin_unlock(&g_lock);
    for (int i = 0; i < n; i++) kfree(old[i]);
}

bool ClipPut(UINT32 fmt, const char *name, UINT8 *data, UINT32 size)
{
    UINT8 *old = NULL;
    bool ok = true;
    spin_lock(&g_lock);
    int i = 0;
    while (i < g_n && !same(&g_fmt[i], fmt, name)) i++;
    if (i < g_n) old = g_fmt[i].data;
    else if (g_n < MAX_FMTS) {
        i = g_n++;
        g_fmt[i].fmt = fmt;
        g_fmt[i].name[0] = 0;
        if (fmt >= 0xC000 && name) {
            strncpy(g_fmt[i].name, name, CLIP_NAME_MAX - 1);
            g_fmt[i].name[CLIP_NAME_MAX - 1] = 0;
        }
    } else ok = false;
    if (ok) { g_fmt[i].data = data; g_fmt[i].size = size; g_seq++; }
    spin_unlock(&g_lock);
    kfree(old);
    if (!ok) kfree(data);
    return ok;
}

/* -----------------------------------------------------------------------
 * Text conversions
 * ----------------------------------------------------------------------- */
/* UTF-16 (up to a NUL or @units) -> UTF-8 with a NUL; bytes written, or
 * the size needed (NUL included) when @out is NULL */
static UINT32 w_to_u8(const UINT8 *w, UINT32 units, UINT8 *out, UINT32 cap)
{
    UINT32 o = 0;
    for (UINT32 i = 0; i < units; i++) {
        UINT32 c = (UINT32)w[2 * i] | (UINT32)w[2 * i + 1] << 8;
        if (!c) break;
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < units) {
            UINT32 d = (UINT32)w[2 * i + 2] | (UINT32)w[2 * i + 3] << 8;
            if (d >= 0xDC00 && d < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00); i++; }
        }
        UINT8 t[4];
        int k = 0;
        if (c < 0x80) t[k++] = (UINT8)c;
        else if (c < 0x800) { t[k++] = (UINT8)(0xC0 | c >> 6); t[k++] = (UINT8)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { t[k++] = (UINT8)(0xE0 | c >> 12); t[k++] = (UINT8)(0x80 | ((c >> 6) & 0x3F)); t[k++] = (UINT8)(0x80 | (c & 0x3F)); }
        else { t[k++] = (UINT8)(0xF0 | c >> 18); t[k++] = (UINT8)(0x80 | ((c >> 12) & 0x3F)); t[k++] = (UINT8)(0x80 | ((c >> 6) & 0x3F)); t[k++] = (UINT8)(0x80 | (c & 0x3F)); }
        for (int j = 0; j < k; j++) { if (out && o < cap) out[o] = t[j]; o++; }
    }
    if (out && o < cap) out[o] = 0;
    return o + 1;
}

/* UTF-8 (up to a NUL or @n bytes) -> UTF-16 with a NUL; bytes */
static UINT32 u8_to_w(const UINT8 *s, UINT32 n, UINT8 *out, UINT32 cap)
{
    UINT32 o = 0;
    for (UINT32 i = 0; i < n && s[i]; ) {
        UINT32 c = s[i++];
        if (c >= 0xC0) {
            int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
            c &= 0x3F >> more;
            while (more-- && i < n && (s[i] & 0xC0) == 0x80) c = c << 6 | (s[i++] & 0x3F);
        }
        UINT16 u[2];
        int k = 0;
        if (c >= 0x10000) { c -= 0x10000; u[k++] = (UINT16)(0xD800 + (c >> 10)); u[k++] = (UINT16)(0xDC00 + (c & 0x3FF)); }
        else u[k++] = (UINT16)c;
        for (int j = 0; j < k; j++) {
            if (out && o + 2 <= cap) { out[o] = (UINT8)u[j]; out[o + 1] = (UINT8)(u[j] >> 8); }
            o += 2;
        }
    }
    if (out && o + 2 <= cap) { out[o] = 0; out[o + 1] = 0; }
    return o + 2;
}

static bool is_text(UINT32 f) { return f == CLIP_CF_TEXT || f == CLIP_CF_OEMTEXT || f == CLIP_CF_UNICODETEXT; }

int ClipGet(UINT32 fmt, const char *name, UINT8 *out, UINT32 cap)
{
    spin_lock(&g_lock);
    int found = -1, text = -1;
    for (int i = 0; i < g_n; i++) {
        if (same(&g_fmt[i], fmt, name)) found = i;
        if (is_text(g_fmt[i].fmt) && (text < 0 || g_fmt[i].fmt == CLIP_CF_UNICODETEXT)) text = i;
    }
    int r = -1;
    if (found >= 0) {
        Fmt *f = &g_fmt[found];
        r = (int)f->size;
        if (out) memcpy(out, f->data, f->size < cap ? f->size : cap);
    } else if (is_text(fmt) && text >= 0) {                  /* converted */
        Fmt *f = &g_fmt[text];
        bool want_w = fmt == CLIP_CF_UNICODETEXT, have_w = f->fmt == CLIP_CF_UNICODETEXT;
        if (want_w == have_w) {
            r = (int)f->size;
            if (out) memcpy(out, f->data, f->size < cap ? f->size : cap);
        } else if (want_w) r = (int)u8_to_w(f->data, f->size, out, cap);
        else r = (int)w_to_u8(f->data, f->size / 2, out, cap);
    } else if (fmt == CLIP_CF_LOCALE && text >= 0) {         /* en-US */
        r = 4;
        if (out && cap >= 4) { out[0] = 0x09; out[1] = 0x04; out[2] = 0; out[3] = 0; }
    }
    spin_unlock(&g_lock);
    return r;
}

int ClipList(ClipEntry *out, int max)
{
    spin_lock(&g_lock);
    int n = 0;
    bool text = false, has[3] = { false, false, false };
    for (int i = 0; i < g_n && n < max; i++) {
        out[n].fmt = g_fmt[i].fmt;
        strcpy(out[n].name, g_fmt[i].name);
        out[n].size = g_fmt[i].size;
        n++;
        if (is_text(g_fmt[i].fmt)) {
            text = true;
            has[g_fmt[i].fmt == CLIP_CF_TEXT ? 0 : g_fmt[i].fmt == CLIP_CF_OEMTEXT ? 1 : 2] = true;
        }
    }
    static const UINT32 tf[3] = { CLIP_CF_TEXT, CLIP_CF_OEMTEXT, CLIP_CF_UNICODETEXT };
    for (int k = 0; text && k < 3 && n < max; k++) {         /* what text can become */
        if (has[k]) continue;
        out[n].fmt = tf[k];
        out[n].name[0] = 0;
        out[n].size = 0;
        n++;
    }
    if (text && n < max) { out[n].fmt = CLIP_CF_LOCALE; out[n].name[0] = 0; out[n].size = 4; n++; }
    spin_unlock(&g_lock);
    return n;
}

UINT32 ClipSequence(void) { return __atomic_load_n(&g_seq, __ATOMIC_ACQUIRE); }
UINT64 ClipOwner(void)    { return g_owner; }

/* -----------------------------------------------------------------------
 * The built-in apps
 * ----------------------------------------------------------------------- */
void ClipSetText(const char *utf8, UINT32 len)
{
    UINT32 bytes = u8_to_w((const UINT8 *)utf8, len, NULL, 0);
    UINT8 *w = kmalloc(bytes);
    if (!w) return;
    u8_to_w((const UINT8 *)utf8, len, w, bytes);
    ClipEmpty(0);
    ClipPut(CLIP_CF_UNICODETEXT, NULL, w, bytes);
}

char *ClipGetText(UINT32 *len)
{
    int need = ClipGet(CLIP_CF_TEXT, NULL, NULL, 0);
    if (need <= 0) return NULL;
    char *s = kmalloc((UINT32)need + 1);
    if (!s) return NULL;
    int got = ClipGet(CLIP_CF_TEXT, NULL, (UINT8 *)s, (UINT32)need);
    if (got > need) got = need;
    s[got > 0 ? got : 0] = 0;
    UINT32 n = (UINT32)strlen(s);
    if (len) *len = n;
    return s;
}

/* DROPFILES: { DWORD pFiles; POINT pt; BOOL fNC; BOOL fWide } then the
 * paths (UTF-16), each ended by NUL, and one more NUL */
void ClipSetFiles(const char *const *paths, int n)
{
    UINT32 bytes = 20 + 2;
    for (int i = 0; i < n; i++) bytes += u8_to_w((const UINT8 *)paths[i], (UINT32)strlen(paths[i]), NULL, 0);
    UINT8 *d = kzalloc(bytes);
    if (!d) return;
    d[0] = 20;                                   /* pFiles */
    d[16] = 1;                                   /* fWide */
    UINT32 o = 20;
    for (int i = 0; i < n; i++) o += u8_to_w((const UINT8 *)paths[i], (UINT32)strlen(paths[i]), d + o, bytes - o);
    ClipEmpty(0);
    ClipPut(CLIP_CF_HDROP, NULL, d, bytes);
    /* "Preferred DropEffect": copy (1) */
    UINT8 *eff = kzalloc(4);
    if (eff) { eff[0] = 1; ClipPut(0xC000, "Preferred DropEffect", eff, 4); }
}

bool ClipGetFile(int idx, char *out, int cap)
{
    int size = ClipGet(CLIP_CF_HDROP, NULL, NULL, 0);
    if (size < 20) return false;
    UINT8 *d = kmalloc((UINT32)size);
    if (!d) return false;
    ClipGet(CLIP_CF_HDROP, NULL, d, (UINT32)size);
    UINT32 off = (UINT32)d[0] | (UINT32)d[1] << 8 | (UINT32)d[2] << 16 | (UINT32)d[3] << 24;
    bool wide = d[16] != 0, ok = false;
    UINT32 o = off;
    for (int i = 0; o < (UINT32)size; i++) {
        UINT32 start = o, len = 0;
        if (wide) { while (o + 1 < (UINT32)size && (d[o] || d[o + 1])) { o += 2; len++; } }
        else { while (o < (UINT32)size && d[o]) { o++; len++; } }
        if (!len) break;
        if (i == idx) {
            if (wide) w_to_u8(d + start, len, (UINT8 *)out, (UINT32)cap);
            else { UINT32 k = len < (UINT32)cap - 1 ? len : (UINT32)cap - 1; memcpy(out, d + start, k); out[k] = 0; }
            ok = true;
            break;
        }
        o += wide ? 2 : 1;
    }
    kfree(d);
    return ok;
}
