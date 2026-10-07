/*
 * vmbus.h — Hyper-V's VMBus: the synthetic devices of a Hyper-V VM
 *
 * A Generation 2 Hyper-V VM has no PS/2 controller, no USB and no PCI
 * input devices: its keyboard and mouse (and disks, network, video) are
 * channels on VMBus, offered by the host through messages and run over
 * shared-memory rings.  vmbus.c finds Hyper-V, connects, opens the
 * channels a NovaOS driver wants (hv_input.c: the keyboard and the mouse)
 * and polls them.
 */

#pragma once

#include "../include/types.h"

typedef struct VmbusChannel VmbusChannel;

/* A channel driver: the device type it takes (the offer's GUID, as its 16
 * bytes go on the wire), opened() once the channel is open (its context
 * back), packet() for each in-band packet the host sends on it */
typedef struct {
    UINT8       type[16];
    const char *name;
    void      *(*opened)(VmbusChannel *ch);
    void       (*packet)(void *ctx, const UINT8 *data, UINT32 len);
} VmbusDriver;

/* On CPU 0 with interrupts still off (the SynIC registers are per CPU):
 * find Hyper-V, connect to VMBus and open the channels NovaOS drives.
 * False (one log line) when this is not Hyper-V or the host said no */
bool VmbusInit(void);
/* The device poll thread: read what came on every open channel */
void VmbusPoll(void);

/* A driver sends an in-band packet on its channel; false when its ring is full */
bool VmbusSend(VmbusChannel *ch, const void *data, UINT32 len, UINT64 trans_id, bool want_completion);
