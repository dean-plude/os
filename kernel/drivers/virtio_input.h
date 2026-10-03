/*
 * virtio_input.h — virtio input devices (multi-touch screens)
 */

#pragma once

#include "../include/types.h"

/* Find and start every virtio multi-touch screen (needs InputInit) */
void VirtioInputInit(void);
/* Desktop loop: turn the events that came into INPUT_TOUCH events */
void VirtioInputPoll(void);
/* After S3: set the devices up again */
void VirtioInputResume(void);
