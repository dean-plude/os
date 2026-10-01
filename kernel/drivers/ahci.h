/*
 * ahci.h — AHCI (SATA) disk driver: registers each disk as a block device
 */

#pragma once

/* Probe every AHCI controller; returns the number of disks found. */
int AhciInit(void);

/* After S3: set the disks' ports up again */
void AhciResume(void);
