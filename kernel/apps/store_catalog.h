/* Bounded JSON catalog reader. A candidate owns its decoded strings and is
 * published only after the entire document and every install path validate. */
#pragma once
#define STORE_MAX_APPS 256
#define STORE_JSON_MAX (512u * 1024u)
#define STORE_CATALOG_PATH "\\Windows\\AppStore\\catalog.json"
#define STORE_DEFAULT_PATH "\\Windows\\AppStore\\default-catalog.json"
#define STORE_CHANNEL_PATH "\\Windows\\AppStore\\catalog-url.txt"
#define STORE_CHANNEL "https://raw.githubusercontent.com/dean-plude/os/main/userland/store/catalog.json"

typedef struct {
    int count;
    StoreApp apps[STORE_MAX_APPS];
    char text[];
} StoreCatalog;

typedef struct { char *p, *end; } StoreJson;
static void sj_space(StoreJson *j)
{
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\r' || *j->p == '\n' || *j->p == '\t')) j->p++;
}
static bool sj_take(StoreJson *j, char c)
{
    sj_space(j);
    if (j->p == j->end || *j->p != c) return false;
    j->p++; return true;
}
static int sj_hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static bool sj_u16(StoreJson *j, unsigned *u)
{
    *u = 0;
    for (int i = 0; i < 4; i++) {
        if (j->p == j->end) return false;
        int h = sj_hex(*j->p++);
        if (h < 0) return false;
        *u = (*u << 4) | (unsigned)h;
    }
    return true;
}
static char *sj_string(StoreJson *j)
{
    if (!sj_take(j, '"')) return NULL;
    char *start = j->p, *out = start;
    while (j->p < j->end) {
        unsigned char c = (unsigned char)*j->p++;
        if (c == '"') { *out = 0; return start; }
        if (c < 32) return NULL;
        if (c != '\\') {
            *out++ = (char)c;
            if (c >= 128) {
                int extra = c >= 0xC2 && c <= 0xDF ? 1 : c >= 0xE0 && c <= 0xEF ? 2 : c >= 0xF0 && c <= 0xF4 ? 3 : -1;
                if (extra < 0 || j->end - j->p < extra) return NULL;
                unsigned char next = (unsigned char)*j->p;
                if ((c == 0xE0 && next < 0xA0) || (c == 0xED && next > 0x9F) ||
                    (c == 0xF0 && next < 0x90) || (c == 0xF4 && next > 0x8F)) return NULL;
                while (extra--) {
                    next = (unsigned char)*j->p++;
                    if ((next & 0xC0) != 0x80) return NULL;
                    *out++ = (char)next;
                }
            }
            continue;
        }
        if (j->p == j->end) return NULL;
        c = (unsigned char)*j->p++;
        switch (c) {
        case '"': case '\\': case '/': *out++ = (char)c; break;
        case 'b': *out++ = '\b'; break;
        case 'f': *out++ = '\f'; break;
        case 'n': *out++ = '\n'; break;
        case 'r': *out++ = '\r'; break;
        case 't': *out++ = '\t'; break;
        case 'u': {
            unsigned u, low;
            if (!sj_u16(j, &u) || !u) return NULL;
            if (u >= 0xD800 && u <= 0xDBFF) {
                if (j->end - j->p < 6 || j->p[0] != '\\' || j->p[1] != 'u') return NULL;
                j->p += 2;
                if (!sj_u16(j, &low) || low < 0xDC00 || low > 0xDFFF) return NULL;
                u = 0x10000 + ((u - 0xD800) << 10) + low - 0xDC00;
            } else if (u >= 0xDC00 && u <= 0xDFFF) return NULL;
            if (u < 0x80) *out++ = (char)u;
            else if (u < 0x800) { *out++ = (char)(0xC0 | (u >> 6)); *out++ = (char)(0x80 | (u & 63)); }
            else if (u < 0x10000) {
                *out++ = (char)(0xE0 | (u >> 12)); *out++ = (char)(0x80 | ((u >> 6) & 63)); *out++ = (char)(0x80 | (u & 63));
            } else {
                *out++ = (char)(0xF0 | (u >> 18)); *out++ = (char)(0x80 | ((u >> 12) & 63));
                *out++ = (char)(0x80 | ((u >> 6) & 63)); *out++ = (char)(0x80 | (u & 63));
            }
            break; }
        default: return NULL;
        }
    }
    return NULL;
}
static bool sj_literal(StoreJson *j, const char *s)
{
    sj_space(j);
    size_t n = strlen(s);
    if ((size_t)(j->end - j->p) < n || memcmp(j->p, s, n)) return false;
    j->p += n; return true;
}
static bool sj_uint(StoreJson *j, unsigned *n)
{
    sj_space(j); *n = 0;
    if (j->p == j->end || *j->p < '0' || *j->p > '9') return false;
    bool zero = *j->p == '0';
    do {
        unsigned d = (unsigned)(*j->p++ - '0');
        if (*n > (0xFFFFFFFFu - d) / 10) return false;
        *n = *n * 10 + d;
        if (zero) break;
    } while (j->p < j->end && *j->p >= '0' && *j->p <= '9');
    return true;
}
/* Unknown metadata is ignored, but its JSON syntax must still be valid. */
static bool sj_skip(StoreJson *j, int depth)
{
    sj_space(j);
    if (depth > 16 || j->p == j->end) return false;
    if (*j->p == '"') return sj_string(j) != NULL;
    if (*j->p == '{' || *j->p == '[') {
        bool object = *j->p++ == '{'; char end = object ? '}' : ']';
        if (sj_take(j, end)) return true;
        do {
            if (object && (!sj_string(j) || !sj_take(j, ':'))) return false;
            if (!sj_skip(j, depth + 1)) return false;
            if (sj_take(j, end)) return true;
        } while (sj_take(j, ','));
        return false;
    }
    if (*j->p == 't') return sj_literal(j, "true");
    if (*j->p == 'f') return sj_literal(j, "false");
    if (*j->p == 'n') return sj_literal(j, "null");
    if (*j->p == '-') j->p++;
    /* Skip JSON numbers without imposing the known fields' integer range. */
    if (j->p == j->end || *j->p < '0' || *j->p > '9') return false;
    if (*j->p == '0') j->p++;
    else while (j->p < j->end && *j->p >= '0' && *j->p <= '9') j->p++;
    if (j->p < j->end && *j->p == '.') {
        j->p++; char *s = j->p;
        while (j->p < j->end && *j->p >= '0' && *j->p <= '9') j->p++;
        if (s == j->p) return false;
    }
    if (j->p < j->end && (*j->p == 'e' || *j->p == 'E')) {
        j->p++;
        if (j->p < j->end && (*j->p == '+' || *j->p == '-')) j->p++;
        char *s = j->p;
        while (j->p < j->end && *j->p >= '0' && *j->p <= '9') j->p++;
        if (s == j->p) return false;
    }
    return true;
}
static bool sc_text(const char *s, size_t limit)
{
    if (!s || !*s || strlen(s) > limit) return false;
    for (; *s; s++) if ((unsigned char)*s < 32 || (unsigned char)*s == 127) return false;
    return true;
}
static bool sc_url(const char *url)
{
    char host[128], path[512]; UINT16 port; bool https;
    return sc_text(url, 500) && !strchr(url, ' ') && !strncmp(url, "https://", 8) &&
           NetParseUrl(url, host, sizeof(host), &port, path, sizeof(path), &https) && https;
}
static bool sc_path(const char *s, bool single, bool absolute)
{
    if (!sc_text(s, single ? 180 : 220) || strchr(s, '"') || strchr(s, ':')) return false;
    if (single) for (const char *p = s; *p; p++) if (strchr("\\/<>|*?", *p)) return false;
    if (!absolute && (*s == '\\' || *s == '/')) return false;
    for (const char *p = s; *p;) {
        while (*p == '\\' || *p == '/') p++;
        const char *start = p;
        while (*p && *p != '\\' && *p != '/') p++;
        size_t n = (size_t)(p - start);
        if ((n == 1 && start[0] == '.') || (n == 2 && start[0] == '.' && start[1] == '.')) return false;
    }
    return true;
}
static bool sc_system(const char *s, const char *dest)
{
    if (!s) return true;
    if (!sc_text(s, 480) || !dest) return false;
    while (*s) {
        char token[RAMFS_PATH_MAX]; int n = 0;
        while (*s && *s != ' ') { if (n == (int)sizeof(token) - 1) return false; token[n++] = *s++; }
        token[n] = 0;
        if (*s) s++;
        char *as = strchr(token, '>');
        if (as) { *as++ = 0; if (strchr(as, '>') || !sc_path(as, true, false)) return false; }
        /* Passed to 7-Zip without quotes: no options, whitespace or metacharacters. */
        for (char *p = token; *p; p++)
            if (!( (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                   (*p >= '0' && *p <= '9') || strchr("\\._-*", *p))) return false;
        if (token[0] == '-' || !sc_path(token, false, false) || strlen(dest) + strlen(token) > 220) return false;
    }
    return true;
}
static bool sc_equal(const char *a, const char *b)
{
    while (*a && (*a | 0x20) == (*b | 0x20)) { a++; b++; }
    return !*a && !*b;
}
static bool sc_app(StoreJson *j, StoreApp *a)
{
    static const char *keys[] = { "name", "publisher", "summary", "category", "url", "file", "dest", "exe",
                                 "kind", "size_mb", "note", "label", "color", "system" };
    const char *values[14] = {0}; unsigned seen = 0;
    if (!sj_take(j, '{')) return false;
    do {
        char *key = sj_string(j); if (!key || !sj_take(j, ':')) return false;
        int k = 0; while (k < 14 && strcmp(key, keys[k])) k++;
        if (k == 14) { if (!sj_skip(j, 0)) return false; }
        else {
            if (seen & (1u << k)) return false;
            seen |= 1u << k;
            if (k == 9) { if (!sj_uint(j, &a->size_mb) || a->size_mb > 4096) return false; }
            else if ((k == 6 || k == 7 || k == 13) && sj_literal(j, "null")) values[k] = NULL;
            else if (!(values[k] = sj_string(j))) return false;
        }
        if (sj_take(j, '}')) break;
        if (!sj_take(j, ',')) return false;
    } while (true);
    const unsigned required = 0x1FFFu & ~((1u << 6) | (1u << 7));
    if ((seen & required) != required) return false;
    a->name=values[0]; a->publisher=values[1]; a->summary=values[2]; a->url=values[4];
    a->file=values[5]; a->dest=values[6]; a->exe=values[7]; a->note=values[10]; a->label=values[11]; a->system=values[13];
    static const char *categories[] = { "", "utilities", "internet", "media", "graphics", "office", "developer", "runtimes" };
    a->category = 1;
    while (a->category < 8 && strcmp(values[3], categories[a->category])) a->category++;
    if (a->category == 8) return false;
    if (!strcmp(values[8], "setup")) a->kind = KIND_SETUP;
    else if (!strcmp(values[8], "portable")) a->kind = KIND_PORTABLE;
    else if (!strcmp(values[8], "archive")) a->kind = KIND_ARCHIVE;
    else return false;
    if (!sc_text(a->name, 80) || !sc_text(a->publisher, 100) || !sc_text(a->summary, 320) ||
        !sc_text(a->note, 512) || !sc_text(a->label, 4) || !sc_url(a->url) ||
        !sc_path(a->file, true, false) || strchr(a->file, '*') || strchr(a->file, '?') ||
        (a->dest && (!sc_path(a->dest, true, false) || strchr(a->dest, '*') || strchr(a->dest, '?'))) ||
        (a->exe && !sc_path(a->exe, false, true)) ||
        (a->kind == KIND_ARCHIVE && !a->dest) || (a->system && a->kind != KIND_ARCHIVE) || !sc_system(a->system, a->dest)) return false;
    const char *color = values[12];
    if (strlen(color) != 7 || color[0] != '#') return false;
    unsigned rgb = 0;
    for (int i = 1; i < 7; i++) { int h = sj_hex(color[i]); if (h < 0) return false; rgb = (rgb << 4) | (unsigned)h; }
    a->color = GDI_C((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
    return true;
}
static StoreCatalog *store_catalog_parse(const char *data, UINT32 len)
{
    if (!data || !len || len > STORE_JSON_MAX || memchr(data, 0, len)) return NULL;
    StoreCatalog *c = kzalloc(sizeof(*c) + len + 1); if (!c) return NULL;
    memcpy(c->text, data, len);
    StoreJson j = { c->text, c->text + len }; unsigned seen = 0, version = 0;
    if (!sj_take(&j, '{')) goto bad;
    do {
        char *key = sj_string(&j); if (!key || !sj_take(&j, ':')) goto bad;
        if (!strcmp(key, "schema_version")) {
            if ((seen & 1) || !sj_uint(&j, &version) || version != 1) goto bad;
            seen |= 1;
        } else if (!strcmp(key, "apps")) {
            if ((seen & 2) || !sj_take(&j, '[')) goto bad;
            seen |= 2;
            if (!sj_take(&j, ']')) do {
                if (c->count == STORE_MAX_APPS || !sc_app(&j, &c->apps[c->count])) goto bad;
                for (int i = 0; i < c->count; i++)
                    if (sc_equal(c->apps[i].name, c->apps[c->count].name) ||
                        sc_equal(c->apps[i].file, c->apps[c->count].file)) goto bad;
                c->count++;
                if (sj_take(&j, ']')) break;
                if (!sj_take(&j, ',')) goto bad;
            } while (true);
        } else if (!sj_skip(&j, 0)) goto bad;
        if (sj_take(&j, '}')) break;
        if (!sj_take(&j, ',')) goto bad;
    } while (true);
    sj_space(&j);
    if (seen != 3 || j.p != j.end) goto bad;
    return c;
bad:
    kfree(c); return NULL;
}
