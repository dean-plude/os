/*
 * e1000.c — Intel 8254x/8257x Gigabit Ethernet driver (e1000 / e1000e)
 *
 * Register layout and descriptor formats follow Intel's "PCI/PCI-X Family
 * of Gigabit Ethernet Controllers Software Developer's Manual" (8254x)
 * and the 82574 datasheet; only the legacy-descriptor subset is used,
 * which both families support.
 */

#include "e1000.h"
#include "../hal/pci.h"
#include "../mm/vmm.h"
#include "../lib/string.h"
#include "../ke/printf.h"
#include "../ke/scheduler.h"
#include "../arch/x86_64/cpu.h"

/* Registers */
#define REG_CTRL    0x0000
#define REG_STATUS  0x0008
#define REG_EERD    0x0014
#define REG_ICR     0x00C0
#define REG_IMC     0x00D8
#define REG_RCTL    0x0100
#define REG_TCTL    0x0400
#define REG_TIPG    0x0410
#define REG_RDBAL   0x2800
#define REG_RDBAH   0x2804
#define REG_RDLEN   0x2808
#define REG_RDH     0x2810
#define REG_RDT     0x2818
#define REG_TDBAL   0x3800
#define REG_TDBAH   0x3804
#define REG_TDLEN   0x3808
#define REG_TDH     0x3810
#define REG_TDT     0x3818
#define REG_MTA     0x5200
#define REG_RAL0    0x5400
#define REG_RAH0    0x5404

#define CTRL_ASDE   (1u << 5)
#define CTRL_SLU    (1u << 6)
#define CTRL_RST    (1u << 26)
#define STATUS_LU   (1u << 1)

#define RCTL_EN     (1u << 1)
#define RCTL_BAM    (1u << 15)       /* accept broadcast */
#define RCTL_SECRC  (1u << 26)       /* strip Ethernet CRC */
                                     /* BSIZE = 00 → 2048-byte buffers */
#define TCTL_EN     (1u << 1)
#define TCTL_PSP    (1u << 3)
#define TCTL_CT     (0x0Fu << 4)
#define TCTL_COLD   (0x40u << 12)

#define TXCMD_EOP   (1u << 0)
#define TXCMD_IFCS  (1u << 1)
#define TXCMD_RS    (1u << 3)
#define DESC_DD     (1u << 0)
#define RXS_EOP     (1u << 1)

#define N_RX    32
#define N_TX    32
#define BUF_SZ  2048

typedef struct __attribute__((packed)) {
    UINT64 addr;
    UINT16 length;
    UINT16 csum;
    UINT8  status;
    UINT8  errors;
    UINT16 special;
} RxDesc;

typedef struct __attribute__((packed)) {
    UINT64 addr;
    UINT16 length;
    UINT8  cso;
    UINT8  cmd;
    UINT8  status;
    UINT8  css;
    UINT16 special;
} TxDesc;

static struct {
    bool              present;
    const char       *name;
    volatile UINT8   *mmio;
    RxDesc           *rx;             /* rings (physmap virtual addresses) */
    TxDesc           *tx;
    UINT8            *rxbuf, *txbuf;
    int               rx_next, tx_next;
    UINT8             mac[6];
    bool              eerd_82574;     /* EERD field layout differs */
} g;

static inline UINT32 rd(UINT32 reg)          { return *(volatile UINT32 *)(g.mmio + reg); }
static inline void   wr(UINT32 reg, UINT32 v){ *(volatile UINT32 *)(g.mmio + reg) = v; }
static inline UINT64 phys(const void *va)    { return (UINT64)(uintptr_t)va - PHYSMAP_BASE; }

static void delay_ms(int ms)
{
    UINT64 end = sched_ticks() + (UINT64)(ms + 9) / 10;
    if (!(read_rflags() & 0x200)) {                 /* interrupts off: spin */
        for (volatile int i = 0; i < ms * 20000; i++) { }
        return;
    }
    while (sched_ticks() < end) pause_cpu();
}

static UINT16 eeprom_read(UINT8 word)
{
    UINT32 start = g.eerd_82574 ? ((UINT32)word << 2) | 1 : ((UINT32)word << 8) | 1;
    UINT32 done  = g.eerd_82574 ? (1u << 1) : (1u << 4);
    wr(REG_EERD, start);
    for (int i = 0; i < 100000; i++) {
        UINT32 v = rd(REG_EERD);
        if (v & done) return (UINT16)(v >> 16);
    }
    return 0;
}

static void read_mac(void)
{
    UINT32 lo = rd(REG_RAL0), hi = rd(REG_RAH0);
    if (lo || (hi & 0xFFFF)) {
        for (int i = 0; i < 4; i++) g.mac[i] = (UINT8)(lo >> (i * 8));
        g.mac[4] = (UINT8)hi; g.mac[5] = (UINT8)(hi >> 8);
        return;
    }
    for (int i = 0; i < 3; i++) {               /* fall back to the EEPROM */
        UINT16 w = eeprom_read((UINT8)i);
        g.mac[i * 2] = (UINT8)w; g.mac[i * 2 + 1] = (UINT8)(w >> 8);
    }
}

bool E1000Init(void)
{
    static const UINT16 e1000_ids[]  = { 0x100E, 0x100F, 0x1004, 0x100C, 0x1015 };
    static const UINT16 e1000e_ids[] = { 0x10D3 };
    PciDevice d;
    if (PciFind(0x8086, e1000e_ids, 1, &d)) {
        g.name = "Intel 82574L Gigabit Network Connection (e1000e)";
        g.eerd_82574 = true;
    } else if (PciFind(0x8086, e1000_ids, 5, &d)) {
        g.name = "Intel PRO/1000 MT Network Connection (e1000)";
    } else {
        kprintf("[E1000] No supported Intel NIC found\n");
        return false;
    }

    UINT64 bar = PciBarAddress(&d, 0);
    if (!bar) { kprintf("[E1000] BAR0 is not a memory BAR\n"); return false; }
    PciEnableDevice(&d);
    g.mmio = (volatile UINT8 *)(uintptr_t)(PHYSMAP_BASE + bar);

    /* Reset, then mask every interrupt source (we poll) */
    wr(REG_IMC, 0xFFFFFFFFu);
    wr(REG_CTRL, rd(REG_CTRL) | CTRL_RST);
    delay_ms(10);
    for (int i = 0; i < 100000 && (rd(REG_CTRL) & CTRL_RST); i++) pause_cpu();
    wr(REG_IMC, 0xFFFFFFFFu);
    (void)rd(REG_ICR);

    wr(REG_CTRL, (rd(REG_CTRL) | CTRL_SLU | CTRL_ASDE));
    read_mac();
    for (int i = 0; i < 128; i++) wr(REG_MTA + i * 4, 0);

    /* Descriptor rings and buffers (physically contiguous, via the physmap) */
    g.rx    = kernel_alloc_pages(1);
    g.tx    = kernel_alloc_pages(1);
    g.rxbuf = kernel_alloc_pages(N_RX * BUF_SZ / PAGE_SIZE);
    g.txbuf = kernel_alloc_pages(N_TX * BUF_SZ / PAGE_SIZE);
    if (!g.rx || !g.tx || !g.rxbuf || !g.txbuf) {
        kprintf("[E1000] Out of memory for rings\n");
        return false;
    }
    memset(g.rx, 0, PAGE_SIZE);
    memset(g.tx, 0, PAGE_SIZE);

    for (int i = 0; i < N_RX; i++) g.rx[i].addr = phys(g.rxbuf + i * BUF_SZ);
    wr(REG_RDBAL, (UINT32)phys(g.rx));
    wr(REG_RDBAH, (UINT32)(phys(g.rx) >> 32));
    wr(REG_RDLEN, N_RX * sizeof(RxDesc));
    wr(REG_RDH, 0);
    wr(REG_RDT, N_RX - 1);
    wr(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);

    for (int i = 0; i < N_TX; i++) {
        g.tx[i].addr   = phys(g.txbuf + i * BUF_SZ);
        g.tx[i].status = DESC_DD;              /* free */
    }
    wr(REG_TDBAL, (UINT32)phys(g.tx));
    wr(REG_TDBAH, (UINT32)(phys(g.tx) >> 32));
    wr(REG_TDLEN, N_TX * sizeof(TxDesc));
    wr(REG_TDH, 0);
    wr(REG_TDT, 0);
    wr(REG_TIPG, 0x0060200A);
    wr(REG_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT | TCTL_COLD);

    g.rx_next = 0;
    g.tx_next = 0;
    g.present = true;
    kprintf("[E1000] %s at %02x:%02x.%x, MAC %02x:%02x:%02x:%02x:%02x:%02x, link %s\n",
            g.name, d.bus, d.dev, d.func, g.mac[0], g.mac[1], g.mac[2], g.mac[3],
            g.mac[4], g.mac[5], E1000LinkUp() ? "up" : "down");
    return true;
}

bool        E1000Present(void) { return g.present; }
const char *E1000Name(void)    { return g.present ? g.name : "No network adapter"; }
void        E1000Mac(UINT8 mac[6]) { memcpy(mac, g.mac, 6); }
bool        E1000LinkUp(void)  { return g.present && (rd(REG_STATUS) & STATUS_LU); }

bool E1000Transmit(const void *frame, UINT16 len)
{
    if (!g.present || len > BUF_SZ) return false;
    TxDesc *d = &g.tx[g.tx_next];
    if (!(d->status & DESC_DD)) return false;        /* ring full */
    memcpy(g.txbuf + g.tx_next * BUF_SZ, frame, len);
    d->length = len;
    d->cmd    = TXCMD_EOP | TXCMD_IFCS | TXCMD_RS;
    d->status = 0;
    g.tx_next = (g.tx_next + 1) % N_TX;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);         /* descriptor before tail */
    wr(REG_TDT, (UINT32)g.tx_next);
    return true;
}

int E1000Receive(void *buf, int cap)
{
    if (!g.present) return 0;
    RxDesc *d = &g.rx[g.rx_next];
    if (!(d->status & DESC_DD)) return 0;
    int len = 0;
    if ((d->status & RXS_EOP) && !d->errors) {       /* whole, good frame */
        len = d->length < cap ? d->length : cap;
        memcpy(buf, g.rxbuf + g.rx_next * BUF_SZ, (size_t)len);
    }
    d->status = 0;
    int done = g.rx_next;
    g.rx_next = (g.rx_next + 1) % N_RX;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    wr(REG_RDT, (UINT32)done);                       /* hand the slot back */
    return len;
}
