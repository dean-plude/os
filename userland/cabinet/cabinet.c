/*
 * cabinet.dll — the File Decompression Interface (FDI): FDICreate,
 * FDIIsCabinet, FDICopy, FDIDestroy.
 *
 * Installers extract their payloads through it (WiX Burn bundles such as
 * the Visual C++ Redistributable keep theirs in a cabinet attached to the
 * .exe).  Every file and memory operation goes through the caller's
 * functions, so a caller may hand over a cabinet that sits inside another
 * file (Burn's open and seek functions add the container's offset).
 *
 * A cabinet is read whole through those functions; the decompressors are
 * the Windows Installer's (userland/msi/cab.c: stored and MSZIP blocks,
 * userland/msi/lzx.c: LZX), compiled into this DLL as well.  A folder is
 * decompressed once, front to back, while its files are written out in
 * the cabinet's order; a folder that continues in the next cabinet of a
 * set is followed there (fdintNEXT_CABINET), and the files of that next
 * cabinet are then extracted too, as Windows does.
 *
 * Not here: the compression side (FCI), Quantum compression, decryption
 * callbacks (never called: no cabinet uses them), FDITruncateCabinet.
 */
#define CABINETAPI __declspec(dllexport)
#include <windows.h>
#include <fdi.h>
#include "../msi/msi_int.h"

#ifndef ERROR_BAD_ARGUMENTS
#define ERROR_BAD_ARGUMENTS 160
#endif

#define FDI_MAGIC 0x4E494446u                       /* "FDIN" */
#define O_RDONLY_BINARY  0x8000                     /* _O_RDONLY | _O_BINARY */
#define S_IREAD_IWRITE   0x0180                     /* _S_IREAD | _S_IWRITE */

typedef struct {
    uint32_t magic;
    PFNALLOC pfnalloc;
    PFNFREE  pfnfree;
    PFNOPEN  pfnopen;
    PFNREAD  pfnread;
    PFNWRITE pfnwrite;
    PFNCLOSE pfnclose;
    PFNSEEK  pfnseek;
    PERF     perf;
} Fdi;

/* One cabinet in memory, with the header fields cab_open() leaves out */
typedef struct {
    uint8_t *data;
    size_t   size;
    Cab      cab;
    uint16_t flags, set_id, icabinet;
    char     next_disk[256];
    char     prev_disk[256];
} Loaded;

static void set_error(Fdi *f, FDIERROR e)
{
    if (f->perf) {
        f->perf->erfOper = e;
        f->perf->erfType = 0;
        f->perf->fError = e != FDIERROR_NONE;
    }
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24); }

static size_t copy_str(const uint8_t *p, size_t max, char *out, size_t cap)
{
    size_t n = 0;
    while (n < max && p[n]) n++;
    size_t c = n < cap - 1 ? n : cap - 1;
    memcpy(out, p, c);
    out[c] = '\0';
    return n + 1;
}

/* Read exactly @n bytes; false on a short read */
static bool read_all(Fdi *f, INT_PTR hf, void *buf, uint32_t n)
{
    uint8_t *p = buf;
    while (n) {
        UINT got = f->pfnread(hf, p, n);
        if (got == (UINT)-1 || got == 0) return false;
        p += got;
        n -= got;
    }
    return true;
}

/* The cabinet at @hf's current position, whole */
static FDIERROR load(Fdi *f, INT_PTR hf, Loaded *l)
{
    memset(l, 0, sizeof(*l));
    uint8_t h[36];
    if (!read_all(f, hf, h, sizeof(h)) || memcmp(h, "MSCF", 4)) return FDIERROR_NOT_A_CABINET;
    if (h[25] != 1 || h[24] > 3) return FDIERROR_UNKNOWN_CABINET_VERSION;
    uint32_t size = rd32(h + 8);
    if (size < sizeof(h) || size > 0x7FFFFFFFu) return FDIERROR_CORRUPT_CABINET;
    l->data = malloc(size);
    if (!l->data) return FDIERROR_ALLOC_FAIL;
    memcpy(l->data, h, sizeof(h));
    if (!read_all(f, hf, l->data + sizeof(h), size - (uint32_t)sizeof(h))) {
        free(l->data);
        l->data = NULL;
        return FDIERROR_CORRUPT_CABINET;
    }
    l->size = size;
    if (!cab_open(&l->cab, l->data, size)) {
        free(l->data);
        l->data = NULL;
        return FDIERROR_CORRUPT_CABINET;
    }
    l->flags = rd16(h + 30);
    l->set_id = rd16(h + 32);
    l->icabinet = rd16(h + 34);
    size_t pos = 36;                                /* the disk names, for the notifications */
    if (l->flags & 4) pos = 40 + (size > 40 ? rd16(l->data + 36) : 0);
    char name[256];
    if ((l->flags & 1) && pos < size) {
        pos += copy_str(l->data + pos, size - pos, name, sizeof(name));
        if (pos < size) pos += copy_str(l->data + pos, size - pos, l->prev_disk, sizeof(l->prev_disk));
    }
    if ((l->flags & 2) && pos < size) {
        pos += copy_str(l->data + pos, size - pos, name, sizeof(name));
        if (pos < size) copy_str(l->data + pos, size - pos, l->next_disk, sizeof(l->next_disk));
    }
    return FDIERROR_NONE;
}

static void unload(Loaded *l)
{
    if (l->data) cab_close(&l->cab);
    free(l->data);
    memset(l, 0, sizeof(*l));
}

/* Open PATH+NAME through the caller and load it */
static FDIERROR open_cabinet(Fdi *f, const char *path, const char *name, Loaded *l)
{
    char full[CB_MAX_CAB_PATH + CB_MAX_CABINET_NAME + 2];
    snprintf(full, sizeof(full), "%s%s", path ? path : "", name ? name : "");
    INT_PTR hf = f->pfnopen(full, O_RDONLY_BINARY, S_IREAD_IWRITE);
    if (hf == (INT_PTR)-1) return FDIERROR_CABINET_NOT_FOUND;
    FDIERROR e = load(f, hf, l);
    f->pfnclose(hf);
    return e;
}

HFDI DIAMONDAPI FDICreate(PFNALLOC pfnalloc, PFNFREE pfnfree, PFNOPEN pfnopen, PFNREAD pfnread,
                                 PFNWRITE pfnwrite, PFNCLOSE pfnclose, PFNSEEK pfnseek, int cpuType, PERF perf)
{
    (void)cpuType;
    if (!pfnalloc || !pfnfree || !pfnopen || !pfnread || !pfnwrite || !pfnclose || !pfnseek) {
        if (perf) { perf->erfOper = FDIERROR_ALLOC_FAIL; perf->erfType = ERROR_INVALID_PARAMETER; perf->fError = TRUE; }
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    Fdi *f = pfnalloc(sizeof(Fdi));
    if (!f) {
        if (perf) { perf->erfOper = FDIERROR_ALLOC_FAIL; perf->erfType = 0; perf->fError = TRUE; }
        return NULL;
    }
    f->magic = FDI_MAGIC;
    f->pfnalloc = pfnalloc;
    f->pfnfree = pfnfree;
    f->pfnopen = pfnopen;
    f->pfnread = pfnread;
    f->pfnwrite = pfnwrite;
    f->pfnclose = pfnclose;
    f->pfnseek = pfnseek;
    f->perf = perf;
    if (perf) { perf->erfOper = FDIERROR_NONE; perf->erfType = 0; perf->fError = FALSE; }
    return f;
}

static Fdi *get(HFDI h)
{
    Fdi *f = h;
    if (!f || f->magic != FDI_MAGIC) { SetLastError(ERROR_INVALID_HANDLE); return NULL; }
    return f;
}

BOOL DIAMONDAPI FDIDestroy(HFDI hfdi)
{
    Fdi *f = get(hfdi);
    if (!f) return FALSE;
    f->magic = 0;
    f->pfnfree(f);
    return TRUE;
}

/* The header at @hf's current position (the file stays where the caller
 * left it plus the header's 36 bytes, as on Windows) */
BOOL DIAMONDAPI FDIIsCabinet(HFDI hfdi, INT_PTR hf, PFDICABINETINFO pfdici)
{
    Fdi *f = get(hfdi);
    if (!f) return FALSE;
    if (!pfdici) { set_error(f, FDIERROR_NONE); SetLastError(ERROR_BAD_ARGUMENTS); return FALSE; }
    uint8_t h[36];
    if (!read_all(f, hf, h, sizeof(h)) || memcmp(h, "MSCF", 4)) {
        set_error(f, FDIERROR_NONE);                /* not a cabinet is not an error */
        return FALSE;
    }
    if (h[25] != 1 || h[24] > 3) { set_error(f, FDIERROR_UNKNOWN_CABINET_VERSION); return FALSE; }
    uint16_t flags = rd16(h + 30);
    pfdici->cbCabinet = (long)rd32(h + 8);
    pfdici->cFolders = rd16(h + 26);
    pfdici->cFiles = rd16(h + 28);
    pfdici->setID = rd16(h + 32);
    pfdici->iCabinet = rd16(h + 34);
    pfdici->fReserve = (flags & 4) != 0;
    pfdici->hasprev = (flags & 1) != 0;
    pfdici->hasnext = (flags & 2) != 0;
    set_error(f, FDIERROR_NONE);
    return TRUE;
}

BOOL DIAMONDAPI FDITruncateCabinet(HFDI hfdi, char *pszCabinetName, USHORT iFolderToDelete)
{
    (void)pszCabinetName; (void)iFolderToDelete;
    Fdi *f = get(hfdi);
    if (!f) return FALSE;
    set_error(f, FDIERROR_MDI_FAIL);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

/* -----------------------------------------------------------------------
 * FDICopy
 * ----------------------------------------------------------------------- */
typedef struct {
    Fdi          *f;
    PFNFDINOTIFY  notify;
    void         *user;
    char          path[CB_MAX_CAB_PATH];        /* where the set's cabinets are */
    Loaded        cabs[2];                       /* the one being listed, and the next once a folder spans */
    int           list, data;                    /* indexes into cabs[] */
    /* the folder being decompressed */
    bool          active;
    int           folder;                        /* in cabs[data] when it started */
    CabReader     reader;
    uint64_t      blk_start;                     /* folder offset of reader.out[0] */
    uint32_t      blk_len;
} Copy;

static INT_PTR notify(Copy *c, FDINOTIFICATIONTYPE type, FDINOTIFICATION *n)
{
    n->pv = c->user;
    return c->notify(type, n);
}

static void folder_end(Copy *c)
{
    if (c->active) cab_reader_end(&c->reader);
    c->active = false;
    c->blk_len = 0;
}

static FDIERROR folder_start(Copy *c, int folder)
{
    folder_end(c);
    const Cab *cab = &c->cabs[c->data].cab;
    if (folder < 0 || folder >= cab->nfolders) return FDIERROR_CORRUPT_CABINET;
    int type = cab->folders[folder].compress & tcompMASK_TYPE;
    if (type != tcompTYPE_NONE && type != tcompTYPE_MSZIP && type != tcompTYPE_LZX) return FDIERROR_BAD_COMPR_TYPE;
    if (!cab_reader_start(&c->reader, cab, folder)) {
        cab_reader_end(&c->reader);
        return FDIERROR_BAD_COMPR_TYPE;
    }
    c->active = true;
    c->folder = folder;
    c->blk_start = 0;
    c->blk_len = 0;
    return FDIERROR_NONE;
}

/* The folder's data ran out in this cabinet: go on in the next one */
static FDIERROR next_cabinet(Copy *c)
{
    Loaded *cur = &c->cabs[c->data];
    if (!cur->cab.next[0] || c->folder != cur->cab.nfolders - 1) return FDIERROR_CORRUPT_CABINET;
    if (c->data != c->list) return FDIERROR_CORRUPT_CABINET;   /* one cabinet ahead at most */
    int slot = 1 - c->list;
    char name[CB_MAX_CABINET_NAME], disk[CB_MAX_DISK_NAME];
    snprintf(name, sizeof(name), "%s", cur->cab.next);
    snprintf(disk, sizeof(disk), "%s", cur->next_disk);
    FDIERROR why = FDIERROR_NONE;
    for (int tries = 0; ; tries++) {
        FDINOTIFICATION n = {0};
        n.psz1 = name;
        n.psz2 = disk;
        n.psz3 = c->path;                            /* the callback may change the path */
        n.fdie = why;
        n.setID = cur->set_id;
        n.iCabinet = (USHORT)(cur->icabinet + 1);
        if (notify(c, fdintNEXT_CABINET, &n) == -1) return FDIERROR_USER_ABORT;
        why = open_cabinet(c->f, c->path, name, &c->cabs[slot]);
        if (why == FDIERROR_NONE) break;
        if (why != FDIERROR_CABINET_NOT_FOUND || tries >= 8) return why;
    }
    Loaded *nx = &c->cabs[slot];
    if (nx->set_id != cur->set_id || nx->icabinet != cur->icabinet + 1) { unload(nx); return FDIERROR_WRONG_CABINET; }
    if (!cab_reader_continue(&c->reader, &nx->cab)) { unload(nx); return FDIERROR_CORRUPT_CABINET; }
    c->data = slot;
    c->folder = 0;
    return FDIERROR_NONE;
}

static FDIERROR next_block(Copy *c)
{
    c->blk_start += c->blk_len;
    c->blk_len = 0;
    for (;;) {
        if (cab_reader_next(&c->reader)) {
            c->blk_len = c->reader.out_len;
            return FDIERROR_NONE;
        }
        if (c->reader.error[0] || c->reader.blocks_left > 0) return FDIERROR_CORRUPT_CABINET;
        FDIERROR e = next_cabinet(c);
        if (e != FDIERROR_NONE) return e;
    }
}

/* Write bytes [off, off+len) of the folder to @hf (0: decompress past them) */
static FDIERROR folder_copy(Copy *c, uint64_t off, uint32_t len, INT_PTR hf)
{
    while (len) {
        if (off < c->blk_start) return FDIERROR_CORRUPT_CABINET;
        if (off >= c->blk_start + c->blk_len) {
            FDIERROR e = next_block(c);
            if (e != FDIERROR_NONE) return e;
            if (!c->blk_len) return FDIERROR_CORRUPT_CABINET;
            continue;
        }
        uint32_t at = (uint32_t)(off - c->blk_start);
        uint32_t n = c->blk_len - at < len ? c->blk_len - at : len;
        if (hf && c->f->pfnwrite(hf, c->reader.out + at, n) != n) return FDIERROR_TARGET_FILE;
        off += n;
        len -= n;
    }
    return FDIERROR_NONE;
}

/* Extract (or skip) the files of cabs[c->list]; @continuing: this cabinet
 * was reached through a folder that spans, whose files are done */
static FDIERROR list_files(Copy *c, bool continuing)
{
    Loaded *l = &c->cabs[c->list];
    for (int i = 0; i < l->cab.nfiles; i++) {
        CabFile *cf = &l->cab.files[i];
        FDINOTIFICATION n = {0};
        n.setID = l->set_id;
        n.iCabinet = l->icabinet;
        if (cf->folder < 0) {                                  /* begins in the previous cabinet */
            if (continuing) continue;
            n.psz1 = cf->name;
            n.psz2 = l->cab.prev;
            n.psz3 = l->prev_disk;
            if (notify(c, fdintPARTIAL_FILE, &n) == -1) return FDIERROR_USER_ABORT;
            continue;
        }
        n.psz1 = cf->name;
        n.cb = (long)cf->size;
        n.date = cf->date;
        n.time = cf->time;
        n.attribs = cf->attribs;
        n.iFolder = (USHORT)cf->folder;
        INT_PTR hf = notify(c, fdintCOPY_FILE, &n);
        if (hf == -1) return FDIERROR_USER_ABORT;
        if (!hf) continue;

        /* the folder: carry on in the one being decompressed, else start it
         * (a folder that spans cabinets can only be read forwards) */
        bool same = c->active && c->data == c->list && c->folder == cf->folder &&
                    cf->folder_off >= c->blk_start;
        if (!same) {
            if (continuing && cf->folder == 0) { c->f->pfnclose(hf); return FDIERROR_CORRUPT_CABINET; }
            c->data = c->list;
            FDIERROR e = folder_start(c, cf->folder);
            if (e != FDIERROR_NONE) { c->f->pfnclose(hf); return e; }
        }
        FDIERROR e = folder_copy(c, cf->folder_off, cf->size, hf);
        if (e != FDIERROR_NONE) { c->f->pfnclose(hf); return e; }

        FDINOTIFICATION d = {0};
        d.psz1 = cf->name;
        d.hf = hf;
        d.date = cf->date;
        d.time = cf->time;
        d.attribs = (USHORT)(cf->attribs & ~_A_EXEC);
        d.cb = (cf->attribs & _A_EXEC) ? 1 : 0;
        d.setID = l->set_id;
        d.iCabinet = l->icabinet;
        d.iFolder = (USHORT)cf->folder;
        if (!notify(c, fdintCLOSE_FILE_INFO, &d)) return FDIERROR_USER_ABORT;
    }
    return FDIERROR_NONE;
}

BOOL DIAMONDAPI FDICopy(HFDI hfdi, char *pszCabinet, char *pszCabPath, int flags,
                               PFNFDINOTIFY pfnfdin, PFNFDIDECRYPT pfnfdid, void *pvUser)
{
    (void)flags; (void)pfnfdid;
    Fdi *f = get(hfdi);
    if (!f) return FALSE;
    if (!pfnfdin || !pszCabinet) { set_error(f, FDIERROR_CABINET_NOT_FOUND); SetLastError(ERROR_BAD_ARGUMENTS); return FALSE; }
    Copy *c = calloc(1, sizeof(Copy));
    if (!c) { set_error(f, FDIERROR_ALLOC_FAIL); return FALSE; }
    c->f = f;
    c->notify = pfnfdin;
    c->user = pvUser;
    snprintf(c->path, sizeof(c->path), "%s", pszCabPath ? pszCabPath : "");

    FDIERROR e = open_cabinet(f, c->path, pszCabinet, &c->cabs[0]);
    bool continuing = false;
    while (e == FDIERROR_NONE) {
        Loaded *l = &c->cabs[c->list];
        FDINOTIFICATION n = {0};
        n.psz1 = l->cab.next;
        n.psz2 = l->next_disk;
        n.psz3 = c->path;
        n.setID = l->set_id;
        n.iCabinet = l->icabinet;
        if (notify(c, fdintCABINET_INFO, &n) == -1) { e = FDIERROR_USER_ABORT; break; }
        e = list_files(c, continuing);
        if (e != FDIERROR_NONE || c->data == c->list) break;
        /* a folder went on into the next cabinet: its files come next */
        unload(&c->cabs[c->list]);
        c->list = c->data;
        continuing = true;
    }
    folder_end(c);
    unload(&c->cabs[0]);
    unload(&c->cabs[1]);
    free(c);
    set_error(f, e);
    return e == FDIERROR_NONE;
}
