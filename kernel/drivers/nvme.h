/*
 * nvme.h — NVM Express disk driver: registers each namespace as a block device
 */

#pragma once

#include "../include/types.h"

/* Probe every NVMe controller; returns the number of disks found. */
int  NvmeInit(void);
/* An Intel VMD controller (which NovaOS doesn't drive) hides NVMe disks */
bool NvmeBehindVmd(void);
/* After S3: reset the controllers and set their queues up again */
void NvmeResume(void);
