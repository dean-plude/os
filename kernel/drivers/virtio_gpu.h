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

/* 3D (a card with VIRGL, blobs, context capability sets and host-visible
 * memory, e.g. QEMU's virtio-vga-gl,venus=on,blob=on,hostmem=...): what a
 * Vulkan driver in a program (Mesa's Venus) needs, through um_gpu.c.
 * A context belongs to one program; its timelines are numbered from 1. */
typedef struct VgpuCtx VgpuCtx;
typedef struct VgpuBlob VgpuBlob;
bool      Vgpu3dPresent(void);
UINT64    Vgpu3dHostVisibleSize(void);
/* Capability set @id at @version into @out (at most @size bytes): its size, or -1 */
int       Vgpu3dCapset(UINT32 id, UINT32 version, void *out, UINT32 size);
VgpuCtx  *VgpuCtxCreate(UINT32 capset, const char *name);
void      VgpuCtxDestroy(VgpuCtx *c);
/* Queue a command stream for the host's ring @ring; when it has run there,
 * timeline @syncs[k] reaches @vals[k] (n == 0: no fence) */
bool      VgpuCtxSubmit(VgpuCtx *c, const void *cs, UINT32 size, UINT32 ring, int n,
                        const UINT32 *syncs, const UINT64 *vals);
/* A blob (after running the command stream @cs, which may create what it
 * refers to by @blob_id); one reference, the caller's */
VgpuBlob *VgpuBlobCreate(VgpuCtx *c, UINT32 blob_mem, UINT32 flags, UINT64 blob_id, UINT64 size,
                         const void *cs, UINT32 cs_size);
UINT32    VgpuBlobId(VgpuBlob *b);
/* Map it into the host-visible region: its physical address and size */
bool      VgpuBlobMap(VgpuBlob *b, UINT64 *pa, UINT64 *size);
void      VgpuBlobRef(VgpuBlob *b);
void      VgpuBlobUnref(VgpuBlob *b);
UINT32    VgpuSyncCreate(VgpuCtx *c, UINT64 value);
void      VgpuSyncDestroy(VgpuCtx *c, UINT32 id);
bool      VgpuSyncAccess(VgpuCtx *c, UINT32 id, int op, UINT64 *value);
int       VgpuWait(VgpuCtx *c, int n, const UINT32 *ids, const UINT64 *vals, bool any, UINT64 timeout_ns);
