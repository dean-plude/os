/*
 * virtio_input.h — virtio input devices (multi-touch screens, pens, tablets)
 */

#pragma once

#include "../include/types.h"

/* Find and start every virtio multi-touch screen, pen and tablet (needs InputInit) */
void VirtioInputInit(void);
/* Desktop loop: turn the events that came into INPUT_TOUCH, INPUT_PEN and INPUT_MOUSE events */
void VirtioInputPoll(void);
/* After S3: set the devices up again */
void VirtioInputResume(void);
/* usbcheck: the pen and tablet decoding on canned events; says a line a
 * check and returns how many failed */
int  VirtioInputSelfCheck(void (*say)(void *ctx, const char *line), void *ctx);
