/*
 * setup.c — installing NovaOS on a disk (see setup.h)
 */

#include "setup.h"
#include "fat.h"
#include "ntfs.h"
#include "persist.h"
#include "drives.h"
#include "bootlog.h"
#include "../mm/vmm.h"
#include "../lib/string.h"
#include "../ke/printf.h"
#include "../ke/scheduler.h"
#include "../hal/rtc.h"
#include "../arch/x86_64/cpu.h"

#define PHYSMAP(p) ((const void *)(uintptr_t)(PHYSMAP_BASE + (p)))

static bool          g_live, g_live_usb;
static const UINT8  *g_media_kernel, *g_media_loader;     /* from the bootloader */
static UINT64        g_media_kernel_size, g_media_loader_size;

void SetupBootInfo(const BootInfo *info)
{
    if (!info || info->version < 3) return;
    g_live = (info->boot_flags & BOOT_FLAG_LIVE_MEDIA) != 0;
    g_live_usb = g_live && (info->boot_flags & BOOT_FLAG_LIVE_USB);
    if (info->media_kernel_base && info->media_kernel_size && info->media_loader_base && info->media_loader_size) {
        g_media_kernel = PHYSMAP(info->media_kernel_base);
        g_media_kernel_size = info->media_kernel_size;
        g_media_loader = PHYSMAP(info->media_loader_base);
        g_media_loader_size = info->media_loader_size;
    }
    if (g_live)
        kprintf("[SETUP] Running from the installation %s (kernel %u KB, loader %u KB in memory)\n",
                SetupMediaName(), (unsigned)(g_media_kernel_size >> 10), (unsigned)(g_media_loader_size >> 10));
    else if (info->boot_flags & BOOT_FLAG_ENTRY_ADDED)
        kprintf("[SETUP] Added the firmware boot entry \"NovaOS\" for this disk\n");
    else if (info->boot_flags & BOOT_FLAG_BOOT_ENTRY)
        kprintf("[SETUP] The firmware has a \"NovaOS\" boot entry for this disk\n");
}

bool SetupIsLive(void) { return g_live; }
const char *SetupMediaName(void) { return g_live_usb ? "USB stick" : "disc"; }
bool SetupLiveFromUsb(void) { return g_live_usb; }

/* ---------------------------------------------------------------------------
 * Disks
 * ------------------------------------------------------------------------- */
int SetupListDisks(SetupDisk *out, int max)
{
    int n = 0;
    BlockDev *cdev = PersistDevice();
    for (int i = 0; i < BlockCount() && n < max; i++) {
        BlockDev *d = BlockGet(i);
        SetupDisk *s = &out[n++];
        memset(s, 0, sizeof(*s));
        s->dev = d;
        s->bytes = d->sectors * BLOCK_SECTOR;
        s->too_small = s->bytes < SETUP_MIN_BYTES;
        s->holds_c = d == cdev;
        s->boot = d == BootLogDevice();              /* the USB stick NovaOS is running from */
        FatVol *v[8];
        bool blank = false;
        int nf = PersistFindVolumes(d, v, 8, &blank);
        for (int k = 0; k < nf; k++) {
            FatEntry e;
            /* (from the disc, a disk with NovaOS on it is not what booted) */
            if (!g_live && FatLookupPath(v[k], "\\EFI\\NOVA\\kernel.elf", &e) && !e.dir) s->boot = true;
            FatUnmount(v[k]);
        }
        char labels[48];
        int nv = PersistDiskLabels(d, labels, sizeof(labels), &blank);
        bool nova = strstr(labels, "NOVA_EFI") && strstr(labels, "NOVADATA");
        if (blank)             ksnprintf(s->contents, sizeof(s->contents), "Empty");
        else if (nova)         ksnprintf(s->contents, sizeof(s->contents), "NovaOS is installed on it");
        else if (nv)           ksnprintf(s->contents, sizeof(s->contents), "Volumes: %s", labels[0] ? labels : "no label");
        else                   ksnprintf(s->contents, sizeof(s->contents), "Unknown contents");
    }
    return n;
}

/* ---------------------------------------------------------------------------
 * The installation
 * ------------------------------------------------------------------------- */
static volatile SetupStatus g_status;
static BlockDev *g_target;
static bool      g_ntfs;                  /* drive C: on NTFS */

void SetupGetStatus(SetupStatus *out)
{
    memcpy(out, (const void *)&g_status, sizeof(*out));
}

static void step(int percent, const char *text)
{
    g_status.percent = percent;
    strncpy((char *)g_status.step, text, sizeof(g_status.step) - 1);
    kprintf("[SETUP] %d%% %s\n", percent, text);
}

static void failed(const char *why)
{
    strncpy((char *)g_status.error, why, sizeof(g_status.error) - 1);
    kprintf("[SETUP] Failed: %s\n", why);
    g_status.state = SETUP_FAILED;
}

/* The installation files, in memory: from the bootloader, or read from
 * the FAT volume NovaOS booted from */
static UINT8 *g_kernel_copy, *g_loader_copy;

static bool load_media(const UINT8 **kernel, UINT32 *ksize, const UINT8 **loader, UINT32 *lsize)
{
    if (g_media_kernel) {
        *kernel = g_media_kernel; *ksize = (UINT32)g_media_kernel_size;
        *loader = g_media_loader; *lsize = (UINT32)g_media_loader_size;
        return true;
    }
    for (int i = 0; i < BlockCount(); i++) {
        FatVol *v[8];
        bool blank;
        int nv = PersistFindVolumes(BlockGet(i), v, 8, &blank);
        bool found = false;
        for (int k = 0; k < nv; k++) {
            FatEntry ke, le;
            if (!found && FatLookupPath(v[k], "\\EFI\\NOVA\\kernel.elf", &ke) && !ke.dir &&
                FatLookupPath(v[k], "\\EFI\\BOOT\\BOOTX64.EFI", &le) && !le.dir) {
                g_kernel_copy = kmalloc(ke.size + 1);
                g_loader_copy = kmalloc(le.size + 1);
                if (g_kernel_copy && g_loader_copy && FatRead(v[k], &ke, g_kernel_copy) && FatRead(v[k], &le, g_loader_copy)) {
                    *kernel = g_kernel_copy; *ksize = ke.size;
                    *loader = g_loader_copy; *lsize = le.size;
                    found = true;
                }
            }
            FatUnmount(v[k]);
        }
        if (found) return true;
    }
    return false;
}

/* CRC-32 (IEEE), as GPT uses */
static UINT32 crc32(const void *data, UINT32 len)
{
    const UINT8 *p = data;
    UINT32 c = 0xFFFFFFFFu;
    for (UINT32 i = 0; i < len; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

static UINT64 g_rng;
static UINT64 rnd(void)
{
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return g_rng;
}

/* A random (version 4) GUID in its on-disk byte order */
static void random_guid(UINT8 g[16])
{
    UINT64 a = rnd(), b = rnd();
    memcpy(g, &a, 8);
    memcpy(g + 8, &b, 8);
    g[7] = (UINT8)((g[7] & 0x0F) | 0x40);
    g[8] = (UINT8)((g[8] & 0x3F) | 0x80);
}

/* A GUID string's value in GPT (mixed-endian) byte order */
static void guid_bytes(UINT8 g[16], UINT32 d1, UINT16 d2, UINT16 d3, const UINT8 d4[8])
{
    memcpy(g, &d1, 4);
    memcpy(g + 4, &d2, 2);
    memcpy(g + 6, &d3, 2);
    memcpy(g + 8, d4, 8);
}

static void put_name(UINT8 *e, const char *name)
{
    for (int i = 0; name[i] && i < 36; i++) { e[56 + 2 * i] = (UINT8)name[i]; e[57 + 2 * i] = 0; }
}

#define GPT_ENTRIES   128
#define GPT_ESIZE     128
#define GPT_TABLE_SEC (GPT_ENTRIES * GPT_ESIZE / BLOCK_SECTOR)      /* 32 */

/* Reserve the installed payload and a complete staged update, plus FAT
 * metadata/configuration. Match the installation image's 32 MiB rounding. */
static UINT64 setup_esp_sectors(UINT32 kernel_size, UINT32 loader_size)
{
    UINT64 payload_mb = ((UINT64)kernel_size + loader_size + 1048575) >> 20;
    UINT64 mb = ((2 * payload_mb + 32 + 31) / 32) * 32;
    if (mb < 128) mb = 128;
    return mb * 2048;
}

/* Validate geometry before detaching volumes or erasing any disk sectors. */
static bool setup_layout(UINT64 sectors, UINT64 esp_sectors,
                         UINT64 *esp_lba, UINT64 *data_lba, UINT64 *data_sectors)
{
    if (sectors < 4096 || esp_sectors > sectors - 2048) return false;
    *esp_lba = 2048;
    *data_lba = *esp_lba + esp_sectors;
    UINT64 end = (sectors - 1 - GPT_TABLE_SEC) & ~2047ull;
    if (end <= *data_lba || end - *data_lba <= 131072) return false;
    *data_sectors = end - *data_lba;
    if (!g_ntfs && *data_sectors > (1ull << 31)) *data_sectors = 1ull << 31;
    return true;
}

/* Write a protective MBR and primary + backup GPTs with the two partitions. */
static bool write_gpt(BlockDev *d, UINT64 *esp_lba, UINT64 *esp_sectors, UINT64 *data_lba, UINT64 *data_sectors)
{
    UINT64 last = d->sectors - 1;
    UINT64 first_usable = 2 + GPT_TABLE_SEC, last_usable = last - 1 - GPT_TABLE_SEC;
    if (!setup_layout(d->sectors, *esp_sectors, esp_lba, data_lba, data_sectors)) return false;

    UINT8 *buf = kzalloc((2 + GPT_TABLE_SEC) * BLOCK_SECTOR);
    if (!buf) return false;
    /* protective MBR: one 0xEE partition covering the disk */
    UINT8 *mbr = buf;
    UINT8 *pe = mbr + 446;
    pe[1] = 0x00; pe[2] = 0x02; pe[3] = 0x00;
    pe[4] = 0xEE;
    pe[5] = 0xFF; pe[6] = 0xFF; pe[7] = 0xFF;
    UINT32 one = 1, cover = last > 0xFFFFFFFFull ? 0xFFFFFFFFu : (UINT32)last;
    memcpy(pe + 8, &one, 4);
    memcpy(pe + 12, &cover, 4);
    mbr[510] = 0x55; mbr[511] = 0xAA;

    /* the partition entries */
    UINT8 *ent = buf + 2 * BLOCK_SECTOR;
    static const UINT8 esp4[8]  = { 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B };
    static const UINT8 data4[8] = { 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7 };
    UINT8 *e0 = ent, *e1 = ent + GPT_ESIZE;
    guid_bytes(e0, 0xC12A7328, 0xF81F, 0x11D2, esp4);            /* EFI System Partition */
    random_guid(e0 + 16);
    UINT64 a = *esp_lba, b = *esp_lba + *esp_sectors - 1;
    memcpy(e0 + 32, &a, 8);
    memcpy(e0 + 40, &b, 8);
    put_name(e0, "EFI System Partition");
    guid_bytes(e1, 0xEBD0A0A2, 0xB9E5, 0x4433, data4);           /* Basic data */
    random_guid(e1 + 16);
    a = *data_lba; b = *data_lba + *data_sectors - 1;
    memcpy(e1 + 32, &a, 8);
    memcpy(e1 + 40, &b, 8);
    put_name(e1, "NovaOS");
    UINT32 ecrc = crc32(ent, GPT_TABLE_SEC * BLOCK_SECTOR);

    /* the header (primary at LBA 1; the backup mirrors it at the end) */
    UINT8 disk_guid[16];
    random_guid(disk_guid);
    UINT8 *h = buf + BLOCK_SECTOR;
    UINT64 v64;
    UINT32 v32;
    memcpy(h, "EFI PART", 8);
    v32 = 0x00010000; memcpy(h + 8, &v32, 4);                     /* revision 1.0 */
    v32 = 92;         memcpy(h + 12, &v32, 4);                    /* header size */
    v64 = 1;          memcpy(h + 24, &v64, 8);                    /* this header */
    v64 = last;       memcpy(h + 32, &v64, 8);                    /* the other one */
    v64 = first_usable; memcpy(h + 40, &v64, 8);
    v64 = last_usable;  memcpy(h + 48, &v64, 8);
    memcpy(h + 56, disk_guid, 16);
    v64 = 2;          memcpy(h + 72, &v64, 8);                    /* entries at */
    v32 = GPT_ENTRIES; memcpy(h + 80, &v32, 4);
    v32 = GPT_ESIZE;   memcpy(h + 84, &v32, 4);
    memcpy(h + 88, &ecrc, 4);
    v32 = 0; memcpy(h + 16, &v32, 4);
    v32 = crc32(h, 92); memcpy(h + 16, &v32, 4);

    UINT8 *bh = kzalloc(BLOCK_SECTOR);
    if (!bh) { kfree(buf); return false; }
    memcpy(bh, h, 92);
    v64 = last;  memcpy(bh + 24, &v64, 8);
    v64 = 1;     memcpy(bh + 32, &v64, 8);
    v64 = last - GPT_TABLE_SEC; memcpy(bh + 72, &v64, 8);
    v32 = 0; memcpy(bh + 16, &v32, 4);
    v32 = crc32(bh, 92); memcpy(bh + 16, &v32, 4);

    bool ok = d->write(d, 0, 2 + GPT_TABLE_SEC, buf) &&
              d->write(d, last - GPT_TABLE_SEC, GPT_TABLE_SEC, ent) &&
              d->write(d, last, 1, bh);
    kfree(bh);
    kfree(buf);
    return ok;
}

/* Zero the start of each partition's area and the old partition tables so
 * no stale volume or GPT is found there */
static bool wipe(BlockDev *d, UINT64 esp_lba, UINT64 data_lba)
{
    UINT8 *z = kzalloc(64 * BLOCK_SECTOR);
    if (!z) return false;
    bool ok = d->write(d, 0, 64, z) &&
              d->write(d, d->sectors - 64, 64, z) &&
              d->write(d, esp_lba, 64, z) &&
              d->write(d, data_lba, 64, z);
    kfree(z);
    return ok;
}

static void setup_thread(void *arg)
{
    (void)arg;
    BlockDev *d = g_target;
    RtcTime t;
    rtc_read(&t);
    g_rng = rdtsc() ^ ((UINT64)t.second << 40) ^ ((UINT64)t.minute << 48) ^ 0x9E3779B97F4A7C15ull;
    if (!g_rng) g_rng = 1;

    step(2, "Getting the installation files ready");
    const UINT8 *kernel, *loader;
    UINT32 ksize, lsize;
    if (!load_media(&kernel, &ksize, &loader, &lsize)) {
        failed("The installation files were not found (boot from the NovaOS disc).");
        return;
    }

    UINT64 esp_lba, esp_sec = setup_esp_sectors(ksize, lsize), data_lba, data_sec;
    if (!setup_layout(d->sectors, esp_sec, &esp_lba, &data_lba, &data_sec)) {
        failed("The disk is too small for NovaOS and a staged update.");
        return;
    }

    /* C: may be saved on this very disk: stop that before erasing it */
    bool had_c = PersistActive(), c_here = PersistDevice() == d;
    if (c_here) {
        step(6, "Saving this session's files for the move");
        PersistDetach();
    }

    DrivesDetach(d);                                             /* (its volumes are not D:, E:, ... any more) */
    step(10, "Creating partitions");
    if (!wipe(d, esp_lba, data_lba) || !write_gpt(d, &esp_lba, &esp_sec, &data_lba, &data_sec)) {
        failed("Could not write the partition table (a disk error, or the disk is too small).");
        return;
    }
    if (d->flush) d->flush(d);

    step(18, "Formatting the EFI System Partition");
    FatVol *esp = FatFormat(d, esp_lba, esp_sec, "NOVA_EFI");
    if (!esp) { failed("Could not format the EFI System Partition."); return; }
    UINT32 dir;
    step(26, "Copying the boot loader");
    if (!FatMkdirPath(esp, "\\EFI\\BOOT", &dir) ||
        !FatWriteFile(esp, dir, "BOOTX64.EFI", loader, lsize)) {
        FatUnmount(esp);
        failed("Could not copy the boot loader.");
        return;
    }
    step(34, "Copying NovaOS (the kernel and its programs)");
    if (!FatMkdirPath(esp, "\\EFI\\NOVA", &dir) ||
        !FatWriteFile(esp, dir, "kernel.elf", kernel, ksize)) {
        FatUnmount(esp);
        failed("Could not copy the NovaOS system files (is the disk big enough?).");
        return;
    }
    step(70, "Checking the copied files");
    FatEntry e;
    bool good = FatSync(esp) && FatLookupPath(esp, "\\EFI\\NOVA\\kernel.elf", &e) && e.size == ksize;
    if (good) {
        /* read the kernel back and compare */
        UINT8 *back = kmalloc(ksize + 1);
        good = back && FatRead(esp, &e, back) && !memcmp(back, kernel, ksize);
        kfree(back);
    }
    FatUnmount(esp);
    if (!good) { failed("The copied system files did not read back correctly."); return; }

    if (g_ntfs) {
        step(80, "Formatting the data partition for drive C: (NTFS)");
        if (!NtfsFormat(d, data_lba, data_sec, "NOVADATA", rnd())) { failed("Could not format the data partition."); return; }
    } else {
        step(80, "Formatting the data partition for drive C: (FAT32)");
        FatVol *data = FatFormat(d, data_lba, data_sec, "NOVADATA");
        if (!data) { failed("Could not format the data partition."); return; }
        UINT32 nova_dir;
        FatMkdirPath(data, "\\NOVA\\C", &nova_dir);
        FatSync(data);
        FatUnmount(data);
    }

    /* This session's files go to the new disk when C: has nowhere else to
     * be saved; a C: kept on another disk stays there */
    if (c_here || !had_c) {
        step(88, "Copying your files to the new disk");
        g_status.moved_c = PersistAdopt(d, data_lba);
    }
    if (d->flush) d->flush(d);
    if (g_kernel_copy) { kfree(g_kernel_copy); g_kernel_copy = NULL; }
    if (g_loader_copy) { kfree(g_loader_copy); g_loader_copy = NULL; }
    step(100, "NovaOS is installed");
    g_status.state = SETUP_DONE;
}

bool SetupStart(BlockDev *dev, bool ntfs)
{
    if (g_status.state == SETUP_RUNNING || !dev) return false;
    memset((void *)&g_status, 0, sizeof(g_status));
    g_status.state = SETUP_RUNNING;
    g_target = dev;
    g_ntfs = ntfs;
    kprintf("[SETUP] Installing NovaOS on %s (%s, %u MiB), drive C: on %s\n", dev->name, dev->model,
            (unsigned)(dev->sectors >> 11), ntfs ? "NTFS" : "FAT32");
    if (!sched_create_thread("setup", setup_thread, NULL, PRIO_SERVICE)) {
        g_status.state = SETUP_FAILED;
        strncpy((char *)g_status.error, "Could not start the installer.", sizeof(g_status.error) - 1);
        return false;
    }
    return true;
}
