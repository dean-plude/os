/*
 * e1000.h — Intel 8254x/8257x Gigabit Ethernet driver (e1000 / e1000e)
 *
 * Supports the 82540EM (QEMU "e1000") and 82574L (QEMU "e1000e", the
 * default NIC of the q35 machine) using legacy descriptors.  Receive is
 * polled; device interrupts are masked.
 */

#pragma once

#include "../include/types.h"

#define E1000_MTU_FRAME  1518     /* largest Ethernet frame we send/receive */

bool        E1000Init(void);
bool        E1000Present(void);
const char *E1000Name(void);
void        E1000Mac(UINT8 mac[6]);
bool        E1000LinkUp(void);

/* Queue one frame for transmission; false if the ring is full. */
bool        E1000Transmit(const void *frame, UINT16 len);
/* Copy the next received frame into buf; returns its length, or 0. */
int         E1000Receive(void *buf, int cap);
