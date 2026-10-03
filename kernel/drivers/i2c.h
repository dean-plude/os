/*
 * i2c.h — I2C buses
 *
 * A bus moves bytes to and from 7-bit addresses; the I2C-HID driver
 * (i2chid.c) uses it to reach touchpads.  The one controller driver is
 * Intel's LPSS I2C (a Synopsys DesignWare core, i2c_dw.c), found through
 * the ACPI namespace; the built-in checks plug in modelled buses.
 */

#pragma once

#include "../include/types.h"

typedef struct I2cBus I2cBus;
struct I2cBus {
    const char *name;
    /* Write @wn bytes to @addr, then (with a repeated start) read @rn
     * bytes; either may be 0.  False when the device did not answer
     * (no acknowledge) or the controller failed. */
    bool (*xfer)(I2cBus *bus, UINT16 addr, const UINT8 *w, int wn, UINT8 *r, int rn);
    void *ctx;
};

/* Intel LPSS I2C controller at PCI function 00:@dev.@fn: take it out of
 * reset and set it up as a master at @speed Hz (fast mode at 400 kHz and
 * above).  @fmcn and @sscn are the firmware's timings (ACPI FMCN and SSCN:
 * SCL high count, SCL low count, SDA hold), or NULL.  NULL when there is
 * no such controller.  Opening the same function again returns its bus. */
I2cBus *DwI2cOpen(UINT8 dev, UINT8 fn, UINT32 speed, const UINT16 *fmcn, const UINT16 *sscn);

/* After S3: the controllers lost their setup; set them up again */
void    DwI2cResume(void);
