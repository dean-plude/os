/*
 * um_hid.c — HID device handles: a game controller opened by its HID path
 *
 * Every controller (drivers/gamepad.h) is a HID device with a path as
 * Windows names its HID collections:
 *
 *     \\?\HID#VID_045E&PID_028E&IG_00#8&SERIAL&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}
 *
 * ("&IG_00" for an Xbox controller; SERIAL the controller's PadInfo.serial
 * in hex, new each time it is plugged in), listed by setupapi, cfgmgr32
 * and Raw Input (RIDI_DEVICENAME).  NtCreateFile on such a path gives a
 * handle that reads the controller's input reports, one per read, each
 * starting with its report ID byte and HIDP_CAPS.InputReportByteLength
 * long, as hid.sys hands them out; up to 32 wait for the reader (the
 * oldest goes first, HidD_SetNumInputBuffers' default), and a read on a
 * handle opened for overlapped I/O finishes when the next one comes.
 *
 * The handle is the client end of a message-type pipe (um_pipe.c) the
 * device poll thread writes each report into, so blocking, overlapped
 * and cancelled reads, waits and closing work as for any pipe.  When the
 * controller is unplugged the pipe breaks: reads fail.  Writing (output
 * reports) is refused; the descriptor, attributes and strings are hid.dll's,
 * through CTL_GAMEPAD (um_gui.c), which asks here which controller a
 * handle is.
 */

#include "um_internal.h"
#include "../drivers/gamepad.h"
#include "../ke/printf.h"
#include "../lib/string.h"

#define ST_SUCCESS               0x00000000u
#define ST_NO_MEMORY             0xC0000017u
#define ST_OBJECT_NAME_NOT_FOUND 0xC0000034u
#define ST_TOO_MANY_OPENED_FILES 0xC000011Fu

#define HID_HANDLES  64
#define HID_BUFFERS  32

typedef struct {
    UmObject *srv;                    /* the end the kernel writes (referenced); NULL: free */
    UINT32    serial;
    int       slot;
} HidOpen;

static UmLock  g_hl;
static HidOpen g_open[HID_HANDLES];
static bool    g_sink_set;

static char lc(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

/* A controller's serial from its HID path (NT prefix removed), or 0 */
static UINT32 serial_of(const char *path)
{
    static const char pre[] = "hid#vid_";
    for (int i = 0; pre[i]; i++) if (lc(path[i]) != pre[i]) return 0;
    const char *s = path + sizeof(pre) - 1;
    while (*s && *s != '#') s++;                                    /* past VID_..&PID_..[&IG_..] */
    if (s[0] != '#' || s[1] != '8' || s[2] != '&') return 0;
    s += 3;
    UINT32 v = 0;
    int n = 0;
    for (; *s && *s != '&'; s++, n++) {
        char c = lc(*s);
        if (c >= '0' && c <= '9') v = v * 16 + (UINT32)(c - '0');
        else if (c >= 'a' && c <= 'f') v = v * 16 + (UINT32)(c - 'a' + 10);
        else return 0;
    }
    return n && n <= 8 && *s == '&' ? v : 0;
}

/* Whether @path (NT prefix removed) names a HID device */
bool um_hid_name(const char *path) { return lc(path[0]) == 'h' && lc(path[1]) == 'i' && lc(path[2]) == 'd' && path[3] == '#'; }

/* Each report to every handle open on its controller; the controller
 * gone: their pipes break */
static void sink(int slot, UINT32 serial, const UINT8 *r, int len)
{
    UmObject *drop[HID_HANDLES];
    int nd = 0;
    um_lock(&g_hl);
    for (int i = 0; i < HID_HANDLES; i++) {
        HidOpen *o = &g_open[i];
        if (!o->srv || o->slot != slot || o->serial != serial) continue;
        if (len < 0 || !um_pipe_feed(o->srv, r, (UINT32)len, HID_BUFFERS)) {   /* gone, or its handle closed */
            drop[nd++] = o->srv;
            o->srv = NULL;
        }
    }
    um_unlock(&g_hl);
    for (int i = 0; i < nd; i++) um_ob_unref(drop[i]);
}

/* NtCreateFile on a HID path: *@out the handle's object (a pipe end) */
UINT32 um_hid_open(const char *path, UINT32 options, UmObject **out)
{
    UINT32 serial = serial_of(path);
    int slot = serial ? PadSlotOfSerial(serial) : -1;
    if (slot < 0) return ST_OBJECT_NAME_NOT_FOUND;
    um_lock(&g_hl);
    if (!g_sink_set) { PadSetRawSink(sink); g_sink_set = true; }
    int at = -1;
    for (int i = 0; i < HID_HANDLES && at < 0; i++) if (!g_open[i].srv) at = i;
    if (at < 0) {                                                 /* drop the closed ones */
        for (int i = 0; i < HID_HANDLES; i++)
            if (g_open[i].srv && !um_pipe_device_open(g_open[i].srv)) { um_ob_unref(g_open[i].srv); g_open[i].srv = NULL; if (at < 0) at = i; }
    }
    if (at < 0) { um_unlock(&g_hl); return ST_TOO_MANY_OPENED_FILES; }
    static UINT32 seq;
    char name[160];
    ksnprintf(name, sizeof(name), "\\Device\\NamedPipe\\hid#%x#%x#%u", slot, serial, ++seq);
    UmObject *srv, *cli;
    UINT32 st = um_pipe_device(name, options, &srv, &cli);
    if (st) { um_unlock(&g_hl); return st; }
    g_open[at].srv = srv;
    g_open[at].serial = serial;
    g_open[at].slot = slot;
    um_unlock(&g_hl);
    if (PadSlotOfSerial(serial) != slot) sink(slot, serial, NULL, -1);   /* unplugged meanwhile */
    *out = cli;
    return ST_SUCCESS;
}

/* The controller a handle of @p reads (its slot), or -1; *@serial its serial */
int um_hid_slot(UmProcess *p, UINT64 h, UINT32 *serial)
{
    UmObject *o = um_handle_object(p, h, UO_PIPE);
    if (!o) return -1;
    char name[96];
    um_pipe_end_name(o, name, sizeof(name));
    um_ob_unref(o);
    if (strncmp(name, "hid#", 4)) return -1;
    UINT32 v[2] = { 0, 0 };
    const char *s = name + 4;
    for (int k = 0; k < 2; k++) {
        for (; *s && *s != '#'; s++) {
            char c = *s;
            v[k] = v[k] * 16 + (UINT32)(c >= 'a' ? c - 'a' + 10 : c - '0');
        }
        if (*s != '#') return -1;
        s++;
    }
    if (PadSlotOfSerial(v[1]) != (int)v[0]) return -1;           /* unplugged */
    if (serial) *serial = v[1];
    return (int)v[0];
}

/* HidD_FlushQueue: the reports waiting on handle @h go */
bool um_hid_flush(UmProcess *p, UINT64 h)
{
    if (um_hid_slot(p, h, NULL) < 0) return false;
    UmObject *o = um_handle_object(p, h, UO_PIPE);
    if (!o) return false;
    um_pipe_device_flush(o);
    um_ob_unref(o);
    return true;
}
