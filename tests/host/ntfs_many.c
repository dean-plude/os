/* A volume of many files (tests/tools/test_ntfs_many.py): the real
 * kernel/fs/ntfs.c on a RAM disk, formatted the way Setup formats drive C:
 * and the way mkntfs does (IMAGE, when given).  40,000 files go into
 * folders of 500, small ones and ones with clusters between them, so the
 * MFT grows past what its first bitmap cluster covers (32,768 records)
 * with files' clusters after each extent of it.  It grew 64 records at a
 * time once: one more run each time, until its runs no longer fitted in
 * record 0 and no file could be made (about 7,000 files: Steam's update).
 * Then files that fill their record (small contents kept in it) are made
 * under a short name and renamed to a long one, as installers do (Inno
 * Setup's is-XXXXX.tmp), and given a second long name (a link): the
 * contents move to clusters to make room, as on Windows (GIMP's install
 * failed on a rename once).  The volume must mount again with every file
 * there and pass scripts/ntfs-check.py.
 *
 *   ntfs_many CHECKER OUT [IMAGE]   OUT: the disk saved for the check */
#include "../../kernel/fs/ntfs.c"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

void *kmalloc(size_t n) { return malloc(n ? n : 1); }
void *kzalloc(size_t n) { return calloc(1, n ? n : 1); }
void kfree(void *p) { free(p); }
void kprintf(const char *f, ...)
{
    va_list a; va_start(a, f); vprintf(f, a); va_end(a);
}
static UINT64 tick = 132000000000000000ull;
static UINT64 now(void) { return tick += 10000000; }

#define DISK_BYTES (1ull << 30)
#define FILES      40000
#define PER_DIR    500
#define RENAMED    600

typedef struct { BlockDev d; UINT8 *data; } Ram;
static bool ram_read(BlockDev *d, UINT64 lba, UINT32 n, void *buf)
{
    if ((lba + n) * BLOCK_SECTOR > DISK_BYTES) return false;
    memcpy(buf, ((Ram *)d->ctx)->data + lba * BLOCK_SECTOR, (size_t)n * BLOCK_SECTOR);
    return true;
}
static bool ram_write(BlockDev *d, UINT64 lba, UINT32 n, const void *buf)
{
    if ((lba + n) * BLOCK_SECTOR > DISK_BYTES) return false;
    memcpy(((Ram *)d->ctx)->data + lba * BLOCK_SECTOR, buf, (size_t)n * BLOCK_SECTOR);
    return true;
}
static bool ram_flush(BlockDev *d) { (void)d; return true; }

/* Renamed file i: 600-899 bytes, in its record when made */
static UINT32 renamed_size(UINT32 i) { return 600 + i * 37 % 300; }
static void long_name(char *out, UINT32 i, char tag)
{
    snprintf(out, 200, "%c-a-file-name-long-enough-to-need-the-room-its-contents-took-in-the-record-%04u.png", tag, i);
}

/* File i: every fourth one has clusters (5-9 KB), the rest fit in their record */
static UINT32 file_size(UINT32 i) { return i % 4 ? 100 + i % 300 : 5000 + i % 4000; }

int main(int argc, char **argv)
{
    if (argc < 3) { printf("usage: ntfs_many CHECKER OUT [IMAGE]\n"); return 2; }
    NtfsSetClock(now);
    static Ram r;
    r.data = calloc(1, DISK_BYTES);
    assert(r.data);
    strcpy(r.d.name, "ram0");
    r.d.sectors = DISK_BYTES / BLOCK_SECTOR;
    r.d.read = ram_read; r.d.write = ram_write; r.d.flush = ram_flush;
    r.d.ctx = &r;
    if (argc > 3) {                                               /* mkntfs' volume */
        FILE *f = fopen(argv[3], "rb");
        assert(f && fread(r.data, 1, DISK_BYTES, f) == DISK_BYTES);
        fclose(f);
    } else
        assert(NtfsFormat(&r.d, 0, DISK_BYTES / BLOCK_SECTOR, "NOVADATA", 0x1234));
    NtfsVol *v = NtfsMount(&r.d, 0);
    assert(v && NtfsEnableWrite(v));
    static UINT8 buf[9000];
    UINT64 dir = 0, m;
    for (UINT32 i = 0; i < FILES; i++) {
        char name[32];
        if (i % PER_DIR == 0) {
            snprintf(name, sizeof(name), "d%u", i / PER_DIR);
            if (!NtfsCreate(v, NTFS_ROOT, name, true, &dir)) { printf("ntfs_many: folder %s not made (%u files)\n", name, i); return 1; }
        }
        snprintf(name, sizeof(name), "f%u.bin", i);
        memset(buf, (int)(i & 0xFF), sizeof(buf));
        if (!NtfsCreate(v, dir, name, false, &m) || !NtfsWriteFile(v, m, buf, file_size(i))) {
            printf("ntfs_many: %s not made (%u files)\n", name, i);
            return 1;
        }
    }
    UINT64 rdir;
    assert(NtfsCreate(v, NTFS_ROOT, "renamed", true, &rdir));
    for (UINT32 i = 0; i < RENAMED; i++) {
        char tmp[32], name[200];
        snprintf(tmp, sizeof(tmp), "is-%05X.tmp", i * 7919u % 0xFFFFF);
        long_name(name, i, 'r');
        memset(buf, (int)(i & 0xFF), sizeof(buf));
        if (!NtfsCreate(v, rdir, tmp, false, &m) || !NtfsWriteFile(v, m, buf, renamed_size(i)) ||
            !NtfsRename(v, rdir, m, tmp, rdir, name)) {
            printf("ntfs_many: %s not renamed to %s\n", tmp, name);
            return 1;
        }
        long_name(name, i, 'l');
        if (i % 10 == 0 && !NtfsLink(v, m, rdir, name)) { printf("ntfs_many: no link %s\n", name); return 1; }
    }
    NtfsUnmount(v);

    /* Again, as the next boot: every file there, whole */
    v = NtfsMount(&r.d, 0);
    assert(v && NtfsEnableWrite(v));
    static UINT8 back[9000];
    int bad = 0;
    for (UINT32 i = 0; i < FILES; i += 97) {
        char name[32];
        snprintf(name, sizeof(name), "d%u", i / PER_DIR);
        bool is_dir;
        UINT64 size = 0;
        if (!NtfsLookup(v, NTFS_ROOT, name, &dir, &is_dir)) { printf("ntfs_many: %s is missing\n", name); bad++; continue; }
        snprintf(name, sizeof(name), "f%u.bin", i);
        memset(buf, (int)(i & 0xFF), sizeof(buf));
        if (!NtfsLookup(v, dir, name, &m, &is_dir) || !NtfsSize(v, m, &size) || size != file_size(i) ||
            !NtfsRead(v, m, 0, back, size) || memcmp(back, buf, size)) {
            printf("ntfs_many: %s is not whole\n", name);
            bad++;
        }
    }
    bool is_dir;
    assert(NtfsLookup(v, NTFS_ROOT, "renamed", &rdir, &is_dir));
    for (UINT32 i = 0; i < RENAMED; i++) {
        char name[200];
        UINT64 size = 0;
        memset(buf, (int)(i & 0xFF), sizeof(buf));
        for (int k = 0; k < (i % 10 ? 1 : 2); k++) {
            long_name(name, i, k ? 'l' : 'r');
            if (!NtfsLookup(v, rdir, name, &m, &is_dir) || !NtfsSize(v, m, &size) || size != renamed_size(i) ||
                !NtfsRead(v, m, 0, back, size) || memcmp(back, buf, size)) {
                printf("ntfs_many: %s is not whole\n", name);
                bad++;
            }
        }
    }
    NtfsUnmount(v);
    FILE *f = fopen(argv[2], "wb");
    assert(f && fwrite(r.data, 1, DISK_BYTES, f) == DISK_BYTES);
    fclose(f);
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "%s %s", argv[1], argv[2]);
    if (system(cmd)) { printf("ntfs_many: the check found errors\n"); bad++; }
    printf("ntfs_many: %d files, %d bad\n", FILES, bad);
    return bad != 0;
}
