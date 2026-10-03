/*
 * acpi.c — ACPI tables and fixed-hardware power control
 *
 * Power-off writes SLP_TYP(S5) | SLP_EN to the PM1 control registers the
 * FADT names (or, on hardware-reduced systems, to its sleep control
 * register).  The S5 sleep type comes from the \_S5 package in the DSDT
 * or an SSDT: a Name() whose Package holds integer constants, so it can
 * be read without an AML interpreter.  Reset uses the FADT's reset
 * register when the firmware says it works.  The power button is the
 * fixed-feature one (PWRBTN_STS in the PM1 status register), which the
 * desktop polls; once the AML interpreter (aml.c) is running it handles
 * the events, control-method buttons included.
 */

#include "acpi.h"
#include "aml.h"
#include "pci.h"
#include "../arch/x86_64/cpu.h"
#include "../arch/x86_64/apic.h"
#include "../ke/printf.h"
#include "../lib/string.h"

typedef struct __attribute__((packed)) {
    char   sig[4];
    UINT32 len;
    UINT8  rev, csum;
    char   oem[6], oem_table[8];
    UINT32 oem_rev, creator, creator_rev;
} AcpiHeader;

/* Generic Address Structure */
typedef struct __attribute__((packed)) {
    UINT8  space;                        /* 0 memory, 1 I/O, 2 PCI configuration */
    UINT8  width, offset, access;
    UINT64 addr;
} Gas;

#define GAS_MEMORY 0
#define GAS_IO     1
#define GAS_PCI    2

/* FADT offsets (ACPI 6.x, table 5.9) */
#define FADT_FACS          36
#define FADT_DSDT          40
#define FADT_SMI_CMD       48
#define FADT_ACPI_ENABLE   52
#define FADT_PM1A_EVT      56
#define FADT_PM1B_EVT      60
#define FADT_PM1A_CNT      64
#define FADT_PM1B_CNT      68
#define FADT_PM1_EVT_LEN   88
#define FADT_FLAGS         112
#define FADT_RESET_REG     116
#define FADT_RESET_VALUE   128
#define FADT_X_FACS        132
#define FADT_X_DSDT        140
#define FADT_X_PM1A_EVT    148
#define FADT_X_PM1B_EVT    160
#define FADT_X_PM1A_CNT    172
#define FADT_X_PM1B_CNT    184
#define FADT_SLEEP_CTL     244
#define FADT_SLEEP_STS     256

#define FLAG_PWR_BUTTON    (1u << 4)     /* the power button is a control-method device */
#define FLAG_RESET_REG_SUP (1u << 10)
#define FLAG_HW_REDUCED    (1u << 20)

/* PM1 registers */
#define PM1_STS_PWRBTN     (1u << 8)
#define PM1_STS_WAK        (1u << 15)
#define PM1_EN_PWRBTN      (1u << 8)
#define PM1_CNT_SCI_EN     (1u << 0)
#define PM1_CNT_SLP_TYP    (7u << 10)
#define PM1_CNT_SLP_EN     (1u << 13)

static UINT64 g_rsdp;
static const UINT8 *g_fadt_ptr;
static bool   g_fadt;                    /* found and read */
static Gas    g_pm1a_evt, g_pm1b_evt, g_pm1a_cnt, g_pm1b_cnt;
static UINT8  g_evt_len;
static UINT32 g_flags;
static Gas    g_reset;
static UINT8  g_reset_value;
static Gas    g_sleep_ctl, g_sleep_sts;
static int    g_s5a = -1, g_s5b = -1;    /* \_S5 sleep types; -1 unknown */
static int    g_s3a = -1, g_s3b = -1;    /* \_S3 */
static UINT8 *g_facs;                    /* firmware ACPI control structure */
static bool   g_button;                  /* fixed power button enabled */

/* -----------------------------------------------------------------------
 * Tables
 * ----------------------------------------------------------------------- */
static const AcpiHeader *table_at(UINT64 pa)
{
    return pa ? (const AcpiHeader *)PHYS_TO_VIRT(pa) : NULL;
}

/* The @index'th table with this signature */
static const AcpiHeader *find_nth(const char *sig, int index)
{
    if (!g_rsdp) return NULL;
    const UINT8 *r = PHYS_TO_VIRT(g_rsdp);
    if (memcmp(r, "RSD PTR ", 8)) return NULL;
    UINT64 xsdt = r[15] >= 2 ? *(const UINT64 *)(r + 24) : 0;
    UINT32 rsdt = *(const UINT32 *)(r + 16);
    const AcpiHeader *root = table_at(xsdt ? xsdt : rsdt);
    if (!root) return NULL;
    UINT32 esize = xsdt ? 8 : 4;
    UINT32 n = (root->len - sizeof(AcpiHeader)) / esize;
    const UINT8 *ents = (const UINT8 *)(root + 1);
    for (UINT32 i = 0; i < n; i++) {
        UINT64 pa = esize == 8 ? *(const UINT64 *)(ents + 8 * i) : *(const UINT32 *)(ents + 4 * i);
        const AcpiHeader *h = table_at(pa);
        if (h && !memcmp(h->sig, sig, 4) && index-- == 0) return h;
    }
    return NULL;
}

const void *AcpiFindTable(const char *sig) { return find_nth(sig, 0); }

/* -----------------------------------------------------------------------
 * Register access through a Generic Address Structure
 * ----------------------------------------------------------------------- */
static bool gas_valid(const Gas *g) { return g->addr != 0; }

static UINT32 gas_read(const Gas *g, int bits)
{
    switch (g->space) {
    case GAS_IO:
        return bits == 8 ? inb((UINT16)g->addr) : bits == 16 ? inw((UINT16)g->addr) : inl((UINT16)g->addr);
    case GAS_MEMORY: {
        volatile void *p = PHYS_TO_VIRT(g->addr);
        return bits == 8 ? *(volatile UINT8 *)p : bits == 16 ? *(volatile UINT16 *)p : *(volatile UINT32 *)p;
    }
    default:
        return 0;
    }
}

static void gas_write(const Gas *g, int bits, UINT32 v)
{
    switch (g->space) {
    case GAS_IO:
        if (bits == 8) outb((UINT16)g->addr, (UINT8)v);
        else if (bits == 16) outw((UINT16)g->addr, (UINT16)v);
        else outl((UINT16)g->addr, v);
        break;
    case GAS_MEMORY: {
        volatile void *p = PHYS_TO_VIRT(g->addr);
        if (bits == 8) *(volatile UINT8 *)p = (UINT8)v;
        else if (bits == 16) *(volatile UINT16 *)p = (UINT16)v;
        else *(volatile UINT32 *)p = v;
        break;
    }
    case GAS_PCI: {                      /* bus 0; device, function, offset in the address */
        UINT8 dev = (UINT8)(g->addr >> 32), fn = (UINT8)(g->addr >> 16), off = (UINT8)g->addr;
        UINT32 old = PciRead32(0, dev, fn, off & ~3);
        int sh = (off & 3) * 8;
        PciWrite32(0, dev, fn, off & ~3, (old & ~(0xFFu << sh)) | ((v & 0xFF) << sh));
        break;
    }
    }
}

/* An X_ (64-bit GAS) field when the FADT is long enough to have it and
 * it's filled in, else the legacy 32-bit I/O port */
static Gas fadt_block(const UINT8 *f, UINT32 len, int legacy, int x)
{
    Gas g = {0};
    if (x + (int)sizeof(Gas) <= (int)len) {
        memcpy(&g, f + x, sizeof(Gas));
        if (g.addr) return g;
    }
    g.space = GAS_IO;
    g.addr = *(const UINT32 *)(f + legacy);
    return g;
}

/* -----------------------------------------------------------------------
 * \_S5: Name(_S5_, Package() { SLP_TYPa, SLP_TYPb, ... })
 * ----------------------------------------------------------------------- */
static int aml_integer(const UINT8 **pp, const UINT8 *end)
{
    const UINT8 *p = *pp;
    if (p >= end) return -1;
    switch (*p) {
    case 0x00: case 0x01: *pp = p + 1; return *p;            /* ZeroOp, OneOp */
    case 0x0A: if (p + 2 > end) return -1; *pp = p + 2; return p[1];          /* BytePrefix */
    case 0x0B: if (p + 3 > end) return -1; *pp = p + 3; return p[1] | p[2] << 8;   /* WordPrefix */
    case 0x0C: if (p + 5 > end) return -1; *pp = p + 5; return p[1] | p[2] << 8;   /* DWordPrefix (low bits) */
    default:   return -1;
    }
}

/* The sleep types in Name(@name, Package() { SLP_TYPa, SLP_TYPb, ... }) */
static bool find_sleep_type(const AcpiHeader *t, const char *name, int *ta, int *tb)
{
    if (!t || t->len <= sizeof(AcpiHeader)) return false;
    const UINT8 *body = (const UINT8 *)(t + 1), *end = (const UINT8 *)t + t->len;
    for (const UINT8 *p = body + 1; p + 4 < end; p++) {
        if (memcmp(p, name, 4)) continue;
        /* NameOp, possibly with a root prefix: 08 _S5_ or 08 5C _S5_ */
        if (!(p[-1] == 0x08 || (p[-1] == '\\' && p - 2 >= body && p[-2] == 0x08))) continue;
        const UINT8 *q = p + 4;
        if (q >= end || *q != 0x12) continue;                    /* PackageOp */
        q++;
        if (q >= end) continue;
        q += 1 + (*q >> 6);                                      /* PkgLength */
        q++;                                                     /* NumElements */
        int a = aml_integer(&q, end);
        int b = aml_integer(&q, end);
        if (a < 0) continue;
        *ta = a & 7;
        *tb = (b < 0 ? a : b) & 7;
        return true;
    }
    return false;
}

/* \_Sx in the DSDT, else in an SSDT */
static bool sleep_type(const AcpiHeader *dsdt, const char *name, int *ta, int *tb)
{
    if (find_sleep_type(dsdt, name, ta, tb)) return true;
    for (int i = 0; i < 16; i++) {
        const AcpiHeader *ssdt = find_nth("SSDT", i);
        if (!ssdt) break;
        if (find_sleep_type(ssdt, name, ta, tb)) return true;
    }
    return false;
}

/* -----------------------------------------------------------------------
 * Initialization
 * ----------------------------------------------------------------------- */
static void enable_acpi_mode(const UINT8 *f)
{
    if (!gas_valid(&g_pm1a_cnt) || (gas_read(&g_pm1a_cnt, 16) & PM1_CNT_SCI_EN)) return;
    UINT32 smi = *(const UINT32 *)(f + FADT_SMI_CMD);
    UINT8 enable = f[FADT_ACPI_ENABLE];
    if (!smi || !enable) return;
    outb((UINT16)smi, enable);
    for (int i = 0; i < 300 && !(gas_read(&g_pm1a_cnt, 16) & PM1_CNT_SCI_EN); i++)
        udelay(10000);                   /* up to 3 s, as the spec allows */
    if (!(gas_read(&g_pm1a_cnt, 16) & PM1_CNT_SCI_EN))
        kprintf("[ACPI] The firmware didn't enter ACPI mode\n");
}

static void enable_power_button(void)
{
    if ((g_flags & FLAG_PWR_BUTTON) || (g_flags & FLAG_HW_REDUCED)) return;
    if (!gas_valid(&g_pm1a_evt) || g_evt_len < 4) return;
    Gas en = g_pm1a_evt;
    en.addr += g_evt_len / 2;
    gas_write(&g_pm1a_evt, 16, PM1_STS_PWRBTN);                  /* clear a stale press (write 1) */
    gas_write(&en, 16, gas_read(&en, 16) | PM1_EN_PWRBTN);
    g_button = true;
}

void AcpiInitialize(UINT64 rsdp_physical)
{
    g_rsdp = rsdp_physical;
    const AcpiHeader *fh = find_nth("FACP", 0);
    if (!fh || fh->len < FADT_FLAGS + 4) {
        kprintf("[ACPI] No FADT: power-off and reset use the common virtual machines' ports\n");
        return;
    }
    const UINT8 *f = (const UINT8 *)fh;
    UINT32 len = fh->len;
    g_fadt      = true;
    g_fadt_ptr  = f;
    g_flags     = *(const UINT32 *)(f + FADT_FLAGS);
    g_pm1a_evt  = fadt_block(f, len, FADT_PM1A_EVT, FADT_X_PM1A_EVT);
    g_pm1b_evt  = fadt_block(f, len, FADT_PM1B_EVT, FADT_X_PM1B_EVT);
    g_pm1a_cnt  = fadt_block(f, len, FADT_PM1A_CNT, FADT_X_PM1A_CNT);
    g_pm1b_cnt  = fadt_block(f, len, FADT_PM1B_CNT, FADT_X_PM1B_CNT);
    g_evt_len   = f[FADT_PM1_EVT_LEN];
    if (len >= FADT_RESET_VALUE + 1) {
        memcpy(&g_reset, f + FADT_RESET_REG, sizeof(Gas));
        g_reset_value = f[FADT_RESET_VALUE];
    }
    if (len >= FADT_SLEEP_STS + sizeof(Gas)) {
        memcpy(&g_sleep_ctl, f + FADT_SLEEP_CTL, sizeof(Gas));
        memcpy(&g_sleep_sts, f + FADT_SLEEP_STS, sizeof(Gas));
    }

    UINT64 dsdt = len >= FADT_X_DSDT + 8 ? *(const UINT64 *)(f + FADT_X_DSDT) : 0;
    if (!dsdt) dsdt = *(const UINT32 *)(f + FADT_DSDT);
    bool s5 = sleep_type(table_at(dsdt), "_S5_", &g_s5a, &g_s5b);
    sleep_type(table_at(dsdt), "_S3_", &g_s3a, &g_s3b);
    UINT64 facs = len >= FADT_X_FACS + 8 ? *(const UINT64 *)(f + FADT_X_FACS) : 0;
    if (!facs) facs = *(const UINT32 *)(f + FADT_FACS);
    if (facs && !memcmp(PHYS_TO_VIRT(facs), "FACS", 4)) g_facs = PHYS_TO_VIRT(facs);

    if (!(g_flags & FLAG_HW_REDUCED)) enable_acpi_mode(f);
    enable_power_button();

    kprintf("[ACPI] PM1a_CNT %s 0x%lx, S5 %s", g_pm1a_cnt.space == GAS_IO ? "port" : "mmio",
            g_pm1a_cnt.addr, s5 ? "" : "not found");
    if (s5) kprintf("%d/%d", g_s5a, g_s5b);
    kprintf(", S3 %s", AcpiSleepSupported() ? "" : "not supported");
    if (AcpiSleepSupported()) kprintf("%d/%d", g_s3a, g_s3b);
    kprintf(", reset %s, power button %s\n",
            (g_flags & FLAG_RESET_REG_SUP) && gas_valid(&g_reset) ? "register" : "fallback",
            g_button ? "fixed" : "none");
}

/* -----------------------------------------------------------------------
 * Power-off, reset, power button
 * ----------------------------------------------------------------------- */
static void legacy_power_off(void)
{
    /* What the common virtual machines decode: QEMU q35/ICH9, QEMU
     * i440fx/PIIX4 and Bochs, VirtualBox */
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    outw(0x4004, 0x3400);
}

/* Write SLP_TYPx | SLP_EN: the machine sleeps (or powers off) during the
 * write, or soon after; returns if it didn't */
static void enter_sleep_state(int ta, int tb)
{
    if ((g_flags & FLAG_HW_REDUCED) && gas_valid(&g_sleep_ctl)) {
        if (gas_valid(&g_sleep_sts)) gas_write(&g_sleep_sts, 8, 0x80);   /* WAK_STS */
        gas_write(&g_sleep_ctl, 8, (UINT32)(ta << 2) | 0x20);           /* SLP_TYPx | SLP_EN */
    } else if (gas_valid(&g_pm1a_cnt)) {
        if (gas_valid(&g_pm1a_evt)) gas_write(&g_pm1a_evt, 16, PM1_STS_WAK);
        if (gas_valid(&g_pm1b_evt)) gas_write(&g_pm1b_evt, 16, PM1_STS_WAK);
        UINT32 a = gas_read(&g_pm1a_cnt, 16) & ~(PM1_CNT_SLP_TYP | PM1_CNT_SLP_EN);
        UINT32 b = gas_valid(&g_pm1b_cnt) ? gas_read(&g_pm1b_cnt, 16) & ~(PM1_CNT_SLP_TYP | PM1_CNT_SLP_EN) : 0;
        /* The type first, then SLP_EN, to both blocks */
        gas_write(&g_pm1a_cnt, 16, a | (UINT32)ta << 10);
        if (gas_valid(&g_pm1b_cnt)) gas_write(&g_pm1b_cnt, 16, b | (UINT32)tb << 10);
        gas_write(&g_pm1a_cnt, 16, a | (UINT32)ta << 10 | PM1_CNT_SLP_EN);
        if (gas_valid(&g_pm1b_cnt)) gas_write(&g_pm1b_cnt, 16, b | (UINT32)tb << 10 | PM1_CNT_SLP_EN);
    }
}

void AcpiPowerOff(void)
{
    cli();
    if (g_fadt && g_s5a >= 0) {
        enter_sleep_state(g_s5a, g_s5b);
        udelay(100000);
        kprintf("[ACPI] Still running after the S5 request\n");
    }
    legacy_power_off();
    udelay(100000);
}

bool AcpiSleepSupported(void)
{
    return g_fadt && g_s3a >= 0 && g_facs
        && (gas_valid(&g_pm1a_cnt) || ((g_flags & FLAG_HW_REDUCED) && gas_valid(&g_sleep_ctl)));
}

bool AcpiEnterS3(UINT32 real_vector, UINT32 pm32_vector)
{
    if (!AcpiSleepSupported()) return false;
    *(volatile UINT32 *)(g_facs + 12) = real_vector;             /* real mode, CS:IP = vector */
    if (*(const UINT32 *)(g_facs + 4) >= 40) {                    /* ACPI 2.0+: X_ vector, 32-bit */
        *(volatile UINT64 *)(g_facs + 24) = pm32_vector;
        *(volatile UINT32 *)(g_facs + 36) &= ~1u;                 /* OSPM flags: not 64BIT_WAKE */
    }
    wbinvd();
    enter_sleep_state(g_s3a, g_s3b);
    for (int i = 0; i < 1000; i++) udelay(1000);   /* the platform takes a moment to go down */
    return false;
}

static UINT16 g_wake_sts;
UINT16 AcpiWakeStatus(void) { return g_wake_sts; }

void AcpiResume(void)
{
    if (!g_fadt) return;
    g_wake_sts = gas_valid(&g_pm1a_evt) ? (UINT16)gas_read(&g_pm1a_evt, 16) : 0;
    if (!(g_flags & FLAG_HW_REDUCED)) enable_acpi_mode(g_fadt_ptr);
    if (gas_valid(&g_pm1a_evt)) gas_write(&g_pm1a_evt, 16, PM1_STS_WAK);
    g_button = false;
    enable_power_button();               /* a press that woke the machine doesn't count */
}

void AcpiReset(void)
{
    cli();
    if (g_fadt && (g_flags & FLAG_RESET_REG_SUP) && gas_valid(&g_reset)
        && g_reset.space <= GAS_PCI) {
        gas_write(&g_reset, 8, g_reset_value);
        udelay(100000);
    }
    outb(0xCF9, 0x02);                   /* PCI reset control: hard reset */
    udelay(10);
    outb(0xCF9, 0x06);
    udelay(100000);
    for (int i = 0; i < 100000; i++)     /* 8042: pulse the reset line */
        if (!(inb(0x64) & 2)) break;
    outb(0x64, 0xFE);
    udelay(100000);
    static const struct __attribute__((packed)) { UINT16 limit; UINT64 base; } none = {0, 0};
    __asm__ volatile ("lidt %0; int3" : : "m"(none));   /* triple fault */
    for (;;) hlt();
}

UINT64 AcpiRsdpAddress(void) { return g_rsdp; }

bool AcpiPowerButtonPressed(void)
{
    if (AmlReady()) return AmlPowerButtonPressed();   /* the interpreter handles the events */
    if (!g_button) return false;
    if (!(gas_read(&g_pm1a_evt, 16) & PM1_STS_PWRBTN)) return false;
    gas_write(&g_pm1a_evt, 16, PM1_STS_PWRBTN);                 /* write 1 to clear */
    return true;
}
