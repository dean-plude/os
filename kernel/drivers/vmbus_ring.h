/*
 * vmbus_ring.h — VMBus channel ring buffers
 *
 * A channel has two rings in memory the guest gives the host (a GPADL):
 * the outbound one the guest writes and the host reads, then the inbound
 * one the host writes and the guest reads.  A ring's first page is its
 * header (the indexes and the signalling hints); the data area after it
 * holds packets, each a VmPacketDesc, its payload padded to 8 bytes, and
 * 8 bytes of trailer (the writer's old write index, which nobody uses).
 * The indexes are byte offsets into the data area, and the writer always
 * leaves at least one byte free, so equal indexes mean empty.
 *
 * The layout is the one Hyper-V defines (and Linux's and FreeBSD's vmbus
 * drivers use).  Header-only so tests/host/hv_input.c runs it on the host.
 */

#pragma once

#include "../include/types.h"

#define VMBUS_PAGE 4096u

typedef struct {
    volatile UINT32 write_index;
    volatile UINT32 read_index;
    volatile UINT32 interrupt_mask;    /* the reader: "no signal, I poll" */
    volatile UINT32 pending_send_sz;   /* the writer waits for this much room */
    UINT32 reserved1[12];
    UINT32 feature_bits;               /* bit 0: pending_send_sz is honoured */
} VmRingHeader;

typedef struct {
    UINT16 type;                       /* VM_PKT_* */
    UINT16 offset8;                    /* where the payload starts, in 8 bytes */
    UINT16 len8;                       /* the whole packet without its trailer, in 8 bytes */
    UINT16 flags;
    UINT64 trans_id;
} VmPacketDesc;

#define VM_PKT_DATA_INBAND   6
#define VM_PKT_COMP          11
#define VM_PKT_FLAG_COMPLETION_REQUESTED 1

typedef struct {
    VmRingHeader *hdr;
    UINT8        *data;
    UINT32        size;                /* bytes in the data area */
} VmRing;

/* @pages: the ring with its header page (at least 2) */
static inline void vm_ring_init(VmRing *r, void *base, UINT32 pages)
{
    r->hdr  = (VmRingHeader *)base;
    r->data = (UINT8 *)base + VMBUS_PAGE;
    r->size = (pages - 1) * VMBUS_PAGE;
    UINT8 *h = (UINT8 *)base;
    for (UINT32 i = 0; i < VMBUS_PAGE; i++) h[i] = 0;
    r->hdr->feature_bits = 1;
}

static inline UINT32 vm_ring_used(const VmRing *r, UINT32 rd, UINT32 wr)
{
    return wr >= rd ? wr - rd : r->size - rd + wr;
}

static inline void vm_ring_copy_in(VmRing *r, UINT32 at, const void *src, UINT32 len)
{
    const UINT8 *s = src;
    for (UINT32 i = 0; i < len; i++) r->data[(at + i) % r->size] = s[i];
}

static inline void vm_ring_copy_out(const VmRing *r, UINT32 at, void *dst, UINT32 len)
{
    UINT8 *d = dst;
    for (UINT32 i = 0; i < len; i++) d[i] = r->data[(at + i) % r->size];
}

/* Write one in-band packet.  False when the ring has no room.  *signal
 * says whether the reader wants to be told (it is not polling) */
static inline bool vm_ring_write(VmRing *r, const void *payload, UINT32 len,
                                 UINT64 trans_id, UINT16 flags, bool *signal)
{
    UINT32 padded = (len + 7) & ~7u;
    VmPacketDesc d = { VM_PKT_DATA_INBAND, sizeof(VmPacketDesc) / 8,
                       (UINT16)((sizeof(VmPacketDesc) + padded) / 8), flags, trans_id };
    UINT32 total = sizeof(d) + padded + 8;
    UINT32 wr = r->hdr->write_index, rd = r->hdr->read_index;
    if (r->size - vm_ring_used(r, rd, wr) <= total) return false;
    UINT64 zero = 0, trailer = (UINT64)wr << 32;
    UINT32 at = wr;
    vm_ring_copy_in(r, at, &d, sizeof(d));                 at += sizeof(d);
    vm_ring_copy_in(r, at, payload, len);                  at += len;
    vm_ring_copy_in(r, at, &zero, padded - len);           at += padded - len;
    vm_ring_copy_in(r, at, &trailer, 8);                   at += 8;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);               /* the packet before the index */
    r->hdr->write_index = at % r->size;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (signal) *signal = !r->hdr->interrupt_mask;
    return true;
}

/* Read the next packet: its descriptor and up to @cap bytes of payload
 * (*len: the payload's full length; a longer one is cut to @cap).  False
 * when the ring is empty.  *signal says whether the writer waits for room
 * this read made */
static inline bool vm_ring_read(VmRing *r, VmPacketDesc *d, void *out, UINT32 cap,
                                UINT32 *len, bool *signal)
{
    UINT32 rd = r->hdr->read_index, wr = r->hdr->write_index;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);               /* the index before the packet */
    UINT32 used = vm_ring_used(r, rd, wr);
    if (used < sizeof(*d) + 8) return false;
    vm_ring_copy_out(r, rd, d, sizeof(*d));
    UINT32 total = (UINT32)d->len8 * 8, off = (UINT32)d->offset8 * 8;
    if (total < off || off < sizeof(*d) || total + 8 > used) {
        r->hdr->read_index = wr;                           /* garbage: drop what is there */
        *len = 0;
        return false;
    }
    UINT32 n = total - off;
    *len = n;
    vm_ring_copy_out(r, rd + off, out, n < cap ? n : cap);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);               /* copied before the room is given back */
    r->hdr->read_index = (rd + total + 8) % r->size;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (signal) {
        UINT32 want = r->hdr->pending_send_sz;
        *signal = want && r->size - vm_ring_used(r, r->hdr->read_index, r->hdr->write_index) >= want;
    }
    return true;
}
