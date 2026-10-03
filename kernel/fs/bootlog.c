/*
 * bootlog.c — the boot log on the USB stick NovaOS started from (see bootlog.h)
 */

#include "bootlog.h"
#include "fat.h"
#include "persist.h"
#include "setup.h"
#include "../mm/vmm.h"
#include "../lib/string.h"
#include "../ke/printf.h"
#include "../ke/scheduler.h"

#define LOG_PATH   "\\EFI\\NOVA\\bootlog.txt"
#define CHUNK      128                      /* sectors per write */

static const char FULL_NOTE[] = "\n[The boot log is full: later messages are not in this file]\n";

static BlockDev     *g_dev;
static UINT64        g_lba;                 /* the file's first sector on g_dev */
static UINT32        g_bytes;               /* its size (whole sectors) */
static UINT32        g_cap;                 /* how much of the log fits, leaving room for FULL_NOTE */
static size_t        g_written;             /* log bytes already on the stick */
static bool          g_cleared;             /* the whole file was written once (the old log is gone) */
static bool          g_full;
static UINT64        g_last;                /* tick of the last write */
static volatile int  g_busy;
static UINT8        *g_buf;                 /* CHUNK sectors */

BlockDev *BootLogDevice(void) { return g_dev; }

void BootLogAttach(BlockDev *d)
{
    if (g_dev || !SetupLiveFromUsb() || !d->removable) return;
    FatVol *v[8];
    bool blank;
    int n = PersistFindVolumes(d, v, 8, &blank);
    for (int i = 0; i < n; i++) {
        FatEntry e;
        UINT64 lba;
        if (!g_dev && FatLookupPath(v[i], LOG_PATH, &e) && !e.dir && e.size >= 2 * BLOCK_SECTOR) {
            if (!FatContiguous(v[i], &e, &lba)) {
                kprintf("[BOOTLOG] %s on %s is fragmented: the log is not written there\n", LOG_PATH, d->name);
            } else if ((g_buf = kmalloc(CHUNK * BLOCK_SECTOR)) != NULL) {
                g_lba = lba;
                g_bytes = e.size & ~(UINT32)(BLOCK_SECTOR - 1);
                g_cap = g_bytes - BLOCK_SECTOR;
                if (g_cap > KLOG_BOOT_SIZE) g_cap = KLOG_BOOT_SIZE;
                __atomic_store_n(&g_dev, d, __ATOMIC_RELEASE);
                kprintf("[BOOTLOG] Writing the boot log to %s on %s (%u KB, sector %llu)\n",
                        LOG_PATH, d->name, g_bytes >> 10, (unsigned long long)g_lba);
            }
        }
        FatUnmount(v[i]);
    }
}

/* Fill @count sectors from file sector @first: the log's text, then the
 * note if it is full, then blank lines */
static void fill(UINT32 first, UINT32 count, const char *text, size_t len)
{
    UINT64 at = (UINT64)first * BLOCK_SECTOR;
    for (UINT32 i = 0; i < count * BLOCK_SECTOR; i++, at++) {
        char c = '\n';
        if (at < len) c = text[at];
        else if (g_full && at < len + sizeof(FULL_NOTE) - 1) c = FULL_NOTE[at - len];
        g_buf[i] = (UINT8)c;
    }
}

/* Write file sectors [first, end) */
static bool put(UINT32 first, UINT32 end, const char *text, size_t len)
{
    while (first < end) {
        UINT32 n = end - first < CHUNK ? end - first : CHUNK;
        fill(first, n, text, len);
        if (g_dev->gone || !g_dev->write(g_dev, g_lba + first, n, g_buf)) return false;
        first += n;
    }
    return true;
}

static void write_new(void)
{
    BlockDev *d = __atomic_load_n(&g_dev, __ATOMIC_ACQUIRE);
    if (!d) return;
    if (d->gone) {                          /* (unplugged: nothing more to do) */
        g_dev = NULL;
        return;
    }
    size_t len;
    const char *text = klog_boot_text(&len);
    if (len > g_cap) len = g_cap;
    bool full = len == g_cap || len == KLOG_BOOT_SIZE;
    if (g_cleared && len == g_written && full == g_full) return;
    g_full = full;
    size_t end = len + (g_full ? sizeof(FULL_NOTE) - 1 : 0);
    UINT32 first = g_cleared ? (UINT32)(g_written / BLOCK_SECTOR) : 0;
    UINT32 last = g_cleared ? (UINT32)((end + BLOCK_SECTOR - 1) / BLOCK_SECTOR) : g_bytes / BLOCK_SECTOR;
    if (put(first, last, text, len) && (!d->flush || d->flush(d))) {
        g_written = len;
        g_cleared = true;
    }
}

static bool lock(void) { return !__atomic_exchange_n(&g_busy, 1, __ATOMIC_ACQUIRE); }
static void unlock(void) { __atomic_store_n(&g_busy, 0, __ATOMIC_RELEASE); }

void BootLogPoll(void)
{
    if (!__atomic_load_n(&g_dev, __ATOMIC_ACQUIRE) || sched_ticks() - g_last < 100 || !lock()) return;
    g_last = sched_ticks();
    write_new();
    unlock();
}

void BootLogSync(void)
{
    if (!__atomic_load_n(&g_dev, __ATOMIC_ACQUIRE)) return;
    while (!lock()) sched_yield();
    write_new();
    g_last = sched_ticks();
    unlock();
}

void BootLogPanic(void)
{
    if (!__atomic_load_n(&g_dev, __ATOMIC_ACQUIRE) || !lock()) return;   /* (a write was under way: leave it) */
    write_new();
    unlock();
}
