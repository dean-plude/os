/*
 * usbhub.c — USB hubs (USB 2.0 chapter 11, USB 3.2 chapter 10)
 *
 * A hub reports which of its ports changed on its status-change interrupt
 * endpoint (a bitmap: bit 0 the hub itself, bit n port n).  The callback
 * only records the bits; the usb thread then reads each port's status,
 * clears its change bits and, for a connection, resets the port and has
 * the core enumerate the device behind it.
 */

#include "usb.h"
#include "../mm/vmm.h"
#include "../lib/string.h"
#include "../ke/printf.h"
#include "../ke/scheduler.h"
#include "../arch/x86_64/cpu.h"
#include "../arch/x86_64/apic.h"

/* Hub class requests and port features */
#define REQ_GET_STATUS      0
#define REQ_CLEAR_FEATURE   1
#define REQ_SET_FEATURE     3
#define REQ_GET_DESCRIPTOR  6
#define REQ_SET_HUB_DEPTH   12

#define PORT_CONNECTION     0
#define PORT_ENABLE         1
#define PORT_RESET          4
#define PORT_POWER          8
#define C_PORT_CONNECTION   16
#define C_PORT_ENABLE       17
#define C_PORT_SUSPEND      18
#define C_PORT_OVER_CURRENT 19
#define C_PORT_RESET        20
#define C_PORT_LINK_STATE   25
#define C_PORT_CONFIG_ERROR 26
#define C_BH_PORT_RESET     29

/* wPortStatus */
#define PS_CONNECTION       (1u << 0)
#define PS_ENABLE           (1u << 1)
#define PS_LOW_SPEED        (1u << 9)
#define PS_HIGH_SPEED       (1u << 10)
/* wPortChange */
#define PC_CONNECTION       (1u << 0)
#define PC_RESET            (1u << 4)

#define MAX_HUBS            16
#define MAX_HUB_PORTS       15

typedef struct {
    UsbDev        *dev;
    UsbPipe       *status;
    UINT8          ports;
    bool           super;            /* a USB 3 hub */
    UINT16         power_ms;         /* power-on to power-good */
    volatile UINT32 change;          /* bitmap from the status endpoint */
    bool           present[MAX_HUB_PORTS + 1];
    bool           dead;
} Hub;

static Hub *g_hubs[MAX_HUBS];

static void hub_delay(int ms)
{
    if (interrupts_enabled() && sched_current())
        sched_sleep_until(NULL, sched_ticks() + (UINT64)(ms + 9) / 10 + 1);
    else
        udelay((UINT64)ms * 1000);
}

static bool on_status(UsbPipe *p, const UINT8 *data, int len, void *ctx)
{
    Hub *h = ctx;
    if (h->dead) return false;
    UINT32 bits = 0;
    for (int i = 0; i < len && i < 4; i++) bits |= (UINT32)data[i] << (8 * i);
    __atomic_or_fetch(&h->change, bits, __ATOMIC_ACQ_REL);
    UsbHubSignal(h);
    return true;
}

static bool port_status(Hub *h, int port, UINT16 *status, UINT16 *change)
{
    UINT8 st[4];
    if (UsbControl(h->dev, 0xA3, REQ_GET_STATUS, 0, (UINT16)port, 4, st) < 4) return false;
    *status = (UINT16)(st[0] | st[1] << 8);
    *change = (UINT16)(st[2] | st[3] << 8);
    return true;
}

static void clear_feature(Hub *h, int port, UINT16 feature)
{
    UsbControl(h->dev, 0x23, REQ_CLEAR_FEATURE, feature, (UINT16)port, 0, NULL);
}

static void clear_changes(Hub *h, int port, UINT16 change)
{
    static const UINT16 usb2[] = { C_PORT_CONNECTION, C_PORT_ENABLE, C_PORT_SUSPEND, C_PORT_OVER_CURRENT, C_PORT_RESET };
    /* USB 3 change bits: connection 0, over-current 3, reset 4, BH reset 5, link state 6, config error 7 */
    static const UINT8  bit3[]  = { 0, 3, 4, 5, 6, 7 };
    static const UINT16 usb3[]  = { C_PORT_CONNECTION, C_PORT_OVER_CURRENT, C_PORT_RESET, C_BH_PORT_RESET,
                                    C_PORT_LINK_STATE, C_PORT_CONFIG_ERROR };
    if (h->super) {
        for (int i = 0; i < 6; i++) if (change & (1u << bit3[i])) clear_feature(h, port, usb3[i]);
    } else {
        for (int i = 0; i < 5; i++) if (change & (1u << i)) clear_feature(h, port, usb2[i]);
    }
}

/* Reset @port and return the speed of the device behind it, 0 if it failed */
static UINT8 reset_port(Hub *h, int port)
{
    UsbControl(h->dev, 0x23, REQ_SET_FEATURE, PORT_RESET, (UINT16)port, 0, NULL);
    UINT16 st = 0, ch = 0;
    for (int tries = 0; tries < 50; tries++) {
        hub_delay(10);
        if (!port_status(h, port, &st, &ch)) return 0;
        if (ch & PC_RESET) break;
    }
    clear_changes(h, port, ch);
    if (!(st & PS_CONNECTION) || !(st & PS_ENABLE)) return 0;
    hub_delay(10);                                    /* reset recovery */
    if (h->super) return USB_SPEED_SUPER;
    if (st & PS_LOW_SPEED) return USB_SPEED_LOW;
    if (st & PS_HIGH_SPEED) return USB_SPEED_HIGH;
    return USB_SPEED_FULL;
}

static void service_port(Hub *h, int port)
{
    UINT16 st, ch;
    if (!port_status(h, port, &st, &ch)) return;
    clear_changes(h, port, ch);
    bool connected = (st & PS_CONNECTION) != 0;
    if (h->present[port] && (!connected || (ch & PC_CONNECTION))) {
        UsbDetachChild(h->dev, (UINT8)port);
        h->present[port] = false;
    }
    if (connected && !h->present[port]) {
        hub_delay(100);                               /* debounce */
        UINT8 speed = reset_port(h, port);
        if (!speed) {
            kprintf("[USB] %s: port %d did not come up after reset\n", UsbDevName(h->dev), port);
            return;
        }
        h->present[port] = true;                      /* (even if enumeration fails: no retry loop) */
        UsbAttachChild(h->dev, (UINT8)port, speed);
    }
}

void UsbHubService(void *inst)
{
    Hub *h = inst;
    if (h->dead) return;
    UINT32 bits = __atomic_exchange_n(&h->change, 0, __ATOMIC_ACQ_REL);
    for (int port = 1; port <= h->ports; port++)
        if (bits & (1u << port)) service_port(h, port);
}

void UsbHubServiceAll(void)
{
    for (int i = 0; i < MAX_HUBS; i++)
        if (g_hubs[i] && !g_hubs[i]->dead && g_hubs[i]->change) UsbHubService(g_hubs[i]);
}

static void hub_gone(void *inst)
{
    Hub *h = inst;
    h->dead = true;
    for (int i = 0; i < MAX_HUBS; i++)
        if (g_hubs[i] == h) g_hubs[i] = NULL;
    /* (the core removes the devices behind it; the Hub is left allocated:
     * a status callback may still be on its way) */
}

void *UsbHubProbe(UsbDev *d, const UsbIface *f)
{
    int slot = -1;
    for (int i = 0; i < MAX_HUBS && slot < 0; i++) if (!g_hubs[i]) slot = i;
    if (slot < 0) return NULL;
    int off = 0;
    const UINT8 *ep = UsbIfaceFind(f, USB_DT_ENDPOINT, &off);
    if (!ep || !(ep[2] & 0x80) || (ep[3] & 3) != 3) return NULL;

    Hub *h = kzalloc(sizeof(Hub));
    if (!h) return NULL;
    h->dev = d;
    h->super = UsbDevSpeed(d) == USB_SPEED_SUPER;
    UINT8 desc[16];
    memset(desc, 0, sizeof(desc));
    UINT16 type = h->super ? USB_DT_SS_HUB : USB_DT_HUB;
    if (UsbControl(d, 0xA0, REQ_GET_DESCRIPTOR, (UINT16)(type << 8), 0, 12, desc) < 7) {
        kprintf("[USB] %s: no hub descriptor\n", UsbDevName(d));
        kfree(h);
        return NULL;
    }
    h->ports = desc[2] > MAX_HUB_PORTS ? MAX_HUB_PORTS : desc[2];
    UINT16 chars = (UINT16)(desc[3] | desc[4] << 8);
    h->power_ms = (UINT16)(desc[5] * 2);
    bool mtt = f->proto == 2;                          /* the multi-TT interface */
    UINT8 think = (UINT8)((chars >> 5) & 3);
    if (h->super)
        UsbControl(d, 0x20, REQ_SET_HUB_DEPTH, UsbDevDepth(d), 0, 0, NULL);
    if (!UsbSetHub(d, h->ports, mtt, think)) {
        kprintf("[USB] %s: the controller refused the hub\n", UsbDevName(d));
        kfree(h);
        return NULL;
    }

    for (int port = 1; port <= h->ports; port++)
        UsbControl(d, 0x23, REQ_SET_FEATURE, PORT_POWER, (UINT16)port, 0, NULL);
    hub_delay(h->power_ms > 100 ? h->power_ms : 100);

    h->status = UsbOpenPipe(d, ep, 64);
    if (!h->status) { kfree(h); return NULL; }
    g_hubs[slot] = h;
    UsbBind(d, h, hub_gone);
    kprintf("[USB] %s: %s hub, %d ports\n", UsbDevName(d), h->super ? "USB 3" : "USB 2", h->ports);

    /* Look at every port once now (devices already plugged in), then
     * whenever the status endpoint says so */
    h->change = ((1u << (h->ports + 1)) - 1) & ~1u;
    UsbHubSignal(h);
    int bytes = (h->ports + 1 + 7) / 8;
    UsbPipeListen(h->status, bytes, on_status, h);
    return h;
}
