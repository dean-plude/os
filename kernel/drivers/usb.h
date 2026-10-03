/*
 * usb.h — the USB core as the class drivers see it
 *
 * The host controller driver (xhci.c) enumerates devices on its root ports
 * and on hubs, sets their configuration and offers each interface to the
 * class drivers: hubs (usbhub.c), HID keyboards, mice, tablets and touch
 * screens (usbhid.c) and mass storage (usbmsc.c).  A driver that takes an
 * interface opens pipes to its endpoints and keeps an instance; when the
 * device goes away its gone() callback runs and the pipes stop.
 *
 * Threads: attach(), the hub driver's port handling and gone() run on the
 * "usb" thread, which is the only one that enumerates.  Control and bulk
 * transfers may be issued from any thread (they wait, spinning); interrupt
 * IN completions run from the event-ring poll with interrupts off, so
 * their callbacks must be short and must not issue transfers.
 */

#pragma once

#include "../include/types.h"

typedef struct UsbDev  UsbDev;
typedef struct UsbPipe UsbPipe;

/* One interface of the active configuration, as offered to a driver */
typedef struct {
    UINT8        number, alt;
    UINT8        cls, sub, proto;
    const UINT8 *desc;        /* the interface descriptor, then its class and endpoint descriptors */
    int          len;         /* up to the next interface descriptor */
} UsbIface;

/* Standard descriptor types */
#define USB_DT_DEVICE     1
#define USB_DT_CONFIG     2
#define USB_DT_INTERFACE  4
#define USB_DT_ENDPOINT   5
#define USB_DT_HID        0x21
#define USB_DT_REPORT     0x22
#define USB_DT_HUB        0x29
#define USB_DT_SS_HUB     0x2A

/* Speeds (xHCI port speed IDs) */
#define USB_SPEED_FULL    1
#define USB_SPEED_LOW     2
#define USB_SPEED_HIGH    3
#define USB_SPEED_SUPER   4

/* Device facts drivers need */
UINT8  UsbDevSpeed(const UsbDev *d);
UINT8  UsbDevDepth(const UsbDev *d);        /* 0: on a root port */
UINT16 UsbDevVendor(const UsbDev *d);
UINT16 UsbDevProduct(const UsbDev *d);
/* The device was unplugged: transfers to it fail at once */
bool   UsbDevGone(const UsbDev *d);
/* A short name for logs, e.g. "port 3.2" */
const char *UsbDevName(const UsbDev *d);

/* Find the next descriptor of @type in an interface's descriptors after
 * offset *@off (start at 0); NULL when there are no more. */
const UINT8 *UsbIfaceFind(const UsbIface *f, UINT8 type, int *off);

/* A control transfer on endpoint 0.  @data is copied to (OUT) or from (IN)
 * the device; up to 4096 bytes.  Returns the bytes moved, or -1. */
int UsbControl(UsbDev *d, UINT8 type, UINT8 req, UINT16 value, UINT16 index, UINT16 len, void *data);

/* Open a pipe to the endpoint an endpoint descriptor describes (interrupt
 * or bulk, either direction); @buf_bytes of DMA buffer come with it.
 * NULL on failure. */
UsbPipe *UsbOpenPipe(UsbDev *d, const UINT8 *ep_desc, UINT32 buf_bytes);
UINT16   UsbPipeMaxPacket(const UsbPipe *p);

/* Interrupt IN: keep one transfer of @len bytes queued; each completion
 * calls @cb (interrupts off: be quick, issue no transfers) and is queued
 * again unless @cb returns false or the transfer failed. */
typedef bool (*UsbInCallback)(UsbPipe *p, const UINT8 *data, int len, void *ctx);
bool UsbPipeListen(UsbPipe *p, int len, UsbInCallback cb, void *ctx);

/* A bulk transfer, waiting for it (at most @timeout_ms).  Returns the
 * bytes moved, or -1 on an error (*stalled: the endpoint halted, see
 * UsbPipeReset). */
int  UsbBulk(UsbPipe *p, void *buf, UINT32 len, UINT32 timeout_ms, bool *stalled);
/* Recover a halted endpoint: Reset Endpoint, rewind its ring and clear
 * the device's ENDPOINT_HALT. */
bool UsbPipeReset(UsbPipe *p);

/* Driver instances.  A driver that takes an interface records itself here;
 * @gone runs on the usb thread when the device leaves. */
void UsbBind(UsbDev *d, void *inst, void (*gone)(void *inst));

/* ---- for the hub driver ---- */

/* A hub's port changed: have the usb thread call @fn(@hub_inst) soon.
 * Safe from an interrupt IN callback. */
void UsbHubSignal(void *hub_inst);
/* Enumerate the device on port @port of hub @hub (reset and enabled by the
 * hub driver, at @speed).  Returns the new device, or NULL. */
UsbDev *UsbAttachChild(UsbDev *hub, UINT8 port, UINT8 speed);
/* The device on that hub port went away (and everything behind it) */
void UsbDetachChild(UsbDev *hub, UINT8 port);
/* Tell the controller @d is a hub with @ports ports (multi-TT, think time) */
bool UsbSetHub(UsbDev *d, UINT8 ports, bool mtt, UINT8 think_time);

/* ---- class drivers ---- */
void *UsbHubProbe(UsbDev *d, const UsbIface *f);      /* usbhub.c */
void  UsbHubService(void *inst);                       /* usb thread: handle port changes */
void  UsbHubServiceAll(void);                          /* ... of every hub that signalled */
void *UsbHidProbe(UsbDev *d, const UsbIface *f);      /* usbhid.c */
void  UsbHidTickAll(UINT64 now);                       /* key repeat, every tick */
void *UsbMscProbe(UsbDev *d, const UsbIface *f);      /* usbmsc.c */
