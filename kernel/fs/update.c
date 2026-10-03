/*
 * update.c — updating an installed NovaOS (see update.h)
 *
 * Checking and installing run on a thread of their own ("update"), one
 * request at a time; the App Store and the Terminal's `update` start them
 * and show UpdateGetStatus.  Finishing an update the boot loader started
 * runs on another, once the desktop is up (UpdateBootDone).
 */

#include "update.h"
#include "fat.h"
#include "persist.h"
#include "setup.h"
#include "../net/net.h"
#include "../mm/vmm.h"
#include "../lib/string.h"
#include "../ke/printf.h"
#include "../ke/scheduler.h"
#include "../um/um.h"
#include "mbedtls/sha256.h"

#define NOVA_DIR     "\\EFI\\NOVA"
#define BOOT_DIR     "\\EFI\\BOOT"
/* in NOVA_DIR (the boot loader knows these names too) */
#define F_KERNEL     "kernel.elf"
#define F_NEW        "kernel.new"
#define F_OLD        "kernel.old"
#define F_LOADER_NEW "bootx64.new"
#define F_PENDING    "update.pnd"
#define F_TRYING     "update.try"
#define F_INFO       "update.txt"        /* "version X" and "from Y" of the staged update */
#define F_LOADER     "BOOTX64.EFI"       /* in BOOT_DIR */

static UINT64 g_boot_flags;
static char   g_notice[160];

static volatile UpdateStatus g_st;
static volatile bool g_busy;
static bool   g_want_install;            /* the request: check, then install if newer */

/* What the last check found */
typedef struct {
    char   url[512];
    UINT64 size;
    UINT8  sha[32];
    bool   present;
} UpdFile;
static UpdFile g_kernel, g_loader;
static char    g_channel_used[512];      /* the channel the last check read */

/* ---------------------------------------------------------------------------
 * Status
 * ------------------------------------------------------------------------- */
void UpdateGetStatus(UpdateStatus *out)
{
    memcpy(out, (const void *)&g_st, sizeof(*out));
    out->busy = g_busy;
}

static void set_state(UpdateState s) { g_st.state = s; }

static void step(const char *text)
{
    strncpy((char *)g_st.step, text, sizeof(g_st.step) - 1);
    g_st.step[sizeof(g_st.step) - 1] = '\0';
    kprintf("[UPDATE] %s\n", text);
}

static void fail(const char *fmt, ...)
{
    char buf[sizeof(g_st.error)];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    kvsnprintf(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);
    memcpy((char *)g_st.error, buf, sizeof(buf));
    kprintf("[UPDATE] Failed: %s\n", buf);
    set_state(UPDATE_FAILED);
}

const char *UpdateBootNotice(void) { return g_notice; }

/* ---------------------------------------------------------------------------
 * The channel
 * ------------------------------------------------------------------------- */
void UpdateGetChannel(char *out, int cap)
{
    if (!um_registry_get_sz(UPDATE_KEY, "Channel", out, cap) || !out[0]) {
        strncpy(out, UPDATE_DEFAULT_CHANNEL, (size_t)cap - 1);
        out[cap - 1] = '\0';
    }
}

void UpdateSetChannel(const char *url)
{
    um_registry_set_sz(UPDATE_KEY, "Channel", url ? url : "");
    kprintf("[UPDATE] Channel: %s\n", url && *url ? url : UPDATE_DEFAULT_CHANNEL " (the default)");
}

/* ---------------------------------------------------------------------------
 * Downloading (on the update thread: it waits for the network)
 * ------------------------------------------------------------------------- */
/* GET @url, following redirects; the finished operation (status 200, the
 * body in *body) or NULL with the error set.  The caller releases it. */
static NetOp *fetch(const char *url, const char **body, UINT32 *blen, bool progress)
{
    char host[128], path[512], loc[512];
    UINT16 port;
    bool https;
    if (!NetParseUrl(url, host, sizeof(host), &port, path, sizeof(path), &https)) { fail("Bad address: %s", url); return NULL; }
    for (int redirects = 0; redirects < 8; redirects++) {
        NetOp *op = NetResolve(host);
        if (!op) { fail("The network is busy."); return NULL; }
        while (op->state == NET_PENDING) sched_sleep_tick();
        if (op->state == NET_FAILED) { fail("Could not find %s (%s).", host, op->error[0] ? op->error : "no answer"); NetRelease(op); return NULL; }
        NetIp ip = op->addr;
        NetRelease(op);
        op = NetHttpGetAddr(&ip, port, host, path, https);
        if (!op) { fail("The network is busy."); return NULL; }
        while (op->state == NET_PENDING) {
            if (progress) g_st.got = op->len;
            sched_sleep_tick();
        }
        if (op->state == NET_FAILED) { fail("Downloading from %s failed: %s.", host, op->error[0] ? op->error : "connection lost"); NetRelease(op); return NULL; }
        int status = NetHttpParse(op, body, blen, loc, sizeof(loc));
        if (status >= 300 && status < 400 && loc[0]) {
            NetRelease(op);
            if (loc[0] == '/') { strncpy(path, loc, sizeof(path) - 1); path[sizeof(path) - 1] = '\0'; }
            else if (!NetParseUrl(loc, host, sizeof(host), &port, path, sizeof(path), &https)) { fail("Bad redirect from %s.", host); return NULL; }
            continue;
        }
        if (status != 200) { fail("%s replied %d to %s.", host, status, path); NetRelease(op); return NULL; }
        return op;
    }
    fail("Too many redirects for %s.", url);
    return NULL;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void sha256(const void *data, UINT64 len, UINT8 out[32])
{
    mbedtls_sha256((const unsigned char *)data, (size_t)len, out, 0);
}

/* @name relative to the channel's URL (or a full URL) into @out */
static void resolve(const char *channel, const char *name, char *out, int cap)
{
    if (strstr(name, "://")) { strncpy(out, name, (size_t)cap - 1); out[cap - 1] = '\0'; return; }
    const char *q = strchr(channel, '?');
    int end = q ? (int)(q - channel) : (int)strlen(channel);
    while (end > 0 && channel[end - 1] != '/') end--;
    if (end > cap - 1) end = cap - 1;
    memcpy(out, channel, (size_t)end);
    out[end] = '\0';
    strncpy(out + end, name, (size_t)(cap - 1 - end));
    out[cap - 1] = '\0';
}

/* "kernel NAME SIZE SHA256": false if malformed */
static bool parse_file(const char *channel, char *rest, UpdFile *f)
{
    char *name = rest, *p = rest;
    while (*p && *p != ' ') p++;
    if (!*p) return false;
    *p++ = '\0';
    UINT64 size = 0;
    if (*p < '0' || *p > '9') return false;
    while (*p >= '0' && *p <= '9') size = size * 10 + (UINT64)(*p++ - '0');
    while (*p == ' ') p++;
    for (int i = 0; i < 32; i++) {
        int hi = hexval(p[2 * i]), lo = hi < 0 ? -1 : hexval(p[2 * i + 1]);
        if (lo < 0) return false;
        f->sha[i] = (UINT8)(hi << 4 | lo);
    }
    resolve(channel, name, f->url, sizeof(f->url));
    f->size = size;
    f->present = true;
    return size > 0;
}

/* The channel's file: the version, the files and notes into the status;
 * false (with the error set) if it is not one */
static bool parse_channel(const char *channel, const char *body, UINT32 len)
{
    memset(&g_kernel, 0, sizeof(g_kernel));
    memset(&g_loader, 0, sizeof(g_loader));
    g_st.version[0] = g_st.notes[0] = '\0';
    char line[700];
    bool header = false;
    for (UINT32 i = 0; i < len;) {
        int n = 0;
        while (i < len && body[i] != '\n') {
            if (n < (int)sizeof(line) - 1 && body[i] != '\r') line[n++] = body[i];
            i++;
        }
        i++;
        line[n] = '\0';
        char *rest = strchr(line, ' ');
        if (rest) *rest++ = '\0'; else rest = line + n;
        while (*rest == ' ') rest++;
        if (!header) {
            if (strcmp(line, "NovaOS") || strncmp(rest, "update 1", 8)) break;
            header = true;
        } else if (!strcmp(line, "version")) {
            strncpy((char *)g_st.version, rest, sizeof(g_st.version) - 1);
        } else if (!strcmp(line, "kernel")) {
            if (!parse_file(channel, rest, &g_kernel)) { fail("The update channel's kernel line is malformed."); return false; }
        } else if (!strcmp(line, "loader")) {
            if (!parse_file(channel, rest, &g_loader)) { fail("The update channel's loader line is malformed."); return false; }
        } else if (!strcmp(line, "notes")) {
            strncpy((char *)g_st.notes, rest, sizeof(g_st.notes) - 1);
        }
        /* (other lines: for later versions of the format) */
    }
    if (!header) { fail("%s is not a NovaOS update channel.", channel); return false; }
    if (!g_st.version[0] || !g_kernel.present) { fail("The update channel names no version or kernel."); return false; }
    if (g_kernel.size > NET_HTTP_MAX || g_loader.size > NET_HTTP_MAX) { fail("The update is too large to download."); return false; }
    g_st.size = g_kernel.size + g_loader.size;
    return true;
}

static bool check(void)
{
    char channel[512];
    UpdateGetChannel(channel, sizeof(channel));
    strcpy(g_channel_used, channel);
    set_state(UPDATE_CHECKING);
    kprintf("[UPDATE] Checking %s (this is NovaOS %s)\n", channel, NovaVersion());
    const char *body;
    UINT32 blen;
    NetOp *op = fetch(channel, &body, &blen, false);
    if (!op) return false;
    bool ok = parse_channel(channel, body, blen);
    NetRelease(op);
    if (!ok) return false;
    if (NovaVersionCompare((const char *)g_st.version, NovaVersion()) <= 0) {
        kprintf("[UPDATE] NovaOS %s is up to date (the channel has %s)\n", NovaVersion(), (const char *)g_st.version);
        set_state(UPDATE_CURRENT);
        return true;
    }
    kprintf("[UPDATE] NovaOS %s is available (%u KB): %s\n", (const char *)g_st.version,
            (unsigned)(g_st.size >> 10), (const char *)g_st.notes);
    set_state(UPDATE_AVAILABLE);
    return true;
}

/* ---------------------------------------------------------------------------
 * The EFI System Partition NovaOS started from
 * ------------------------------------------------------------------------- */
static bool has_file(FatVol *v, const char *path)
{
    FatEntry e;
    return FatLookupPath(v, path, &e) && !e.dir;
}

/* Its FAT volume (mounted; the caller unmounts it) and \EFI\NOVA's
 * cluster, or NULL: the volume with NovaOS's kernel (or an update's
 * files) on it, the one on drive C:'s disk first */
static FatVol *open_esp(UINT32 *nova_dir)
{
    BlockDev *pref = PersistDevice();
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < BlockCount(); i++) {
            BlockDev *d = BlockGet(i);
            if ((pass == 0) != (d == pref)) continue;
            FatVol *v[8];
            bool blank;
            int n = PersistFindVolumes(d, v, 8, &blank), found = -1;
            for (int k = 0; k < n; k++) {
                if (found < 0 && (has_file(v[k], NOVA_DIR "\\" F_KERNEL) || has_file(v[k], NOVA_DIR "\\" F_TRYING) ||
                                  has_file(v[k], NOVA_DIR "\\" F_NEW))) {
                    FatEntry e;
                    if (FatLookupPath(v[k], NOVA_DIR, &e) && e.dir) { found = k; *nova_dir = e.cluster; continue; }
                }
                FatForget(v[k]);                          /* (only read: maybe drive C:'s volume) */
            }
            if (found >= 0) return v[found];
        }
    return NULL;
}

static bool sync(FatVol *v)
{
    bool ok = FatSync(v);
    BlockDev *d = FatDevice(v);
    if (d && d->flush) d->flush(d);
    return ok;
}

/* A file of the ESP read whole into a new buffer (*len), or NULL */
static UINT8 *read_file(FatVol *v, const char *path, UINT32 *len)
{
    FatEntry e;
    if (!FatLookupPath(v, path, &e) || e.dir) return NULL;
    UINT8 *buf = kmalloc(e.size + 1);
    if (!buf) return NULL;
    if (!FatRead(v, &e, buf)) { kfree(buf); return NULL; }
    buf[e.size] = '\0';
    *len = e.size;
    return buf;
}

/* "version X" / "from Y" of update.txt */
static void info_field(const char *info, const char *key, char *out, int cap)
{
    out[0] = '\0';
    if (!info) return;
    size_t kl = strlen(key);
    for (const char *p = info; *p; ) {
        if (!strncmp(p, key, kl) && p[kl] == ' ') {
            p += kl + 1;
            int n = 0;
            while (*p && *p != '\n' && *p != '\r' && n < cap - 1) out[n++] = *p++;
            out[n] = '\0';
            return;
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
}

/* ---------------------------------------------------------------------------
 * Installing: download, check, stage
 * ------------------------------------------------------------------------- */
/* The version stamped in a kernel image (version.c), or false */
static bool stamped_version(const UINT8 *img, UINT64 len, char *out)
{
    const char *mark = NovaVersionMark();
    for (UINT64 i = 0; i + NOVA_STAMP_MARK_LEN + NOVA_STAMP_VER_MAX <= len; i++)
        if (img[i] == (UINT8)mark[0] && !memcmp(img + i, mark, NOVA_STAMP_MARK_LEN)) {
            memcpy(out, img + i + NOVA_STAMP_MARK_LEN, NOVA_STAMP_VER_MAX);
            out[NOVA_STAMP_VER_MAX - 1] = '\0';
            return true;
        }
    return false;
}

/* Download @f and check its size and SHA-256 (and @what's format): the
 * operation holding it, or NULL with the error set */
static NetOp *download(const UpdFile *f, const char *what, UINT64 before, const UINT8 **data)
{
    char m[96];
    ksnprintf(m, sizeof(m), "Downloading the %s (%u KB)", what, (unsigned)(f->size >> 10));
    step(m);
    const char *body;
    UINT32 blen;
    NetOp *op = fetch(f->url, &body, &blen, true);
    g_st.got = before + (op ? blen : 0);
    if (!op) return NULL;
    UINT8 sum[32];
    sha256(body, blen, sum);
    if (blen != f->size || memcmp(sum, f->sha, 32)) {
        fail("The downloaded %s is not the one the channel names (%u bytes, expected %u; or its SHA-256 differs).",
             what, (unsigned)blen, (unsigned)f->size);
        NetRelease(op);
        return NULL;
    }
    *data = (const UINT8 *)body;
    return op;
}

/* Write @len bytes as \EFI\NOVA\@name and read them back */
static bool write_checked(FatVol *esp, UINT32 dir, const char *name, const UINT8 *data, UINT32 len)
{
    if (!FatWriteFile(esp, dir, name, data, len) || !sync(esp)) return false;
    char path[64];
    ksnprintf(path, sizeof(path), NOVA_DIR "\\%s", name);
    UINT32 got = 0;
    UINT8 *back = read_file(esp, path, &got);
    bool ok = back && got == len && !memcmp(back, data, len);
    kfree(back);
    return ok;
}

/* Delete the files an earlier update left (and kernel.old, for room) */
static void clear_staging(FatVol *esp, UINT32 dir, bool old_too)
{
    FatDelete(esp, dir, F_PENDING);
    FatDelete(esp, dir, F_TRYING);
    FatDelete(esp, dir, F_NEW);
    FatDelete(esp, dir, F_LOADER_NEW);
    FatDelete(esp, dir, F_INFO);
    if (old_too) FatDelete(esp, dir, F_OLD);
}

static void install(void)
{
    set_state(UPDATE_DOWNLOADING);
    g_st.got = 0;
    const UINT8 *kdata = NULL, *ldata = NULL;
    NetOp *kop = download(&g_kernel, "new kernel", 0, &kdata), *lop = NULL;
    if (!kop) return;
    char ver[NOVA_STAMP_VER_MAX];
    if (kdata[0] != 0x7F || kdata[1] != 'E' || kdata[2] != 'L' || kdata[3] != 'F' ||
        !stamped_version(kdata, g_kernel.size, ver)) {
        fail("The downloaded kernel is not a NovaOS kernel.");
        goto out;
    }
    if (strcmp(ver, (const char *)g_st.version)) {
        fail("The downloaded kernel is NovaOS %s, not %s as the channel says.", ver, (const char *)g_st.version);
        goto out;
    }
    if (g_loader.present) {
        lop = download(&g_loader, "new boot loader", g_kernel.size, &ldata);
        if (!lop) goto out;
        if (ldata[0] != 'M' || ldata[1] != 'Z') { fail("The downloaded boot loader is not an EFI program."); goto out; }
    }

    step("Writing the update to the EFI System Partition");
    UINT32 dir;
    FatVol *esp = open_esp(&dir);
    if (!esp) { fail("The EFI System Partition NovaOS started from was not found."); goto out; }
    clear_staging(esp, dir, true);
    sync(esp);
    UINT64 need = g_kernel.size + g_loader.size + (1u << 20), room = FatFreeBytes(esp);
    if (room < need) {
        fail("The EFI System Partition has %u MB free; the update needs %u MB.", (unsigned)(room >> 20), (unsigned)((need + (1u << 20) - 1) >> 20));
        FatUnmount(esp);
        goto out;
    }
    if (!write_checked(esp, dir, F_NEW, kdata, (UINT32)g_kernel.size) ||
        (ldata && !write_checked(esp, dir, F_LOADER_NEW, ldata, (UINT32)g_loader.size))) {
        clear_staging(esp, dir, false);
        sync(esp);
        FatUnmount(esp);
        fail("Writing the update to the EFI System Partition failed (it was removed again).");
        goto out;
    }
    char info[128];
    ksnprintf(info, sizeof(info), "version %s\nfrom %s\n", (const char *)g_st.version, NovaVersion());
    /* the mark last: until it is on the disk, the boot loader ignores the files */
    if (!FatWriteFile(esp, dir, F_INFO, info, (UINT32)strlen(info)) || !sync(esp) ||
        !FatWriteFile(esp, dir, F_PENDING, "pending\n", 8) || !sync(esp)) {
        clear_staging(esp, dir, false);
        sync(esp);
        FatUnmount(esp);
        fail("Writing the update to the EFI System Partition failed (it was removed again).");
        goto out;
    }
    FatUnmount(esp);
    step("The update is ready: restart NovaOS to finish it");
    kprintf("[UPDATE] NovaOS %s is staged; it starts at the next restart\n", (const char *)g_st.version);
    set_state(UPDATE_READY);
out:
    if (kop) NetRelease(kop);
    if (lop) NetRelease(lop);
}

static void update_thread(void *arg)
{
    (void)arg;
    if (g_want_install) {
        /* the check first unless it just found this (and from this channel) */
        char channel[512];
        UpdateGetChannel(channel, sizeof(channel));
        if ((g_st.state != UPDATE_AVAILABLE || strcmp(channel, g_channel_used)) && !check()) goto done;
        if (g_st.state == UPDATE_CURRENT) {
            fail("There is no newer NovaOS than %s.", NovaVersion());
            goto done;
        }
        install();
    } else {
        check();
    }
done:
    g_busy = false;
}

static bool start(bool install_too)
{
    if (SetupIsLive()) {
        fail("NovaOS is running from its installation %s: install it first (a newer build is a new %s).",
             SetupMediaName(), SetupMediaName());
        return false;
    }
    if (g_st.state == UPDATE_READY) return false;          /* (it waits for the restart) */
    if (__atomic_exchange_n(&g_busy, true, __ATOMIC_ACQ_REL)) return false;
    if (!NetAvailable()) { g_busy = false; fail("There is no network connection."); return false; }
    g_want_install = install_too;
    g_st.error[0] = '\0';
    g_st.step[0] = '\0';
    if (!sched_create_thread("update", update_thread, NULL, PRIO_SERVICE)) {
        g_busy = false;
        fail("Could not start the updater.");
        return false;
    }
    return true;
}

bool UpdateCheck(void)   { return start(false); }
bool UpdateInstall(void) { return start(true); }

/* ---------------------------------------------------------------------------
 * At boot
 * ------------------------------------------------------------------------- */
void UpdateBootInfo(const BootInfo *info)
{
    if (!info || info->version < 3) return;
    g_boot_flags = info->boot_flags & (BOOT_FLAG_UPDATE_TRIAL | BOOT_FLAG_UPDATE_FAILED);
    if (g_boot_flags & BOOT_FLAG_UPDATE_TRIAL)
        kprintf("[UPDATE] This is the update's first start (NovaOS %s, from \\EFI\\NOVA\\kernel.new)\n", NovaVersion());
    if (g_boot_flags & BOOT_FLAG_UPDATE_FAILED)
        kprintf("[UPDATE] The boot loader started the previous kernel: the update did not finish starting\n");
}

/* The trial start reached the desktop: make the new kernel and loader the
 * installed ones */
static void finish(FatVol *esp, UINT32 dir, const char *info)
{
    char from[NOVA_STAMP_VER_MAX];
    info_field(info, "from", from, sizeof(from));
    UINT32 llen = 0;
    UINT8 *loader = read_file(esp, NOVA_DIR "\\" F_LOADER_NEW, &llen);
    FatDelete(esp, dir, F_OLD);
    bool ok = (!has_file(esp, NOVA_DIR "\\" F_KERNEL) || FatRename(esp, dir, F_KERNEL, F_OLD)) &&
              FatRename(esp, dir, F_NEW, F_KERNEL) && sync(esp);
    if (ok && loader) {
        FatEntry e;
        ok = FatLookupPath(esp, BOOT_DIR, &e) && e.dir && FatWriteFile(esp, e.cluster, F_LOADER, loader, llen) && sync(esp);
    }
    kfree(loader);
    if (!ok) {
        /* (the trying mark stays: the next start goes back to the old kernel) */
        kprintf("[UPDATE] Could not finish the update on the EFI System Partition\n");
        ksnprintf(g_notice, sizeof(g_notice), "The update to NovaOS %s could not be finished; the next restart goes back to %s.",
                  NovaVersion(), from[0] ? from : "the previous version");
        return;
    }
    FatDelete(esp, dir, F_LOADER_NEW);
    FatDelete(esp, dir, F_INFO);
    FatDelete(esp, dir, F_TRYING);                      /* last: the update is done */
    sync(esp);
    kprintf("[UPDATE] Updated NovaOS from %s to %s (the previous kernel is kept as \\EFI\\NOVA\\kernel.old)\n",
            from[0] ? from : "?", NovaVersion());
    ksnprintf(g_notice, sizeof(g_notice), "NovaOS was updated from %s to %s.", from[0] ? from : "?", NovaVersion());
    um_registry_set_sz(UPDATE_KEY, "LastUpdate", g_notice);
}

static void boot_thread(void *arg)
{
    (void)arg;
    UINT32 dir;
    FatVol *esp = open_esp(&dir);
    if (!esp) {
        if (g_boot_flags) kprintf("[UPDATE] The EFI System Partition NovaOS started from was not found\n");
        return;
    }
    UINT32 ilen = 0;
    char *info = (char *)read_file(esp, NOVA_DIR "\\" F_INFO, &ilen);
    char ver[NOVA_STAMP_VER_MAX];
    info_field(info, "version", ver, sizeof(ver));
    if (g_boot_flags & BOOT_FLAG_UPDATE_TRIAL) {
        /* the trying mark is the boot loader's; an update.txt naming another
         * version would mean a mix-up, but this kernel did start */
        finish(esp, dir, info);
    } else if (g_boot_flags & BOOT_FLAG_UPDATE_FAILED) {
        clear_staging(esp, dir, false);
        if (!has_file(esp, NOVA_DIR "\\" F_KERNEL) && has_file(esp, NOVA_DIR "\\" F_OLD))
            FatRename(esp, dir, F_OLD, F_KERNEL);         /* (the power went in the middle of finishing) */
        sync(esp);
        kprintf("[UPDATE] The update to NovaOS %s did not finish starting, so NovaOS %s started again; the update was removed\n",
                ver[0] ? ver : "?", NovaVersion());
        ksnprintf(g_notice, sizeof(g_notice), "The update to NovaOS %s did not start, so NovaOS went back to %s.",
                  ver[0] ? ver : "?", NovaVersion());
        um_registry_set_sz(UPDATE_KEY, "LastUpdate", g_notice);
    }
    kfree(info);
    FatUnmount(esp);
}

void UpdateBootDone(void)
{
    /* (an update that was written but never marked ready is deleted by
     * the next install: nothing to do at an ordinary start) */
    if (SetupIsLive() || !g_boot_flags) return;
    sched_create_thread("update", boot_thread, NULL, PRIO_SERVICE);
}
