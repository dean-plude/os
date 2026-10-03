/*
 * virtio_gpu.h — virtio GPU, 2D: every output (scanout) of the card a monitor
 *
 * QEMU's virtio-vga and virtio-gpu-pci have up to 16 outputs on one card
 * (max_outputs=N).  The display driver (hal/display.c) makes each output
 * that has a monitor connected a head: a picture in guest memory that the
 * card copies to its output when told which part changed (VgpuFlush).
 * Monitors come and go at run time (QEMU: a UI window, or a VNC client's
 * SetDesktopSize, per output); VgpuChanged says when.
 */

#pragma once

#include "../include/types.h"

#define VGPU_MAX_DEVICES  2
#define VGPU_MAX_SCANOUTS 16

/* Find and start every virtio GPU (after PciInitialize); how many */
int  VgpuInit(void);
int  VgpuCount(void);
/* The card's name ("QEMU virtio-vga", "QEMU virtio-gpu"), its outputs, and
 * whether it is the VGA-compatible one whose framebuffer (BAR0) is @fb */
const char *VgpuName(int dev);
int  VgpuScanouts(int dev);
bool VgpuIsAt(int dev, UINT64 fb);

/* The outputs with a monitor (bit N: output N), and each one's preferred
 * size (w[N] x h[N], what the monitor asks for) */
UINT32 VgpuConnected(int dev, int *w, int *h);
/* A monitor came or went since the last call (the card said so) */
bool VgpuChanged(int dev);

/* Show a @w x @h picture on output @scanout: *pixels (little-endian XRGB,
 * *stride pixels per line, physically contiguous) replaces the old one,
 * which is freed; @keep copies what fits of it across.  @w == 0 turns the
 * output off. */
bool VgpuShow(int dev, int scanout, int w, int h, bool keep, UINT32 **pixels, int *stride);
/* Copy a rectangle of the picture to the output */
void VgpuFlush(int dev, int scanout, int x, int y, int w, int h);

/* After S3 (QEMU resets the card): start it again and show every picture
 * it showed */
void VgpuResume(void);
