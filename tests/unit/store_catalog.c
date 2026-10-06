/* Host regression harness: executes the production parser and refresh state
 * machine. Only networking, persistence and the UI scroll operation are mocked. */
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#undef __always_inline
#include "../../kernel/apps/store.c"

void *kzalloc(size_t n) { return calloc(1, n); }
void kfree(void *p) { free(p); }
void kprintf(const char *fmt, ...) { (void)fmt; }
void UmDetach(UmProcess *p) { (void)p; }
void UiScrollTo(UiScroll *s, int p) { s->pos = p; }
static RamNode windows, dir, offline, saved, temporary, channel;
static bool have_cache, write_ok = true, rename_ok = true, available = true, have_channel;
static NetOp operation;
static int releases, requests, response_status = 200;
static const char *response, *redirect = "";
static UINT32 response_len;
RamNode *RamfsResolve(RamNode *cwd, const char *path)
{
    (void)cwd;
    if (!strcmp(path, "\\Windows")) return &windows;
    if (!strcmp(path, STORE_DEFAULT_PATH)) return &offline;
    if (!strcmp(path, STORE_CATALOG_PATH)) return have_cache ? &saved : NULL;
    if (!strcmp(path, STORE_CHANNEL_PATH)) return have_channel ? &channel : NULL;
    return NULL;
}
bool RamfsLoad(RamNode *f) { return f->data != NULL; }
RamNode *RamfsCreate(RamNode *parent, const char *name, bool isdir)
{
    (void)parent; (void)isdir;
    return !strcmp(name, "AppStore") ? &dir : &temporary;
}
bool RamfsWrite(RamNode *f, const char *data, UINT32 len)
{
    if (!write_ok) return false;
    free(f->data); f->data = malloc(len); assert(f->data); memcpy(f->data, data, len); f->size = len;
    return true;
}
bool RamfsRename(RamNode *n, RamNode *parent, const char *name, bool replace)
{
    (void)parent; (void)name; (void)replace;
    if (!rename_ok) return false;
    free(saved.data); saved.data = n->data; saved.size = n->size;
    n->data = NULL; n->size = 0; have_cache = true; return true;
}
bool RamfsDelete(RamNode *n) { free(n->data); n->data = NULL; n->size = 0; return true; }
bool NetAvailable(void) { return available; }
NetOp *NetResolve(const char *h) { (void)h; requests++; memset(&operation, 0, sizeof(operation)); return &operation; }
NetOp *NetHttpGetAddr(const NetIp *ip, UINT16 port, const char *host, const char *path, bool tls)
{
    (void)ip; (void)port; (void)host; (void)path; assert(tls);
    requests++; memset(&operation, 0, sizeof(operation)); return &operation;
}
void NetRelease(NetOp *op) { assert(op == &operation); releases++; }
/* Use the real URL parser from kernel/net/url.c (linked by the Python runner). */
int NetHttpParse(const NetOp *op, const char **body, UINT32 *len, char *loc, int cap)
{
    (void)op; *body = response; *len = response_len;
    snprintf(loc, (size_t)cap, "%s", redirect); return response_status;
}
static char *read_file(const char *path, UINT32 *len)
{
    FILE *f = fopen(path, "rb"); assert(f); fseek(f, 0, SEEK_END); long n = ftell(f);
    assert(n >= 0); rewind(f); char *p = malloc((size_t)n + 1); assert(p);
    assert(fread(p, 1, (size_t)n, f) == (size_t)n); fclose(f); p[n] = 0; *len = (UINT32)n; return p;
}
static void complete(Store *s, const char *body, UINT32 len)
{
    catalog_refresh(s); assert(s->catalog_op);
    operation.state = NET_DONE; assert(catalog_tick(s)); assert(s->catalog_phase == DL_FETCH);
    response = body; response_len = len; operation.state = NET_DONE;
    assert(catalog_tick(s)); assert(!s->catalog_op);
}
int main(int argc, char **argv)
{
    assert(argc >= 2); UINT32 len; char *body = read_file(argv[1], &len);
    StoreCatalog *c = store_catalog_parse(body, len);
    if (argc == 2) {
        if (!c) { free(body); return 1; }
        printf("%d\n", c->count);
        for (int i = 0; i < c->count; i++) printf("%s\n", c->apps[i].name);
        kfree(c); free(body); return 0;
    }
    assert(c); int original_count = c->count; char original_first[81] = "";
    if (original_count) strcpy(original_first, c->apps[0].name);
    kfree(c); offline.data = body; offline.size = len;
    Store s = {0}; s.dl = s.unpack_i = s.pressed = -1;
    assert(catalog_load(&s)); assert(N_APPS == original_count);
    if (original_count) assert(!strcmp(g_catalog[0].name, original_first));
    StoreCatalog *old = g_store_catalog;
    available = false; catalog_refresh(&s); assert(!s.catalog_op && g_store_catalog == old); available = true;
    s.dl = 0; catalog_refresh(&s); assert(!s.catalog_op); s.dl = -1;
    s.unpack = (UmProcess *)&s; catalog_refresh(&s); assert(!s.catalog_op); s.unpack = NULL;
    have_channel = true; channel.data = "http://example.org/list.json"; channel.size = (UINT32)strlen(channel.data);
    catalog_refresh(&s); assert(!s.catalog_op); have_channel = false;
    catalog_refresh(&s); operation.state = NET_FAILED; assert(catalog_tick(&s)); assert(g_store_catalog == old);
    complete(&s, "{}", 2); assert(g_store_catalog == old && !have_cache);
    write_ok = false; complete(&s, body, len); assert(g_store_catalog == old && !have_cache); write_ok = true;
    rename_ok = false; complete(&s, body, len); assert(g_store_catalog == old && !have_cache); rename_ok = true;
    catalog_refresh(&s); operation.state = NET_DONE; catalog_tick(&s);
    response_status = 302; redirect = "http://example.org/catalog.json"; operation.state = NET_DONE;
    assert(catalog_tick(&s)); assert(!s.catalog_op && g_store_catalog == old);
    response_status = 200; redirect = "";
    complete(&s, body, len); assert(g_store_catalog != old && have_cache);
    assert(saved.size == len && !memcmp(saved.data, body, len));
    if (original_count) assert(!strcmp(g_catalog[0].name, original_first));
    old = g_store_catalog;
    complete(&s, "{\"schema_version\":1,\"apps\":[]}", 30);
    assert(g_store_catalog != old && N_APPS == 0); /* independent list replacement */
    assert(catalog_load(&s)); assert(N_APPS == 0); /* saved copy survives reopening */
    old = g_store_catalog;
    complete(&s, "broken", 6); assert(g_store_catalog == old && N_APPS == 0);
    saved.data[0] = '!'; assert(catalog_load(&s)); assert(N_APPS == original_count); /* damaged cache -> offline fallback */
    catalog_refresh(&s); operation.state = NET_DONE; catalog_tick(&s);
    operation.len = STORE_JSON_MAX + 65537; assert(catalog_tick(&s)); assert(!s.catalog_op);
    response_status = 302; redirect = "/moved-catalog.json";
    catalog_refresh(&s); operation.state = NET_DONE; catalog_tick(&s);
    operation.state = NET_DONE; assert(catalog_tick(&s)); assert(s.catalog_op && s.catalog_phase == DL_RESOLVE);
    assert(!strcmp(s.catalog_path, redirect));
    operation.state = NET_DONE; catalog_tick(&s);
    response_status = 200; redirect = ""; response = body; response_len = len;
    operation.state = NET_DONE; assert(catalog_tick(&s)); assert(!s.catalog_op);
    Store *closing = kzalloc(sizeof(*closing)); closing->dl = closing->unpack_i = -1;
    catalog_refresh(closing); assert(closing->catalog_op);
    WND window = {0}; window.user = closing; g_store = &window;
    int before_close = releases; store_close(&window);
    assert(releases == before_close + 1 && !g_store && !window.user);
    assert(releases > 0 && requests > 0);
    kfree(g_store_catalog); g_store_catalog = NULL; free(saved.data); free(body);
    puts("refresh and offline fallback checks passed"); return 0;
}
