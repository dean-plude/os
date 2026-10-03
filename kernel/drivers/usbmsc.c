/*
 * usbmsc.c — USB mass storage: Bulk-Only Transport and SCSI
 *
 * USB Mass Storage Class Bulk-Only Transport 1.0: each command is a
 * 31-byte Command Block Wrapper on the bulk OUT endpoint, then the data on
 * the bulk IN or OUT endpoint, then a 13-byte Command Status Wrapper on
 * bulk IN.  The commands are SCSI (SPC/SBC): INQUIRY, TEST UNIT READY,
 * REQUEST SENSE, READ CAPACITY, READ, WRITE and SYNCHRONIZE CACHE.  Each
 * stick (its first LUN) becomes a removable block device, whose volumes
 * are mounted as the next drive letters (drives.h).
 */

#include "usb.h"
#include "../fs/block.h"
#include "../fs/drives.h"
#include "../mm/vmm.h"
#include "../lib/string.h"
#include "../ke/printf.h"
#include "../ke/scheduler.h"
#include "../ke/smp.h"
#include "../arch/x86_64/cpu.h"
#include "../arch/x86_64/apic.h"

#define CBW_SIG      0x43425355u
#define CSW_SIG      0x53425355u
#define XFER_BYTES   65536u                 /* per SCSI command */
#define TIMEOUT_MS   10000

typedef struct {
    BlockDev      dev;
    UsbDev       *usb;
    UsbPipe      *in, *out;
    UINT8         iface;
    UINT32        tag;
    UINT32        block;                    /* the medium's block size */
    volatile int  busy;
    bool          dead;
} Msc;

static int g_count;

static void take(volatile int *f)
{
    while (__atomic_exchange_n(f, 1, __ATOMIC_ACQUIRE)) {
        if (interrupts_enabled()) sched_yield();
        else pause_cpu();
    }
}
static void drop(volatile int *f) { __atomic_store_n(f, 0, __ATOMIC_RELEASE); }

/* Bulk-Only Mass Storage Reset, then clear both halts (BOT 5.3.4) */
static void reset_recovery(Msc *m)
{
    UsbControl(m->usb, 0x21, 0xFF, 0, m->iface, 0, NULL);
    UsbPipeReset(m->in);
    UsbPipeReset(m->out);
}

/* One SCSI command: @cdb (@cdb_len bytes), @len bytes of data in or out
 * of @buf.  Returns the CSW status (0 passed, 1 failed), or -1 on a
 * transport error. */
static int scsi(Msc *m, const UINT8 *cdb, int cdb_len, bool in, void *buf, UINT32 len)
{
    if (m->dead || UsbDevGone(m->usb)) return -1;
    UINT8 cbw[31];
    memset(cbw, 0, sizeof(cbw));
    UINT32 tag = ++m->tag;
    UINT32 sig = CBW_SIG;
    memcpy(cbw, &sig, 4);
    memcpy(cbw + 4, &tag, 4);
    memcpy(cbw + 8, &len, 4);
    cbw[12] = in ? 0x80 : 0x00;
    cbw[13] = 0;                                        /* LUN 0 */
    cbw[14] = (UINT8)cdb_len;
    memcpy(cbw + 15, cdb, (size_t)cdb_len);
    bool stalled;
    if (UsbBulk(m->out, cbw, 31, TIMEOUT_MS, &stalled) != 31) {
        reset_recovery(m);
        return -1;
    }
    if (len) {
        int n = UsbBulk(in ? m->in : m->out, buf, len, TIMEOUT_MS, &stalled);
        if (n < 0) {
            if (!stalled) { reset_recovery(m); return -1; }
            UsbPipeReset(in ? m->in : m->out);          /* the CSW still follows */
        }
    }
    UINT8 csw[13];
    int n = UsbBulk(m->in, csw, 13, TIMEOUT_MS, &stalled);
    if (n < 0 && stalled) {
        UsbPipeReset(m->in);
        n = UsbBulk(m->in, csw, 13, TIMEOUT_MS, &stalled);
    }
    UINT32 csig, ctag;
    memcpy(&csig, csw, 4);
    memcpy(&ctag, csw + 4, 4);
    if (n != 13 || csig != CSW_SIG || ctag != tag || csw[12] > 1) {
        reset_recovery(m);
        return -1;
    }
    return csw[12];
}

static void request_sense(Msc *m, UINT8 *key, UINT8 *asc)
{
    UINT8 cdb[6] = { 0x03, 0, 0, 0, 18, 0 }, sense[18];
    memset(sense, 0, sizeof(sense));
    scsi(m, cdb, 6, true, sense, 18);
    if (key) *key = sense[2] & 0xF;
    if (asc) *asc = sense[12];
}

/* Read or write @count blocks at @lba, retrying once after a check condition */
static bool rw(Msc *m, bool write, UINT64 lba, UINT32 count, void *buf)
{
    for (int attempt = 0; attempt < 3; attempt++) {
        UINT8 cdb[16];
        memset(cdb, 0, sizeof(cdb));
        int cl;
        if (lba + count > 0xFFFFFFFFull) {               /* READ(16) / WRITE(16) */
            cdb[0] = write ? 0x8A : 0x88;
            for (int i = 0; i < 8; i++) cdb[2 + i] = (UINT8)(lba >> (56 - 8 * i));
            for (int i = 0; i < 4; i++) cdb[10 + i] = (UINT8)(count >> (24 - 8 * i));
            cl = 16;
        } else {                                         /* READ(10) / WRITE(10) */
            cdb[0] = write ? 0x2A : 0x28;
            for (int i = 0; i < 4; i++) cdb[2 + i] = (UINT8)(lba >> (24 - 8 * i));
            cdb[7] = (UINT8)(count >> 8);
            cdb[8] = (UINT8)count;
            cl = 10;
        }
        int st = scsi(m, cdb, cl, !write, buf, count * m->block);
        if (st == 0) return true;
        if (m->dead || UsbDevGone(m->usb)) return false;
        if (st == 1) request_sense(m, NULL, NULL);
    }
    return false;
}

static bool msc_xfer(BlockDev *bd, bool write, UINT64 lba, UINT32 count, void *buf)
{
    Msc *m = bd->ctx;
    if (m->dead || bd->gone) return false;
    bool ok = true;
    UINT8 *p = buf;
    UINT32 per = XFER_BYTES / m->block;
    while (count && ok) {
        UINT32 n = count < per ? count : per;
        bkl_acquire();                      /* (the USB stack wants the big lock: a command at a time, */
        take(&m->busy);                     /*  so a long write never keeps it from the others) */
        ok = rw(m, write, lba, n, p);
        drop(&m->busy);
        bkl_release();
        lba += n; count -= n; p += (size_t)n * m->block;
    }
    return ok;
}

static bool msc_read(BlockDev *bd, UINT64 lba, UINT32 count, void *buf)
{
    return msc_xfer(bd, false, lba, count, buf);
}

static bool msc_write(BlockDev *bd, UINT64 lba, UINT32 count, const void *buf)
{
    return msc_xfer(bd, true, lba, count, (void *)buf);
}

static bool msc_flush(BlockDev *bd)
{
    Msc *m = bd->ctx;
    if (m->dead || bd->gone) return false;
    bkl_acquire();
    take(&m->busy);
    UINT8 cdb[10] = { 0x35 };                          /* SYNCHRONIZE CACHE(10) */
    int st = scsi(m, cdb, 10, false, NULL, 0);
    if (st == 1) request_sense(m, NULL, NULL);          /* (many sticks have no cache: fine) */
    drop(&m->busy);
    bkl_release();
    return st >= 0;
}

static void msc_gone(void *inst)
{
    Msc *m = inst;
    m->dead = true;
    BlockUnregister(&m->dev);
    DrivesDetach(&m->dev);
    kprintf("[USB] %s: %s unplugged\n", UsbDevName(m->usb), m->dev.name);
    /* (the Msc stays allocated: file systems may still hold its BlockDev) */
}

static void wait_ms(int ms)
{
    if (interrupts_enabled() && sched_current()) sched_sleep_until(NULL, sched_ticks() + (UINT64)(ms + 9) / 10 + 1);
    else udelay((UINT64)ms * 1000);
}

void *UsbMscProbe(UsbDev *d, const UsbIface *f)
{
    if (f->sub != 0x06 || f->proto != 0x50) return NULL;   /* SCSI transparent, Bulk-Only */
    const UINT8 *ein = NULL, *eout = NULL;
    int off = 0;
    for (const UINT8 *e; (e = UsbIfaceFind(f, USB_DT_ENDPOINT, &off)) != NULL;) {
        if ((e[3] & 3) != 2) continue;
        if (e[2] & 0x80) { if (!ein) ein = e; }
        else if (!eout) eout = e;
    }
    if (!ein || !eout) return NULL;
    Msc *m = kzalloc(sizeof(Msc));
    if (!m) return NULL;
    m->usb = d;
    m->iface = f->number;
    m->in = UsbOpenPipe(d, ein, XFER_BYTES);
    m->out = UsbOpenPipe(d, eout, XFER_BYTES);
    if (!m->in || !m->out) { kfree(m); return NULL; }

    UINT8 *buf = kmalloc(512);
    if (!buf) { kfree(m); return NULL; }

    /* INQUIRY: peripheral type (0 = disk) and the names */
    UINT8 inq[6] = { 0x12, 0, 0, 0, 36, 0 };
    memset(buf, 0, 36);
    if (scsi(m, inq, 6, true, buf, 36) != 0 || (buf[0] & 0x1F) != 0) {
        kprintf("[USB] %s: mass storage device is not a disk\n", UsbDevName(d));
        kfree(buf); kfree(m);
        return NULL;
    }
    char model[41];
    memset(model, 0, sizeof(model));
    memcpy(model, buf + 8, 8);
    model[8] = ' ';
    memcpy(model + 9, buf + 16, 16);
    for (int i = 24; i >= 0 && (model[i] == ' ' || !model[i]); i--) model[i] = 0;

    /* TEST UNIT READY until the medium is ready (a stick spins up quickly) */
    bool ready = false;
    for (int tries = 0; tries < 20 && !ready; tries++) {
        UINT8 tur[6] = { 0 };
        int st = scsi(m, tur, 6, false, NULL, 0);
        if (st == 0) ready = true;
        else if (st < 0 && UsbDevGone(d)) break;
        else { request_sense(m, NULL, NULL); wait_ms(100); }
    }

    /* READ CAPACITY(10), or (16) for 2 TiB and more */
    UINT8 rc10[10] = { 0x25 };
    UINT64 blocks = 0;
    UINT32 bsize = 0;
    if (ready && scsi(m, rc10, 10, true, buf, 8) == 0) {
        UINT32 last = (UINT32)buf[0] << 24 | (UINT32)buf[1] << 16 | (UINT32)buf[2] << 8 | buf[3];
        bsize = (UINT32)buf[4] << 24 | (UINT32)buf[5] << 16 | (UINT32)buf[6] << 8 | buf[7];
        blocks = (UINT64)last + 1;
        if (last == 0xFFFFFFFFu) {
            UINT8 rc16[16] = { 0x9E, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 32, 0, 0 };
            if (scsi(m, rc16, 16, true, buf, 32) == 0) {
                UINT64 l = 0;
                for (int i = 0; i < 8; i++) l = l << 8 | buf[i];
                blocks = l + 1;
                bsize = (UINT32)buf[8] << 24 | (UINT32)buf[9] << 16 | (UINT32)buf[10] << 8 | buf[11];
            }
        }
    }
    kfree(buf);
    if (!ready || !blocks) {
        kprintf("[USB] %s: %s has no medium\n", UsbDevName(d), model);
        kfree(m);
        return NULL;
    }
    if (bsize != BLOCK_SECTOR) {
        kprintf("[USB] %s: %s uses %u-byte blocks, not supported\n", UsbDevName(d), model, bsize);
        kfree(m);
        return NULL;
    }
    m->block = bsize;

    ksnprintf(m->dev.name, sizeof(m->dev.name), "usb%d", g_count++);
    strncpy(m->dev.model, model, sizeof(m->dev.model) - 1);
    m->dev.sectors = blocks;
    m->dev.read = msc_read;
    m->dev.write = msc_write;
    m->dev.flush = msc_flush;
    m->dev.ctx = m;
    m->dev.removable = true;
    UsbBind(d, m, msc_gone);
    kprintf("[USB] %s: mass storage \"%s\", %llu MiB\n", UsbDevName(d), model,
            (unsigned long long)(blocks * bsize >> 20));
    BlockRegister(&m->dev);
    DrivesAttach(&m->dev);
    return m;
}
