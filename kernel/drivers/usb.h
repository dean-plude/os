/*
 * usb.h — the USB core as the class drivers see it
 *
 * The core (usb.c) enumerates devices on the root ports of every host
 * controller (xHCI, EHCI, OHCI and UHCI: usb_hc.h) and on hubs, sets their
 * configuration and offers each interface to the class drivers: hubs
 * (usbhub.c), HID keyboards, mice, tablets and touch screens (usbhid.c),
 * mass storage (usbmsc.c) and audio (usbaudio.c).  A driver that takes an
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
#define USB_DT_CS_INTERFACE 0x24
#define USB_DT_CS_ENDPOINT  0x25
#define USB_DT_HID        0x21
#define USB_DT_REPORT     0x22
#define USB_DT_HUB        0x29
#define USB_DT_SS_HUB     0x2A

/* Start every USB controller and attach what is plugged in; returns the
 * number of devices.  Needs InputInit() first. */
int  UsbInit(void);
/* Timer tick, any CPU: finish interrupt transfers, repeat held keys */
void UsbPoll(void);
/* Before S3: arm the controllers (xHCI) to wake the machine */
void UsbPrepareSleep(void);
/* After waking from S3 (or a sleep that didn't happen): restart the
 * controllers and enumerate again */
void UsbResume(void);

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
/* The device's product name (its iProduct string, in ASCII) into @out;
 * false when it has none */
bool UsbDevProductName(UsbDev *d, char *out, int cap);

/* The active configuration descriptor, every interface and alternate
 * setting of it; *@len its bytes */
const UINT8 *UsbDevConfig(const UsbDev *d, int *len);

/* Find the next descriptor of @type in an interface's descriptors after
 * offset *@off (start at 0); NULL when there are no more. */
const UINT8 *UsbIfaceFind(const UsbIface *f, UINT8 type, int *off);

/* A control transfer on endpoint 0.  @data is copied to (OUT) or from (IN)
 * the device; up to 4096 bytes.  Returns the bytes moved, or -1. */
int UsbControl(UsbDev *d, UINT8 type, UINT8 req, UINT16 value, UINT16 index, UINT16 len, void *data);

/* Open a pipe to the endpoint an endpoint descriptor describes (interrupt,
 * bulk or isochronous, either direction, of the interface's current
 * alternate setting); @buf_bytes of DMA buffer come with it.  NULL on
 * failure. */
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
/* SET_INTERFACE: switch interface @iface to alternate setting @alt.  The
 * pipes of its old setting close (stop their streams first; don't use
 * them again) and those of the new one can be opened. */
bool UsbSetInterface(UsbDev *d, UINT8 iface, UINT8 alt);

/* Isochronous streaming.  The pipe keeps @xfers transfers of @packets
 * packets queued, one packet per service interval (a frame or
 * microframe), back to back.  Packet i of transfer k has room for
 * UsbIsoPacketSize() bytes at data + i * that; @lens[i] is its length.
 * OUT: @cb fills a transfer (data and lengths) before it is queued, at
 * the start and each time it finishes.  IN: @cb gets each finished
 * transfer (lengths received; 0 for a packet lost), which is then queued
 * again.  @cb runs from the controller's poll with interrupts off: be
 * quick, issue no transfers. */
typedef void (*UsbIsoCallback)(UsbPipe *p, UINT8 *data, UINT16 *lens, int packets, void *ctx);
bool   UsbIsoStart(UsbPipe *p, int xfers, int packets, UsbIsoCallback cb, void *ctx);
void   UsbIsoStop(UsbPipe *p);
UINT32 UsbIsoPacketSize(const UsbPipe *p);         /* bytes a packet can carry */
UINT32 UsbIsoIntervalUs(const UsbPipe *p);         /* microseconds between packets */

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
void  UsbHidSyncLeds(void);                            /* usb thread: lock-key LEDs */
/* The Terminal's usbcheck: run the report parser on report descriptors
 * QEMU has no device for; says a line per check, returns how many failed */
int   UsbHidSelfCheck(void (*say)(void *ctx, const char *line), void *ctx);
void *UsbMscProbe(UsbDev *d, const UsbIface *f);      /* usbmsc.c */
void *UsbAudioProbe(UsbDev *d, const UsbIface *f);    /* usbaudio.c */
