/* The VMBus ring buffer and Hyper-V's keyboard and mouse protocols, on the
 * host (tests/tools/test_hv_input.py): what the drivers send goes through a
 * real ring, what the host would send is fed to them */
#include "../../kernel/drivers/vmbus_ring.h"
#include "../../kernel/drivers/hv_input.c"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static VmRing wire;                         /* what the drivers sent */
static InputEvent posted[8];
static int nposted;
static UINT8 attached[256];
static int attached_len, started, reports;
static UINT8 last_report[16];

bool VmbusSend(VmbusChannel *ch, const void *data, UINT32 len, UINT64 trans_id, bool want_completion)
{
    (void)ch;
    return vm_ring_write(&wire, data, len, trans_id, want_completion ? VM_PKT_FLAG_COMPLETION_REQUESTED : 0, NULL);
}
void InputPost(const InputEvent *ev) { posted[nposted++] = *ev; }
void *HidAttach(const UINT8 *desc, int len, const char **kind)
{
    memcpy(attached, desc, len); attached_len = len; *kind = "absolute pointer"; return attached;
}
void HidStart(void *hid) { assert(hid == attached); started++; }
void HidInput(void *hid, const UINT8 *r, int len) { (void)hid; memcpy(last_report, r, len); reports++; }
void kprintf(const char *f, ...) { va_list v; va_start(v, f); vprintf(f, v); va_end(v); }
void *kzalloc(size_t n) { return calloc(1, n); }

/* The next packet the drivers sent: its payload in @out, its length */
static UINT32 sent(UINT8 *out, UINT16 *flags)
{
    VmPacketDesc d; UINT32 len; bool sig;
    assert(vm_ring_read(&wire, &d, out, 512, &len, &sig));
    assert(d.type == VM_PKT_DATA_INBAND);
    *flags = d.flags;
    return len;
}

static void ring_checks(void)
{
    static UINT8 mem[3 * VMBUS_PAGE];
    VmRing r;
    vm_ring_init(&r, mem, 3);
    assert(r.size == 2 * VMBUS_PAGE && r.hdr->feature_bits == 1);
    UINT8 in[600], out[600];
    unsigned seed = 1;
    for (int round = 0; round < 5000; round++) {            /* wraps the data area many times */
        UINT32 n = 1 + (seed = seed * 1103515245 + 12345) % 500;
        for (UINT32 i = 0; i < n; i++) in[i] = (UINT8)(round + i * 7);
        bool sig = false;
        assert(vm_ring_write(&r, in, n, round, 0, &sig) && sig);
        VmPacketDesc d; UINT32 len;
        assert(vm_ring_read(&r, &d, out, sizeof(out), &len, &sig));
        assert(len == ((n + 7) & ~7u) && d.trans_id == (UINT64)round && !memcmp(in, out, n));
        assert(!vm_ring_read(&r, &d, out, sizeof(out), &len, &sig));
    }
    int k = 0;                                               /* full: refused, not overwritten */
    while (vm_ring_write(&r, in, 100, k, 0, NULL)) k++;
    assert(k == (int)(r.size - 1) / (16 + 104 + 8));
    r.hdr->pending_send_sz = 200;                            /* a waiting writer is told */
    VmPacketDesc d; UINT32 len; bool sig = false;
    assert(vm_ring_read(&r, &d, out, sizeof(out), &len, &sig) && sig && d.trans_id == 0);
    r.hdr->interrupt_mask = 1;                               /* a polling reader is not */
    while (vm_ring_read(&r, &d, out, sizeof(out), &len, &sig)) {}
    assert(vm_ring_write(&r, in, 8, 0, 0, &sig) && !sig);
    puts("ring buffer passed");
}

static void keyboard_checks(void)
{
    UINT8 b[512]; UINT16 fl;
    void *ctx = HvKeyboardDriver.opened(NULL);
    UINT32 n = sent(b, &fl);
    assert(n == 8 && rd32(b) == 1 && rd32(b + 4) == 0x10000 && (fl & VM_PKT_FLAG_COMPLETION_REQUESTED));
    UINT8 resp[8] = { 2, 0, 0, 0, 1, 0, 0, 0 };
    HvKeyboardDriver.packet(ctx, resp, 8);
    UINT8 a_down[16] = { 3, 0, 0, 0, 0x1E, 0, 0, 0, 0, 0, 0, 0 };
    UINT8 a_up[16]   = { 3, 0, 0, 0, 0x1E, 0, 0, 0, 2, 0, 0, 0 };
    UINT8 up_e0[16]  = { 3, 0, 0, 0, 0x48, 0, 0, 0, 4, 0, 0, 0 };
    UINT8 text[16]   = { 3, 0, 0, 0, 'x', 0, 0, 0, 1, 0, 0, 0 };
    UINT8 pause[16]  = { 3, 0, 0, 0, 0x1D, 0, 0, 0, 8, 0, 0, 0 };
    HvKeyboardDriver.packet(ctx, a_down, 16);
    HvKeyboardDriver.packet(ctx, a_up, 16);
    HvKeyboardDriver.packet(ctx, up_e0, 16);
    HvKeyboardDriver.packet(ctx, text, 16);
    HvKeyboardDriver.packet(ctx, pause, 16);
    assert(nposted == 3);
    assert(posted[0].type == INPUT_KEY && posted[0].scancode == 0x1E && posted[0].pressed && !posted[0].extended);
    assert(posted[1].scancode == 0x1E && !posted[1].pressed);
    assert(posted[2].scancode == 0x48 && posted[2].pressed && posted[2].extended);
    puts("keyboard passed");
}

static void mouse_checks(void)
{
    /* Hyper-V's mouse: 5 buttons, absolute X/Y 0-32767, a wheel */
    static const UINT8 rdesc[] = {
        0x05,0x01, 0x09,0x02, 0xA1,0x01, 0x09,0x01, 0xA1,0x00, 0x05,0x09, 0x19,0x01, 0x29,0x05,
        0x15,0x00, 0x25,0x01, 0x95,0x05, 0x75,0x01, 0x81,0x02, 0x95,0x01, 0x75,0x03, 0x81,0x01,
        0x05,0x01, 0x09,0x30, 0x09,0x31, 0x15,0x00, 0x26,0xFF,0x7F, 0x75,0x10, 0x95,0x02, 0x81,0x02,
        0x09,0x38, 0x15,0x81, 0x25,0x7F, 0x75,0x08, 0x95,0x01, 0x81,0x06, 0xC0, 0xC0 };
    UINT8 b[512]; UINT16 fl;
    void *ctx = HvMouseDriver.opened(NULL);
    UINT32 n = sent(b, &fl);
    assert(n == 24 && rd32(b) == 1 && rd32(b + 4) == 12 && rd32(b + 8) == 0 && rd32(b + 12) == 4 &&
           rd32(b + 16) == 0x20000 && (fl & VM_PKT_FLAG_COMPLETION_REQUESTED));

    UINT8 resp[24] = { 1, 0, 0, 0, 13, 0, 0, 0, 1, 0, 0, 0, 5, 0, 0, 0, 0, 0, 2, 0, 1 };
    HvMouseDriver.packet(ctx, resp, sizeof(resp));

    UINT8 info[256] = { 0 };
    UINT32 body = 8 + 30 + 9 + sizeof(rdesc);
    info[0] = 1; info[4] = (UINT8)body;                      /* pipe: data, size */
    info[8] = 2; info[12] = (UINT8)(body - 8);               /* INITIAL_DEVICE_INFO */
    info[16] = 30; info[18] = 0x5E; info[19] = 0x04; info[20] = 0x21; info[21] = 0x06;   /* 045e:0621 */
    UINT8 *hd = info + 16 + 30;
    hd[0] = 9; hd[1] = 0x21; hd[2] = 0x01; hd[3] = 0x01; hd[5] = 1; hd[6] = 0x22;
    hd[7] = sizeof(rdesc); hd[8] = 0;
    memcpy(hd + 9, rdesc, sizeof(rdesc));
    HvMouseDriver.packet(ctx, info, 8 + body);
    assert(attached_len == (int)sizeof(rdesc) && !memcmp(attached, rdesc, sizeof(rdesc)) && started == 1);
    n = sent(b, &fl);
    assert(n == 24 && rd32(b) == 1 && rd32(b + 4) == 9 && rd32(b + 8) == 3 && rd32(b + 12) == 1 && b[16] == 0);

    UINT8 rep[24] = { 1, 0, 0, 0, 14, 0, 0, 0, 4, 0, 0, 0, 6, 0, 0, 0, 0x01, 0xFF, 0x3F, 0x00, 0x40, 0xFF };
    HvMouseDriver.packet(ctx, rep, sizeof(rep));
    assert(reports == 1 && !memcmp(last_report, rep + 16, 6));
    UINT8 bad[24] = { 2, 0, 0, 0, 14, 0, 0, 0, 4, 0, 0, 0, 6, 0, 0, 0 };   /* not a data pipe message */
    HvMouseDriver.packet(ctx, bad, sizeof(bad));
    assert(reports == 1);
    puts("mouse passed");
}

int main(void)
{
    static UINT8 wire_mem[4 * VMBUS_PAGE];
    vm_ring_init(&wire, wire_mem, 4);
    ring_checks();
    keyboard_checks();
    mouse_checks();
    puts("hv_input: all passed");
    return 0;
}
