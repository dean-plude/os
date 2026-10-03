/*
 * e1000.c — Intel 8254x/8257x and I219 Gigabit Ethernet driver (e1000 / e1000e)
 *
 * Register layout and descriptor formats follow Intel's "PCI/PCI-X Family
 * of Gigabit Ethernet Controllers Software Developer's Manual" (8254x)
 * and the 82574 datasheet; only the legacy-descriptor subset is used,
 * which every family here supports.
 *
 * The I219 (Intel's "PCH LAN": the MAC is part of the chipset, the PHY a
 * separate chip reached over MDIO, or SMBus while the PHY sleeps) is set
 * up after Intel's shared e1000 code as FreeBSD ships it
 * (sys/dev/e1000/e1000_ich8lan.c, e1000_phy.c and if_em.c): the device
 * IDs, register and bit values, and the order of the PHY bring-up (Ultra
 * Low Power exit, the LANPHYPC power cycle, SMBus to PCIe mode, the reset
 * the MAC and PHY share, the per-generation errata and the link-up
 * settings) come from there.  That code is
 *
 *   Copyright (c) 2001-2020, Intel Corporation.  All rights reserved.
 *   SPDX-License-Identifier: BSD-3-Clause
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions are
 *   met: 1. Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 *   2. Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 *   3. Neither the name of the Intel Corporation nor the names of its
 *   contributors may be used to endorse or promote products derived from
 *   this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 *   IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 *   TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 *   PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 *   TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 *   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 *   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 *   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 *   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * QEMU has no I219 model: its e1000e (82574L) shares the rings, the MDIO
 * interface (MDIC) and the PHY's standard registers with it, and runs the
 * PHY code both use (PHY ID, auto-negotiation, link changes).  What only
 * the I219 has (the semaphore, ULP, LANPHYPC, PHY pages) logs what it did,
 * so a boot log from a real machine shows where a bring-up stopped.
 */

#include "e1000.h"
#include "../hal/pci.h"
#include "../mm/vmm.h"
#include "../lib/string.h"
#include "../ke/printf.h"
#include "../ke/scheduler.h"
#include "../arch/x86_64/cpu.h"
#include "../arch/x86_64/apic.h"

/* Registers */
#define REG_CTRL    0x0000
#define REG_STATUS  0x0008
#define REG_STRAP   0x000C           /* (PCH, read-only: written as a delay) */
#define REG_EERD    0x0014
#define REG_CTRL_EXT 0x0018
#define REG_MDIC    0x0020
#define REG_FEXTNVM4 0x0024
#define REG_FEXTNVM 0x0028
#define REG_FEXTNVM3 0x003C
#define REG_FEXTNVM7 0x00E4
#define REG_ICR     0x00C0
#define REG_IMC     0x00D8
#define REG_RCTL    0x0100
#define REG_TCTL    0x0400
#define REG_TIPG    0x0410
#define REG_EXTCNF_CTRL 0x0F00
#define REG_IOSFPC  0x0F28
#define REG_PBECCSTS 0x100C
#define REG_RDBAL   0x2800
#define REG_RDBAH   0x2804
#define REG_RDLEN   0x2808
#define REG_RDH     0x2810
#define REG_RDT     0x2818
#define REG_RXDCTL  0x2828
#define REG_KABGTXD 0x3004
#define REG_TDBAL   0x3800
#define REG_TDBAH   0x3804
#define REG_TDLEN   0x3808
#define REG_TDH     0x3810
#define REG_TDT     0x3818
#define REG_TXDCTL0 0x3828
#define REG_TARC0   0x3840
#define REG_TXDCTL1 0x3928
#define REG_TARC1   0x3940
#define REG_RFCTL   0x5008
#define REG_MTA     0x5200
#define REG_RAL0    0x5400
#define REG_RAH0    0x5404
#define REG_H2ME    0x5B50
#define REG_FWSM    0x5B54
#define REG_FEXTNVM11 0x5BBC
#define REG_FEXTNVM12 0x5BC0
#define REG_FFLT_DBG 0x5F04

#define CTRL_ASDE   (1u << 5)
#define CTRL_SLU    (1u << 6)
#define CTRL_FRCSPD (1u << 11)
#define CTRL_FRCDPLX (1u << 12)
#define CTRL_LANPHYPC_OVERRIDE (1u << 16)
#define CTRL_LANPHYPC_VALUE    (1u << 17)
#define CTRL_MEHE   (1u << 19)
#define CTRL_RST    (1u << 26)
#define CTRL_PHY_RST (1u << 31)
#define STATUS_FD   (1u << 0)
#define STATUS_LU   (1u << 1)
#define STATUS_SPEED_100  (1u << 6)
#define STATUS_SPEED_1000 (1u << 7)
#define STATUS_LAN_INIT_DONE (1u << 9)
#define STATUS_PHYRA (1u << 10)
#define CTRL_EXT_LPCD (1u << 2)
#define CTRL_EXT_DPG_EN (1u << 3)
#define CTRL_EXT_FORCE_SMBUS (1u << 11)
#define CTRL_EXT_RO_DIS (1u << 17)
#define CTRL_EXT_PHYPDEN (1u << 20)

#define MDIC_OP_WRITE (1u << 26)
#define MDIC_OP_READ  (2u << 26)
#define MDIC_READY    (1u << 28)
#define MDIC_ERROR    (1u << 30)

#define EXTCNF_SWFLAG (1u << 5)
#define FWSM_RSPCIPHY (1u << 6)          /* clear: the ME firmware blocks PHY resets */
#define FWSM_ULP_CFG_DONE (1u << 10)
#define FWSM_FW_VALID (1u << 15)         /* the ME firmware runs (vPro, AMT) */
#define H2ME_ULP      (1u << 11)
#define H2ME_ENFORCE_SETTINGS (1u << 12)
#define FEXTNVM_SW_CONFIG (1u << 27)
#define FEXTNVM3_PHY_CFG_COUNTER_MASK (3u << 26)
#define FEXTNVM3_PHY_CFG_COUNTER_50MS (2u << 26)
#define FEXTNVM7_DISABLE_SMB_PERST (1u << 5)
#define FEXTNVM11_DISABLE_MULR_FIX (1u << 13)
#define FEXTNVM12_PHYPD_CTRL_MASK (3u << 22)
#define FEXTNVM12_PHYPD_CTRL_P1   (2u << 22)
#define TXDCTL_FULL_TX_DESC_WB 0x01010000u      /* GRAN=1, WTHRESH=1 */
#define TXDCTL_MAX_TX_DESC_PREFETCH 0x0100001Fu /* GRAN=1, PTHRESH=31 */
#define TXDCTL_WTHRESH 0x003F0000u
#define TXDCTL_PTHRESH 0x0000003Fu
#define TARC0_CB_MULTIQ_3_REQ 0x30000000u
#define TARC0_CB_MULTIQ_2_REQ 0x20000000u
#define RCTL_RDMTS_HEX (1u << 16)
#define RFCTL_NFSW_DIS (1u << 6)
#define RFCTL_NFSR_DIS (1u << 7)
#define PBECCSTS_ECC_ENABLE (1u << 16)
#define KABGTXD_BGSQLBIAS 0x00050000u

#define PCI_DESC_RING_STATUS 0xE4        /* (I219 configuration space) */
#define FLUSH_DESC_REQUIRED  0x100

#define RCTL_EN     (1u << 1)
#define RCTL_BAM    (1u << 15)       /* accept broadcast */
#define RCTL_SECRC  (1u << 26)       /* strip Ethernet CRC */
                                     /* BSIZE = 00 → 2048-byte buffers */
#define TCTL_EN     (1u << 1)
#define TCTL_PSP    (1u << 3)
#define TCTL_CT     (0x0Fu << 4)
#define TCTL_COLD   (0x40u << 12)
#define TCTL_MULR   (1u << 28)

#define TXCMD_EOP   (1u << 0)
#define TXCMD_IFCS  (1u << 1)
#define TXCMD_RS    (1u << 3)
#define DESC_DD     (1u << 0)
#define RXS_EOP     (1u << 1)

/* PHY registers: the standard MII ones, and the I219's on its pages
 * (PHY_REG(page, register); pages from 768 answer at MDIO address 1,
 * page 0 at address 2) */
#define PHY_REG(page, reg) (((UINT32)(page) << 5) | ((reg) & 0x1F))
#define MII_BMCR    0
#define MII_BMSR    1
#define MII_PHYID1  2
#define MII_PHYID2  3
#define MII_ANAR    4
#define MII_1000T_CTRL 9
#define BMCR_ANENABLE  0x1000
#define BMCR_ANRESTART 0x0200
#define ANAR_10_100_ALL 0x01E1           /* 10/100, half and full duplex, IEEE 802.3 */
#define GTCR_1000_ALL  0x0300            /* 1000BASE-T, half and full duplex */
#define PHY_PAGE_SELECT 0x1F
#define BM_PORT_GEN_CFG PHY_REG(769, 17)
#define BM_WUC_HOST_WU_BIT (1u << 4)
#define CV_SMB_CTRL     PHY_REG(769, 23)
#define CV_SMB_CTRL_FORCE_SMBUS 0x0001
#define HV_PM_CTRL      PHY_REG(770, 17)
#define HV_PM_CTRL_K1_CLK_REQ 0x0200
#define HV_PM_CTRL_K1_ENABLE  0x4000
#define PHY_TIMEOUTS    PHY_REG(770, 21)
#define PHY_TIMEOUTS_K1_EXIT_MASK 0x0FC0
#define I217_PLL_CLOCK_GATE PHY_REG(772, 28)
#define I217_PLL_CLOCK_GATE_MASK 0x07FF
#define I219_PTR_GAP    PHY_REG(776, 20)
#define I218_ULP_CONFIG1 PHY_REG(779, 16)
#define ULP_CONFIG1_START 0x0001
#define ULP_CONFIG1_CLEAR 0x1D74         /* IND, STICKY_ULP, INBAND_EXIT, WOL_HOST, RESET_TO_SMBUS,
                                            EN_ULP_LANPHYPC, DIS_CLR_STICKY_ON_PERST, DISABLE_SMB_PERST */
#define EMI_ADDR        0x10             /* (page 0) */
#define EMI_DATA        0x11
#define I217_RX_CONFIG  0xB20C

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

/* The kinds of adapter, and the I219's chipset (PCH) generations, in the
 * order FreeBSD's e1000 code names them; errata apply from or up to one */
enum { FAM_8254X, FAM_82574, FAM_PCH };
enum { PCH_SPT = 1, PCH_CNP, PCH_TGP, PCH_ADP, PCH_MTP, PCH_PTP, PCH_NVP };
static const char *const pch_names[] = {
    "", "Sunrise Point", "Cannon Point", "Tiger Lake", "Alder Lake / Raptor Lake",
    "Meteor Lake / Lunar Lake", "Arrow Lake / Panther Lake", "Nova Lake",
};

/* Every I219 by PCI device ID: its PCH generation, the number in its name
 * ("Ethernet Connection (16) I219-LM") and LM (vPro) or V */
static const struct { UINT16 id; UINT8 pch, n, lm; } i219_ids[] = {
    { 0x156F, PCH_SPT, 1, 1 },  { 0x1570, PCH_SPT, 1, 0 },  { 0x15B7, PCH_SPT, 2, 1 },
    { 0x15B8, PCH_SPT, 2, 0 },  { 0x15B9, PCH_SPT, 3, 1 },  { 0x15D7, PCH_SPT, 4, 1 },
    { 0x15D8, PCH_SPT, 4, 0 },  { 0x15E3, PCH_SPT, 5, 1 },  { 0x15D6, PCH_SPT, 5, 0 },
    { 0x0D53, PCH_SPT, 12, 1 }, { 0x0D55, PCH_SPT, 12, 0 },
    { 0x15BD, PCH_CNP, 6, 1 },  { 0x15BE, PCH_CNP, 6, 0 },  { 0x15BB, PCH_CNP, 7, 1 },
    { 0x15BC, PCH_CNP, 7, 0 },  { 0x15DF, PCH_CNP, 8, 1 },  { 0x15E0, PCH_CNP, 8, 0 },
    { 0x15E1, PCH_CNP, 9, 1 },  { 0x15E2, PCH_CNP, 9, 0 },  { 0x0D4E, PCH_CNP, 10, 1 },
    { 0x0D4F, PCH_CNP, 10, 0 }, { 0x0D4C, PCH_CNP, 11, 1 }, { 0x0D4D, PCH_CNP, 11, 0 },
    { 0x15FB, PCH_TGP, 13, 1 }, { 0x15FC, PCH_TGP, 13, 0 }, { 0x15F9, PCH_TGP, 14, 1 },
    { 0x15FA, PCH_TGP, 14, 0 }, { 0x15F4, PCH_TGP, 15, 1 }, { 0x15F5, PCH_TGP, 15, 0 },
    { 0x1A1E, PCH_ADP, 16, 1 }, { 0x1A1F, PCH_ADP, 16, 0 }, { 0x1A1C, PCH_ADP, 17, 1 },
    { 0x1A1D, PCH_ADP, 17, 0 }, { 0x550C, PCH_ADP, 19, 1 }, { 0x550D, PCH_ADP, 19, 0 },
    { 0x0DC7, PCH_ADP, 22, 1 }, { 0x0DC8, PCH_ADP, 22, 0 }, { 0x0DC5, PCH_ADP, 23, 1 },
    { 0x0DC6, PCH_ADP, 23, 0 },
    { 0x550A, PCH_MTP, 18, 1 }, { 0x550B, PCH_MTP, 18, 0 }, { 0x550E, PCH_MTP, 20, 1 },
    { 0x550F, PCH_MTP, 20, 0 }, { 0x5510, PCH_MTP, 21, 1 }, { 0x5511, PCH_MTP, 21, 0 },
    { 0x57A0, PCH_PTP, 24, 1 }, { 0x57A1, PCH_PTP, 24, 0 }, { 0x57B3, PCH_PTP, 25, 1 },
    { 0x57B4, PCH_PTP, 25, 0 }, { 0x57B5, PCH_PTP, 26, 1 }, { 0x57B6, PCH_PTP, 26, 0 },
    { 0x57B7, PCH_PTP, 27, 1 }, { 0x57B8, PCH_PTP, 27, 0 },
    { 0x57B9, PCH_NVP, 29, 1 }, { 0x57BA, PCH_NVP, 29, 0 },
};

static struct {
    bool              present;
    const char       *name;
    char              namebuf[64];
    volatile UINT8   *mmio;
    PciDevice         pci;
    RxDesc           *rx;             /* rings (physmap virtual addresses) */
    TxDesc           *tx;
    UINT8            *rxbuf, *txbuf;
    int               rx_next, tx_next;
    UINT8             mac[6];
    int               fam;            /* FAM_* */
    int               pch;            /* PCH_* (FAM_PCH) */
    UINT32            phy_id;         /* PHY ID registers 2 and 3 (0: none answered) */
    bool              me;             /* the ME firmware runs (FWSM.FW_VALID) */
    bool              link;           /* link as last reported */
} g;

static inline UINT32 rd(UINT32 reg)          { return *(volatile UINT32 *)(g.mmio + reg); }
static inline void   wr(UINT32 reg, UINT32 v){ *(volatile UINT32 *)(g.mmio + reg) = v; }
static inline void   flush(void)             { (void)rd(REG_STATUS); }
static inline UINT64 phys(const void *va)    { return (UINT64)(uintptr_t)va - PHYSMAP_BASE; }

static void delay_ms(int ms)
{
    UINT64 end = sched_ticks() + (UINT64)(ms + 9) / 10;
    if (!(read_rflags() & 0x200)) {                 /* interrupts off: the TSC */
        udelay((UINT64)ms * 1000);
        return;
    }
    while (sched_ticks() < end) pause_cpu();
}

static UINT16 eeprom_read(UINT8 word)
{
    bool e82574 = g.fam == FAM_82574;
    UINT32 start = e82574 ? ((UINT32)word << 2) | 1 : ((UINT32)word << 8) | 1;
    UINT32 done  = e82574 ? (1u << 1) : (1u << 4);
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
    if (lo || (hi & 0xFFFF) || g.fam == FAM_PCH) {   /* (the I219 loads it from its NVM at reset) */
        for (int i = 0; i < 4; i++) g.mac[i] = (UINT8)(lo >> (i * 8));
        g.mac[4] = (UINT8)hi; g.mac[5] = (UINT8)(hi >> 8);
        return;
    }
    for (int i = 0; i < 3; i++) {               /* fall back to the EEPROM */
        UINT16 w = eeprom_read((UINT8)i);
        g.mac[i * 2] = (UINT8)w; g.mac[i * 2 + 1] = (UINT8)(w >> 8);
    }
}

/* ---- the PHY over MDIO (MDIC) --------------------------------------------
 * One transaction: @addr is the PHY's MDIO address, @reg 0-31 */
static bool mdic(UINT8 addr, UINT8 reg, bool write, UINT16 *val)
{
    UINT32 v = (UINT32)reg << 16 | (UINT32)addr << 21 | (write ? MDIC_OP_WRITE | *val : MDIC_OP_READ);
    wr(REG_MDIC, v);
    for (int i = 0; i < 2000; i++) {           /* up to 100 ms */
        v = rd(REG_MDIC);
        if (v & MDIC_READY) break;
        udelay(50);
    }
    if (!(v & MDIC_READY) || (v & MDIC_ERROR)) return false;
    if (!write) {
        if (((v >> 16) & 0x1F) != reg) return false;
        *val = (UINT16)v;
    }
    return true;
}

/* A PHY register: @r is a register number (0-31) or, on the I219, a
 * PHY_REG(page, register).  The 82574's PHY is at address 1; the I219's
 * answers page 0 at address 2 and pages 768 and up at address 1, with
 * the page chosen through register 31 for registers above 15 */
static bool phy_rw(UINT32 r, bool write, UINT16 *val)
{
    if (g.fam != FAM_PCH) return mdic(1, (UINT8)(r & 0x1F), write, val);
    UINT32 page = r >> 5, reg = r & 0x1F;
    if (page > 0 && page < 768) return false;      /* (debug pages: not used) */
    if (reg > 15) {
        UINT16 sel = (UINT16)((page == 768 ? 0 : page) << 5);
        if (!mdic(1, PHY_PAGE_SELECT, true, &sel)) return false;
    }
    return mdic(page >= 768 ? 1 : 2, (UINT8)reg, write, val);
}
static bool phy_read(UINT32 r, UINT16 *val) { return phy_rw(r, false, val); }
static bool phy_write(UINT32 r, UINT16 val) { return phy_rw(r, true, &val); }

static UINT32 read_phy_id(void)
{
    for (int i = 0; i < 2; i++) {
        UINT16 a, b;
        if (phy_read(MII_PHYID1, &a) && a != 0xFFFF && phy_read(MII_PHYID2, &b) && b != 0xFFFF)
            return (UINT32)a << 16 | b;
    }
    return 0;
}

/* Advertise every speed and (re)start auto-negotiation */
static void phy_autoneg(void)
{
    UINT16 bmcr;
    if (!phy_write(MII_ANAR, ANAR_10_100_ALL) || !phy_write(MII_1000T_CTRL, GTCR_1000_ALL) ||
        !phy_read(MII_BMCR, &bmcr) || !phy_write(MII_BMCR, (UINT16)(bmcr | BMCR_ANENABLE | BMCR_ANRESTART)))
        kprintf("[E1000] Could not start auto-negotiation on the PHY\n");
}

/* ---- the I219 ------------------------------------------------------------ */

/* The software flag (EXTCNF_CTRL.SWFLAG) the driver, the hardware and the
 * ME firmware take before touching the PHY */
static bool swflag_take(void)
{
    int i;
    for (i = 0; i < 100 && (rd(REG_EXTCNF_CTRL) & EXTCNF_SWFLAG); i++) udelay(1000);
    if (i == 100) { kprintf("[E1000] PHY semaphore held by another owner\n"); return false; }
    wr(REG_EXTCNF_CTRL, rd(REG_EXTCNF_CTRL) | EXTCNF_SWFLAG);
    for (i = 0; i < 1000 && !(rd(REG_EXTCNF_CTRL) & EXTCNF_SWFLAG); i++) udelay(1000);
    if (i == 1000) {
        kprintf("[E1000] PHY semaphore not granted (FWSM %08x)\n", rd(REG_FWSM));
        wr(REG_EXTCNF_CTRL, rd(REG_EXTCNF_CTRL) & ~EXTCNF_SWFLAG);
        return false;
    }
    return true;
}

static void swflag_give(void)
{
    UINT32 v = rd(REG_EXTCNF_CTRL);
    if (v & EXTCNF_SWFLAG) wr(REG_EXTCNF_CTRL, v & ~EXTCNF_SWFLAG);
}

/* The ME firmware can forbid resetting the PHY (it shares it, for AMT) */
static bool phy_reset_blocked(void)
{
    for (int i = 0; i < 30; i++) {
        if (rd(REG_FWSM) & FWSM_RSPCIPHY) return false;
        delay_ms(10);
    }
    return true;
}

/* Power-cycle the PHY by toggling the LANPHYPC pin: it comes back out of
 * SMBus mode and ULP */
static void toggle_lanphypc(void)
{
    wr(REG_FEXTNVM3, (rd(REG_FEXTNVM3) & ~FEXTNVM3_PHY_CFG_COUNTER_MASK) | FEXTNVM3_PHY_CFG_COUNTER_50MS);
    UINT32 c = rd(REG_CTRL);
    c = (c | CTRL_LANPHYPC_OVERRIDE) & ~CTRL_LANPHYPC_VALUE;
    wr(REG_CTRL, c);
    flush();
    udelay(1000);
    wr(REG_CTRL, c & ~CTRL_LANPHYPC_OVERRIDE);
    flush();
    for (int i = 0; i < 20 && !(rd(REG_CTRL_EXT) & CTRL_EXT_LPCD); i++) delay_ms(5);
    delay_ms(30);
}

static void force_smbus(bool on)
{
    UINT32 v = rd(REG_CTRL_EXT);
    wr(REG_CTRL_EXT, on ? v | CTRL_EXT_FORCE_SMBUS : v & ~CTRL_EXT_FORCE_SMBUS);
}

/* The PHY answers on MDIO; then (without ME firmware) take the PHY and the
 * MAC out of SMBus mode.  Holds the software flag */
static bool phy_accessible(void)
{
    UINT32 id = read_phy_id();
    if (!id) return false;
    g.phy_id = id;
    if (!g.me) {
        UINT16 v;
        if (phy_read(CV_SMB_CTRL, &v)) phy_write(CV_SMB_CTRL, (UINT16)(v & ~CV_SMB_CTRL_FORCE_SMBUS));
        force_smbus(false);
    }
    return true;
}

/* Leave Ultra Low Power mode, which Windows' driver or the firmware may
 * have left the PHY in (it then answers on SMBus only) */
static void ulp_disable(void)
{
    if (g.me) {                                     /* the ME firmware does it, asked */
        wr(REG_H2ME, (rd(REG_H2ME) & ~H2ME_ULP) | H2ME_ENFORCE_SETTINGS);
        int i = 0;
        while ((rd(REG_FWSM) & FWSM_ULP_CFG_DONE) && i++ < 250) delay_ms(10);
        if (i > 250) kprintf("[E1000] ME firmware did not take the PHY out of ULP\n");
        wr(REG_H2ME, rd(REG_H2ME) & ~H2ME_ENFORCE_SETTINGS);
        return;
    }
    if (!swflag_take()) return;
    toggle_lanphypc();
    UINT16 v;
    if (!phy_read(CV_SMB_CTRL, &v)) {               /* the MAC may be in PCIe mode: SMBus for now */
        force_smbus(true);
        delay_ms(50);
        if (!phy_read(CV_SMB_CTRL, &v)) {
            kprintf("[E1000] PHY not answering while leaving ULP\n");
            swflag_give();
            return;
        }
    }
    phy_write(CV_SMB_CTRL, (UINT16)(v & ~CV_SMB_CTRL_FORCE_SMBUS));
    force_smbus(false);
    if (phy_read(HV_PM_CTRL, &v)) phy_write(HV_PM_CTRL, (UINT16)(v | HV_PM_CTRL_K1_ENABLE));   /* ULP turned K1 off */
    if (phy_read(I218_ULP_CONFIG1, &v)) {
        v &= (UINT16)~ULP_CONFIG1_CLEAR;
        phy_write(I218_ULP_CONFIG1, v);
        phy_write(I218_ULP_CONFIG1, (UINT16)(v | ULP_CONFIG1_START));   /* commit */
    }
    wr(REG_FEXTNVM7, rd(REG_FEXTNVM7) & ~FEXTNVM7_DISABLE_SMB_PERST);
    swflag_give();
}

/* Meteor Lake and later: K1 power-down state P1 and a longer K1 exit
 * timeout, against a MAC/PHY clock problem.  Holds the software flag */
static void k1_exit_timeout(void)
{
    if (g.pch < PCH_MTP) return;
    wr(REG_FEXTNVM12, (rd(REG_FEXTNVM12) & ~FEXTNVM12_PHYPD_CTRL_MASK) | FEXTNVM12_PHYPD_CTRL_P1);
    udelay(1000);
    UINT16 v;
    if (phy_read(PHY_TIMEOUTS, &v)) phy_write(PHY_TIMEOUTS, (UINT16)((v & ~PHY_TIMEOUTS_K1_EXIT_MASK) | 0xF00));
}

/* Get the PHY talking over MDIO (FreeBSD: e1000_init_phy_workarounds_pchlan) */
static void pch_phy_bringup(void)
{
    g.me = rd(REG_FWSM) & FWSM_FW_VALID;
    ulp_disable();
    if (!swflag_take()) return;
    k1_exit_timeout();                  /* (may fail with the PHY away; done again below) */
    const char *how = "answering";
    if (!phy_accessible()) {
        force_smbus(true);              /* first try it with the MAC in SMBus mode */
        delay_ms(50);
        how = "answering in SMBus mode";
        if (!phy_accessible()) {
            if (phy_reset_blocked()) how = "not answering (the ME firmware blocks its power cycle)";
            else {
                toggle_lanphypc();
                how = "answering after a LANPHYPC power cycle";
                if (!phy_accessible()) {
                    force_smbus(false);
                    if (!phy_accessible()) how = "not answering after a LANPHYPC power cycle";
                }
            }
        }
    }
    swflag_give();
    kprintf("[E1000] I219: ME firmware %s, PHY %s\n", g.me ? "present" : "absent", how);
}

/* Empty the descriptor rings before a reset if the I219 says they must be
 * (else it hangs until the next PCI reset; FreeBSD em_flush_desc_rings).
 * Only rings we set up are flushed */
static void pch_flush_rings(void)
{
    wr(REG_FEXTNVM11, rd(REG_FEXTNVM11) | FEXTNVM11_DISABLE_MULR_FIX);
    PciDevice *d = &g.pci;
    if (!(PciRead16(d->bus, d->dev, d->func, PCI_DESC_RING_STATUS) & FLUSH_DESC_REQUIRED) || !rd(REG_TDLEN))
        return;
    if (rd(REG_TDBAL) != (UINT32)phys(g.tx)) {
        kprintf("[E1000] Descriptor rings need flushing, but they are not ours\n");
        return;
    }
    wr(REG_TCTL, rd(REG_TCTL) | TCTL_EN);          /* one dummy 512-byte frame from the ring itself */
    TxDesc *t = &g.tx[g.tx_next];
    t->addr = phys(g.tx);
    t->length = 512;
    t->cmd = TXCMD_IFCS;
    t->status = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    wr(REG_TDT, (UINT32)((g.tx_next + 1) % N_TX));
    udelay(250);
    if (!(PciRead16(d->bus, d->dev, d->func, PCI_DESC_RING_STATUS) & FLUSH_DESC_REQUIRED)) return;
    UINT32 rctl = rd(REG_RCTL);
    wr(REG_RCTL, rctl & ~RCTL_EN);
    flush();
    udelay(150);
    wr(REG_RXDCTL, (rd(REG_RXDCTL) & 0xFFFFC000u) | 0x1F | (1u << 8) | (1u << 24));
    wr(REG_RCTL, rctl | RCTL_EN);
    flush();
    udelay(150);
    wr(REG_RCTL, rctl & ~RCTL_EN);
}

/* The reset the MAC and the PHY share (FreeBSD e1000_reset_hw_ich8lan) */
static void pch_reset(void)
{
    if (g.pch >= PCH_SPT) pch_flush_rings();
    wr(REG_IMC, 0xFFFFFFFFu);
    wr(REG_RCTL, 0);
    wr(REG_TCTL, TCTL_PSP);
    flush();
    delay_ms(10);

    UINT32 ctrl = rd(REG_CTRL);
    if (!phy_reset_blocked()) ctrl |= CTRL_PHY_RST;
    bool flag = swflag_take();
    PciDevice *d = &g.pci;
    wr(REG_STRAP, PciRead16(d->bus, d->dev, d->func, 0));  /* a configuration read first: a delay the reset needs */
    wr(REG_CTRL, ctrl | CTRL_RST);
    delay_ms(20);                                    /* (no flush: reading now hangs the chip) */
    wr(REG_STRAP, PciRead16(d->bus, d->dev, d->func, 0));
    if (flag) swflag_give();                         /* (the reset normally clears it) */

    if (ctrl & CTRL_PHY_RST) {
        delay_ms(10);
        int i;
        for (i = 0; i < 1500 && !(rd(REG_STATUS) & STATUS_LAN_INIT_DONE); i++) udelay(100);
        if (i == 1500) kprintf("[E1000] LAN init not done after the reset\n");
        UINT32 st = rd(REG_STATUS) & ~STATUS_LAN_INIT_DONE;
        wr(REG_STATUS, st & ~STATUS_PHYRA);
        if (!phy_reset_blocked()) {                  /* after the PHY's reset */
            delay_ms(10);
            if (swflag_take()) {
                UINT16 v;
                if (phy_read(BM_PORT_GEN_CFG, &v)) phy_write(BM_PORT_GEN_CFG, (UINT16)(v & ~BM_WUC_HOST_WU_BIT));
                swflag_give();
            }
            if (rd(REG_FEXTNVM) & FEXTNVM_SW_CONFIG)
                kprintf("[E1000] The NVM asks the driver to configure the PHY (not done)\n");
        }
    }
    wr(REG_IMC, 0xFFFFFFFFu);
    (void)rd(REG_ICR);
    wr(REG_KABGTXD, rd(REG_KABGTXD) | KABGTXD_BGSQLBIAS);
    if (g.pch >= PCH_PTP) wr(REG_CTRL_EXT, rd(REG_CTRL_EXT) & ~CTRL_EXT_DPG_EN);
}

/* The bits the I219 needs set before it runs (FreeBSD
 * e1000_initialize_hw_bits_ich8lan and e1000_init_hw_ich8lan) */
static void pch_init_bits(void)
{
    wr(REG_CTRL_EXT, rd(REG_CTRL_EXT) | (1u << 22) | CTRL_EXT_PHYPDEN);
    wr(REG_TXDCTL0, rd(REG_TXDCTL0) | (1u << 22));
    wr(REG_TXDCTL1, rd(REG_TXDCTL1) | (1u << 22));
    wr(REG_TARC0, rd(REG_TARC0) | (1u << 23) | (1u << 24) | (1u << 26) | (1u << 27));
    wr(REG_TARC1, (rd(REG_TARC1) & ~(1u << 28)) | (1u << 24) | (1u << 26) | (1u << 30));   /* (TCTL.MULR on) */
    wr(REG_RFCTL, rd(REG_RFCTL) | RFCTL_NFSW_DIS | RFCTL_NFSR_DIS);
    wr(REG_PBECCSTS, rd(REG_PBECCSTS) | PBECCSTS_ECC_ENABLE);
    wr(REG_CTRL, rd(REG_CTRL) | CTRL_MEHE);
    if (g.pch >= PCH_MTP && swflag_take()) { k1_exit_timeout(); swflag_give(); }
    for (int q = 0; q < 2; q++) {
        UINT32 r = q ? REG_TXDCTL1 : REG_TXDCTL0, v = rd(r);
        v = (v & ~TXDCTL_WTHRESH) | TXDCTL_FULL_TX_DESC_WB;
        v = (v & ~TXDCTL_PTHRESH) | TXDCTL_MAX_TX_DESC_PREFETCH;
        wr(r, v);
    }
    if (g.pch >= PCH_TGP) wr(REG_FFLT_DBG, rd(REG_FFLT_DBG) | (1u << 12));   /* DMA clock on: no lost frames */
    wr(REG_CTRL_EXT, rd(REG_CTRL_EXT) | CTRL_EXT_RO_DIS);
}

static void emi_write(UINT16 addr, UINT16 val)
{
    if (phy_write(EMI_ADDR, addr)) phy_write(EMI_DATA, val);
}

/* Link came up at @speed: the settings the I219 needs for it (FreeBSD
 * e1000_check_for_copper_link_ich8lan) */
static void pch_link_up(int speed, bool full)
{
    UINT32 tipg = rd(REG_TIPG) & ~0x3FFu;
    UINT16 emi = 1;
    if (!full && speed == 10) { tipg |= 0xFF; emi = 0; }
    else if (full && speed != 1000) tipg |= 0x0C;
    else tipg |= 0x08;
    wr(REG_TIPG, tipg);
    if (!swflag_take()) return;
    emi_write(I217_RX_CONFIG, emi);
    UINT16 v;
    if (phy_read(I217_PLL_CLOCK_GATE, &v))
        phy_write(I217_PLL_CLOCK_GATE, (UINT16)((v & ~I217_PLL_CLOCK_GATE_MASK) |
                  (speed != 1000 ? 0x3E8 : g.pch >= PCH_MTP ? 0x1D5 : 0xFA)));
    if (speed == 1000 && phy_read(HV_PM_CTRL, &v)) phy_write(HV_PM_CTRL, (UINT16)(v | HV_PM_CTRL_K1_CLK_REQ));
    if (speed == 1000) {
        if (phy_read(I219_PTR_GAP, &v) && ((v >> 2) & 0x3FF) < 0x18)
            phy_write(I219_PTR_GAP, (UINT16)((v & ~(0x3FFu << 2)) | (0x18u << 2)));
    } else {
        phy_write(I219_PTR_GAP, 0xC023);
    }
    swflag_give();
    wr(REG_FEXTNVM4, rd(REG_FEXTNVM4) | 0x7);        /* beacon duration 8 µs (I217 packet loss) */
}

/* ---- set-up ---------------------------------------------------------------- */

/* Reset the adapter and give it the rings: at start-up, and after S3
 * (@resume: the MAC address is known, write it back) */
static void hw_setup(bool resume)
{
    if (g.fam == FAM_PCH) {
        pch_phy_bringup();
        pch_reset();
        pch_init_bits();
    } else {
        /* Reset, then mask every interrupt source (we poll) */
        wr(REG_IMC, 0xFFFFFFFFu);
        wr(REG_CTRL, rd(REG_CTRL) | CTRL_RST);
        delay_ms(10);
        for (int i = 0; i < 100000 && (rd(REG_CTRL) & CTRL_RST); i++) pause_cpu();
        wr(REG_IMC, 0xFFFFFFFFu);
        (void)rd(REG_ICR);
    }

    UINT32 ctrl = rd(REG_CTRL) | CTRL_SLU | CTRL_ASDE;
    if (g.fam != FAM_8254X) ctrl &= ~(CTRL_FRCSPD | CTRL_FRCDPLX);   /* speed and duplex from the PHY */
    wr(REG_CTRL, ctrl);
    if (resume) {
        wr(REG_RAL0, (UINT32)g.mac[0] | (UINT32)g.mac[1] << 8 | (UINT32)g.mac[2] << 16 | (UINT32)g.mac[3] << 24);
        wr(REG_RAH0, (UINT32)g.mac[4] | (UINT32)g.mac[5] << 8 | (1u << 31));   /* address valid */
    } else {
        read_mac();
    }
    for (int i = 0; i < 128; i++) wr(REG_MTA + i * 4, 0);

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
    wr(REG_TIPG, g.fam == FAM_PCH ? 0x00602008 : 0x0060200A);
    wr(REG_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT | TCTL_COLD | (g.fam == FAM_PCH ? TCTL_MULR : 0));
    if (g.pch == PCH_SPT) {                          /* Sunrise Point errata (I218/I219 spec update 1.5.4.5) */
        wr(REG_IOSFPC, rd(REG_IOSFPC) | RCTL_RDMTS_HEX);
        wr(REG_TARC0, (rd(REG_TARC0) & ~TARC0_CB_MULTIQ_3_REQ) | TARC0_CB_MULTIQ_2_REQ);
    }
    g.rx_next = 0;
    g.tx_next = 0;

    /* The PHY: who it is, and auto-negotiation at every speed */
    if (g.fam != FAM_8254X) {
        bool flag = g.fam != FAM_PCH || swflag_take();
        if (flag) {
            g.phy_id = read_phy_id();
            if (g.phy_id) phy_autoneg();
            if (g.fam == FAM_PCH) swflag_give();
        }
    }
    g.link = false;
}

/* Put the function in D0 (firmware may leave the I219 in D3hot) */
static void pci_power_on(const PciDevice *d)
{
    if (!(PciRead16(d->bus, d->dev, d->func, 0x06) & (1u << 4))) return;    /* no capability list */
    UINT8 cap = (UINT8)(PciRead32(d->bus, d->dev, d->func, 0x34) & 0xFC);
    for (int n = 0; cap && n < 48; n++) {
        UINT32 hdr = PciRead32(d->bus, d->dev, d->func, cap);
        if ((hdr & 0xFF) == 0x01) {                  /* power management */
            UINT16 pmcsr = PciRead16(d->bus, d->dev, d->func, (UINT8)(cap + 4));
            if (pmcsr & 3) {
                PciWrite16(d->bus, d->dev, d->func, (UINT8)(cap + 4), (UINT16)(pmcsr & ~3u));
                delay_ms(10);
                kprintf("[E1000] Woke the adapter from D%u\n", pmcsr & 3);
            }
            return;
        }
        cap = (UINT8)((hdr >> 8) & 0xFC);
    }
}

bool E1000Init(void)
{
    static const UINT16 e1000_ids[]  = { 0x100E, 0x100F, 0x1004, 0x100C, 0x1015 };
    static const UINT16 e1000e_ids[] = { 0x10D3 };
    PciDevice d;
    bool found = false;
    for (UINT32 i = 0; i < sizeof(i219_ids) / sizeof(i219_ids[0]) && !found; i++) {
        if (!PciFind(0x8086, &i219_ids[i].id, 1, &d)) continue;
        found = true;
        g.fam = FAM_PCH;
        g.pch = i219_ids[i].pch;
        ksnprintf(g.namebuf, sizeof(g.namebuf), "Intel Ethernet Connection (%u) I219-%s",
                  i219_ids[i].n, i219_ids[i].lm ? "LM" : "V");
        g.name = g.namebuf;
    }
    if (!found && PciFind(0x8086, e1000e_ids, 1, &d)) {
        g.name = "Intel 82574L Gigabit Network Connection (e1000e)";
        g.fam = FAM_82574;
    } else if (!found && PciFind(0x8086, e1000_ids, 5, &d)) {
        g.name = "Intel PRO/1000 MT Network Connection (e1000)";
        g.fam = FAM_8254X;
    } else if (!found) {
        kprintf("[E1000] No supported Intel NIC found\n");
        return false;
    }
    g.pci = d;

    pci_power_on(&d);
    PciEnableDevice(&d);
    g.mmio = (volatile UINT8 *)PciMapBar(&d, 0);
    if (!g.mmio) { kprintf("[E1000] BAR0 is not a memory BAR\n"); return false; }

    /* Descriptor rings and buffers (physically contiguous, via the physmap) */
    g.rx    = kernel_alloc_pages(1);
    g.tx    = kernel_alloc_pages(1);
    g.rxbuf = kernel_alloc_pages(N_RX * BUF_SZ / PAGE_SIZE);
    g.txbuf = kernel_alloc_pages(N_TX * BUF_SZ / PAGE_SIZE);
    if (!g.rx || !g.tx || !g.rxbuf || !g.txbuf) {
        kprintf("[E1000] Out of memory for rings\n");
        return false;
    }
    memset(g.tx, 0, PAGE_SIZE);
    hw_setup(false);

    g.present = true;
    PciClaim(&d, g.fam == FAM_8254X ? "e1000" : "e1000e");
    kprintf("[E1000] %s at %02x:%02x.%x, MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            g.name, d.bus, d.dev, d.func, g.mac[0], g.mac[1], g.mac[2], g.mac[3], g.mac[4], g.mac[5]);
    if (g.fam == FAM_PCH) kprintf("[E1000] Device %04x, chipset %s\n", d.device, pch_names[g.pch]);
    if (g.fam != FAM_8254X) {
        if (g.phy_id) kprintf("[E1000] PHY %04x:%04x, auto-negotiating\n", g.phy_id >> 16, g.phy_id & 0xFFFF);
        else kprintf("[E1000] PHY not answering on MDIO\n");
    }
    E1000LinkUp();
    return true;
}

/* After S3: the adapter was reset; set it up again with the same rings
 * (frames in flight are lost, as on a cable pulled and plugged back) */
void E1000Resume(void)
{
    if (!g.present) return;
    g.present = false;                   /* the network thread leaves it alone meanwhile */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    hw_setup(true);
    g.present = true;
}

bool        E1000Present(void) { return g.present; }
const char *E1000Name(void)    { return g.present ? g.name : "No network adapter"; }
void        E1000Mac(UINT8 mac[6]) { memcpy(mac, g.mac, 6); }

/* Polled by the network thread: logs changes, and on the I219 applies the
 * settings for the speed it came up at */
bool E1000LinkUp(void)
{
    if (!g.present) return false;
    UINT32 st = rd(REG_STATUS);
    bool up = st & STATUS_LU;
    if (up != g.link) {
        g.link = up;
        if (up) {
            int speed = (st & STATUS_SPEED_1000) ? 1000 : (st & STATUS_SPEED_100) ? 100 : 10;
            bool full = st & STATUS_FD;
            if (g.fam == FAM_PCH) pch_link_up(speed, full);
            kprintf("[E1000] Link up at %d Mb/s, %s duplex\n", speed, full ? "full" : "half");
        } else {
            kprintf("[E1000] Link down\n");
        }
    }
    return up;
}

bool E1000Transmit(const void *frame, UINT16 len)
{
    if (!g.present || len > BUF_SZ) return false;
    TxDesc *d = &g.tx[g.tx_next];
    if (!(d->status & DESC_DD)) return false;        /* ring full */
    memcpy(g.txbuf + g.tx_next * BUF_SZ, frame, len);
    d->addr   = phys(g.txbuf + g.tx_next * BUF_SZ);
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
