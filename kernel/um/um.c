/*
 * um.c — NovaOS user-mode subsystem: processes, address spaces, PE loader
 *
 * Address spaces: each process gets its own PML4 whose upper half is the
 * kernel's (paging_create_process_pt).  User memory is tracked as reserved
 * regions; pages are committed (backed by zeroed frames) on demand of the
 * loader and NtAllocateVirtualMemory, and all of it is freed by walking
 * the lower half of the page table when the process is reclaimed.
 *
 * Loader: the executable and every DLL it imports (recursively, from
 * C:\Windows\System32) are assembled in kernel buffers — sections placed,
 * base relocations applied, import address tables filled from the
 * exporting modules — then copied into committed user pages with
 * per-section protection (NX for data, read-only headers).  DLL entry
 * points are not called: NovaOS's system DLLs initialize lazily.
 */

#include "um_internal.h"
#include "../ke/printf.h"
#include "../ke/kpcr.h"
#include "../mm/vmm.h"
#include "../mm/pmm.h"
#include "../lib/string.h"
#include "../arch/x86_64/cpu.h"
#include "../arch/x86_64/gdt.h"
#include "../arch/x86_64/paging.h"
#include "userland_files.h"

static UmProcess     *g_procs[UM_MAX_PROCS];
static UINT32         g_next_pid = 100;
static volatile int   g_plock;

static void plock(void)   { while (__atomic_exchange_n(&g_plock, 1, __ATOMIC_ACQUIRE)) sched_yield(); }
static void punlock(void) { __atomic_store_n(&g_plock, 0, __ATOMIC_RELEASE); }

/* -----------------------------------------------------------------------
 * The desktop lock (recursive for its owner)
 * ----------------------------------------------------------------------- */
static volatile int g_dlock;
static Thread      *g_downer;
static int          g_ddepth;

void DesktopLock(void)
{
    Thread *me = sched_current();
    if (g_downer == me) { g_ddepth++; return; }
    while (__atomic_exchange_n(&g_dlock, 1, __ATOMIC_ACQUIRE)) sched_yield();
    g_downer = me;
    g_ddepth = 1;
}

void DesktopUnlock(void)
{
    if (--g_ddepth > 0) return;
    g_downer = NULL;
    __atomic_store_n(&g_dlock, 0, __ATOMIC_RELEASE);
}

/* -----------------------------------------------------------------------
 * Initialization
 * ----------------------------------------------------------------------- */
void UmInit(void)
{
    /* SSE for user code: FPU present (EM=0, MP=1), FXSAVE/FXRSTOR and
     * SSE exceptions enabled.  The kernel itself is built without SSE. */
    write_cr0((read_cr0() & ~CR0_EM & ~CR0_TS) | CR0_MP);
    write_cr4(read_cr4() | (1u << 9) /* OSFXSR */ | (1u << 10) /* OSXMMEXCPT */);
    __asm__ volatile ("fninit");
    um_syscall_init();

    /* Install the system DLLs and programs on drive C: */
    int installed = 0;
    RamfsCreate(RamfsRoot(), "Temp", true);
    for (int i = 0; i < g_userland_file_count; i++) {
        const UserlandFile *uf = &g_userland_files[i];
        char dir[RAMFS_PATH_MAX];
        strncpy(dir, uf->path, sizeof(dir) - 1);
        dir[sizeof(dir) - 1] = '\0';
        char *slash = strrchr(dir, '\\');
        if (!slash) continue;
        *slash = '\0';
        RamNode *d = RamfsRoot();
        for (char *part = dir + 1, *next; part && *part; part = next) {
            next = strchr(part, '\\');
            if (next) *next++ = '\0';
            d = d ? RamfsCreate(d, part, true) : NULL;
        }
        RamNode *f = d ? RamfsCreate(d, slash + 1, false) : NULL;
        if (f && RamfsWrite(f, (const char *)uf->data, uf->size)) installed++;
    }
    kprintf("[UM] User-mode subsystem ready: %d system files installed (C:\\Windows\\System32, C:\\Programs)\n",
            installed);
}

UmProcess *UmCurrent(void)
{
    return (UmProcess *)sched_current()->um;
}

/* -----------------------------------------------------------------------
 * Page tables
 * ----------------------------------------------------------------------- */
#define PT(pa)  ((pte_t *)(uintptr_t)(PHYSMAP_BASE + ((pa) & PTE_ADDR_MASK)))

static pte_t *walk(UINT64 pml4, UINT64 va, bool create)
{
    pte_t *t = PT(pml4);
    static const int shift[3] = { 39, 30, 21 };
    for (int l = 0; l < 3; l++) {
        int i = (int)((va >> shift[l]) & 511);
        if (!(t[i] & PTE_PRESENT)) {
            if (!create) return NULL;
            PADDR n = pmm_alloc_page();
            if (!n) return NULL;
            memset(PT(n), 0, PAGE_SIZE);
            t[i] = n | PTE_PRESENT | PTE_WRITE | PTE_USER;
        }
        t = PT(t[i]);
    }
    return &t[(va >> 12) & 511];
}

static UINT64 pte_flags(UINT32 protect)
{
    UINT64 f = PTE_PRESENT | PTE_USER;
    UINT32 p = protect & 0xFF;
    if (p & (0x04 | 0x08 | 0x40 | 0x80)) f |= PTE_WRITE;        /* RW, WC, XRW, XWC */
    if (!(p & (0x10 | 0x20 | 0x40 | 0x80))) f |= PTE_NX;        /* not executable */
    return f;
}

static bool is_current(UmProcess *p) { return read_cr3() == p->pml4; }

bool um_commit(UmProcess *p, UINT64 va, UINT64 size, UINT32 protect)
{
    UINT64 f = pte_flags(protect);
    for (UINT64 a = va & ~0xFFFULL; a < va + size; a += PAGE_SIZE) {
        pte_t *e = walk(p->pml4, a, true);
        if (!e) return false;
        if (*e & PTE_PRESENT) {                         /* re-commit: new protection */
            *e = (*e & PTE_ADDR_MASK) | f;
            if (is_current(p)) invlpg(a);
            continue;
        }
        PADDR fr = pmm_alloc_page();
        if (!fr) return false;
        memset((void *)(uintptr_t)(PHYSMAP_BASE + fr), 0, PAGE_SIZE);
        *e = fr | f;
        p->pages++;
    }
    return true;
}

void um_decommit(UmProcess *p, UINT64 va, UINT64 size)
{
    for (UINT64 a = va & ~0xFFFULL; a < va + size; a += PAGE_SIZE) {
        pte_t *e = walk(p->pml4, a, false);
        if (!e || !(*e & PTE_PRESENT)) continue;
        pmm_free_page(*e & PTE_ADDR_MASK);
        *e = 0;
        p->pages--;
        if (is_current(p)) invlpg(a);
    }
}

bool um_is_committed(UmProcess *p, UINT64 va)
{
    pte_t *e = walk(p->pml4, va, false);
    return e && (*e & PTE_PRESENT);
}

static bool copy_pages(UmProcess *p, UINT64 va, void *buf, UINT64 n, bool to_user)
{
    UINT8 *b = buf;
    while (n) {
        pte_t *e = walk(p->pml4, va, false);
        if (!e || !(*e & PTE_PRESENT)) return false;
        UINT64 off = va & 0xFFF, chunk = PAGE_SIZE - off;
        if (chunk > n) chunk = n;
        UINT8 *k = (UINT8 *)(uintptr_t)(PHYSMAP_BASE + (*e & PTE_ADDR_MASK) + off);
        if (to_user) memcpy(k, b, chunk); else memcpy(b, k, chunk);
        va += chunk; b += chunk; n -= chunk;
    }
    return true;
}

bool um_write(UmProcess *p, UINT64 va, const void *src, UINT64 n) { return copy_pages(p, va, (void *)src, n, true); }
bool um_read(UmProcess *p, UINT64 va, void *dst, UINT64 n)        { return copy_pages(p, va, dst, n, false); }

/* Free every user page and page table, then the PML4 itself. */
static void free_address_space(UINT64 pml4)
{
    pte_t *l4 = PT(pml4);
    for (int i = 0; i < 256; i++) {
        if (!(l4[i] & PTE_PRESENT)) continue;
        pte_t *l3 = PT(l4[i]);
        for (int j = 0; j < 512; j++) {
            if (!(l3[j] & PTE_PRESENT)) continue;
            pte_t *l2 = PT(l3[j]);
            for (int k = 0; k < 512; k++) {
                if (!(l2[k] & PTE_PRESENT)) continue;
                pte_t *l1 = PT(l2[k]);
                for (int m = 0; m < 512; m++)
                    if (l1[m] & PTE_PRESENT) pmm_free_page(l1[m] & PTE_ADDR_MASK);
                pmm_free_page(l2[k] & PTE_ADDR_MASK);
            }
            pmm_free_page(l3[j] & PTE_ADDR_MASK);
        }
        pmm_free_page(l4[i] & PTE_ADDR_MASK);
    }
    pmm_free_page(pml4);
}

/* -----------------------------------------------------------------------
 * Regions (reserved address ranges)
 * ----------------------------------------------------------------------- */
bool um_is_free(UmProcess *p, UINT64 base, UINT64 size)
{
    if (base < 0x10000 || base + size > UINT64_C(0x00007FFFFFFF0000) || base + size < base) return false;
    for (int i = 0; i < p->nregions; i++) {
        UmRegion *r = &p->regions[i];
        if (base < r->base + r->size && r->base < base + size) return false;
    }
    return true;
}

UINT64 um_find_free(UmProcess *p, UINT64 size, UINT64 lo, UINT64 hi)
{
    size = (size + 0xFFFF) & ~0xFFFFULL;                  /* 64 KiB granularity */
    for (UINT64 a = lo; a + size <= hi; ) {
        UINT64 next = 0;
        for (int i = 0; i < p->nregions; i++) {
            UmRegion *r = &p->regions[i];
            if (a < r->base + r->size && r->base < a + size) {
                UINT64 e = (r->base + r->size + 0xFFFF) & ~0xFFFFULL;
                if (e > next) next = e;
            }
        }
        if (!next) return a;
        a = next;
    }
    return 0;
}

UmRegion *um_region_add(UmProcess *p, UINT64 base, UINT64 size, UINT32 protect, bool image)
{
    if (p->nregions >= UM_MAX_REGIONS) return NULL;
    UmRegion *r = &p->regions[p->nregions++];
    r->base = base;
    r->size = (size + 0xFFF) & ~0xFFFULL;
    r->protect = protect;
    r->image = image;
    return r;
}

UmRegion *um_region_find(UmProcess *p, UINT64 va)
{
    for (int i = 0; i < p->nregions; i++)
        if (va >= p->regions[i].base && va < p->regions[i].base + p->regions[i].size)
            return &p->regions[i];
    return NULL;
}

void um_region_remove(UmProcess *p, UmRegion *r)
{
    int i = (int)(r - p->regions);
    p->regions[i] = p->regions[--p->nregions];
}

/* -----------------------------------------------------------------------
 * PE loader
 * ----------------------------------------------------------------------- */
typedef struct {
    UINT8   *img;               /* assembled image (kernel copy) */
    UINT32   size;              /* SizeOfImage */
    UINT64   base;              /* chosen load address */
    UINT32   entry;             /* AddressOfEntryPoint (RVA) */
    UINT32   exp_rva, exp_size; /* export directory */
    UINT32   hdr_size;
} Image;

typedef struct {
    UmProcess *p;
    Image      img[UM_MAX_MODULES];
    char      *err;
    int        err_cap;
} Loader;

static UINT16 rd16(const UINT8 *b) { return (UINT16)(b[0] | b[1] << 8); }
static UINT32 rd32(const UINT8 *b) { return (UINT32)b[0] | (UINT32)b[1] << 8 | (UINT32)b[2] << 16 | (UINT32)b[3] << 24; }
static UINT64 rd64(const UINT8 *b) { return rd32(b) | (UINT64)rd32(b + 4) << 32; }
static void   wr64(UINT8 *b, UINT64 v) { for (int i = 0; i < 8; i++) b[i] = (UINT8)(v >> (8 * i)); }

static void lower_copy(char *dst, const char *src, int cap)
{
    int i = 0;
    for (; src[i] && i < cap - 1; i++) dst[i] = (src[i] >= 'A' && src[i] <= 'Z') ? src[i] + 32 : src[i];
    dst[i] = '\0';
}

static int load_module(Loader *L, RamNode *file, const char *name, int depth);

/* Address of an export (by name, or ordinal if name == NULL); 0 if none. */
static UINT64 find_export(Loader *L, int m, const char *name, UINT32 ordinal, int depth)
{
    Image *im = &L->img[m];
    if (!im->exp_rva || im->exp_rva + 40 > im->size) return 0;
    const UINT8 *ed = im->img + im->exp_rva;
    UINT32 base = rd32(ed + 16), nfunc = rd32(ed + 20), nnames = rd32(ed + 24);
    UINT32 funcs = rd32(ed + 28), names = rd32(ed + 32), ords = rd32(ed + 36);
    if (funcs + 4ULL * nfunc > im->size || names + 4ULL * nnames > im->size || ords + 2ULL * nnames > im->size)
        return 0;
    UINT32 idx = 0xFFFFFFFF;
    if (name) {
        for (UINT32 i = 0; i < nnames; i++) {
            UINT32 nr = rd32(im->img + names + 4 * i);
            if (nr < im->size && !strcmp((const char *)im->img + nr, name)) {
                idx = rd16(im->img + ords + 2 * i);
                break;
            }
        }
    } else {
        idx = ordinal - base;
    }
    if (idx >= nfunc) return 0;
    UINT32 rva = rd32(im->img + funcs + 4 * idx);
    if (!rva || rva >= im->size) return 0;
    if (rva >= im->exp_rva && rva < im->exp_rva + im->exp_size) {
        /* forwarder "DLL.Function" */
        const char *fw = (const char *)im->img + rva;
        const char *dot = strchr(fw, '.');
        if (!dot || depth > 4 || dot - fw > 20) return 0;
        char dll[32], fn[64];
        int n = (int)(dot - fw);
        memcpy(dll, fw, (size_t)n);
        memcpy(dll + n, ".dll", 5);
        strncpy(fn, dot + 1, sizeof(fn) - 1);
        fn[sizeof(fn) - 1] = '\0';
        int fm = load_module(L, NULL, dll, depth + 1);
        return fm < 0 ? 0 : find_export(L, fm, fn, 0, depth + 1);
    }
    return im->base + rva;
}

static int fail(Loader *L, const char *fmt, const char *arg)
{
    ksnprintf(L->err, L->err_cap, fmt, arg);
    return -1;
}

/* Load @file (or C:\Windows\System32\@name when file is NULL); returns
 * the module index. */
static int load_module(Loader *L, RamNode *file, const char *name, int depth)
{
    UmProcess *p = L->p;
    char lname[32];
    lower_copy(lname, name, sizeof(lname));
    for (int i = 0; i < p->nmodules; i++)
        if (!strcmp(p->modules[i].name, lname)) return i;
    if (depth > 8) return fail(L, "Imports nested too deeply at %s", name);
    if (p->nmodules >= UM_MAX_MODULES) return fail(L, "Too many DLLs (at %s)", name);

    if (!file) {
        RamNode *sys = RamfsResolve(NULL, "\\Windows\\System32");
        file = sys ? RamfsFind(sys, name) : NULL;
        if (!file || file->dir) return fail(L, "The DLL %s was not found in C:\\Windows\\System32", name);
    }
    const UINT8 *f = (const UINT8 *)file->data;
    UINT32 fsz = file->size;
    if (fsz < 0x40 || rd16(f) != 0x5A4D) return fail(L, "%s is not a Windows program (no MZ header)", name);
    UINT32 nt = rd32(f + 0x3C);
    if (nt > fsz - 0x108 || rd32(f + nt) != 0x00004550) return fail(L, "%s is not a valid PE file", name);
    const UINT8 *fh = f + nt + 4, *oh = fh + 20;
    if (rd16(fh) != 0x8664) return fail(L, "%s is not a 64-bit (x64) program", name);
    if (rd16(oh) != 0x20B) return fail(L, "%s is not a PE32+ image", name);
    UINT16 nsec = rd16(fh + 2), opt_size = rd16(fh + 16), chars = rd16(fh + 18);
    UINT32 size = rd32(oh + 56), hdr = rd32(oh + 60), ndirs = rd32(oh + 108);
    UINT64 pref = rd64(oh + 24);
    if (!size || size > 64 * 1024 * 1024 || hdr > fsz || hdr > size)
        return fail(L, "%s has an invalid image size", name);
    const UINT8 *sec = oh + opt_size;
    if ((UINT64)(sec - f) + 40ULL * nsec > fsz) return fail(L, "%s has a truncated section table", name);
    const UINT8 *dir = oh + 112;
    UINT32 dirv[16] = { 0 }, dirs[16] = { 0 };
    for (UINT32 i = 0; i < ndirs && i < 16; i++) { dirv[i] = rd32(dir + 8 * i); dirs[i] = rd32(dir + 8 * i + 4); }
    if (dirv[9]) return fail(L, "%s uses thread-local storage (not supported yet)", name);

    int m = p->nmodules;
    Image *im = &L->img[m];
    memset(im, 0, sizeof(*im));
    im->size = (size + 0xFFF) & ~0xFFFU;
    im->img = kzalloc(im->size + 16);             /* zero tail: names always terminate */
    if (!im->img) return fail(L, "Out of memory loading %s", name);
    memcpy(im->img, f, hdr);
    for (int i = 0; i < nsec; i++) {
        const UINT8 *s = sec + 40 * i;
        UINT32 va = rd32(s + 12), vsz = rd32(s + 8), raw = rd32(s + 16), ptr = rd32(s + 20);
        UINT32 n = raw < vsz || !vsz ? raw : vsz;
        if (va >= im->size) { kfree(im->img); return fail(L, "%s has a section outside the image", name); }
        if (n > im->size - va) n = im->size - va;
        if (ptr > fsz || n > fsz - ptr) n = ptr > fsz ? 0 : fsz - ptr;
        memcpy(im->img + va, f + ptr, n);
    }
    im->entry = rd32(oh + 16);
    im->exp_rva = dirv[0]; im->exp_size = dirs[0];
    im->hdr_size = hdr;

    /* Choose the load address */
    UINT64 base = pref;
    if (!um_is_free(p, base, im->size)) {
        if (!dirv[5] || (chars & 0x0001 /* RELOCS_STRIPPED */)) {
            kfree(im->img);
            return fail(L, "%s cannot be relocated and its address is taken", name);
        }
        base = um_find_free(p, im->size, UINT64_C(0x0000000180000000), UINT64_C(0x00007FF000000000));
        if (!base) { kfree(im->img); return fail(L, "No address space left for %s", name); }
    }
    im->base = base;
    /* Relocations */
    INT64 delta = (INT64)(base - pref);
    if (delta && dirv[5]) {
        UINT32 off = dirv[5], end = dirv[5] + dirs[5];
        if (end > im->size) end = im->size;
        while (off + 8 <= end) {
            UINT32 page = rd32(im->img + off), bsz = rd32(im->img + off + 4);
            if (bsz < 8 || off + bsz > end) break;
            for (UINT32 e = 8; e + 2 <= bsz; e += 2) {
                UINT16 ent = rd16(im->img + off + e);
                UINT32 at = page + (ent & 0xFFF);
                if ((ent >> 12) == 10 && at + 8 <= im->size)
                    wr64(im->img + at, rd64(im->img + at) + (UINT64)delta);
            }
            off += bsz;
        }
    }
    /* Register before resolving imports (DLLs may import each other) */
    um_region_add(p, base, im->size, 0x04, true);
    UmModule *mod = &p->modules[p->nmodules++];
    strncpy(mod->name, lname, sizeof(mod->name) - 1);
    mod->base = base;
    mod->size = im->size;

    /* Imports */
    for (UINT32 d = dirv[1]; d && d + 20 <= im->size; d += 20) {
        UINT32 ilt = rd32(im->img + d), nm = rd32(im->img + d + 12), iat = rd32(im->img + d + 16);
        if (!nm && !iat) break;
        if (nm >= im->size) return fail(L, "%s has a corrupt import table", name);
        char dll[32];
        strncpy(dll, (const char *)im->img + nm, sizeof(dll) - 1);
        dll[sizeof(dll) - 1] = '\0';
        int dm = load_module(L, NULL, dll, depth + 1);
        if (dm < 0) return -1;
        im = &L->img[m];                            /* (stable, but be explicit) */
        UINT32 t = ilt ? ilt : iat;
        for (UINT32 k = 0; t + 8 * (k + 1) <= im->size && iat + 8 * (k + 1) <= im->size; k++) {
            UINT64 th = rd64(im->img + t + 8 * k);
            if (!th) break;
            UINT64 addr;
            char what[80];
            if (th >> 63) {
                addr = find_export(L, dm, NULL, (UINT32)(th & 0xFFFF), 0);
                ksnprintf(what, sizeof(what), "#%u in %s", (unsigned)(th & 0xFFFF), dll);
            } else {
                UINT32 hn = (UINT32)th;
                if (hn + 2 >= im->size) return fail(L, "%s has a corrupt import entry", name);
                const char *fn = (const char *)im->img + hn + 2;
                addr = find_export(L, dm, fn, 0, 0);
                ksnprintf(what, sizeof(what), "%s in %s", fn, dll);
            }
            if (!addr) return fail(L, "The procedure entry point %s could not be located", what);
            wr64(im->img + iat + 8 * k, addr);
        }
    }

    /* Commit and copy in; then per-section protection */
    if (!um_commit(p, base, im->size, 0x04) || !um_write(p, base, im->img, im->size))
        return fail(L, "Out of memory loading %s", name);
    um_commit(p, base, hdr, 0x02);                         /* headers: read-only */
    for (int i = 0; i < nsec; i++) {
        const UINT8 *s = sec + 40 * i;
        UINT32 va = rd32(s + 12), vsz = rd32(s + 8), ch = rd32(s + 36);
        if (!vsz) vsz = rd32(s + 16);
        bool x = ch & 0x20000000, w = ch & 0x80000000;
        UINT32 prot = x ? (w ? 0x40 : 0x20) : (w ? 0x04 : 0x02);
        if (va < im->size) um_commit(p, base + va, vsz > im->size - va ? im->size - va : vsz, prot);
    }
    return m;
}

/* -----------------------------------------------------------------------
 * Process environment: PEB, TEB, RTL_USER_PROCESS_PARAMETERS
 * ----------------------------------------------------------------------- */
static void put_u16(UINT8 *b, UINT16 v) { b[0] = (UINT8)v; b[1] = (UINT8)(v >> 8); }
static void put_u32(UINT8 *b, UINT32 v) { for (int i = 0; i < 4; i++) b[i] = (UINT8)(v >> (8 * i)); }

/* Append an ASCII/UTF-8 string as UTF-16 (NUL-terminated) at *off; fill
 * the UNICODE_STRING at @us.  Returns false if it doesn't fit. */
static bool put_ustr(UINT8 *buf, UINT32 cap, UINT32 *off, UINT8 *us, const char *s)
{
    UINT32 n = (UINT32)strlen(s);
    if (*off + 2 * (n + 1) > cap) return false;
    UINT8 *d = buf + *off;
    for (UINT32 i = 0; i < n; i++) put_u16(d + 2 * i, (UINT8)s[i]);
    put_u16(d + 2 * n, 0);
    if (us) {
        put_u16(us, (UINT16)(2 * n));
        put_u16(us + 2, (UINT16)(2 * n + 2));
        wr64(us + 8, UM_PARAMS_VA + *off);
    }
    *off += (2 * (n + 1) + 7) & ~7U;
    return true;
}

static bool setup_environment(UmProcess *p, UINT64 image_base, const char *image_path,
                              const char *cmdline, const char *cwd_path)
{
    UINT32 sz = UM_PARAMS_PAGES * PAGE_SIZE;
    UINT8 *pp = kzalloc(sz);
    UINT8 *peb = kzalloc(PAGE_SIZE), *teb = kzalloc(PAGE_SIZE);
    bool ok = pp && peb && teb;
    if (ok) {
        /* RTL_USER_PROCESS_PARAMETERS */
        UINT32 off = 0x400;
        put_u32(pp + 0x00, sz);
        put_u32(pp + 0x04, sz);
        put_u32(pp + 0x08, 1);                              /* NORMALIZED */
        wr64(pp + 0x20, 4);                                 /* StandardInput  */
        wr64(pp + 0x28, 8);                                 /* StandardOutput */
        wr64(pp + 0x30, 12);                                /* StandardError  */
        ok = put_ustr(pp, sz, &off, pp + 0x38, cwd_path) &&         /* CurrentDirectory */
             put_ustr(pp, sz, &off, pp + 0x50, "C:\\Windows\\System32") && /* DllPath */
             put_ustr(pp, sz, &off, pp + 0x60, image_path) &&       /* ImagePathName */
             put_ustr(pp, sz, &off, pp + 0x70, cmdline);            /* CommandLine */
        /* Environment block: NUL-separated UTF-16 strings, double NUL */
        static const char *env[] = {
            "COMPUTERNAME=NOVA-PC", "OS=NovaOS", "PATH=C:\\Programs;C:\\Windows\\System32",
            "PATHEXT=.EXE", "SystemRoot=C:\\Windows", "TEMP=C:\\Temp", "USERNAME=dean", NULL
        };
        wr64(pp + 0x80, UM_PARAMS_VA + off);
        for (int i = 0; ok && env[i]; i++) {
            UINT32 n = (UINT32)strlen(env[i]);
            if (off + 2 * (n + 1) + 2 > sz) { ok = false; break; }
            for (UINT32 k = 0; k < n; k++) put_u16(pp + off + 2 * k, (UINT8)env[i][k]);
            off += 2 * (n + 1);
        }
        off += 2;                                           /* final NUL */
        wr64(pp + 0x3F0, off);                              /* EnvironmentSize (0x3F0) */

        /* PEB */
        wr64(peb + 0x10, image_base);                       /* ImageBaseAddress */
        wr64(peb + 0x20, UM_PARAMS_VA);                     /* ProcessParameters */
        put_u32(peb + 0x118, 10);                           /* OSMajorVersion */
        put_u32(peb + 0x11C, 0);                            /* OSMinorVersion */
        put_u16(peb + 0x120, 18362);                        /* OSBuildNumber (1903) */
        put_u32(peb + 0x124, 2);                            /* OSPlatformId: NT */

        /* TEB */
        wr64(teb + 0x08, UM_STACK_TOP);                     /* StackBase */
        wr64(teb + 0x10, UM_STACK_TOP - UM_STACK_SIZE);     /* StackLimit */
        wr64(teb + 0x30, UM_TEB_VA);                        /* Self */
        wr64(teb + 0x40, p->pid);                           /* ClientId.UniqueProcess */
        wr64(teb + 0x48, p->pid + 4);                       /* ClientId.UniqueThread */
        wr64(teb + 0x60, UM_PEB_VA);                        /* ProcessEnvironmentBlock */

        ok = ok && um_region_add(p, UM_PEB_VA, (2 + UM_PARAMS_PAGES) * PAGE_SIZE, 0x04, false) &&
             um_commit(p, UM_PEB_VA, (2 + UM_PARAMS_PAGES) * PAGE_SIZE, 0x04) &&
             um_write(p, UM_PEB_VA, peb, PAGE_SIZE) &&
             um_write(p, UM_TEB_VA, teb, PAGE_SIZE) &&
             um_write(p, UM_PARAMS_VA, pp, sz);
    }
    kfree(pp); kfree(peb); kfree(teb);
    return ok;
}

/* -----------------------------------------------------------------------
 * Threads
 * ----------------------------------------------------------------------- */
typedef struct { UINT64 entry, rsp; } StartInfo;

static void um_thread_start(void *arg)
{
    StartInfo si = *(StartInfo *)arg;
    kfree(arg);
    cli();
    /* Enter ring 3: RCX = PEB (as Windows passes it to the process entry) */
    UINT64 cs = GDT_USER_CODE | 3, ss = GDT_USER_DATA | 3, rfl = 0x202, peb = UM_PEB_VA;
    __asm__ volatile (
        "push %[ss]\n\t"
        "push %[sp]\n\t"
        "push %[rfl]\n\t"
        "push %[cs]\n\t"
        "push %[ip]\n\t"
        "mov %[peb], %%rcx\n\t"
        "xor %%eax, %%eax\n\t"  "xor %%edx, %%edx\n\t"  "xor %%ebx, %%ebx\n\t"
        "xor %%esi, %%esi\n\t"  "xor %%edi, %%edi\n\t"  "xor %%ebp, %%ebp\n\t"
        "xor %%r8d, %%r8d\n\t"  "xor %%r9d, %%r9d\n\t"  "xor %%r10d, %%r10d\n\t"
        "xor %%r11d, %%r11d\n\t" "xor %%r12d, %%r12d\n\t" "xor %%r13d, %%r13d\n\t"
        "xor %%r14d, %%r14d\n\t" "xor %%r15d, %%r15d\n\t"
        "iretq\n\t"
        : : [ss] "r"(ss), [sp] "r"(si.rsp), [rfl] "r"(rfl), [cs] "r"(cs), [ip] "r"(si.entry), [peb] "r"(peb)
        : "memory");
    __builtin_unreachable();
}

/* -----------------------------------------------------------------------
 * Spawning
 * ----------------------------------------------------------------------- */
static void destroy(UmProcess *p)
{
    if (p->pml4) free_address_space(p->pml4);
    if (p->con) UmConsoleRelease(p->con);
    kfree(p);
}

RamNode *UmFindProgram(RamNode *cwd, const char *name)
{
    char buf[RAMFS_PATH_MAX];
    bool has_ext = strchr(name, '.') != NULL;
    const char *dirs[] = { NULL, "\\Programs", "\\Windows\\System32" };
    bool path = strchr(name, '\\') || strchr(name, ':');
    for (int d = 0; d < 3; d++) {
        if (path && d) break;
        for (int e = 0; e < 2; e++) {
            if (e && has_ext) break;
            ksnprintf(buf, sizeof(buf), "%s%s%s", name, e ? ".exe" : "", "");
            RamNode *base = d ? RamfsResolve(NULL, dirs[d]) : cwd;
            if (!base && d) continue;
            RamNode *n = RamfsResolve(base, buf);
            if (n && !n->dir) return n;
        }
    }
    return NULL;
}

UmProcess *UmSpawn(RamNode *exe, const char *cmdline, RamNode *cwd, UmConsole *con,
                   char *err, int err_cap)
{
    err[0] = '\0';
    UmProcess *p = kzalloc(sizeof(*p));
    if (!p) { ksnprintf(err, err_cap, "Out of memory"); return NULL; }
    strncpy(p->name, exe->name, sizeof(p->name) - 1);
    plock();
    p->pid = g_next_pid;
    g_next_pid += 4;
    punlock();
    p->pml4 = paging_create_process_pt();
    if (!p->pml4) { kfree(p); ksnprintf(err, err_cap, "Out of memory"); return NULL; }
    p->cwd = cwd ? cwd : RamfsRoot();
    p->con = um_console_ref(con);

    Loader *L = kzalloc(sizeof(*L));
    if (!L) { destroy(p); ksnprintf(err, err_cap, "Out of memory"); return NULL; }
    L->p = p; L->err = err; L->err_cap = err_cap;
    int m = load_module(L, exe, exe->name, 0);
    UINT64 base = m < 0 ? 0 : L->img[m].base;
    UINT32 entry = m < 0 ? 0 : L->img[m].entry;
    for (int i = 0; i < p->nmodules; i++) kfree(L->img[i].img);
    kfree(L);
    if (m < 0) { destroy(p); return NULL; }
    if (!entry) { destroy(p); ksnprintf(err, err_cap, "%s has no entry point (is it a DLL?)", exe->name); return NULL; }

    char image_path[RAMFS_PATH_MAX], cwd_path[RAMFS_PATH_MAX];
    RamfsPath(exe, image_path, sizeof(image_path));
    RamfsPath(p->cwd, cwd_path, sizeof(cwd_path));
    int cl = (int)strlen(cwd_path);
    if (cl && cwd_path[cl - 1] != '\\' && cl < (int)sizeof(cwd_path) - 1) { cwd_path[cl] = '\\'; cwd_path[cl + 1] = 0; }

    UINT64 stack_lo = UM_STACK_TOP - UM_STACK_SIZE;
    if (!um_region_add(p, stack_lo, UM_STACK_SIZE, 0x04, false) ||
        !um_commit(p, stack_lo, UM_STACK_SIZE, 0x04) ||
        !setup_environment(p, base, image_path, cmdline, cwd_path)) {
        destroy(p);
        ksnprintf(err, err_cap, "Out of memory");
        return NULL;
    }
    UINT64 rsp = UM_STACK_TOP - 0x28, zero = 0;           /* ≡ 8 mod 16, as after a call */
    um_write(p, rsp, &zero, 8);                            /* return address: none */

    for (int i = 0; i < 3; i++) p->handles[i].kind = i ? H_CON_OUT : H_CON_IN;   /* 4, 8, 12 */

    StartInfo *si = kmalloc(sizeof(*si));
    UINT8 *fpu = kernel_alloc_pages(1);
    if (!si || !fpu) { kfree(si); if (fpu) kernel_free_pages(fpu, 1); destroy(p); ksnprintf(err, err_cap, "Out of memory"); return NULL; }
    memset(fpu, 0, PAGE_SIZE);
    fpu[0] = 0x7F; fpu[1] = 0x03;                          /* FCW = 0x037F */
    put_u32(fpu + 24, 0x1F80);                             /* MXCSR default */
    si->entry = base + entry;
    si->rsp = rsp;

    plock();
    int slot = -1;
    for (int i = 0; i < UM_MAX_PROCS; i++) if (!g_procs[i]) { slot = i; break; }
    if (slot < 0) {
        punlock();
        kfree(si); kernel_free_pages(fpu, 1); destroy(p);
        ksnprintf(err, err_cap, "Too many programs are running");
        return NULL;
    }
    g_procs[slot] = p;
    punlock();

    /* Create the thread with interrupts off so it can't run before its
     * user-mode state is filled in. */
    IrqState s = irq_save();
    char tname[THREAD_NAME_MAX];
    ksnprintf(tname, sizeof(tname), "%s", p->name);
    Thread *t = sched_create_thread_ex(tname, um_thread_start, si, 8, 32 * 1024);
    if (t) {
        t->um = p;
        t->cr3 = p->pml4;
        t->fpu = fpu;
        t->gs_base = UM_TEB_VA;                            /* user GS = TEB */
        t->kgs_base = (UINT64)(uintptr_t)KiGetCurrentKpcr();
        p->thread = t;
    }
    irq_restore(s);
    if (!t) {
        plock(); g_procs[slot] = NULL; punlock();
        kfree(si); kernel_free_pages(fpu, 1); destroy(p);
        ksnprintf(err, err_cap, "Out of memory");
        return NULL;
    }
    kprintf("[UM] Started %s (PID %u): %d module(s), entry 0x%llx, %u KB\n", p->name, p->pid,
            p->nmodules, (unsigned long long)(base + entry), p->pages * 4);
    return p;
}

/* -----------------------------------------------------------------------
 * Ending processes
 * ----------------------------------------------------------------------- */
void UmExitCurrent(UINT32 status)
{
    cli();
    UmProcess *p = UmCurrent();
    p->exit_status = status;
    p->exited = true;
    sched_exit_current();
}

void UmReturnToUser(void)
{
    UmProcess *p = UmCurrent();
    if (p && p->kill_pending) {
        if (!p->why[0]) ksnprintf(p->why, sizeof(p->why), "%s", "Terminated");
        UmExitCurrent(p->kill_status);
    }
}

void UmFault(UINT32 status, UINT64 rip, UINT64 addr)
{
    UmProcess *p = UmCurrent();
    const char *what = status == UM_STATUS_ACCESS_VIOLATION ? "access violation" :
                       status == 0xC0000094u ? "integer divide by zero" :
                       status == 0xC000001Du ? "illegal instruction" : "unhandled exception";
    const UmModule *mod = NULL;
    for (int i = 0; i < p->nmodules; i++)
        if (rip >= p->modules[i].base && rip < p->modules[i].base + p->modules[i].size) mod = &p->modules[i];
    char where[64];
    if (mod) ksnprintf(where, sizeof(where), "%s+0x%llx", mod->name, (unsigned long long)(rip - mod->base));
    else     ksnprintf(where, sizeof(where), "0x%llx", (unsigned long long)rip);
    if (status == UM_STATUS_ACCESS_VIOLATION)
        ksnprintf(p->why, sizeof(p->why), "crashed: %s at %s (address 0x%llx)", what, where,
                  (unsigned long long)addr);
    else
        ksnprintf(p->why, sizeof(p->why), "crashed: %s at %s", what, where);
    kprintf("[UM] %s (PID %u) %s\n", p->name, p->pid, p->why);
    UmExitCurrent(status);
}

void UmKill(UmProcess *p, UINT32 status)
{
    if (!p || p->exited || p->kill_pending) return;
    p->kill_status = status;
    ksnprintf(p->why, sizeof(p->why), "%s", status == UM_STATUS_CONTROL_C_EXIT ? "stopped (Ctrl+C)" : "terminated");
    p->kill_pending = true;
}

bool UmHasExited(UmProcess *p, UINT32 *status, char *why, int why_cap)
{
    if (!p->exited) return false;
    if (status) *status = p->exit_status;
    if (why && why_cap) ksnprintf(why, why_cap, "%s", p->why);
    return true;
}

void UmRelease(UmProcess *p)
{
    if (!p) return;
    if (!p->exited) UmKill(p, 1);
    p->released = true;
}

UINT32      UmPid(const UmProcess *p)  { return p->pid; }
const char *UmName(const UmProcess *p) { return p->name; }

void UmPoll(void)
{
    for (int i = 0; i < UM_MAX_PROCS; i++) {
        UmProcess *p = g_procs[i];
        if (!p || !p->exited || !sched_thread_gone(p->thread)) continue;
        if (!p->reclaimed) {
            um_close_all_handles(p);
            free_address_space(p->pml4);
            p->pml4 = 0;
            p->pages = 0;
            if (p->thread->fpu) kernel_free_pages(p->thread->fpu, 1);
            sched_free_thread(p->thread);
            p->thread = NULL;
            p->reclaimed = true;
        }
        if (p->released) {
            plock(); g_procs[i] = NULL; punlock();
            if (p->con) UmConsoleRelease(p->con);
            kfree(p);
        }
    }
}

static bool thread_gone(UmProcess *p) { return !p->thread || sched_thread_gone(p->thread); }

int UmList(UmProcInfo *out, int max)
{
    int n = 0;
    for (int i = 0; i < UM_MAX_PROCS && n < max; i++) {
        UmProcess *p = g_procs[i];
        if (!p || (p->exited && thread_gone(p) && p->reclaimed)) continue;
        out[n].pid = p->pid;
        strncpy(out[n].name, p->name, sizeof(out[n].name) - 1);
        out[n].name[sizeof(out[n].name) - 1] = '\0';
        out[n].mem_kb = p->pages * 4;
        out[n].exited = p->exited;
        n++;
    }
    return n;
}

bool UmKillPid(UINT32 pid)
{
    for (int i = 0; i < UM_MAX_PROCS; i++) {
        UmProcess *p = g_procs[i];
        if (p && p->pid == pid && !p->exited) { UmKill(p, 1); return true; }
    }
    return false;
}
