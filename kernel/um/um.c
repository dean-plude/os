/*
 * um.c — NovaOS user-mode subsystem: processes, threads, address spaces,
 * PE loader
 *
 * Address spaces: each process gets its own PML4 whose upper half is the
 * kernel's (paging_create_process_pt).  User memory is tracked as reserved
 * regions; pages are committed (backed by zeroed frames) on demand of the
 * loader and NtAllocateVirtualMemory, and all of it is freed by walking
 * the lower half of the page table when the process is reclaimed.
 *
 * Loader: the executable and every DLL it imports (recursively: the
 * program's own directory first, then C:\Windows\System32) are assembled
 * in kernel buffers — sections placed, base relocations applied, import
 * address tables filled from the exporting modules — then copied into
 * committed user pages with per-section protection (NX for data,
 * read-only headers).  The kernel only maps modules: ntdll runs their
 * initialization (TLS, DllMain) in user mode, in the order recorded on the
 * loader-info page (dependencies first).  LoadLibrary reaches the same
 * loader through NtNovaLoadDll.
 *
 * Threads: every thread (the first one too) enters user mode at
 * ntdll!RtlUserThreadStart(start, argument), which runs the loader or
 * thread-attach work and then calls start.  A process ends when its last
 * thread has ended, or all at once through NtTerminateProcess.
 */

#include "../fs/persist.h"
#include "../fs/drives.h"
#include "../arch/x86_64/idt.h"
#include "um_internal.h"
#include "../ke/syscall.h"
#include "../ke/scheduler.h"
#include "../ke/probe.h"
#include "../ke/printf.h"
#include "../ke/kpcr.h"
#include "../ke/smp.h"
#include "../mm/vmm.h"
#include "../mm/pmm.h"
#include "../lib/string.h"
#include "../arch/x86_64/cpu.h"
#include "../arch/x86_64/gdt.h"
#include "../arch/x86_64/paging.h"
#include "userland_files.h"
#include "../gdi/png.h"

static UmProcess     *g_procs[UM_MAX_PROCS];
static UINT32         g_next_id = 100;
static volatile int   g_plock;

static void plock(void)   { while (__atomic_exchange_n(&g_plock, 1, __ATOMIC_ACQUIRE)) sched_yield(); }
static void punlock(void) { __atomic_store_n(&g_plock, 0, __ATOMIC_RELEASE); }

/* Process and thread IDs share one space, as on Windows (multiples of 4) */
UINT32 um_new_id(void)
{
    return __atomic_fetch_add(&g_next_id, 4, __ATOMIC_RELAXED);
}

/* -----------------------------------------------------------------------
 * Locks
 * ----------------------------------------------------------------------- */
void um_lock(UmLock *l)
{
    Thread *me = sched_current();
    if (l->owner == me) { l->depth++; return; }
    /* Held briefly as a rule: spin a while (the holder is likely running
     * on another CPU) before giving the CPU away — but not holding the big
     * kernel lock, which the holder may be waiting for (yielding lets it go) */
    int most = bkl_held() ? 0 : 2000;
    for (int spins = 0; __atomic_exchange_n(&l->v, 1, __ATOMIC_ACQUIRE); ) {
        while (__atomic_load_n(&l->v, __ATOMIC_RELAXED)) {
            if (++spins < most) pause_cpu();
            else { sched_yield(); spins = 0; }
        }
    }
    l->owner = me;
    l->depth = 1;
}

void um_unlock(UmLock *l)
{
    if (--l->depth > 0) return;
    l->owner = NULL;
    __atomic_store_n(&l->v, 0, __ATOMIC_RELEASE);
}

/* Wait (spin a while, then yield) until @cond holds.  Spinning holding the
 * big kernel lock too: what is waited for (readers leaving) takes moments,
 * and a thread that yields here may not run again for a whole time slice,
 * holding up everyone its taken write lock keeps out. */
#define UM_WAIT_UNTIL(cond) do { \
        int most_ = 2000; \
        for (int spins_ = 0; !(cond); ) { \
            if (++spins_ < most_) pause_cpu(); \
            else { sched_yield(); spins_ = 0; } \
        } \
    } while (0)

/* Readers count on their CPU's counter (and a thread that moved count down
 * on another's): the writer waits for the sum to reach 0 */
static volatile int *my_readers(UmRwLock *l) { return &l->readers[KiGetCurrentKpcr()->CpuNumber % MAX_CPUS].n; }

static int readers_of(UmRwLock *l)
{
    int sum = 0;
    for (int i = 0; i < MAX_CPUS; i++) sum += __atomic_load_n(&l->readers[i].n, __ATOMIC_SEQ_CST);
    return sum;
}

void um_lock_excl(UmRwLock *l)
{
    um_lock(&l->w);                             /* new readers stay out */
    if (l->w.depth > 1) return;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    UM_WAIT_UNTIL(readers_of(l) == 0);
}

void um_unlock_excl(UmRwLock *l) { um_unlock(&l->w); }

void um_lock_shared(UmRwLock *l)
{
    if (l->w.owner == sched_current()) { l->w.depth++; return; }   /* the writer */
    for (;;) {
        volatile int *r = my_readers(l);
        __atomic_add_fetch(r, 1, __ATOMIC_SEQ_CST);
        if (!__atomic_load_n(&l->w.v, __ATOMIC_SEQ_CST)) return;
        __atomic_sub_fetch(r, 1, __ATOMIC_SEQ_CST);                /* a writer: let it go first */
        UM_WAIT_UNTIL(!__atomic_load_n(&l->w.v, __ATOMIC_RELAXED));
    }
}

void um_unlock_shared(UmRwLock *l)
{
    if (l->w.owner == sched_current()) { um_unlock(&l->w); return; }
    __atomic_sub_fetch(my_readers(l), 1, __ATOMIC_RELEASE);
}

/* The desktop lock (recursive for its owner), and the file-system lock it
 * includes: file services take only the latter.  Order: desktop, files,
 * then a process's lock. */
static UmLock g_desktop;
static UmRwLock g_fs;
void DesktopLock(void)   { um_lock(&g_desktop); um_lock_excl(&g_fs); }
void DesktopUnlock(void) { um_unlock_excl(&g_fs); um_unlock(&g_desktop); }
void DesktopLockAlone(void)   { um_lock(&g_desktop); }
void DesktopUnlockAlone(void) { um_unlock(&g_desktop); }
void FsLock(void)        { um_lock_excl(&g_fs); }
void FsUnlock(void)      { um_unlock_excl(&g_fs); }
void FsLockShared(void)  { um_lock_shared(&g_fs); }
void FsUnlockShared(void) { um_unlock_shared(&g_fs); }
Thread *DesktopLockOwner(void) { return g_desktop.owner; }

/* KUSER_SHARED_DATA (see below) */
static UINT8 *g_kusd;
static PADDR  g_kusd_pa;
static void   kusd_init(void);

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
    kusd_init();

    /* Install the system DLLs and programs on drive C: */
    int installed = 0;
    RamfsCreate(RamfsRoot(), "Temp", true);
    /* the profile folders every Windows program can assume exist (%APPDATA%, %LOCALAPPDATA%, ...) */
    RamfsCreate(RamfsRoot(), "ProgramData", true);
    RamNode *appdata = RamfsCreate(RamfsRoot(), "AppData", true);
    if (appdata) {
        RamfsCreate(appdata, "Roaming", true);
        RamfsCreate(appdata, "Local", true);
        RamfsCreate(appdata, "LocalLow", true);
    }
    RamfsSetMode(RAMFS_INSTALLING);                 /* system files: never saved to disk */
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
        if (f && uf->zsize) {                       /* stored compressed (the big ones: ICU, NetSurf) */
            char *buf = kmalloc(uf->size ? uf->size : 1);
            if (buf && ZlibInflate(uf->data, uf->zsize, buf, uf->size) == (long)uf->size &&
                RamfsWriteOwned(f, buf, uf->size))
                installed++;
            else {
                kfree(buf);
                kprintf("[UM] %s: could not unpack\n", uf->path);
            }
        } else if (f && RamfsWrite(f, (const char *)uf->data, uf->size)) installed++;
    }
    kprintf("[UM] User-mode subsystem ready: %d system files installed (C:\\Windows\\System32, C:\\Programs)\n",
            installed);
    PersistLoad();                                  /* the user's files (and the registry hive) from disk */
    um_registry_init();
    um_registry_pending_renames();                  /* before any program runs */
}

UmThread *UmCurrentThread(void)
{
    return (UmThread *)sched_current()->um;
}

UmProcess *UmCurrent(void)
{
    UmThread *t = UmCurrentThread();
    return t ? t->proc : NULL;
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

/* Existing entries of @p changed: this CPU drops them from its TLB as it
 * goes (invlpg), the other CPUs running @p's threads at the end. */
/* Committed pages are backed on first touch (demand-zero): until then the
 * entry is not present and carries PTE_LAZY with the page's flags. */

/* -----------------------------------------------------------------------
 * Image pages: the read-only pages of loaded modules (headers, code,
 * read-only data) are shared by every process whose page holds the same
 * bytes, as Windows shares image sections.  A browser starts many
 * processes of one 160 MB DLL; each private copy would cost that much
 * memory again.  The frames are counted in a table keyed by their
 * contents, so a module relocated or bound differently in one process
 * shares every page that still matches.  PTE_IMAGE marks them; they are
 * never writable: making one writable, or writing to it from outside
 * (another process's WriteProcessMemory), gives the process its own copy
 * first.
 * ----------------------------------------------------------------------- */
typedef struct ImgPage {
    struct ImgPage *by_hash, *by_frame;                 /* bucket chains */
    UINT64 hash;
    PADDR  frame;
    UINT32 refs;
} ImgPage;
#define IMG_BUCKETS 16384
static ImgPage *g_img_hash[IMG_BUCKETS], *g_img_frame[IMG_BUCKETS];
static KSpinLock g_img_lock = KSPINLOCK_INIT;

static UINT64 page_hash(const void *pg)
{
    const UINT64 *w = pg;
    UINT64 h = UINT64_C(0x9E3779B97F4A7C15);
    for (int i = 0; i < (int)(PAGE_SIZE / 8); i += 2) {
        h ^= w[i] * UINT64_C(0xFF51AFD7ED558CCD) + w[i + 1];
        h = ((h << 29) | (h >> 35)) * UINT64_C(0xC4CEB9FE1A85EC53);
    }
    return h ^ (h >> 31);
}

static unsigned frame_bucket(PADDR f) { return (unsigned)((f >> 12) % IMG_BUCKETS); }

/* A shared frame holding @content's page (one more reference), or 0 when
 * memory ran out */
static PADDR img_get(const void *content)
{
    UINT64 h = page_hash(content);
    ImgPage **hb = &g_img_hash[h % IMG_BUCKETS];
    IrqState s = spin_lock_irqsave(&g_img_lock);
    for (ImgPage *i = *hb; i; i = i->by_hash)
        if (i->hash == h && !memcmp(um_frame_ptr(i->frame), content, PAGE_SIZE)) {
            i->refs++;
            spin_unlock_irqrestore(&g_img_lock, s);
            return i->frame;
        }
    spin_unlock_irqrestore(&g_img_lock, s);
    ImgPage *n = kzalloc(sizeof(*n));
    PADDR f = n ? pmm_alloc_page() : 0;
    if (!f) { kfree(n); return 0; }
    memcpy(um_frame_ptr(f), content, PAGE_SIZE);
    n->hash = h; n->frame = f; n->refs = 1;
    s = spin_lock_irqsave(&g_img_lock);
    n->by_hash = *hb; *hb = n;                          /* (a twin added meanwhile only costs a page) */
    ImgPage **fb = &g_img_frame[frame_bucket(f)];
    n->by_frame = *fb; *fb = n;
    spin_unlock_irqrestore(&g_img_lock, s);
    return f;
}

/* One reference to the shared frame @f fewer; the last frees it */
static void img_put(PADDR f)
{
    IrqState s = spin_lock_irqsave(&g_img_lock);
    ImgPage **pp = &g_img_frame[frame_bucket(f)];
    while (*pp && (*pp)->frame != f) pp = &(*pp)->by_frame;
    ImgPage *i = *pp;
    if (!i || --i->refs) { spin_unlock_irqrestore(&g_img_lock, s); return; }
    *pp = i->by_frame;
    for (ImgPage **hp = &g_img_hash[i->hash % IMG_BUCKETS]; *hp; hp = &(*hp)->by_hash)
        if (*hp == i) { *hp = i->by_hash; break; }
    spin_unlock_irqrestore(&g_img_lock, s);
    pmm_free_page(f);
    kfree(i);
}

/* The module page at @va (not committed yet) maps the shared frame with
 * @content, read-only (@protect must not be writable) */
bool um_map_image_page(UmProcess *p, UINT64 va, const void *content, UINT32 protect)
{
    pte_t *e = walk(p->pml4, va, true);
    if (!e || (*e & (PTE_PRESENT | PTE_LAZY))) return false;
    PADDR f = img_get(content);
    if (!f) return false;
    *e = f | (pte_flags(protect) & ~PTE_WRITE) | PTE_IMAGE;
    p->commit++;
    __atomic_add_fetch(&p->pages, 1, __ATOMIC_RELAXED);
    return true;
}

/* The shared page at @e (@va) becomes the process's own copy, keeping
 * the entry's flags; false when memory ran out.  (Two threads may race
 * here: the entry only changes by compare-and-swap, and the loser's copy
 * goes back.) */
static bool img_privatize(UmProcess *p, UINT64 va, pte_t *e)
{
    pte_t v = __atomic_load_n(e, __ATOMIC_ACQUIRE);
    if (!(v & PTE_IMAGE)) return true;
    PADDR old = v & PTE_ADDR_MASK, f = pmm_alloc_page();
    if (!f) return false;
    memcpy(um_frame_ptr(f), um_frame_ptr(old), PAGE_SIZE);
    pte_t nv = f | (v & ~(PTE_ADDR_MASK | PTE_IMAGE));
    if (!__atomic_compare_exchange_n(e, &v, nv, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        pmm_free_page(f);
        return !(__atomic_load_n(e, __ATOMIC_ACQUIRE) & PTE_IMAGE);
    }
    if (is_current(p)) invlpg(va);
    smp_tlb_flush(p->pml4);
    img_put(old);
    return true;
}

bool um_commit(UmProcess *p, UINT64 va, UINT64 size, UINT32 protect)
{
    UINT64 f = pte_flags(protect);
    bool guard = protect & 0x100;                       /* PAGE_GUARD */
    bool ok = true, changed = false;
    for (UINT64 a = va & ~0xFFFULL; a < va + size; a += PAGE_SIZE) {
        pte_t *e = walk(p->pml4, a, true);
        if (!e) { ok = false; break; }
        if ((*e & PTE_PRESENT) && (*e & PTE_ADDR_MASK) == g_kusd_pa) continue;   /* stays read-only */
        if ((*e & PTE_IMAGE) && (f & PTE_WRITE)) {       /* a shared module page made writable: its own copy */
            if (!img_privatize(p, a, e)) { ok = false; break; }
        }
        if (*e & PTE_PRESENT) {                         /* re-commit: new protection */
            UINT64 nf = guard ? (f & ~PTE_USER) | PTE_GUARD : f;
            if (*e & PTE_IMAGE) nf &= ~PTE_WRITE;      /* (read-only: shared) */
            *e = (*e & (PTE_ADDR_MASK | PTE_SHARED | PTE_IMAGE)) | nf;
            if (is_current(p)) invlpg(a);
            changed = true;
            continue;
        }
        if (!(*e & PTE_LAZY)) p->commit++;
        *e = PTE_LAZY | (f & ~PTE_PRESENT) | (guard ? PTE_GUARD : 0);
    }
    if (changed) smp_tlb_flush(p->pml4);
    return ok;
}

/* Back the lazily committed page at @e (another thread may race us: the
 * entry only changes by compare-and-swap here) */
static bool back_page(UmProcess *p, pte_t *e)
{
    pte_t v = __atomic_load_n(e, __ATOMIC_ACQUIRE);
    if (v & PTE_PRESENT) return true;
    if (!(v & PTE_LAZY)) return false;
    PADDR fr = pmm_alloc_page();
    if (!fr) return false;
    memset((void *)(uintptr_t)(PHYSMAP_BASE + fr), 0, PAGE_SIZE);
    pte_t nv = fr | (v & ~(PTE_LAZY | PTE_ADDR_MASK)) | PTE_PRESENT;
    if (v & PTE_GUARD) nv &= ~PTE_USER;                 /* still a guard page */
    if (!__atomic_compare_exchange_n(e, &v, nv, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        pmm_free_page(fr);
        return (__atomic_load_n(e, __ATOMIC_ACQUIRE) & PTE_PRESENT) != 0;
    }
    __atomic_add_fetch(&p->pages, 1, __ATOMIC_RELAXED);
    return true;
}

bool UmDemandFault(UINT64 va)
{
    UmProcess *p = UmCurrent();
    if (!p || va >= UINT64_C(0x00007FFFFFFF0000)) return false;
    pte_t *e = walk(p->pml4, va, false);
    return e && back_page(p, e);
}

/* The program touched @va: if it is a guard page, lift the guard.  On a
 * thread's stack (between the TEB's DeallocationStack and StackBase) the
 * page below becomes the new guard page and StackLimit follows, as
 * Windows grows stacks: 1.  Out of stack: -2 (STATUS_STACK_OVERFLOW).
 * Anywhere else: -1 (STATUS_GUARD_PAGE_VIOLATION).  Not a guard page: 0. */
int UmGuardFault(UINT64 va)
{
    UmProcess *p = UmCurrent();
    UmThread *t = UmCurrentThread();
    if (!p || !t || va >= UINT64_C(0x00007FFFFFFF0000)) return 0;
    UINT64 page = va & ~0xFFFULL;
    /* (interrupts are off: no locks; the entry changes only this way) */
    pte_t *e = walk(p->pml4, va, false);
    if (!e || !(*e & PTE_GUARD)) return 0;
    if (!(*e & PTE_PRESENT) && !back_page(p, e)) return 0;
    *e = (*e & ~PTE_GUARD) | PTE_USER;
    invlpg(page);
    /* the thread's stack, from its TEB */
    UINT64 base = 0, dealloc = 0;
    UINT32 w = p->wow ? 4 : 8;
    UINT64 o_base = p->wow ? 0x4 : 0x8, o_limit = p->wow ? 0x8 : 0x10, o_dealloc = p->wow ? 0xE0C : 0x1478;
    um_read(p, t->teb + o_base, &base, w);
    um_read(p, t->teb + o_dealloc, &dealloc, w);
    int res = -1;
    if (dealloc && va >= dealloc && va < base) {
        UINT64 below = page - PAGE_SIZE;
        if (below >= dealloc + PAGE_SIZE) {               /* the lowest page stays reserved */
            pte_t *b = walk(p->pml4, below, true);
            /* read/write, as Windows grows stacks (the reservation may say PAGE_NOACCESS) */
            if (b && !(*b & (PTE_PRESENT | PTE_LAZY))) um_commit(p, below, PAGE_SIZE, 0x04 | 0x100);
            res = 1;
        } else res = -2;
        um_write(p, t->teb + o_limit, &page, w);
    }
    return res;
}

/* Pages are freed only after every CPU has dropped them from its TLB: until
 * then another thread of the program could still write to them. */
static void free_frame(PADDR f)
{
    if (f & 1) img_put(f & ~(PADDR)1);
    else pmm_free_page(f);
}

void um_decommit(UmProcess *p, UINT64 va, UINT64 size)
{
    PADDR batch[64];
    int n = 0;
    for (UINT64 a = va & ~0xFFFULL; a < va + size; a += PAGE_SIZE) {
        pte_t *e = walk(p->pml4, a, false);
        if (!e) continue;
        if (*e & PTE_SHARED) continue;                  /* a section's page: unmapped with the view */
        if (!(*e & PTE_PRESENT)) {
            if (*e & PTE_LAZY) { *e = 0; p->commit--; }
            continue;
        }
        if ((*e & PTE_ADDR_MASK) == g_kusd_pa) continue;   /* the shared page stays */
        batch[n++] = (*e & PTE_ADDR_MASK) | ((*e & PTE_IMAGE) ? 1 : 0);   /* (bit 0: a shared module page) */
        *e = 0;
        p->pages--;
        p->commit--;
        if (is_current(p)) invlpg(a);
        if (n == 64) {
            smp_tlb_flush(p->pml4);
            while (n) free_frame(batch[--n]);
        }
    }
    if (n) {
        smp_tlb_flush(p->pml4);
        while (n) free_frame(batch[--n]);
    }
}

/* -----------------------------------------------------------------------
 * Section frames: owned by the section object, mapped (PTE_SHARED) into
 * every process with a view; neither decommit nor teardown frees them
 * ----------------------------------------------------------------------- */
PADDR *um_alloc_frames(UINT64 n)
{
    if (!n || n > (UINT64_C(256) << 20) / PAGE_SIZE) return NULL;
    PADDR *f = kzalloc(sizeof(PADDR) * n);
    if (!f) return NULL;
    for (UINT64 i = 0; i < n; i++) {
        f[i] = pmm_alloc_page();
        if (!f[i]) { um_free_frames(f, i); return NULL; }
        memset((void *)(uintptr_t)(PHYSMAP_BASE + f[i]), 0, PAGE_SIZE);
    }
    return f;
}

void *um_frame_ptr(PADDR f) { return (void *)(uintptr_t)(PHYSMAP_BASE + f); }

void um_free_frames(PADDR *f, UINT64 n)
{
    if (!f) return;
    for (UINT64 i = 0; i < n; i++) if (f[i]) pmm_free_page(f[i]);
    kfree(f);
}

bool um_map_frames(UmProcess *p, UINT64 va, const PADDR *f, UINT64 n, UINT32 protect)
{
    UINT64 flags = pte_flags(protect) | PTE_SHARED;
    for (UINT64 i = 0; i < n; i++) {
        pte_t *e = walk(p->pml4, va + i * PAGE_SIZE, true);
        if (!e) { um_unmap_frames(p, va, i); return false; }
        *e = f[i] | flags;
    }
    return true;
}

void um_unmap_frames(UmProcess *p, UINT64 va, UINT64 n)
{
    for (UINT64 i = 0; i < n; i++) {
        UINT64 a = va + i * PAGE_SIZE;
        pte_t *e = walk(p->pml4, a, false);
        if (!e || !(*e & PTE_SHARED)) continue;
        *e = 0;
        if (is_current(p)) invlpg(a);
    }
    smp_tlb_flush(p->pml4);
}

bool um_is_committed(UmProcess *p, UINT64 va)
{
    pte_t *e = walk(p->pml4, va, false);
    return e && (*e & (PTE_PRESENT | PTE_LAZY));
}

/* The PAGE_* protection a committed page has now (from its entry: a
 * module's pages each carry their own section's, and VirtualProtect may
 * have changed single pages); 0 if not committed */
UINT32 um_page_protect(UmProcess *p, UINT64 va)
{
    pte_t *e = walk(p->pml4, va, false);
    if (!e || !(*e & (PTE_PRESENT | PTE_LAZY))) return 0;
    pte_t v = *e;
    bool w = v & PTE_WRITE, x = !(v & PTE_NX);
    UINT32 prot = x ? (w ? 0x40 : 0x20) : (w ? 0x04 : 0x02);
    return prot | ((v & PTE_GUARD) ? 0x100 : 0);
}

bool um_is_guard(UmProcess *p, UINT64 va)
{
    pte_t *e = walk(p->pml4, va, false);
    return e && (*e & PTE_GUARD);
}

static bool copy_pages(UmProcess *p, UINT64 va, void *buf, UINT64 n, bool to_user)
{
    UINT8 *b = buf;
    while (n) {
        pte_t *e = walk(p->pml4, va, false);
        UINT64 off = va & 0xFFF, chunk = PAGE_SIZE - off;
        if (chunk > n) chunk = n;
        if (!e) return false;
        if (!(*e & PTE_PRESENT)) {
            if (!(*e & PTE_LAZY)) return false;
            if (!to_user) {                             /* never touched: reads as zeros */
                memset(b, 0, chunk);
                va += chunk; b += chunk; n -= chunk;
                continue;
            }
            if (!back_page(p, e)) return false;
        }
        if (to_user && (*e & PTE_IMAGE) && !img_privatize(p, va, e)) return false;
        /* device memory mapped into the program (a GPU's host-visible
         * region) is beyond the physmap: the kernel can't reach it there */
        if ((*e & PTE_ADDR_MASK) >= PHYSMAP_SIZE) return false;
        UINT8 *k = (UINT8 *)(uintptr_t)(PHYSMAP_BASE + (*e & PTE_ADDR_MASK) + off);
        if (to_user) memcpy(k, b, chunk); else memcpy(b, k, chunk);
        va += chunk; b += chunk; n -= chunk;
    }
    return true;
}

bool um_write(UmProcess *p, UINT64 va, const void *src, UINT64 n) { return copy_pages(p, va, (void *)src, n, true); }
bool um_read(UmProcess *p, UINT64 va, void *dst, UINT64 n)        { return copy_pages(p, va, dst, n, false); }

/* Free every user page and page table, then the PML4 itself. */
/* -----------------------------------------------------------------------
 * KUSER_SHARED_DATA: one page, mapped read-only at 0x7FFE0000 in every
 * program, with the clocks programs read without a system call (the Go
 * runtime's nanotime, GetTickCount in some CRTs) and the version fields.
 * ----------------------------------------------------------------------- */
#define UM_KUSD_VA UINT64_C(0x7FFE0000)

static void kusd_time(UINT32 off, UINT64 v)       /* KSYSTEM_TIME: High2, Low, then High1 */
{
    volatile UINT32 *t = (volatile UINT32 *)(g_kusd + off);
    t[2] = (UINT32)(v >> 32);
    __asm__ volatile ("" ::: "memory");
    t[0] = (UINT32)v;
    __asm__ volatile ("" ::: "memory");
    t[1] = (UINT32)(v >> 32);
}

static void kusd_init(void)
{
    g_kusd = kernel_alloc_pages(1);
    if (!g_kusd) return;
    memset(g_kusd, 0, PAGE_SIZE);
    g_kusd_pa = (PADDR)((uintptr_t)g_kusd - PHYSMAP_BASE);
    *(UINT32 *)(g_kusd + 0x04) = 10u << 24;               /* TickCountMultiplier: 10 ms per tick */
    *(UINT16 *)(g_kusd + 0x2C) = 0x8664;                  /* ImageNumberLow/High: x64 */
    *(UINT16 *)(g_kusd + 0x2E) = 0x8664;
    static const char root[] = "C:\\Windows";
    for (int i = 0; root[i]; i++) *(UINT16 *)(g_kusd + 0x30 + 2 * i) = (UINT16)root[i];   /* NtSystemRoot */
    *(UINT32 *)(g_kusd + 0x260) = 18362;                  /* NtBuildNumber: 1903, as the PEB says */
    *(UINT32 *)(g_kusd + 0x264) = 1;                      /* NtProductType: workstation */
    g_kusd[0x268] = 1;                                    /* ProductTypeIsValid */
    *(UINT32 *)(g_kusd + 0x26C) = 10;                     /* NtMajorVersion */
    *(UINT32 *)(g_kusd + 0x270) = 0;                      /* NtMinorVersion */
    static const int features[] = { 2, 6, 8, 10, 12, 13, 14 };   /* cmpxchg8b/16b, SSE, SSE2, SSE3, RDTSC, NX */
    for (unsigned i = 0; i < sizeof(features) / sizeof(features[0]); i++) g_kusd[0x274 + features[i]] = 1;
    UmCpuCountChanged();
    UmTimerTick(sched_ticks());
}

/* A CPU came online (smp.c) */
void UmCpuCountChanged(void)
{
    if (!g_kusd) return;
    UINT32 n = g_cpu_count;
    *(UINT32 *)(g_kusd + 0x3C0) = n;                      /* ActiveProcessorCount */
    *(UINT64 *)(g_kusd + 0x3C8) = n >= 64 ? ~0ULL : (1ULL << n) - 1;   /* ActiveProcessorAffinity */
}

/* Called on every timer tick (interrupts off) */
void UmTimerTick(UINT64 ticks)
{
    if (!g_kusd) return;
    kusd_time(0x08, ticks * 100000ULL);                   /* InterruptTime (100 ns units) */
    kusd_time(0x14, um_now_100ns());                      /* SystemTime */
    kusd_time(0x320, ticks);                              /* TickCount */
    *(volatile UINT32 *)g_kusd = (UINT32)ticks;           /* TickCountLowDeprecated */
}

static bool map_kusd(UmProcess *p)
{
    if (!g_kusd) return true;
    pte_t *e = walk(p->pml4, UM_KUSD_VA, true);
    if (!e) return false;
    *e = g_kusd_pa | PTE_PRESENT | PTE_USER | PTE_NX;     /* read-only, shared by all */
    return um_region_add(p, UM_KUSD_VA, PAGE_SIZE, 0x02, false);
}

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
                    if (l1[m] & PTE_IMAGE)
                        img_put(l1[m] & PTE_ADDR_MASK);
                    else if ((l1[m] & PTE_PRESENT) && !(l1[m] & PTE_SHARED) && (l1[m] & PTE_ADDR_MASK) != g_kusd_pa)
                        pmm_free_page(l1[m] & PTE_ADDR_MASK);
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

bool um_addr_requirements(UINT64 ptr, UINT32 n, UINT64 *lo, UINT64 *hi, UINT64 *align)
{
    for (UINT32 i = 0; i < n && i < 16; i++) {
        UINT64 e[2];                                        /* { Type:8 | Reserved:56, Pointer } */
        if (!NT_SUCCESS(CopyFromUser(e, (const void *)(uintptr_t)(ptr + 16 * (UINT64)i), 16))) return false;
        if ((e[0] & 0xFF) != 1 || !e[1]) continue;          /* MemExtendedParameterAddressRequirements */
        UINT64 req[3];                                      /* Lowest, Highest (last byte), Alignment */
        if (!NT_SUCCESS(CopyFromUser(req, (const void *)(uintptr_t)e[1], 24))) return false;
        if (req[0] > *lo) *lo = req[0];
        if (req[1] && req[1] + 1 < *hi) *hi = req[1] + 1;
        if (req[2]) *align = req[2];
    }
    return true;
}

/* A free range of @size at a multiple of @align (0: 64 KiB) within [lo, hi) */
UINT64 um_find_free_aligned(UmProcess *p, UINT64 size, UINT64 lo, UINT64 hi, UINT64 align)
{
    if (align <= 0x10000) return um_find_free(p, size, lo, hi);
    size = (size + 0xFFFF) & ~0xFFFFULL;
    while (lo + size <= hi) {
        UINT64 a = um_find_free(p, size, lo, hi);
        if (!a) return 0;
        UINT64 b = (a + align - 1) & ~(align - 1);
        if (b + size <= hi && um_is_free(p, b, size)) return b;
        lo = b > a ? b : a + 0x10000;
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
    r->section = NULL;
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
    UINT8   *img;               /* assembled image (kernel copy); NULL: fetch from user memory */
    UINT32   size;              /* SizeOfImage */
    UINT64   base;              /* chosen load address */
    UINT32   exp_rva, exp_size; /* export directory */
    bool     fetched;
    RamNode *dir;               /* its folder (DLLs next to it are found there) */
    bool     mapped;            /* mapped by this loader: its imports are to be bound */
    bool     bound;             /* imports bound (or being bound) */
    bool     top;               /* the program itself */
    bool     data;              /* mapped as data: no imports, no entry point */
    UINT64   deps[UM_MAX_MODULES / 64];     /* the modules it imports (a bitmap) */
} Image;

typedef struct {
    UmProcess *p;
    Image      img[UM_MAX_MODULES];
    char      *err;
    int        err_cap;
    bool       data;                /* LoadLibraryEx(LOAD_LIBRARY_AS_DATAFILE / AS_IMAGE_RESOURCE) */
    RamNode   *dep_dir;             /* the folder of the module whose imports are being loaded */
    bool       plock;               /* runs under p->lock (a running process): dropped for file lookups */
    UINT32     bkl;                 /* the big lock's depth, let go of while loading (0: not) */
    RamNode   *pins[UM_MAX_MODULES];/* the files being loaded, pinned until the loader is done */
    int        npins;
    bool       keep;                /* loaded: the process holds the files while it runs */
} Loader;

static UINT16 rd16(const UINT8 *b) { return (UINT16)(b[0] | b[1] << 8); }
static UINT32 rd32(const UINT8 *b) { return (UINT32)b[0] | (UINT32)b[1] << 8 | (UINT32)b[2] << 16 | (UINT32)b[3] << 24; }
static UINT64 rd64(const UINT8 *b) { return rd32(b) | (UINT64)rd32(b + 4) << 32; }
static void   wr64(UINT8 *b, UINT64 v) { for (int i = 0; i < 8; i++) b[i] = (UINT8)(v >> (8 * i)); }
static void   put_u16(UINT8 *b, UINT16 v) { b[0] = (UINT8)v; b[1] = (UINT8)(v >> 8); }
static void   put_u32(UINT8 *b, UINT32 v) { for (int i = 0; i < 4; i++) b[i] = (UINT8)(v >> (8 * i)); }

static void lower_copy(char *dst, const char *src, int cap)
{
    int i = 0;
    for (; src[i] && i < cap - 1; i++) dst[i] = (src[i] >= 'A' && src[i] <= 'Z') ? src[i] + 32 : src[i];
    dst[i] = '\0';
}

static int load_module(Loader *L, RamNode *file, const char *name, bool top);
static int map_module(Loader *L, RamNode *file, const char *name, bool top);

/* A module mapped by an earlier load: read its image back from user
 * memory for export lookups. */
static bool fetch_image(Loader *L, int m)
{
    Image *im = &L->img[m];
    if (im->img) return true;
    const UmModule *mod = &L->p->modules[m];
    im->size = (UINT32)mod->size;
    im->base = mod->base;
    im->img = kzalloc(im->size + 16);
    if (!im->img || !um_read(L->p, mod->base, im->img, im->size)) {
        kfree(im->img);
        im->img = NULL;
        return false;
    }
    im->fetched = true;
    UINT32 nt = rd32(im->img + 0x3C);
    if (nt < im->size - 0x108) {
        UINT32 dirs = rd16(im->img + nt + 24) == 0x10B ? 96 : 112;     /* PE32 or PE32+ */
        im->exp_rva = rd32(im->img + nt + 24 + dirs);
        im->exp_size = rd32(im->img + nt + 24 + dirs + 4);
    }
    return true;
}

/* Address of an export (by name, or ordinal if name == NULL); 0 if none. */
static UINT64 find_export(Loader *L, int m, const char *name, UINT32 ordinal, int depth)
{
    if (!fetch_image(L, m)) return 0;
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
        /* map the target (the loop in load_module binds it; re-entering
         * load_module here would bind modules under a bind in progress and
         * lose that one's error, leaving it mapped but never committed).
         * The target is a dependency of the forwarding module: it goes on
         * the initialization list before it, and ntdll learns of it even
         * when no module imports it directly (msvcrt forwards the C++
         * exception entry points to vcruntime140, whose unwind information
         * every throw needs). */
        int fm = map_module(L, NULL, dll, false);
        if (fm < 0) return 0;
        im->deps[fm / 64] |= UINT64_C(1) << (fm % 64);
        return find_export(L, fm, fn, 0, depth + 1);
    }
    return im->base + rva;
}

static int fail(Loader *L, const char *fmt, const char *arg)
{
    ksnprintf(L->err, L->err_cap, fmt, arg);
    return -1;
}

/* The machine a PE file is built for (0x8664, 0x014C), 0 if not a PE */
UINT16 um_pe_machine(const RamNode *f)
{
    const UINT8 *d = (const UINT8 *)f->data;
    if (!d || f->size < 0x40 || rd16(d) != 0x5A4D) return 0;
    UINT32 nt = rd32(d + 0x3C);
    if (nt > f->size - 6 || rd32(d + nt) != 0x00004550) return 0;
    return rd16(d + nt + 4);
}

/* IMAGE_SUBSYSTEM_*: 2 GUI, 3 console (0: not a PE image) */
UINT16 um_pe_subsystem(RamNode *f)
{
    if (!RamfsLoad(f) || !um_pe_machine(f)) return 0;
    const UINT8 *d = (const UINT8 *)f->data;
    UINT32 nt = rd32(d + 0x3C);
    if (nt + 24 + 70 > f->size) return 0;
    return rd16(d + nt + 24 + 68);                      /* OptionalHeader.Subsystem (PE32 and PE32+) */
}

/* Find a DLL: a path as given, else the program's directory, then the
 * system folder (System32, or SysWOW64 for 32-bit programs) */
/* A 32-bit program's C:\Windows\System32\... is C:\Windows\SysWOW64\...
 * (@path is rewritten in place) */
void um_wow_path(UmProcess *p, char *path)
{
    if (!p->wow) return;
    char *s = path;
    if (((s[0] | 0x20) == 'c') && s[1] == ':') s += 2;
    static const char sys[] = "\\windows\\system32";
    for (int i = 0; sys[i]; i++) {
        char c = s[i];
        if (c == '/') c = '\\';
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c != sys[i]) return;
    }
    if (s[17] && s[17] != '\\' && s[17] != '/') return;
    memcpy(s + 9, "SysWOW64", 8);
}

/* Windows' KnownDLLs (and the Universal C Runtime, which Windows 10 always
 * takes from the system): the system copy wins over one in the program's
 * folder, so a program shipping its own ucrtbase.dll runs on NovaOS's. */
static bool known_dll(const char *name)
{
    static const char *const known[] = {
        "ntdll.dll", "kernel32.dll", "kernelbase.dll", "user32.dll", "gdi32.dll", "advapi32.dll",
        "msvcrt.dll", "ole32.dll", "oleaut32.dll", "shell32.dll", "shlwapi.dll", "comdlg32.dll",
        "comctl32.dll", "ws2_32.dll", "ucrtbase.dll", "combase.dll", "rpcrt4.dll", "sechost.dll",
        "imm32.dll", "psapi.dll", "setupapi.dll", "version.dll", "winmm.dll", "bcrypt.dll",
        "secur32.dll", "iphlpapi.dll", "mswsock.dll", "winhttp.dll", "crypt32.dll", "powrprof.dll",
    };
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++)
        if (!strcmp(name, known[i])) return true;
    return false;
}

/* (@dep_dir: the folder of the DLL that imports @name, searched after the
 * program's, as LoadLibraryEx(LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR) does; a
 * path without an extension gets ".dll", as LoadLibrary adds it) */
static RamNode *find_dll(UmProcess *p, const char *name, RamNode *dep_dir)
{
    if (strchr(name, '\\') || strchr(name, '/') || strchr(name, ':')) {
        char path[RAMFS_PATH_MAX];
        strncpy(path, name, sizeof(path) - 5);
        path[sizeof(path) - 5] = '\0';
        for (char *c = path; *c; c++)       /* GTK's module caches use forward slashes */
            if (*c == '/') *c = '\\';
        um_wow_path(p, path);
        const char *leaf = strrchr(path, '\\');
        if (!strchr(leaf ? leaf : path, '.')) strcat(path, ".dll");
        /* a relative path ("Merge7z\\Merge7z.dll") goes by the search order
         * too: the program's folder first, then the current one */
        bool relative = path[0] != '\\' && path[0] != '/' && !strchr(path, ':');
        RamNode *n = NULL;
        if (relative && p->exe_dir) {
            n = RamfsResolve(p->exe_dir, path);
            if (n && n->dir) n = NULL;
        }
        if (!n) n = RamfsResolve(p->cwd, path);
        return n && !n->dir ? n : NULL;
    }
    RamNode *sys = RamfsResolve(NULL, p->wow ? "\\Windows\\SysWOW64" : "\\Windows\\System32");
    RamNode *n = sys ? RamfsFind(sys, name) : NULL;
    if (n && !n->dir && known_dll(name)) return n;
    if (p->exe_dir) {
        RamNode *a = RamfsFind(p->exe_dir, name);
        if (a && !a->dir && RamfsLoad(a) && um_pe_machine(a) == (p->wow ? 0x014C : 0x8664)) return a;
    }
    if (dep_dir && dep_dir != p->exe_dir) {
        RamNode *a = RamfsFind(dep_dir, name);
        if (a && !a->dir && RamfsLoad(a) && um_pe_machine(a) == (p->wow ? 0x014C : 0x8664)) return a;
    }
    /* SetDllDirectory's folder, then AddDllDirectory's, in the order they
     * were added (Windows leaves that order unspecified) */
    for (int i = -1; i < UM_MAX_DLL_DIRS; i++) {
        const char *d = i < 0 ? p->dll_dir : p->dll_dirs[i];
        RamNode *dir = d[0] ? RamfsResolve(NULL, d) : NULL;
        RamNode *a = dir && dir->dir ? RamfsFind(dir, name) : NULL;
        if (a && !a->dir && RamfsLoad(a) && um_pe_machine(a) == (p->wow ? 0x014C : 0x8664)) return a;
    }
    return n && !n->dir ? n : NULL;
}

/* AddDllDirectory (@op 0: *@cookie gets the slot), RemoveDllDirectory (1:
 * *@cookie names it) and SetDllDirectory (2: @path, or "" for none) */
UINT32 um_dll_directory(UmProcess *p, UINT32 op, const char *path, UINT64 *cookie)
{
    UINT32 st = 0;
    um_lock_excl(&p->lock);
    if (op == 1) {
        if (*cookie >= 1 && *cookie <= UM_MAX_DLL_DIRS && p->dll_dirs[*cookie - 1][0])
            p->dll_dirs[*cookie - 1][0] = '\0';
        else st = 0xC000000Du;                          /* STATUS_INVALID_PARAMETER */
    } else if (op == 2) {
        strncpy(p->dll_dir, path, sizeof(p->dll_dir) - 1);
        p->dll_dir[sizeof(p->dll_dir) - 1] = '\0';
    } else {
        int slot = -1;
        for (int i = 0; i < UM_MAX_DLL_DIRS && slot < 0; i++)
            if (!p->dll_dirs[i][0]) slot = i;
        if (slot < 0) st = 0xC0000017u;                 /* STATUS_NO_MEMORY */
        else {
            strncpy(p->dll_dirs[slot], path, sizeof(p->dll_dirs[0]) - 1);
            p->dll_dirs[slot][sizeof(p->dll_dirs[0]) - 1] = '\0';
            *cookie = (UINT64)slot + 1;
        }
    }
    um_unlock_excl(&p->lock);
    return st;
}

/* The address of a stub for the missing import @what ("f in dll"): calls
 * NtNovaUnimplemented(index), which reports it and ends the program.
 * 0 when the stub page is full. */
static UINT64 stub_for(UmProcess *p, const char *what)
{
    for (int i = 0; i < p->nstubs; i++)
        if (!strcmp(p->stub_names[i], what)) return p->lay.stubs + (UINT64)i * UM_STUB_SIZE;
    if (!p->stub_names) p->stub_names = kzalloc(sizeof(*p->stub_names) * UM_MAX_STUBS);
    if (!p->stub_names || p->nstubs >= UM_MAX_STUBS) return 0;
    strncpy(p->stub_names[p->nstubs], what, sizeof(p->stub_names[0]) - 1);
    return p->lay.stubs + (UINT64)p->nstubs++ * UM_STUB_SIZE;
}

/* (Re)write the stub page: mov r10d, index; mov eax, NtNovaUnimplemented;
 * syscall; ret.  The page is executable and read-only. */
static bool write_stubs(UmProcess *p)
{
    if (!p->nstubs) return true;
    UINT8 *pg = kzalloc(PAGE_SIZE);
    if (!pg) return false;
    for (int i = 0; i < p->nstubs; i++) {
        UINT8 *s = pg + i * UM_STUB_SIZE;
        if (p->wow) {
            /* 32-bit: the argument block (see the int 0x2E path) on the
             * stack: push 0; push index; mov edx, esp; mov eax, N; int 0x2E */
            s[0] = 0x6A; s[1] = 0x00;
            s[2] = 0x68; put_u32(s + 3, (UINT32)i);
            s[7] = 0x89; s[8] = 0xE2;
            s[9] = 0xB8; put_u32(s + 10, SYSCALL_NtNovaUnimplemented);
            s[14] = 0xCD; s[15] = 0x2E;
            continue;
        }
        s[0] = 0x41; s[1] = 0xBA; put_u32(s + 2, (UINT32)i);
        s[6] = 0xB8; put_u32(s + 7, SYSCALL_NtNovaUnimplemented);
        s[11] = 0x0F; s[12] = 0x05; s[13] = 0xC3; s[14] = 0xCC; s[15] = 0xCC;
    }
    UINT64 va = p->lay.stubs;
    bool ok = um_commit(p, va, PAGE_SIZE, 0x04) && um_write(p, va, pg, PAGE_SIZE) &&
              um_commit(p, va, PAGE_SIZE, 0x20);
    kfree(pg);
    return ok;
}

/* API sets: Windows programs import from virtual DLL names that stand for
 * a system DLL (api-ms-win-crt-* is the Universal C Runtime, ucrtbase;
 * the core sets are kernel32).  Rewrites @lname in place. */
static void map_api_set(char *lname, int cap)
{
    static const struct { const char *prefix, *dll; } sets[] = {
        { "api-ms-win-crt-",              "ucrtbase.dll" },
        { "api-ms-win-core-synch-",       "kernelbase.dll" },  /* WaitOnAddress: kernelbase's, as on Windows */
        { "api-ms-win-core-com-",         "ole32.dll" },
        { "api-ms-win-core-winrt-",       "ole32.dll" },      /* combase: HSTRINGs, activation */
        { "combase.dll",                  "ole32.dll" },
        { "api-ms-win-core-",             "kernel32.dll" },
        { "api-ms-win-security-",         "advapi32.dll" },
        { "api-ms-win-eventing-",         "advapi32.dll" },
        { "api-ms-win-shell-",            "shell32.dll" },
        { "api-ms-win-shcore-",           "shlwapi.dll" },
        { "shcore.dll",                   "shlwapi.dll" },    /* GetDpiForMonitor, SHCreateStreamOnFileEx, ... */
        { "ext-ms-win-",                  "kernel32.dll" },
        { "api-ms-win-",                  "kernel32.dll" },   /* any other set: what exists is there */
        { "msvcrt40.dll",                 "msvcrt.dll" },
    };
    for (size_t i = 0; i < sizeof(sets) / sizeof(sets[0]); i++)
        if (!strncmp(lname, sets[i].prefix, strlen(sets[i].prefix))) {
            strncpy(lname, sets[i].dll, (size_t)cap - 1);
            lname[cap - 1] = '\0';
            return;
        }
}

/* Whether the .NET header at @rva (IMAGE_COR20_HEADER) says IL only */
static bool il_only(const UINT8 *f, UINT32 fsz, const UINT8 *sec, int nsec, UINT32 rva)
{
    for (int i = 0; i < nsec; i++) {
        const UINT8 *s = sec + 40 * i;
        UINT32 va = rd32(s + 12), vsz = rd32(s + 8), raw = rd32(s + 16), ptr = rd32(s + 20);
        if (rva < va || rva >= va + (vsz > raw ? vsz : raw)) continue;
        UINT32 off = ptr + (rva - va);
        return off + 20 <= fsz && (rd32(f + off + 16) & 1);       /* COMIMAGE_FLAGS_ILONLY */
    }
    return false;
}

/* Load @file (or the DLL @name when file is NULL); returns the module index. */
/* Find a module's file (or take @file) and pin it, so its contents can be
 * read without the desktop lock; its path and folder come along.  The
 * loader holds the desktop lock only for this, so a large image does not
 * stall the desktop while it is copied, relocated and bound.  Lock order is
 * desktop, then process: a runtime load lets go of p->lock meanwhile (the
 * process's loader lock keeps other loads out). */
static RamNode *loader_file(Loader *L, RamNode *file, const char *name, char *path, int cap, RamNode **dir)
{
    UmProcess *p = L->p;
    if (L->plock) um_unlock_excl(&p->lock);
    bkl_restore(L->bkl);                    /* the file system still wants it */
    DesktopLock();
    if (!file) file = find_dll(p, name, L->dep_dir);
    if (file && L->npins < UM_MAX_MODULES) {
        RamfsPin(file);                     /* (reads it in, on a mounted volume) */
        L->pins[L->npins++] = file;
        RamfsPath(file, path, cap);
        *dir = file->parent;
        if (!file->data && file->size) file = NULL;  /* it could not be read */
    } else file = NULL;
    DesktopUnlock();
    if (L->bkl) bkl_drop();
    if (L->plock) um_lock_excl(&p->lock);
    return file;
}

/* The module index @name (a DLL name, an API set or a path) stands for
 * once loaded, or -1 */
static int module_index(UmProcess *p, const char *name, char *lname, int cap, bool mapped_file)
{
    const char *leaf = strrchr(name, '\\');
    leaf = leaf ? leaf + 1 : name;
    lower_copy(lname, leaf, cap);
    if (!strchr(lname, '.') && strlen(lname) < (size_t)cap - 4) strcat(lname, ".dll");
    if (!mapped_file) map_api_set(lname, cap);
    for (int i = 0; i < p->nmodules; i++)
        if (!strcmp(p->modules[i].name, lname)) return i;
    return -1;
}

/* Map @file (or the DLL @name when file is NULL): copy its image, choose
 * its address, relocate it and register the module.  Its imports are bound
 * afterwards by bind_module; returns the module index (an already loaded
 * module's when it is one). */
static int map_module(Loader *L, RamNode *file, const char *name, bool top)
{
    UmProcess *p = L->p;
    char lname[64];
    int found = module_index(p, name, lname, sizeof(lname), file != NULL);
    if (found >= 0) return found;
    if (strlen(lname) >= sizeof(p->modules[0].name)) return fail(L, "The DLL name %s is too long", lname);
    if (p->nmodules >= UM_MAX_MODULES) return fail(L, "Too many DLLs (at %s)", name);

    char fpath[sizeof(p->modules[0].path)];
    RamNode *fdir = NULL;
    file = loader_file(L, file, strchr(name, '\\') || strchr(name, ':') ? name : lname, fpath, sizeof(fpath), &fdir);
    if (!file) return fail(L, "The DLL %s was not found", name);
    const UINT8 *f = (const UINT8 *)file->data;
    UINT32 fsz = file->size;
    if (fsz < 0x40 || rd16(f) != 0x5A4D) return fail(L, "%s is not a Windows program (no MZ header)", name);
    UINT32 nt = rd32(f + 0x3C);
    if (fsz < 0x108 || nt > fsz - 0x108 || rd32(f + nt) != 0x00004550) return fail(L, "%s is not a valid PE file", name);
    const UINT8 *fh = f + nt + 4, *oh = fh + 20;
    /* 64-bit programs load x64 modules; 32-bit ones (WoW) x86 modules */
    UINT16 machine = rd16(fh);
    bool pe32 = machine == 0x014C;
    if (machine != 0x014C && machine != 0x8664) return fail(L, "%s is not an x86 or x64 program", name);
    if (rd16(oh) != (pe32 ? 0x10B : 0x20B)) return fail(L, "%s has an invalid optional header", name);
    UINT16 nsec = rd16(fh + 2), opt_size = rd16(fh + 16), chars = rd16(fh + 18);
    UINT32 size = rd32(oh + 56), hdr = rd32(oh + 60), ndirs = rd32(oh + (pe32 ? 92 : 108));
    UINT64 pref = pe32 ? rd32(oh + 28) : rd64(oh + 24);
    if (!size || size > RAMFS_FILE_MAX || hdr > fsz || hdr > size)
        return fail(L, "%s has an invalid image size", name);
    const UINT8 *sec = oh + opt_size;
    if ((UINT64)(sec - f) + 40ULL * nsec > fsz) return fail(L, "%s has a truncated section table", name);
    const UINT8 *dir = oh + (pe32 ? 96 : 112);
    UINT32 dirv[16] = { 0 }, dirs[16] = { 0 };
    for (UINT32 i = 0; i < ndirs && i < 16; i++) { dirv[i] = rd32(dir + 8 * i); dirs[i] = rd32(dir + 8 * i + 4); }
    /* A module mapped as data (LoadLibraryEx's AS_DATAFILE / AS_IMAGE_RESOURCE,
     * or a .NET IL-only assembly of either architecture, as Windows maps
     * those): sections in place, no imports, no entry point */
    bool data = top && L->data;
    if (pe32 != p->wow && dirv[14] && il_only(f, fsz, sec, nsec, dirv[14])) data = true;
    if (pe32 != p->wow && !data)
        return fail(L, pe32 ? "%s is a 32-bit (x86) module and cannot be loaded into a 64-bit program"
                            : "%s is a 64-bit (x64) module and cannot be loaded into a 32-bit program", name);

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
        if (va >= im->size) { kfree(im->img); im->img = NULL; return fail(L, "%s has a section outside the image", name); }
        if (n > im->size - va) n = im->size - va;
        if (ptr > fsz || n > fsz - ptr) n = ptr > fsz ? 0 : fsz - ptr;
        memcpy(im->img + va, f + ptr, n);
    }
    im->exp_rva = dirv[0]; im->exp_size = dirs[0];

    /* Choose the load address */
    UINT64 base = pref;
    if (!um_is_free(p, base, im->size)) {
        /* (no relocation table and not RELOCS_STRIPPED: nothing to fix up,
         * e.g. a resource-only DLL; Windows loads it anywhere too) */
        if (!data && (chars & 0x0001 /* RELOCS_STRIPPED */)) {
            kfree(im->img); im->img = NULL;
            return fail(L, "%s cannot be relocated and its address is taken", name);
        }
        base = um_find_free(p, im->size, p->lay.dll_min, p->lay.dll_max);
        if (!base) { kfree(im->img); im->img = NULL; return fail(L, "No address space left for %s", name); }
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
                if ((ent >> 12) == 10 && at + 8 <= im->size)                /* DIR64 */
                    wr64(im->img + at, rd64(im->img + at) + (UINT64)delta);
                else if ((ent >> 12) == 3 && at + 4 <= im->size)            /* HIGHLOW (x86) */
                    put_u32(im->img + at, rd32(im->img + at) + (UINT32)delta);
            }
            off += bsz;
        }
    }
    /* Register before resolving imports (DLLs may import each other) */
    if (!um_region_add(p, base, im->size, 0x04, true)) {
        kfree(im->img); im->img = NULL;
        return fail(L, "Too many memory regions loading %s", name);
    }
    UmModule *mod = &p->modules[p->nmodules];   /* counted once filled in (others read it) */
    memset(mod, 0, sizeof(*mod));
    strncpy(mod->name, lname, sizeof(mod->name) - 1);
    memcpy(mod->path, fpath, sizeof(mod->path));
    mod->base = base;
    mod->size = im->size;
    mod->entry = data ? 0 : rd32(oh + 16);
    mod->dll = data || (chars & 0x2000);
    if (!mod->dll && top) {
        UINT64 reserve = pe32 ? rd32(oh + 72) : rd64(oh + 72);
        p->stack_reserve = (UINT32)(reserve > 0xFFFFFFFFu ? 0xFFFFFFFFu : reserve);
    }
    im->dir = fdir;
    im->mapped = true;
    im->top = top;
    im->data = data;                                /* (nothing to bind) */
    __atomic_store_n(&p->nmodules, p->nmodules + 1, __ATOMIC_RELEASE);
    return m;
}

/* Bind module @m's imports, mapping the DLLs it needs (found in its own
 * folder too), then copy it into the process with each section's
 * protection.  Nothing recurses: a DLL mapped here is bound by the loop in
 * load_module, so a program may chain its DLLs as deep as it likes. */
static int bind_module(Loader *L, int m)
{
    UmProcess *p = L->p;
    Image *im = &L->img[m];
    const char *name = p->modules[m].name;
    im->bound = true;
    const UINT8 *f = im->img;
    UINT32 nt = rd32(f + 0x3C);
    const UINT8 *fh = f + nt + 4, *oh = fh + 20;
    bool pe32 = rd16(fh) == 0x014C;
    UINT16 nsec = rd16(fh + 2), opt_size = rd16(fh + 16);
    UINT32 hdr = rd32(oh + 60), ndirs = rd32(oh + (pe32 ? 92 : 108));
    const UINT8 *sec = oh + opt_size, *dir = oh + (pe32 ? 96 : 112);
    UINT32 imports = ndirs > 1 ? rd32(dir + 8) : 0;
    UINT64 base = im->base;

    if (im->data) imports = 0;                      /* mapped as data: no imports */
    L->dep_dir = im->dir;
    for (UINT32 d = imports; d && d + 20 <= im->size; d += 20) {
        UINT32 ilt = rd32(im->img + d), nm = rd32(im->img + d + 12), iat = rd32(im->img + d + 16);
        if (!nm && !iat) break;
        if (nm >= im->size) return fail(L, "%s has a corrupt import table", name);
        char dll[64];
        strncpy(dll, (const char *)im->img + nm, sizeof(dll) - 1);
        dll[sizeof(dll) - 1] = '\0';
        int dm = map_module(L, NULL, dll, false);
        if (dm < 0) return -1;
        im->deps[dm / 64] |= UINT64_C(1) << (dm % 64);
        /* kernel32 and kernelbase share the API sets: a function one lacks
         * may be the other's (WaitOnAddress is kernelbase's, as on Windows) */
        const char *mapped = p->modules[dm].name;
        int alt = -1;
        if (!strcmp(mapped, "kernelbase.dll")) alt = map_module(L, NULL, "kernel32.dll", false);
        else if (!strcmp(mapped, "kernel32.dll") && (!strncmp(dll, "api-ms-win-core-", 16) ||
                                                      !strncmp(dll, "ext-ms-win-", 11)))
            alt = map_module(L, NULL, "kernelbase.dll", false);
        if (alt >= 0) im->deps[alt / 64] |= UINT64_C(1) << (alt % 64);
        UINT32 t = ilt ? ilt : iat, ts = pe32 ? 4 : 8;              /* thunk size */
        for (UINT32 k = 0; t + ts * (k + 1) <= im->size && iat + ts * (k + 1) <= im->size; k++) {
            UINT64 th = pe32 ? rd32(im->img + t + 4 * k) : rd64(im->img + t + 8 * k);
            if (!th) break;
            UINT64 addr;
            char what[80];
            if (pe32 ? (th >> 31) : (th >> 63)) {                     /* by ordinal */
                addr = find_export(L, dm, NULL, (UINT32)(th & 0xFFFF), 0);
                ksnprintf(what, sizeof(what), "#%u in %s", (unsigned)(th & 0xFFFF), dll);
            } else {
                UINT32 hn = (UINT32)th;
                if (hn + 2 >= im->size) return fail(L, "%s has a corrupt import entry", name);
                const char *fn = (const char *)im->img + hn + 2;
                addr = find_export(L, dm, fn, 0, 0);
                if (!addr && alt >= 0) addr = find_export(L, alt, fn, 0, 0);
                /* ucrtbase's "_o_" exports (api-ms-win-crt-private) are
                 * the plain functions under another name */
                if (!addr && !strncmp(fn, "_o_", 3) &&
                    (!strncmp(dll, "api-ms-win-crt-", 15) || !strncmp(dll, "ucrtbase", 8)))
                    addr = find_export(L, dm, fn + 3, 0, 0);
                ksnprintf(what, sizeof(what), "%s in %s", fn, dll);
            }
            /* A function NovaOS lacks: bind a stub that reports it if the
             * program ever calls it (many programs import functions they
             * never use) */
            if (!addr) addr = stub_for(p, what);
            if (!addr) return fail(L, "The procedure entry point %s could not be located", what);
            if (pe32) put_u32(im->img + iat + 4 * k, (UINT32)addr);
            else      wr64(im->img + iat + 8 * k, addr);
        }
    }
    L->dep_dir = NULL;

    /* Each page's protection: the headers read-only, then each section's
     * (a page two sections share takes the later one's), the rest
     * read/write.  Read-only pages map the frames every process loading
     * the same bytes shares (image pages); the others are copied in. */
    UINT32 npages = im->size >> 12;
    UINT8 *prot = kmalloc(npages);
    if (!prot) return fail(L, "Out of memory loading %s", name);
    memset(prot, 0x04, npages);
    for (UINT32 a = 0; a < hdr; a += PAGE_SIZE) prot[a >> 12] = 0x02;
    for (int i = 0; i < nsec; i++) {
        const UINT8 *s = sec + 40 * i;
        UINT32 va = rd32(s + 12), vsz = rd32(s + 8), ch = rd32(s + 36);
        if (!vsz) vsz = rd32(s + 16);
        bool x = ch & 0x20000000, w = ch & 0x80000000;
        UINT32 end = vsz > im->size - va ? im->size : va + vsz;
        if (va < im->size)
            for (UINT32 a = va & ~0xFFFU; a < end; a += PAGE_SIZE) prot[a >> 12] = x ? (w ? 0x40 : 0x20) : (w ? 0x04 : 0x02);
    }
    bool ok = true;
    for (UINT32 i = 0; ok && i < npages; i++) {
        UINT64 va = base + ((UINT64)i << 12);
        const UINT8 *pg = im->img + ((UINT64)i << 12);
        ok = prot[i] == 0x04 || prot[i] == 0x40
             ? um_commit(p, va, PAGE_SIZE, prot[i]) && um_write(p, va, pg, PAGE_SIZE)
             : um_map_image_page(p, va, pg, prot[i]);
    }
    kfree(prot);
    if (!ok) return fail(L, "Out of memory loading %s", name);
    return m;
}

/* Put the modules loaded for @root on the initialization list, each after
 * everything it imports (DllMain order), with a stack of its own rather
 * than recursion. */
static void order_modules(Loader *L, int root)
{
    UmProcess *p = L->p;
    UINT16 stack[UM_MAX_MODULES], next[UM_MAX_MODULES];
    UINT64 seen[UM_MAX_MODULES / 64] = { 0 };
    for (int i = 0; i < p->ninit; i++) seen[p->init_order[i] / 64] |= UINT64_C(1) << (p->init_order[i] % 64);
    int sp = 0;
    if (seen[root / 64] >> (root % 64) & 1) return;
    seen[root / 64] |= UINT64_C(1) << (root % 64);
    stack[sp] = (UINT16)root; next[sp++] = 0;
    while (sp) {
        int m = stack[sp - 1], d = next[sp - 1];
        const Image *im = &L->img[m];
        while (d < p->nmodules && (!im->mapped || !(im->deps[d / 64] >> (d % 64) & 1) || (seen[d / 64] >> (d % 64) & 1))) d++;
        if (d < p->nmodules) {
            next[sp - 1] = (UINT16)(d + 1);
            seen[d / 64] |= UINT64_C(1) << (d % 64);
            stack[sp] = (UINT16)d; next[sp++] = 0;
        } else {
            sp--;
            if (p->ninit < UM_MAX_MODULES) p->init_order[p->ninit++] = (UINT16)m;
        }
    }
}

/* Load @file (or the DLL @name when file is NULL) and everything it
 * imports; returns the module index. */
static int load_module(Loader *L, RamNode *file, const char *name, bool top)
{
    UmProcess *p = L->p;
    int m = map_module(L, file, name, top);
    if (m < 0) return -1;
    for (int i = 0; i < p->nmodules; i++)              /* (nmodules grows as DLLs are mapped) */
        if (L->img[i].mapped && !L->img[i].bound && bind_module(L, i) < 0) return -1;
    order_modules(L, m);
    for (int i = 0; i < p->nmodules; i++)              /* a module nothing lists as a dependency (a */
        if (L->img[i].mapped) order_modules(L, i);     /* forwarder's target) is still on the list */
    return m;
}

static Loader *loader_new(UmProcess *p, char *err, int err_cap)
{
    Loader *L = kzalloc(sizeof(*L));
    if (L) { L->p = p; L->err = err; L->err_cap = err_cap; }
    return L;
}

static void loader_free(Loader *L)
{
    for (int i = 0; i < UM_MAX_MODULES; i++) kfree(L->img[i].img);

    if (L->npins) {
        DesktopLock();
        UmProcess *p = L->p;
        for (int i = 0; i < L->npins; i++) {
            RamNode *f = L->pins[i];
            bool held = false;
            for (int k = 0; k < p->nimages; k++) held |= p->images[k] == f;
            if (L->keep && !L->data && !held && p->nimages < UM_MAX_MODULES) {   /* as Windows: a running image stays */
                RamfsRef(f);
                p->images[p->nimages++] = f;
            }
            RamfsUnpin(f);
        }
        DesktopUnlock();
    }
    kfree(L);
}

const UmModule *um_module_at(UmProcess *p, UINT64 va)
{
    for (int i = 0; i < p->nmodules; i++)
        if (va >= p->modules[i].base && va < p->modules[i].base + p->modules[i].size) return &p->modules[i];
    return NULL;
}

/* -----------------------------------------------------------------------
 * The loader-info page read by ntdll (see NOVA_LDR_INFO in winternl.h):
 *   UINT32 count, UINT32 reserved, then per module in initialization
 *   order: UINT64 base, size; UINT32 entry_rva, flags (1 = DLL);
 *   char name[64], path[96]  (UM_MAX_MODULES entries fit the 188 KiB area)
 * ----------------------------------------------------------------------- */
#define LDR_ENTRY_SIZE 184
_Static_assert(8 + UM_MAX_MODULES * LDR_ENTRY_SIZE <= UM_LDR_INFO_SIZE, "loader info area too small");

static bool write_ldr_info(UmProcess *p, int from)
{
    UINT8 e[LDR_ENTRY_SIZE];
    for (int i = from; i < p->ninit; i++) {
        const UmModule *m = &p->modules[p->init_order[i]];
        memset(e, 0, sizeof(e));
        wr64(e, m->base);
        wr64(e + 8, m->size);
        put_u32(e + 16, m->entry);
        put_u32(e + 20, m->dll ? 1 : 0);
        memcpy(e + 24, m->name, 64);
        memcpy(e + 88, m->path, 96);
        if (!um_write(p, p->lay.ldr_info + 8 + (UINT64)i * LDR_ENTRY_SIZE, e, sizeof(e))) return false;
    }
    UINT32 n = (UINT32)p->ninit;
    return um_write(p, p->lay.ldr_info, &n, 4);
}

/* Undo a failed runtime load: drop the modules added since @nmod. */
static void unload_since(UmProcess *p, int nmod, int ninit)
{
    for (int i = nmod; i < p->nmodules; i++) {
        UmRegion *r = um_region_find(p, p->modules[i].base);
        if (r && r->image) {
            um_decommit(p, r->base, r->size);
            um_region_remove(p, r);
        }
    }
    p->nmodules = nmod;
    p->ninit = ninit;
}

UINT32 um_load_dll(UmProcess *p, const char *name, UINT64 *base, UINT32 flags)
{
    char err[128];
    Loader *L = loader_new(p, err, sizeof(err));
    if (!L) return 0xC0000017u;
    L->data = (flags & 0x62) != 0;          /* LOAD_LIBRARY_AS_DATAFILE(_EXCLUSIVE), AS_IMAGE_RESOURCE */
    L->plock = true;                        /* (not the desktop lock: see loader_file) */
    um_lock(&p->ldr_lock);
    um_lock_excl(&p->lock);
    int nmod = p->nmodules, ninit = p->ninit;
    L->bkl = bkl_drop();                    /* copying a large image needs no big lock */
    int m = load_module(L, NULL, name, false);
    bkl_restore(L->bkl);
    L->bkl = 0;
    UINT32 st = 0;
    if (m < 0) {
        kprintf("[UM] %s (PID %u): LoadLibrary(%s) failed: %s\n", p->name, p->pid, name, err);
        unload_since(p, nmod, ninit);
        st = strstr(err, "not found") ? 0xC0000135u /* DLL_NOT_FOUND */ :
             strstr(err, "entry point") ? 0xC0000139u /* ENTRYPOINT_NOT_FOUND */ : 0xC000007Bu /* INVALID_IMAGE_FORMAT */;
    } else {
        *base = p->modules[m].base;
        if (!write_ldr_info(p, ninit) || !write_stubs(p)) st = 0xC0000017u;
    }
    um_unlock_excl(&p->lock);
    L->keep = !st;
    loader_free(L);
    um_unlock(&p->ldr_lock);
    return st;
}

/* -----------------------------------------------------------------------
 * Process environment: PEB, RTL_USER_PROCESS_PARAMETERS
 * ----------------------------------------------------------------------- */
/* Offsets in the PEB and RTL_USER_PROCESS_PARAMETERS: x64, and x86 for
 * 32-bit programs (4-byte pointers) */
typedef struct {
    UINT32 us_buf;                              /* UNICODE_STRING.Buffer */
    UINT32 std_in, std_out, std_err, cur_dir, dll_path, image, cmdline, env, env_size, runtime;
    UINT32 peb_image, peb_params, peb_ncpu, peb_major, peb_minor, peb_build, peb_platform;
} EnvLayout;

static const EnvLayout g_env64 = { 8, 0x20, 0x28, 0x30, 0x38, 0x50, 0x60, 0x70, 0x80, 0x3F0, 0xE0,
                                   0x10, 0x20, 0xB8, 0x118, 0x11C, 0x120, 0x124 };
static const EnvLayout g_env32 = { 4, 0x18, 0x1C, 0x20, 0x24, 0x30, 0x38, 0x40, 0x48, 0x290, 0x88,
                                   0x08, 0x10, 0x64, 0xA4, 0xA8, 0xAC, 0xB0 };

/* A pointer-sized field */
static void put_ptr(UINT8 *b, UINT64 v, bool wow) { if (wow) put_u32(b, (UINT32)v); else wr64(b, v); }

/* Append an ASCII/UTF-8 string as UTF-16 (NUL-terminated) at *off; fill
 * the UNICODE_STRING at @us (whose buffer pointer is at @us_buf; the
 * parameters are at user address @va).  False if it doesn't fit. */
static bool put_ustr(UINT8 *buf, UINT32 cap, UINT32 *off, UINT8 *us, const char *s,
                     UINT64 va, const EnvLayout *lay, bool wow)
{
    UINT32 n = (UINT32)strlen(s);
    if (*off + 2 * (n + 1) > cap) return false;
    UINT8 *d = buf + *off;
    for (UINT32 i = 0; i < n; i++) put_u16(d + 2 * i, (UINT8)s[i]);
    put_u16(d + 2 * n, 0);
    if (us) {
        put_u16(us, (UINT16)(2 * n));
        put_u16(us + 2, (UINT16)(2 * n + 2));
        put_ptr(us + lay->us_buf, va + *off, wow);
    }
    *off += (2 * (n + 1) + 7) & ~7U;
    return true;
}

/* The environment block (UTF-16) in a region of its own: @env holds
 * UTF-8 "NAME=value" strings each ended by NUL, then an empty one */
static UINT64 put_environment(UmProcess *p, const char *env, UINT32 env_len, UINT32 *bytes)
{
    UINT32 units = 1;
    for (UINT32 i = 0; i < env_len; i++) if ((env[i] & 0xC0) != 0x80) units++;
    UINT64 size = ((UINT64)units * 2 + 16 + PAGE_SIZE - 1) & ~(UINT64)(PAGE_SIZE - 1);
    UINT8 *b = kzalloc(size);
    if (!b) return 0;
    UINT32 o = 0;
    for (UINT32 i = 0; i < env_len; ) {                     /* UTF-8 -> UTF-16 */
        UINT32 c = (UINT8)env[i++];
        if (c >= 0xC0) {
            int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
            c &= 0x3F >> more;
            while (more-- && i < env_len && ((UINT8)env[i] & 0xC0) == 0x80) c = c << 6 | ((UINT8)env[i++] & 0x3F);
        }
        if (c >= 0x10000) {
            c -= 0x10000;
            put_u16(b + o, (UINT16)(0xD800 + (c >> 10))); o += 2;
            put_u16(b + o, (UINT16)(0xDC00 + (c & 0x3FF))); o += 2;
        } else { put_u16(b + o, (UINT16)c); o += 2; }
        if (o + 8 > size) break;
    }
    o += 2;                                                 /* the final NUL */
    UINT64 va = um_find_free(p, size, p->lay.alloc_min, p->lay.alloc_max);
    bool ok = va && um_region_add(p, va, size, 0x04, false) && um_commit(p, va, size, 0x04) &&
              um_write(p, va, b, size);
    kfree(b);
    *bytes = o;
    return ok ? va : 0;
}

/* STARTUPINFO.lpReserved2 bytes for the new process, in a region of their own */
static UINT64 put_runtime(UmProcess *p, const UINT8 *data, UINT32 len)
{
    UINT64 size = ((UINT64)len + PAGE_SIZE - 1) & ~(UINT64)(PAGE_SIZE - 1);
    UINT64 va = um_find_free(p, size, p->lay.alloc_min, p->lay.alloc_max);
    bool ok = va && um_region_add(p, va, size, 0x04, false) && um_commit(p, va, size, 0x04) &&
              um_write(p, va, data, len);
    return ok ? va : 0;
}

/* -----------------------------------------------------------------------
 * The default environment: NovaOS's own variables, then the registry's
 * (the system's, then the user's), as Windows builds it for a new logon.
 * A block of "NAME=value\0" strings; names compare without case.
 * ----------------------------------------------------------------------- */
typedef struct { char *buf; UINT32 len, cap; bool fail; } EnvB;

static int env_name_eq(const char *a, const char *b, UINT32 bn)
{
    for (UINT32 i = 0; i < bn; i++) {
        char x = a[i], y = b[i];
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y || !x) return 0;
    }
    return a[bn] == '=';
}
static const char *env_get(const EnvB *e, const char *name, UINT32 n)
{
    for (UINT32 o = 0; o < e->len; o += (UINT32)strlen(e->buf + o) + 1)
        if (env_name_eq(e->buf + o, name, n)) return e->buf + o + n + 1;
    return NULL;
}
static void env_set(EnvB *e, const char *name, const char *value)
{
    UINT32 n = (UINT32)strlen(name), vl = (UINT32)strlen(value);
    for (UINT32 o = 0; o < e->len;) {                           /* drop the old one */
        UINT32 l = (UINT32)strlen(e->buf + o) + 1;
        if (env_name_eq(e->buf + o, name, n)) { memmove(e->buf + o, e->buf + o + l, e->len - o - l); e->len -= l; }
        else o += l;
    }
    if (e->len + n + vl + 2 > e->cap) {
        UINT32 cap = (e->len + n + vl + 2) * 2 + 1024;
        char *b = kmalloc(cap + 1);
        if (!b) { e->fail = true; return; }
        if (e->buf) { memcpy(b, e->buf, e->len); kfree(e->buf); }
        e->buf = b;
        e->cap = cap;
    }
    memcpy(e->buf + e->len, name, n);
    e->buf[e->len + n] = '=';
    memcpy(e->buf + e->len + n + 1, value, vl + 1);
    e->len += n + vl + 2;
}
/* %NAME% replaced by the variable's value (left as it is if unset) */
static void env_expand(const EnvB *e, const char *in, char *out, UINT32 cap)
{
    UINT32 o = 0;
    for (const char *s = in; *s && o < cap - 1;) {
        const char *end = *s == '%' ? strchr(s + 1, '%') : NULL;
        const char *v = end && end > s + 1 ? env_get(e, s + 1, (UINT32)(end - s - 1)) : NULL;
        if (v) {
            while (*v && o < cap - 1) out[o++] = *v++;
            s = end + 1;
        } else out[o++] = *s++;
    }
    out[o] = 0;
}
typedef struct { EnvB *e; char sys_path[1024], user_path[1024]; } EnvReg;
static void env_from_registry(void *ctx, const char *name, const char *value, bool user, bool expand)
{
    EnvReg *r = ctx;
    static const char *const own[] = { "OS", "PROCESSOR_ARCHITECTURE", "NUMBER_OF_PROCESSORS", "PATHEXT", NULL };
    for (int i = 0; own[i]; i++) if (env_name_eq(own[i], name, (UINT32)strlen(name)) && strlen(own[i]) == strlen(name)) return;
    char v[1024];
    if (expand) env_expand(r->e, value, v, sizeof(v));
    else { strncpy(v, value, sizeof(v) - 1); v[sizeof(v) - 1] = 0; }
    if (env_name_eq("PATH=", name, (UINT32)strlen(name)) && strlen(name) == 4) {     /* Path: system's, then user's */
        strncpy(user ? r->user_path : r->sys_path, v, sizeof(r->sys_path) - 1);
        return;
    }
    env_set(r->e, name, v);
}
static char *default_environment(const char *const *base, UINT32 *len)
{
    EnvB e = { 0 };
    for (int i = 0; base[i]; i++) {
        const char *eq = strchr(base[i], '=');
        char name[64];
        UINT32 n = (UINT32)(eq - base[i]);
        if (n >= sizeof(name)) continue;
        memcpy(name, base[i], n);
        name[n] = 0;
        env_set(&e, name, eq + 1);
    }
    EnvReg r;
    memset(&r, 0, sizeof(r));
    r.e = &e;
    um_registry_environment(env_from_registry, &r);
    if (r.sys_path[0] || r.user_path[0]) {
        char path[2100];
        const char *sp = r.sys_path[0] ? r.sys_path : env_get(&e, "PATH", 4);
        ksnprintf(path, sizeof(path), "%s%s%s", sp ? sp : "", r.user_path[0] ? ";" : "", r.user_path);
        env_set(&e, "Path", path);
    }
    if (e.fail || !e.buf) { kfree(e.buf); return NULL; }
    *len = e.len;                                               /* (the closing NUL is added when it is copied) */
    return e.buf;
}

static bool setup_environment(UmProcess *p, UINT64 image_base, const char *image_path,
                              const char *cmdline, const char *cwd_path, const UINT64 stdv[3],
                              const char *envp, UINT32 env_len, const UINT8 *runtime, UINT32 runtime_len)
{
    const EnvLayout *L = p->wow ? &g_env32 : &g_env64;
    bool w = p->wow;
    UINT64 pva = p->lay.params;
    UINT32 sz = UM_PARAMS_PAGES * PAGE_SIZE;
    UINT8 *pp = kzalloc(sz);
    UINT8 *peb = kzalloc(PAGE_SIZE);
    bool ok = pp && peb;
    if (ok) {
        /* RTL_USER_PROCESS_PARAMETERS */
        UINT32 off = 0x400;
        put_u32(pp + 0x00, sz);
        put_u32(pp + 0x04, sz);
        put_u32(pp + 0x08, 1);                              /* NORMALIZED */
        put_ptr(pp + L->std_in, stdv[0], w);                /* StandardInput  */
        put_ptr(pp + L->std_out, stdv[1], w);               /* StandardOutput */
        put_ptr(pp + L->std_err, stdv[2], w);               /* StandardError  */
        ok = put_ustr(pp, sz, &off, pp + L->cur_dir, cwd_path, pva, L, w) &&          /* CurrentDirectory */
             put_ustr(pp, sz, &off, pp + L->dll_path, w ? "C:\\Windows\\SysWOW64" : "C:\\Windows\\System32",
                      pva, L, w) &&                                                   /* DllPath */
             put_ustr(pp, sz, &off, pp + L->image, image_path, pva, L, w) &&          /* ImagePathName */
             put_ustr(pp, sz, &off, pp + L->cmdline, cmdline, pva, L, w);             /* CommandLine */
        /* Environment block: the creator's, or the default one */
        char ncpu[32];
        ksnprintf(ncpu, sizeof(ncpu), "NUMBER_OF_PROCESSORS=%u", (unsigned)g_cpu_count);
        const char *env[] = {
            "ALLUSERSPROFILE=C:\\ProgramData", "APPDATA=C:\\AppData\\Roaming", "COMPUTERNAME=NOVA-PC",
            "ComSpec=C:\\Windows\\System32\\cmd.exe",
            "HOMEDRIVE=C:", "HOMEPATH=\\", "LOCALAPPDATA=C:\\AppData\\Local", ncpu, "OS=Windows_NT",
            "PATH=C:\\Programs;C:\\Windows\\System32;C:\\Windows", "PATHEXT=.COM;.EXE;.BAT;.CMD",
            w ? "PROCESSOR_ARCHITECTURE=x86" : "PROCESSOR_ARCHITECTURE=AMD64",
            w ? "PROCESSOR_ARCHITEW6432=AMD64" : "ProgramW6432=C:\\Programs",
            "ProgramData=C:\\ProgramData", "ProgramFiles=C:\\Programs", "ProgramFiles(x86)=C:\\Programs",
            "PROMPT=$P$G", "SystemDrive=C:", "SystemRoot=C:\\Windows",
            "TEMP=C:\\Temp", "TMP=C:\\Temp", "USERNAME=dean", "USERPROFILE=C:\\", "windir=C:\\Windows", NULL
        };
        char *def = NULL;
        if (!envp) {
            def = default_environment(env, &env_len);
            if (!def) ok = false;
            else envp = def;
        }
        UINT32 env_bytes = 0;
        UINT64 env_va = ok ? put_environment(p, envp, env_len, &env_bytes) : 0;
        kfree(def);
        if (!env_va) ok = false;
        put_ptr(pp + L->env, env_va, w);
        put_ptr(pp + L->env_size, env_bytes, w);            /* EnvironmentSize */
        if (runtime && runtime_len) {                       /* RuntimeData: the creator's lpReserved2 */
            UINT64 rva = put_runtime(p, runtime, runtime_len);
            if (!rva) ok = false;
            put_u16(pp + L->runtime, (UINT16)runtime_len);
            put_u16(pp + L->runtime + 2, (UINT16)runtime_len);
            put_ptr(pp + L->runtime + L->us_buf, rva, w);
        }

        /* PEB */
        put_ptr(peb + L->peb_image, image_base, w);         /* ImageBaseAddress */
        put_ptr(peb + L->peb_params, pva, w);               /* ProcessParameters */
        put_u32(peb + L->peb_ncpu, g_cpu_count);            /* NumberOfProcessors */
        put_u32(peb + L->peb_major, 10);                    /* OSMajorVersion */
        put_u32(peb + L->peb_minor, 0);                     /* OSMinorVersion */
        put_u16(peb + L->peb_build, 18362);                 /* OSBuildNumber (1903) */
        put_u32(peb + L->peb_platform, 2);                  /* OSPlatformId: NT */

        UINT64 pv = p->lay.peb;
        ok = ok && map_kusd(p) && um_region_add(p, pv, UM_SYS_SIZE(p->lay.max_threads), 0x04, false) &&
             um_commit(p, pv, (pva - pv) + UM_PARAMS_PAGES * PAGE_SIZE, 0x04) &&
             um_write(p, pv, peb, PAGE_SIZE) &&
             um_write(p, pva, pp, sz) &&
             write_ldr_info(p, 0) && write_stubs(p);
    }
    kfree(pp); kfree(peb);
    return ok;
}

/* -----------------------------------------------------------------------
 * Threads
 * ----------------------------------------------------------------------- */
static void um_thread_start(void *arg)
{
    UmThread *t = arg;
    UmProcess *p = t->proc;
    /* Created suspended: wait for NtResumeThread (or the end of the process) */
    while (t->suspend > 0 && !um_stopping()) sched_yield();
    cli();
    if (p->kill_pending) um_exit_thread(p->kill_status);
    if (t->terminate) um_exit_thread(t->term_status);

    /* Enter ring 3 at ntdll!RtlUserThreadStart(RCX = start, RDX = argument),
     * leaving the kernel lock behind.  The TEB is in MSR_KERNEL_GS_BASE
     * (this thread was created with it): SWAPGS makes it the user GS. */
    bkl_leave_kernel();
    /* 32-bit programs run in compatibility mode: the same entry, with
     * ntdll!RtlUserThreadStart taking ECX and EDX (fastcall); FS (base:
     * the 32-bit TEB, set on every switch to this thread) and DS/ES hold
     * the user data selector already (gdt_reload_segments) */
    UINT64 f[7] = {
        p->thread_start, p->wow ? SEL_USER_CODE32 : GDT_USER_CODE | 3, 0x202,     /* RIP, CS, RFLAGS */
        t->stack_lo + t->stack_size - (p->wow ? 0x10 : 0x28), GDT_USER_DATA | 3,  /* RSP, SS */
        t->start, t->arg                                                          /* RCX, RDX */
    };
    __asm__ volatile (
        "mov 40(%0), %%rcx\n\t"
        "mov 48(%0), %%rdx\n\t"
        "push 32(%0)\n\t"
        "push 24(%0)\n\t"
        "push 16(%0)\n\t"
        "push 8(%0)\n\t"
        "push 0(%0)\n\t"
        "xor %%eax, %%eax\n\t"  "xor %%ebx, %%ebx\n\t"
        "xor %%esi, %%esi\n\t"  "xor %%edi, %%edi\n\t"  "xor %%ebp, %%ebp\n\t"
        "xor %%r8d, %%r8d\n\t"  "xor %%r9d, %%r9d\n\t"  "xor %%r10d, %%r10d\n\t"
        "xor %%r11d, %%r11d\n\t" "xor %%r12d, %%r12d\n\t" "xor %%r13d, %%r13d\n\t"
        "xor %%r14d, %%r14d\n\t" "xor %%r15d, %%r15d\n\t"
        "swapgs\n\t"
        "iretq\n\t"
        : : "a"(f) : "memory");
    __builtin_unreachable();
}

UmThread *um_create_thread(UmProcess *p, UINT64 start, UINT64 arg, UINT64 stack_size,
                           bool suspended, UINT32 *status)
{
    *status = 0xC0000017u;                                 /* NO_MEMORY */
    stack_size = (stack_size + 0xFFFF) & ~0xFFFFULL;
    if (stack_size < 64 * 1024) stack_size = 64 * 1024;
    if (stack_size > 16 * 1024 * 1024) stack_size = 16 * 1024 * 1024;

    UmThread *t = kzalloc(sizeof(*t));
    UINT8 *fpu = kernel_alloc_pages(1);
    if (!t || !fpu) { kfree(t); if (fpu) kernel_free_pages(fpu, 1); return NULL; }
    memset(fpu, 0, PAGE_SIZE);
    fpu[0] = 0x7F; fpu[1] = 0x03;                          /* FCW = 0x037F */
    put_u32(fpu + 24, 0x1F80);                             /* MXCSR default */
    t->ob.type = UO_THREAD;
    t->ob.refs = 1;                                        /* the process's thread table */
    t->proc = p;
    t->tid = um_new_id();
    t->start = start;
    t->arg = arg;
    t->suspend = suspended ? 1 : 0;
    t->stack_size = stack_size;

    um_lock_excl(&p->lock);
    int slot = -1;
    for (int i = 0; i < p->lay.max_threads; i++) if (!p->threads[i]) { slot = i; break; }
    if (slot < 0 || p->kill_pending) {
        um_unlock_excl(&p->lock);
        kfree(t); kernel_free_pages(fpu, 1);
        *status = slot < 0 ? 0xC0000059u /* TOO_MANY_THREADS */ : 0xC000010Au /* PROCESS_IS_TERMINATING */;
        return NULL;
    }
    t->slot = slot;
    t->teb = p->lay.teb_area + (UINT64)slot * UM_TEB_SIZE;
    UINT64 lo = !p->live_threads && um_is_free(p, p->lay.stack_top - stack_size, stack_size)
                ? p->lay.stack_top - stack_size
                : um_find_free(p, stack_size, p->lay.alloc_min, p->lay.alloc_max);
    bool ok = lo && um_region_add(p, lo, stack_size, 0x04, false);
    if (ok && !um_commit(p, lo, stack_size, 0x04)) {
        um_decommit(p, lo, stack_size);
        um_region_remove(p, um_region_find(p, lo));
        ok = false;
    }
    if (ok) {
        t->stack_lo = lo;
        /* TEB */
        UINT8 *teb = kzalloc(UM_TEB_SIZE);
        ok = teb && um_commit(p, t->teb, UM_TEB_SIZE, 0x04);
        if (ok && p->wow) {                                        /* the x86 TEB (fs:0) */
            put_u32(teb + 0x00, 0xFFFFFFFFu);                      /* ExceptionList: end of chain */
            put_u32(teb + 0x04, (UINT32)(lo + stack_size));        /* StackBase */
            put_u32(teb + 0x08, (UINT32)lo);                       /* StackLimit */
            put_u32(teb + 0x18, (UINT32)t->teb);                   /* Self */
            put_u32(teb + 0x20, p->pid);                           /* ClientId.UniqueProcess */
            put_u32(teb + 0x24, t->tid);                           /* ClientId.UniqueThread */
            put_u32(teb + 0x30, (UINT32)p->lay.peb);               /* ProcessEnvironmentBlock */
            put_u32(teb + 0xE0C, (UINT32)lo);                      /* DeallocationStack */
            ok = um_write(p, t->teb, teb, UM_TEB_SIZE);
        } else if (ok) {
            wr64(teb + 0x00, UINT64_C(0xFFFFFFFFFFFFFFFF));        /* ExceptionList: none */
            wr64(teb + 0x08, lo + stack_size);                     /* StackBase */
            wr64(teb + 0x10, lo);                                  /* StackLimit */
            wr64(teb + 0x30, t->teb);                              /* Self */
            wr64(teb + 0x40, p->pid);                              /* ClientId.UniqueProcess */
            wr64(teb + 0x48, t->tid);                              /* ClientId.UniqueThread */
            wr64(teb + 0x60, p->lay.peb);                          /* ProcessEnvironmentBlock */
            wr64(teb + 0x1478, lo);                                /* DeallocationStack */
            ok = um_write(p, t->teb, teb, UM_TEB_SIZE);
        }
        kfree(teb);
        if (!ok) {
            um_decommit(p, t->teb, UM_TEB_SIZE);
            um_decommit(p, lo, stack_size);
            um_region_remove(p, um_region_find(p, lo));
        }
    }
    if (!ok) {
        um_unlock_excl(&p->lock);
        kfree(t); kernel_free_pages(fpu, 1);
        return NULL;
    }

    /* The scheduler thread is queued only once its user-mode state is
     * filled in (another CPU may run it the moment it is). */
    char tname[THREAD_NAME_MAX];
    ksnprintf(tname, sizeof(tname), "%s:%u", p->name, t->tid);
    Thread *kt = sched_new_thread(tname, um_thread_start, t, um_thread_base(p, 0), 32 * 1024);
    if (kt) {
        t->no_boost = kt->no_boost = p->no_boost;      /* (NT: a new thread takes its process's) */
        kt->um = t;
        kt->um_proc = p;
        kt->cr3 = p->pml4;
        kt->fpu = fpu;
        kt->gs_base = t->teb;                              /* user GS = TEB */
        kt->fs_base = p->wow ? t->teb : 0;                 /* 32-bit programs: fs:0 = TEB */
        t->kt = kt;
        p->threads[slot] = t;
        p->live_threads++;
        sched_start_thread(kt);
    }
    if (!kt) {
        um_decommit(p, t->teb, UM_TEB_SIZE);
        um_decommit(p, t->stack_lo, stack_size);
        um_region_remove(p, um_region_find(p, t->stack_lo));
        um_unlock_excl(&p->lock);
        kfree(t); kernel_free_pages(fpu, 1);
        return NULL;
    }
    um_unlock_excl(&p->lock);
    *status = 0;
    return t;
}

/* -----------------------------------------------------------------------
 * Spawning
 * ----------------------------------------------------------------------- */
/* Give a new process a copy of a handle (taking its own reference) */
static void handle_copy(UmHandle *d, const UmHandle *s)
{
    *d = *s;
    if (d->kind == H_FILE || d->kind == H_DIR) RamfsRef(d->node);
    else if (d->kind == H_OBJECT) um_ob_ref(d->obj);
    if (d->kind == H_FILE) um_fpos_ref(d->fp);              /* the same position as the parent's */
}

/* Let go of the program's and DLLs' files (under the desktop lock) */
static void release_images(UmProcess *p)
{
    for (int i = 0; i < p->nimages; i++) RamfsUnref(p->images[i]);
    p->nimages = 0;
}

static void destroy(UmProcess *p)
{
    um_console_flush_log(p);
    release_images(p);
    um_close_all_handles(p);
    if (p->pml4) free_address_space(p->pml4);
    um_release_views(p);
    if (p->con) UmConsoleRelease(p->con);
    if (p->token) um_ob_unref(p->token);
    kfree(p->stub_names);
    kfree(p->regions);
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
            ksnprintf(buf, sizeof(buf), "%s%s", name, e ? ".exe" : "");
            RamNode *base = d ? RamfsResolve(NULL, dirs[d]) : cwd;
            if (!base && d) continue;
            RamNode *n = RamfsResolve(base, buf);
            if (n && !n->dir) return n;
        }
    }
    /* programs installed in a folder of their own: C:\Programs\NAME\NAME.exe */
    if (!path && !has_ext && strlen(name) < 64) {
        ksnprintf(buf, sizeof(buf), "\\Programs\\%s\\%s.exe", name, name);
        RamNode *n = RamfsResolve(NULL, buf);
        if (n && !n->dir) return n;
    }
    return NULL;
}

/* A 32-bit (WoW) or 64-bit address-space layout */
void um_set_layout(UmProcess *p, bool wow)
{
    p->wow = wow;
    UINT64 peb = wow ? UM32_PEB_VA : UM_PEB_VA;
    p->lay.peb = peb;
    p->lay.ldr_info = peb + 0x1000;
    p->lay.params = peb + UM_PARAMS_OFF;
    p->lay.stubs = peb + UM_STUBS_OFF;
    p->lay.teb_area = peb + UM_TEB_OFF;
    p->lay.stack_top = wow ? UM32_STACK_TOP : UM_STACK_TOP;
    p->lay.alloc_min = wow ? UM32_ALLOC_MIN : UM_ALLOC_MIN;
    p->lay.alloc_max = wow ? UM32_ALLOC_MAX : UM_ALLOC_MAX;
    p->lay.dll_min = wow ? UM32_DLL_MIN : UM_DLL_MIN;
    p->lay.dll_max = wow ? UM32_DLL_MAX : UM_DLL_MAX;
    p->lay.max_threads = wow ? UM32_MAX_THREADS : UM_MAX_THREADS;
}

UmProcess *UmSpawn(RamNode *exe, const char *cmdline, RamNode *cwd, UmConsole *con,
                   char *err, int err_cap)
{
    return um_spawn_ex(exe, cmdline, cwd, con, NULL, err, err_cap);
}

UmProcess *um_spawn_ex(RamNode *exe, const char *cmdline, RamNode *cwd, UmConsole *con,
                       const UmSpawnOpts *o, char *err, int err_cap)
{
    UmProcess *p = um_spawn_image(exe, cwd, con, false, err, err_cap);
    return p ? um_spawn_finish(p, exe, cmdline, o, err, err_cap) : NULL;
}

/* A new process with its program and DLLs mapped (not yet running).
 * @yield: the caller holds the desktop lock once and no process lock, and
 * keeps @exe pinned and @cwd referenced; the lock is let go while the
 * images load, so a large program does not stall the desktop. */
UmProcess *um_spawn_image(RamNode *exe, RamNode *cwd, UmConsole *con, bool yield, char *err, int err_cap)
{
    err[0] = '\0';
    UmProcess *p = kzalloc(sizeof(*p));
    if (!p) { ksnprintf(err, err_cap, "Out of memory"); return NULL; }
    strncpy(p->name, exe->name, sizeof(p->name) - 1);
    p->pid = um_new_id();
    p->parent_pid = UmCurrent() ? UmCurrent()->pid : 0;
    p->create_time = um_now_100ns();
    p->regions = kzalloc(sizeof(UmRegion) * UM_MAX_REGIONS);
    p->pml4 = p->regions ? paging_create_process_pt() : 0;
    if (!p->pml4) { kfree(p->regions); kfree(p); ksnprintf(err, err_cap, "Out of memory"); return NULL; }
    p->cwd = cwd ? cwd : RamfsRoot();
    p->exe_dir = exe->parent;
    p->con = um_console_ref(con);
    p->token = um_token_for_process(UmCurrent());        /* its creator's user (the desktop's: the default) */
    /* NORMAL_PRIORITY_CLASS, or an IDLE or BELOW_NORMAL creator's class,
     * as on Windows (CreateProcess's *_PRIORITY_CLASS flags set it after) */
    p->prio_class = 2;
    if (UmCurrent() && (UmCurrent()->prio_class == 1 || UmCurrent()->prio_class == 5)) p->prio_class = UmCurrent()->prio_class;
    um_set_layout(p, um_pe_machine(exe) == 0x014C);

    /* Map the program, ntdll (every process has it) and their imports */
    Loader *L = loader_new(p, err, err_cap);
    if (!L) { destroy(p); ksnprintf(err, err_cap, "Out of memory"); return NULL; }
    if (yield) DesktopUnlock();              /* (the loader takes it for file lookups) */
    L->bkl = bkl_drop();                     /* nor the big lock: the process is ours alone */
    int nt = load_module(L, NULL, "ntdll.dll", false);
    int m = nt < 0 ? -1 : load_module(L, exe, exe->name, true);
    UINT64 base = 0, entry = 0;
    if (m >= 0) {
        base = p->modules[m].base;
        entry = p->modules[m].entry ? base + p->modules[m].entry : 0;
        p->thread_start = find_export(L, nt, "RtlUserThreadStart", 0, 0);
        p->exc_dispatcher = find_export(L, nt, "KiUserExceptionDispatcher", 0, 0);
    }
    bkl_restore(L->bkl);
    L->bkl = 0;
    L->keep = m >= 0;
    loader_free(L);
    if (yield) DesktopLock();
    if (m < 0) { destroy(p); return NULL; }
    if (!entry || p->modules[m].dll) {
        destroy(p);
        ksnprintf(err, err_cap, "%s has no entry point (is it a DLL?)", exe->name);
        return NULL;
    }
    if (!p->thread_start) { destroy(p); ksnprintf(err, err_cap, "ntdll.dll is missing RtlUserThreadStart"); return NULL; }
    p->image_base = base;
    p->image_entry = entry;
    return p;
}

/* Finish a process from um_spawn_image: environment, handles, first thread
 * (under the caller's locks, as um_spawn_ex) */
UmProcess *um_spawn_finish(UmProcess *p, RamNode *exe, const char *cmdline, const UmSpawnOpts *o,
                           char *err, int err_cap)
{
    UINT64 base = p->image_base, entry = p->image_entry;

    char image_path[RAMFS_PATH_MAX], cwd_path[RAMFS_PATH_MAX];
    RamfsPath(exe, image_path, sizeof(image_path));
    RamfsPath(p->cwd, cwd_path, sizeof(cwd_path));
    int cl = (int)strlen(cwd_path);
    if (cl && cwd_path[cl - 1] != '\\' && cl < (int)sizeof(cwd_path) - 1) { cwd_path[cl] = '\\'; cwd_path[cl + 1] = 0; }
    /* Handles: the inherited ones keep their values; each standard handle
     * is one of them, or goes in a slot of its own (the console by default) */
    UINT64 stdv[3] = { 0, 0, 0 };
    for (int i = 0; o && o->inherit && i < UM_MAX_HANDLES; i++)
        if (o->inherit[i].kind != H_FREE) handle_copy(&p->handles[i], &o->inherit[i]);
    for (int i = 0; i < 3; i++) {
        if (o && o->std_value[i]) { stdv[i] = o->std_value[i]; continue; }
        UmHandle h;
        memset(&h, 0, sizeof(h));
        if (o && o->std && o->std[i].kind != H_FREE) h = o->std[i];
        else h.kind = i ? H_CON_OUT : H_CON_IN;
        h.inherit = true;
        int slot = p->handles[i].kind == H_FREE ? i : -1;
        for (int k = 0; slot < 0 && k < UM_MAX_HANDLES; k++) if (p->handles[k].kind == H_FREE) slot = k;
        if (slot < 0) continue;
        handle_copy(&p->handles[slot], &h);
        stdv[i] = (UINT64)(slot + 1) * 4;
    }
    if (!setup_environment(p, base, image_path, cmdline, cwd_path, stdv,
                           o ? o->env : NULL, o ? o->env_len : 0,
                           o ? o->runtime : NULL, o ? o->runtime_len : 0)) {
        destroy(p);
        ksnprintf(err, err_cap, "Out of memory");
        return NULL;
    }

    plock();
    int slot = -1;
    for (int i = 0; i < UM_MAX_PROCS; i++) if (!g_procs[i]) { slot = i; break; }
    if (slot >= 0) g_procs[slot] = p;
    punlock();
    if (slot < 0) { destroy(p); ksnprintf(err, err_cap, "Too many programs are running"); return NULL; }

    /* The first thread: the program's entry point gets the PEB (as on Windows) */
    UINT64 stack = p->stack_reserve ? p->stack_reserve : UM_STACK_SIZE;
    if (stack < UM_STACK_SIZE) stack = UM_STACK_SIZE;
    if (stack > 8 * 1024 * 1024) stack = 8 * 1024 * 1024;
    UINT32 st;
    if (!um_create_thread(p, entry, p->lay.peb, stack, o && o->suspended, &st)) {
        plock(); g_procs[slot] = NULL; punlock();
        destroy(p);
        ksnprintf(err, err_cap, "Out of memory");
        return NULL;
    }
    kprintf("[UM] Started %s (PID %u): %d module(s), entry 0x%llx, %u KB\n", p->name, p->pid,
            p->nmodules, (unsigned long long)entry, p->pages * 4);
    kprintf("[UM]   command line: %s\n", cmdline ? cmdline : "");
    return p;
}

/* -----------------------------------------------------------------------
 * Ending threads and processes
 * ----------------------------------------------------------------------- */
void um_exit_thread(UINT32 status)
{
    bkl_acquire();                          /* (dropped by the final switch) */
    UmThread *t = UmCurrentThread();
    UmProcess *p = t->proc;
    um_abandon_mutants(p, t);

    /* The user stack and TEB go now; the kernel side once off the CPU */
    um_lock_excl(&p->lock);
    UmRegion *r = um_region_find(p, t->stack_lo);
    if (r && r->base == t->stack_lo) {
        um_decommit(p, r->base, r->size);
        um_region_remove(p, r);
    }
    um_decommit(p, t->teb, UM_TEB_SIZE);
    IrqState s = ob_lock();
    t->exit_code = status;
    t->exited = true;
    t->ob.signaled = true;
    um_ob_wake(&t->ob);
    if (--p->live_threads == 0) {
        p->exit_status = p->kill_pending ? p->kill_status : status;
        p->exited = true;
        if (p->exit_ob) { p->exit_ob->signaled = true; um_ob_wake(p->exit_ob); }
    }
    bool last = p->exited && t->exit_code == status && p->live_threads == 0;
    UINT32 code = p->exit_status;
    ob_unlock(s);
    um_unlock_excl(&p->lock);
    um_thread_drop_token(t);
    if (last) kprintf("[UM] %s (PID %u) exited with code %u (0x%x)\n", p->name, p->pid, code, code);
    sched_exit_current();
}

void um_exit_process(UINT32 status)
{
    UmProcess *p = UmCurrent();
    IrqState s = ob_lock();
    if (!p->kill_pending) {
        p->kill_status = status;
        p->kill_pending = true;
    }
    ob_unlock(s);
    um_exit_thread(status);
}

bool um_stopping(void)
{
    UmThread *t = UmCurrentThread();
    return t && (t->terminate || t->proc->kill_pending);
}

void UmReturnToUser(void)
{
    UmThread *t = UmCurrentThread();
    if (!t) return;
    UmProcess *p = t->proc;
    sched_current()->wait_rounds = 0;
    if (g_desktop.owner == sched_current()) {                        /* never back to user mode with it */
        kprintf("[UM] Bug: system call %03x returned holding the desktop lock\n", t->last_sys);
        g_desktop.depth = 1;
        um_unlock(&g_desktop);
    }
    if (g_fs.w.owner == sched_current()) {
        kprintf("[UM] Bug: system call %03x returned holding the file-system lock\n", t->last_sys);
        g_fs.w.depth = 1;
        um_unlock(&g_fs.w);
    }
    while (t->suspend > 0 && !um_stopping()) sched_yield();         /* NtSuspendThread */
    if (p->kill_pending) um_exit_thread(p->kill_status);
    if (t->terminate) um_exit_thread(t->term_status);
    t->park = 0;
}

void UmNoteSyscallFrame(void *frame)
{
    UmThread *t = UmCurrentThread();
    if (t) t->uframe = frame;
}

void UmReturnToUserFrame(void *frame)
{
    UmThread *t = UmCurrentThread();
    if (!t) return;
    t->uframe = frame;
    t->park = 2;
    UmReturnToUser();
}

/* Describe an unhandled exception and end the process */
void UmFault(UINT32 status, UINT64 rip, UINT64 addr)
{
    UmFaultAt(status, rip, addr, 0);
}

/* The return addresses on a user stack from @sp (serial log only) */
void um_log_stack(UmProcess *p, UINT64 sp)
{
    if (!sp) return;
    int shown = 0, bad = 0;
    unsigned step = p->wow ? 4 : 8;
    for (unsigned i = 0; i < 4096 && shown < 24; i++) {
        UINT64 v = 0;
        if (!NT_SUCCESS(CopyFromUser(&v, (const void *)(uintptr_t)(sp + i * step), step))) {
            if (++bad > 1024) break;                /* (it may start below the stack) */
            continue;
        }
        const UmModule *cm = um_module_at(p, v);
        if (!cm || v - cm->base < 0x1000) continue;
        kprintf("[UM]   stack +%04x: %s+0x%llx\n", i * step, cm->name, (unsigned long long)(v - cm->base));
        shown++;
    }
}

/* As UmFault; @sp (0: unknown) is the stack pointer at the fault, which
 * names the caller when the program jumped to a bad address */
void UmFaultAt(UINT32 status, UINT64 rip, UINT64 addr, UINT64 sp)
{
    UmProcess *p = UmCurrent();
    const char *what = status == UM_STATUS_ACCESS_VIOLATION ? "access violation" :
                       status == 0xC0000094u ? "integer divide by zero" :
                       status == 0xC000001Du ? "illegal instruction" :
                       status == 0xC00000FDu ? "stack overflow" :
                       status == 0xC0000096u ? "privileged instruction" :
                       status == 0x80000003u ? "breakpoint" :
                       (status & 0xF0000000u) == 0xC0000000u ? "unhandled exception" : "unhandled software exception";
    const UmModule *mod = um_module_at(p, rip);
    char where[64];
    if (mod) ksnprintf(where, sizeof(where), "%s+0x%llx", mod->name, (unsigned long long)(rip - mod->base));
    else     ksnprintf(where, sizeof(where), "0x%llx", (unsigned long long)rip);
    if (!p->kill_pending) {
        if (status == UM_STATUS_ACCESS_VIOLATION)
            ksnprintf(p->why, sizeof(p->why), "crashed: %s at %s (address 0x%llx)", what, where,
                      (unsigned long long)addr);
        else if (!strcmp(what, "unhandled exception") || !strcmp(what, "unhandled software exception"))
            ksnprintf(p->why, sizeof(p->why), "crashed: %s 0x%08x at %s", what, status, where);
        else
            ksnprintf(p->why, sizeof(p->why), "crashed: %s at %s", what, where);
        char caller[80] = "";
        UINT64 ret = 0;
        if (!mod && sp && NT_SUCCESS(CopyFromUser(&ret, (const void *)(uintptr_t)sp, p->wow ? 4 : 8))) {
            const UmModule *cm = um_module_at(p, ret);
            if (cm) ksnprintf(caller, sizeof(caller), " (called from %s+0x%llx)", cm->name,
                              (unsigned long long)(ret - cm->base));
        }
        kprintf("[UM] %s (PID %u) %s%s\n", p->name, p->pid, p->why, caller);
        if (!mod) {                                         /* generated code (a JIT): show it */
            UINT8 code[16];
            if (NT_SUCCESS(CopyFromUser(code, (const void *)(uintptr_t)rip, sizeof(code))))
                kprintf("[UM]   code: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x  (sp 0x%llx)\n",
                        code[0], code[1], code[2], code[3], code[4], code[5], code[6], code[7], code[8], code[9],
                        code[10], code[11], (unsigned long long)sp);
        }
        um_log_stack(p, sp);                        /* where it came from (serial log only) */
    }
    um_exit_process(status);
}

/* Where each thread is (serial log): helps when a program will not stop */
static void dump_threads(UmProcess *p)
{
    for (int i = 0; i < UM_MAX_THREADS; i++) {
        UmThread *t = p->threads[i];
        if (!t || t->exited) continue;
        const InterruptFrame *uf = t->park == 2 && t->uframe ? (const InterruptFrame *)t->uframe : NULL;
        /* a thread preempted in user mode: the interrupt frame sits at the top of its kernel stack */
        if (!uf && t->park == 0 && t->kt && !t->kt->on_cpu && t->kt->kernel_stack) {
            const InterruptFrame *f = (const InterruptFrame *)((char *)t->kt->kernel_stack + t->kt->stack_size - sizeof(InterruptFrame));
            if ((f->cs & 3) == 3 && (f->ss & 3) == 3) uf = f;
        }
        UINT64 rip = uf ? uf->rip : 0;
        const UmModule *rm = rip ? um_module_at(p, rip) : NULL;
        kprintf("[UM]   thread %u: %s, last system call %03x(%llx), user rip %llx%s%s+0x%llx\n", t->tid,
                t->park == 1 ? "in a system call" : t->park == 2 ? "interrupted" : "running",
                t->last_sys, (unsigned long long)t->last_a1, (unsigned long long)rip,
                rm ? " " : "", rm ? rm->name : "", (unsigned long long)(rm ? rip - rm->base : 0));
        UINT64 word = 0;
        if (t->park == 1 && t->last_sys == SYSCALL_NtWaitForAlertByThreadId && um_read(p, t->last_a1, &word, 8))
            kprintf("[UM]     the address holds %llx\n", (unsigned long long)word);
        if (t->kt)
            kprintf("[UM]     kernel: state %d cpu %u on_cpu %d sleeping %d (cpu %u) wake tick %llu, now %llu\n",
                    (int)t->kt->state, t->kt->cpu, (int)t->kt->on_cpu, (int)t->kt->in_sleepers, t->kt->sleep_cpu,
                    (unsigned long long)t->kt->wake_tick, (unsigned long long)sched_ticks());
        for (int k = 0; k < t->wait_n && k < 4 && t->wait_objs; k++) {
            UmObject *wo = t->wait_objs[k];
            char nm[96];
            um_object_name(wo, nm, sizeof(nm));
            kprintf("[UM]     waits on object type %d%s%s%s\n", wo->type, nm[0] ? " \"" : "", nm, nm[0] ? "\"" : "");
        }
        /* where it came from: return addresses on its user stack (a thread
         * interrupted in user mode: from the interrupted frame's stack) */
        UINT64 sp = t->park == 1 && t->kt ? t->kt->user_rsp : uf ? uf->rsp : 0;
        for (unsigned i = 0, shown = 0; sp && i < 512 && shown < 12; i++) {
            UINT64 v = 0;
            if (!um_read(p, sp + 8 * (UINT64)i, &v, p->wow ? 4 : 8)) break;
            const UmModule *m = um_module_at(p, v);
            if (!m || v - m->base < 0x1000) continue;
            kprintf("[UM]     %s+0x%llx\n", m->name, (unsigned long long)(v - m->base));
            shown++;
        }
    }
}

void UmDumpAll(void)
{
    for (int i = 0; i < UM_MAX_PROCS; i++) {
        UmProcess *p = g_procs[i];
        if (!p || p->exited) continue;
        kprintf("[UM] %s (PID %u):\n", p->name, p->pid);
        dump_threads(p);
    }
}

void UmKill(UmProcess *p, UINT32 status)
{
    if (!p || p->exited) return;
    kprintf("[UM] Stopping %s (PID %u)\n", p->name, p->pid);
    dump_threads(p);
    IrqState s = ob_lock();
    if (!p->kill_pending) {
        p->kill_status = status;
        ksnprintf(p->why, sizeof(p->why), "%s", status == UM_STATUS_CONTROL_C_EXIT ? "stopped (Ctrl+C)" : "terminated");
        p->kill_pending = true;
    }
    ob_unlock(s);
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

/* -----------------------------------------------------------------------
 * Starting a program without holding up the desktop: the images are
 * mapped on a worker thread (which lets go of the desktop lock while it
 * copies), and the desktop polls for the result.  All fields change under
 * the desktop lock.
 * ----------------------------------------------------------------------- */
struct UmSpawnJob {
    RamNode   *exe, *cwd;
    UmConsole *con;
    char      *cmdline;
    UmProcess *proc;
    char       err[160];
    bool       done, abandoned, detached;
};

static void spawn_free(UmSpawnJob *j)
{
    RamfsUnpin(j->exe);
    RamfsUnref(j->cwd);
    UmConsoleRelease(j->con);
    kfree(j->cmdline);
    kfree(j);
}

/* Called with the desktop lock held once */
static void spawn_run(UmSpawnJob *j, bool yield)
{
    UmProcess *p = um_spawn_image(j->exe, j->cwd, j->con, yield, j->err, sizeof(j->err));
    if (p) p = um_spawn_finish(p, j->exe, j->cmdline, NULL, j->err, sizeof(j->err));
    j->proc = p;
    j->done = true;
}

static void spawn_thread(void *arg)
{
    UmSpawnJob *j = arg;
    DesktopLock();
    spawn_run(j, true);
    if (j->detached) {                      /* runs on its own */
        if (j->proc) UmDetach(j->proc);
        else kprintf("[UM] Cannot start %s: %s\n", j->exe->name, j->err);
        spawn_free(j);
    } else if (j->abandoned) {              /* nobody is waiting any more */
        UmRelease(j->proc);
        spawn_free(j);
    }
    DesktopUnlock();
}

static UmSpawnJob *spawn_start(RamNode *exe, const char *cmdline, RamNode *cwd, UmConsole *con, bool detached)
{
    UmSpawnJob *j = kzalloc(sizeof(*j));
    if (!j) return NULL;
    j->cmdline = kmalloc(strlen(cmdline) + 1);
    if (!j->cmdline) { kfree(j); return NULL; }
    strcpy(j->cmdline, cmdline);
    j->exe = exe;
    j->cwd = cwd ? cwd : RamfsRoot();
    j->con = um_console_ref(con);
    j->detached = detached;
    RamfsPin(exe);
    RamfsRef(j->cwd);
    if (!sched_create_thread_ex("spawn", spawn_thread, j, PRIO_DESKTOP, 64 * 1024)) {
        DesktopLock();                      /* no thread: start it here */
        spawn_run(j, false);
        DesktopUnlock();
        if (detached) {
            if (j->proc) UmDetach(j->proc);
            spawn_free(j);
            return (UmSpawnJob *)1;         /* (UmSpawnDetached only tests it) */
        }
    }
    return j;
}

UmSpawnJob *UmSpawnStart(RamNode *exe, const char *cmdline, RamNode *cwd, UmConsole *con)
{
    return spawn_start(exe, cmdline, cwd, con, false);
}

bool UmSpawnDetached(RamNode *exe, const char *cmdline, RamNode *cwd)
{
    return spawn_start(exe, cmdline, cwd, NULL, true) != NULL;
}

bool UmSpawnPoll(UmSpawnJob *j, UmProcess **proc, char *err, int err_cap)
{
    if (!j->done) return false;
    *proc = j->proc;
    if (err) { strncpy(err, j->err, (size_t)err_cap - 1); err[err_cap - 1] = '\0'; }
    spawn_free(j);
    return true;
}

void UmSpawnAbandon(UmSpawnJob *j)
{
    if (!j) return;
    if (j->done) { UmRelease(j->proc); spawn_free(j); }
    else j->abandoned = true;
}

void UmDetach(UmProcess *p)
{
    if (p) p->released = true;          /* reclaimed by UmPoll once it exits */
}

/* A process made by CreateProcess is released when its process object
 * goes; holding that object keeps it */
void UmHold(UmProcess *p)
{
    if (p && p->exit_ob) um_ob_ref(p->exit_ob);
}

void UmUnhold(UmProcess *p)
{
    if (p && p->exit_ob) um_ob_unref(p->exit_ob);
}

/* GetConsoleProcessList: the running processes attached to @c */
int um_console_pids(UmConsole *c, UINT32 *out, int max)
{
    int n = 0;
    if (!c) return 0;
    plock();
    for (int i = 0; i < UM_MAX_PROCS; i++) {
        UmProcess *p = g_procs[i];
        if (!p || p->con != c || p->exited) continue;
        if (n < max) out[n] = p->pid;
        n++;
    }
    punlock();
    return n;
}

UINT32      UmPid(const UmProcess *p)  { return p->pid; }
const char *UmName(const UmProcess *p) { return p->name; }

/* Free the kernel side of threads that have ended and are off the CPU */
static int reap_threads(UmProcess *p)
{
    int left = 0;
    for (int i = 0; i < UM_MAX_THREADS; i++) {
        UmThread *t = p->threads[i];
        if (!t) continue;
        if (t->exited && t->kt && sched_thread_gone(t->kt)) {
            if (t->kt->fpu) kernel_free_pages(t->kt->fpu, 1);
            sched_free_thread(t->kt);
            t->kt = NULL;
            p->threads[i] = NULL;
            um_ob_unref(&t->ob);
            continue;
        }
        left++;
    }
    return left;
}

/* Whether a thread of @p has ended (reap_threads has work); a look without
 * the process lock: threads are added under it, and only taken out here */
static bool reap_due(UmProcess *p)
{
    for (int i = 0; i < UM_MAX_THREADS; i++) {
        UmThread *t = __atomic_load_n(&p->threads[i], __ATOMIC_ACQUIRE);
        if (t && t->exited && t->kt && sched_thread_gone(t->kt)) return true;
    }
    return false;
}

void UmSaveAll(void)
{
    um_registry_flush();
    if (!PersistSync()) kprintf("[PERSIST] Saving drive C: failed\n");
    if (!DrivesSync()) kprintf("[DRIVES] Writing changed files to the drives failed\n");
}

void UmPoll(void)
{
    um_registry_poll();
    PersistPoll();
    DrivesPoll();
    for (int i = 0; i < UM_MAX_PROCS; i++) {
        UmProcess *p = g_procs[i];
        if (!p) continue;
        if (!p->exited && !reap_due(p)) continue;  /* (most ticks: nothing to do) */
        um_lock_excl(&p->lock);
        int left = reap_threads(p);
        um_unlock_excl(&p->lock);
        if (!p->exited || left) continue;
        if (!p->reclaimed) {
            FsLock();                           /* (its images and files) */
            um_gui_process_gone(p);
            um_registry_process_gone(p);
            release_images(p);
            um_pipe_process_gone(p);            /* before its memory goes */
            um_close_all_handles(p);
            free_address_space(p->pml4);
            um_release_views(p);
            p->pml4 = 0;
            p->pages = 0;
            p->commit = 0;
            p->reclaimed = true;
            FsUnlock();
        }
        if (p->released) {
            plock(); g_procs[i] = NULL; punlock();
            IrqState st = ob_lock();                /* handles opened by others outlive it */
            if (p->exit_ob) { p->exit_ob->count = (INT32)p->exit_status; p->exit_ob->proc = NULL; p->exit_ob = NULL; }
            ob_unlock(st);
            if (p->con) UmConsoleRelease(p->con);
            if (p->token) um_ob_unref(p->token);
            kfree(p->stub_names);
            kfree(p->regions);
            kfree(p);
        }
    }
}

/* OpenProcess: the object that is signaled when @pid exits, referenced
 * (NULL if there is no such process).  A process a program created
 * already has one; for others it is made here. */
static void opened_ob_destroy(UmObject *o)
{
    IrqState s = ob_lock();
    if (o->proc && o->proc->exit_ob == o) o->proc->exit_ob = NULL;
    ob_unlock(s);
}

UmObject *um_open_process(UINT32 pid)
{
    UmObject *fresh = kzalloc(sizeof(UmObject));
    UmObject *r = NULL;
    plock();
    for (int i = 0; i < UM_MAX_PROCS && !r; i++) {
        UmProcess *p = g_procs[i];
        if (!p || p->pid != pid) continue;             /* exited but still held: still openable */
        IrqState s = ob_lock();
        if (p->exit_ob) r = um_ob_ref(p->exit_ob);
        else if (fresh) {
            fresh->type = UO_PROCESS;
            fresh->refs = 1;
            fresh->proc = p;
            fresh->signaled = p->exited;
            fresh->destroy = opened_ob_destroy;
            p->exit_ob = fresh;
            r = fresh;
            fresh = NULL;
        }
        ob_unlock(s);
    }
    punlock();
    kfree(fresh);
    return r;
}

/* OpenThread: the thread with id @tid in any process, referenced (NULL if none) */
UmObject *um_open_thread(UINT32 tid)
{
    UmObject *r = NULL;
    plock();
    for (int i = 0; i < UM_MAX_PROCS && !r; i++) {
        UmProcess *p = g_procs[i];
        if (!p || p->reclaimed) continue;
        for (int k = 0; k < UM_MAX_THREADS; k++)
            if (p->threads[k] && p->threads[k]->tid == tid) { r = um_ob_ref(&p->threads[k]->ob); break; }
    }
    punlock();
    return r;
}

int UmList(UmProcInfo *out, int max)
{
    int n = 0;
    for (int i = 0; i < UM_MAX_PROCS && n < max; i++) {
        UmProcess *p = g_procs[i];
        if (!p || p->reclaimed) continue;
        out[n].pid = p->pid;
        strncpy(out[n].name, p->name, sizeof(out[n].name) - 1);
        out[n].name[sizeof(out[n].name) - 1] = '\0';
        out[n].mem_kb = p->pages * 4;
        out[n].threads = (UINT32)p->live_threads;
        out[n].exited = p->exited;
        n++;
    }
    return n;
}

/* Ctrl+C: every program on the console (a command interpreter and what it runs) */
void UmKillConsole(UmConsole *con, UINT32 status)
{
    if (!con) return;
    for (int i = 0; i < UM_MAX_PROCS; i++) {
        UmProcess *p = g_procs[i];
        if (p && p->con == con && !p->exited) UmKill(p, status);
    }
}

bool UmKillPid(UINT32 pid)
{
    for (int i = 0; i < UM_MAX_PROCS; i++) {
        UmProcess *p = g_procs[i];
        if (p && p->pid == pid && !p->exited) { UmKill(p, 1); return true; }
    }
    return false;
}

