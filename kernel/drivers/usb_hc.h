/*
 * usb_hc.h — the USB core as the host controller drivers see it
 *
 * The core (usb.c) owns the devices: it enumerates them on root ports and
 * behind hubs, reads their descriptors, makes a pipe per endpoint, offers
 * the interfaces to the class drivers and takes devices away again.  A
 * host controller driver — xHCI (xhci.c), EHCI (ehci.c), OHCI (ohci.c),
 * UHCI (uhci.c) — moves the bytes: it resets root ports, runs control,
 * bulk and interrupt IN transfers, and keeps whatever per-device and
 * per-endpoint state its hardware needs in the `hcd` fields.  Isochronous
 * pipes stream: the controller keeps a ring of transfers scheduled and
 * hands each finished one to UsbIsoDone().
 *
 * Every controller is polled like the rest of NovaOS's drivers: its
 * interrupts stay off, poll() runs from the timer tick, and a transfer
 * someone waits for is waited for by spinning on its descriptors.
 *
 * Locking: g_usb_lock (taken with interrupts off) covers every
 * controller's periodic and asynchronous schedules and the interrupt IN
 * listeners: poll() and listen() run with it held, and drivers take it
 * themselves to link or unlink a queue.
 */

#pragma once

#include "usb.h"
#include "../hal/pci.h"
#include "../ke/spinlock.h"

#define USB_MAX_PORTS    64        /* root ports per controller */
#define USB_MAX_BINDS    4
#define USB_CTL_BUF      4096      /* bytes of control-transfer data at most */

typedef struct UsbHc UsbHc;

/* A control request's setup packet */
typedef struct __attribute__((packed)) {
    UINT8  type, req;
    UINT16 value, index, len;
} UsbSetup;

struct UsbPipe {
    UsbDev          *dev;
    UINT8            idx;          /* endpoint number x 2, + 1 for IN (xHCI's DCI) */
    UINT8            addr;         /* bEndpointAddress */
    UINT8            xfer;         /* 1 isochronous, 2 bulk, 3 interrupt */
    bool             in;
    UINT16           mps;
    UINT8            mult;         /* high speed periodic: extra transactions per microframe (0-2) */
    UINT8            interval;     /* bInterval */
    UINT8            iface;        /* the interface it belongs to */
    UINT8           *dma;          /* the pipe's buffer (below 4 GiB) */
    UINT32           dma_size;
    volatile int     busy;         /* a bulk transfer is in progress */
    /* interrupt IN listening */
    UsbInCallback    cb;
    void            *cb_ctx;
    int              listen_len;
    /* isochronous streaming: transfer k, packet i is at
     * dma + (k * iso_packets + i) * iso_psize, its length iso_len[k * iso_packets + i] */
    UsbIsoCallback   iso_cb;
    void            *iso_ctx;
    int              iso_xfers, iso_packets;
    UINT32           iso_psize;
    UINT16          *iso_len;
    volatile bool    dead;         /* the device left: never touched again */
    void            *hcd;          /* the controller driver's own state */
};

struct UsbDev {
    UsbHc           *hc;
    volatile bool    gone;
    UINT8            addr;         /* USB address (0 until assigned) */
    UINT8            root_port, speed, depth;
    UINT32           route;        /* xHCI route string: hub ports, 4 bits per tier */
    UsbDev          *parent;       /* hub, or NULL on a root port */
    UINT8            parent_port;
    UsbDev          *tt_hub;       /* low/full speed behind a high-speed hub: the hub whose */
    UINT8            tt_port;      /*   transaction translator serves it, and its port */
    bool             mtt;          /* (a hub) one TT per port */
    bool             is_hub;
    UINT16           vid, pid;
    UINT16           ep0_mps;
    char             name[32];
    UINT8           *buf;          /* control-transfer data (USB_CTL_BUF bytes, below 4 GiB) */
    volatile int     ep0_busy;
    volatile int     users;        /* transfers in progress */
    UsbPipe         *pipes[32];    /* by idx */
    UINT8           *cfg;          /* the configuration descriptor */
    int              cfg_len;
    struct { void *inst; void (*gone)(void *inst); } bind[USB_MAX_BINDS];
    int              nbind;
    void            *hcd;          /* the controller driver's own state */
};

/* port_status() bits */
#define USB_PORT_CONNECTED  1u
#define USB_PORT_CHANGED    2u     /* connected or disconnected since last asked */

typedef struct {
    const char *kind;              /* "xHCI", "EHCI", ... */
    /* Root ports.  port_status() reports (and forgets) a connect change;
     * port_reset() resets and enables the port and returns the speed of
     * the device on it, or 0 (none, or handed to a companion controller). */
    UINT32 (*port_status)(UsbHc *hc, int port);
    UINT8  (*port_reset)(UsbHc *hc, int port);
    /* Devices.  dev_address (xHCI) gives a just-reset device its address
     * itself; when it is NULL the core sends SET_ADDRESS.  dev_ep0 (may be
     * NULL) learns endpoint 0's real packet size; dev_config (may be NULL)
     * runs once every pipe of the configuration is added; dev_hub (may be
     * NULL) learns that a device is a hub.  dev_remove stops and frees all
     * of the device's state on the controller (its pipes' included). */
    bool (*dev_address)(UsbHc *hc, UsbDev *d);
    bool (*dev_ep0)(UsbHc *hc, UsbDev *d);
    bool (*pipe_add)(UsbHc *hc, UsbPipe *p);
    /* pipe_drop: free the controller's state for a pipe of an alternate
     * setting being left (after iface_config dropped it, if there is one).
     * iface_config (may be NULL): the endpoints of the configuration
     * change, @drop and @add being masks of pipe idx; the pipes being
     * added are in d->pipes, those dropped no longer. */
    void (*pipe_drop)(UsbHc *hc, UsbPipe *p);
    bool (*iface_config)(UsbHc *hc, UsbDev *d, UINT32 drop, UINT32 add);
    bool (*dev_config)(UsbHc *hc, UsbDev *d);
    bool (*dev_hub)(UsbHc *hc, UsbDev *d, UINT8 ports, bool mtt, UINT8 think_time);
    void (*dev_remove)(UsbHc *hc, UsbDev *d);
    /* Transfers, waiting for them.  control() moves s->len bytes to or
     * from d->buf (the caller owns endpoint 0) and returns the bytes moved;
     * bulk() moves @len bytes to or from p->dma.  Both return -1 on an
     * error, with *stalled set if the endpoint stalled. */
    int  (*control)(UsbHc *hc, UsbDev *d, const UsbSetup *s, bool *stalled);
    int  (*bulk)(UsbHc *hc, UsbPipe *p, UINT32 len, UINT32 timeout_ms, bool *stalled);
    /* Queue one interrupt IN transfer of p->listen_len bytes into p->dma;
     * when it completes, poll() hands it to UsbPipeDeliver().  Called with
     * g_usb_lock held. */
    bool (*listen)(UsbHc *hc, UsbPipe *p);
    /* Isochronous: schedule the pipe's iso_xfers transfers (OUT ones are
     * filled), each finished one going to UsbIsoDone() from poll() and
     * scheduled again; and stop them all (no lock held, may wait).
     * iso_start is called with g_usb_lock held. */
    bool (*iso_start)(UsbHc *hc, UsbPipe *p);
    void (*iso_stop)(UsbHc *hc, UsbPipe *p);
    /* After a stall: clear the controller's halt and reset the data toggle */
    bool (*pipe_reset)(UsbHc *hc, UsbPipe *p);
    /* Timer tick, g_usb_lock held: finish interrupt transfers, notice port changes */
    void (*poll)(UsbHc *hc);
    /* Before S3 (may be NULL): arm the root ports to wake the machine */
    void (*prepare_sleep)(UsbHc *hc);
    /* After S3: the controller lost its state; start it again (the core
     * then forgets every device and enumerates the ports afresh) */
    bool (*resume)(UsbHc *hc);
} UsbHcOps;

struct UsbHc {
    const UsbHcOps *ops;
    void           *priv;
    int             num;           /* bus number, from 1, for logs */
    int             ports;
    UINT8           addr_used[16]; /* USB addresses in use (bit per address) */
    volatile bool   rescan;        /* treat every root port as changed (after S3) */
};

extern KSpinLock g_usb_lock;

/* Register a started controller with @ports root ports (powered) */
UsbHc *UsbAddController(const UsbHcOps *ops, void *priv, int ports);

/* A root port's name for logs: "port 3", or "usb2 port 3" with several controllers */
void UsbPortName(UsbHc *hc, int port, char *buf, int size);

/* An interrupt IN transfer finished with @len bytes in p->dma: run the
 * listener.  Returns true to queue the next transfer. */
bool UsbPipeDeliver(UsbPipe *p, int len);
/* ... or failed: stop listening */
void UsbPipeListenFailed(UsbPipe *p, int code);
/* Isochronous transfer @k finished (IN: its lengths are in iso_len): hand
 * it to the driver, which refills it (OUT).  False: the stream stopped,
 * don't schedule it again. */
bool UsbIsoDone(UsbPipe *p, int k);
/* Service interval of a periodic pipe, in 125 us microframes */
UINT32 UsbPipePeriod(const UsbPipe *p);

/* Zeroed, physically contiguous pages below 4 GiB (every controller here
 * can reach them); NULL if there are none. */
void *UsbDmaAlloc(int pages);
void  UsbDmaFree(void *p, int pages);
static inline UINT64 UsbPhys(const volatile void *va) { return (UINT64)(uintptr_t)va - PHYSMAP_BASE; }
static inline void *UsbVirt(UINT64 pa) { return (void *)(uintptr_t)(pa + PHYSMAP_BASE); }

/* Wait @ms: sleeping when the scheduler runs, else spinning */
void UsbDelay(int ms);
/* A point @ms from now, for spinning waits; and whether it has passed */
UINT64 UsbDeadline(UINT32 ms);
bool   UsbPast(UINT64 deadline);
/* Spin until *@f is ours (yielding when we can); and let go */
void UsbFlagTake(volatile int *f);
void UsbFlagDrop(volatile int *f);

/* The controller drivers.  Each starts the controller at @pci and
 * registers it; false if it can't. */
bool XhciProbe(const PciDevice *pci);
bool EhciProbe(const PciDevice *pci);
bool OhciProbe(const PciDevice *pci);
bool UhciProbe(const PciDevice *pci);
