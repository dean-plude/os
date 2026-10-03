/*
 * nvme.h — NVM Express disk driver: registers each namespace as a block device
 */

#pragma once

/* Probe every NVMe controller; returns the number of disks found. */
int  NvmeInit(void);
/* After S3: reset the controllers and set their queues up again */
void NvmeResume(void);
