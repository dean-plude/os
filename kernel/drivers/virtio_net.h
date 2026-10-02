/*
 * virtio_net.h — virtio network adapter (QEMU/KVM "virtio-net-pci")
 *
 * A virtio 1.0 ("modern") PCI device with one receive and one transmit
 * split virtqueue.  Receive is polled, like the e1000 driver; the device
 * is told not to interrupt.  The same calls as e1000.h, so net.c drives
 * whichever adapter it finds.
 */

#pragma once

#include "../include/types.h"

bool        VirtioNetInit(void);
void        VirtioNetResume(void);    /* after S3 */
bool        VirtioNetPresent(void);
const char *VirtioNetName(void);
void        VirtioNetMac(UINT8 mac[6]);
bool        VirtioNetLinkUp(void);

/* Queue one frame for transmission; false if the ring is full. */
bool        VirtioNetTransmit(const void *frame, UINT16 len);
/* Copy the next received frame into buf; returns its length, or 0. */
int         VirtioNetReceive(void *buf, int cap);
