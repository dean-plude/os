/*
 * i2chid.h — HID over I2C (touchpads)
 */

#pragma once

#include "../include/types.h"

/* Start the "i2chid" thread: once the ACPI namespace is loaded it opens
 * the I2C-HID devices it lists (touchpads) and reads them when their
 * interrupt fires (a GPIO pin, hal/gpio.c), else polls them */
void I2cHidInit(void);

/* Around S3: no transfer may be under way while the CPUs stop; after
 * waking the controllers and devices are set up again */
void I2cHidPrepareSleep(void);
void I2cHidResume(void);

/* hwcheck: the I2C-HID protocol against a modelled touchpad on a modelled
 * bus; one line per check through @say, returns the failures */
int  I2cHidSelfCheck(void (*say)(void *ctx, const char *line), void *ctx);
