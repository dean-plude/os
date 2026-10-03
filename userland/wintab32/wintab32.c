/*
 * wintab32.dll — Wintab 1.4, the pen tablet interface GTK, Qt, Krita,
 * Inkscape and GIMP use for pressure (written from the Wintab
 * specification; its constants and structures are the spec's).
 *
 * The desktop keeps the last packets of every pen (USB digitizer pens,
 * and synthetic pens made with CreateSyntheticPointerDevice) and hands
 * them out numbered (NtNovaGuiCtl op 30, kernel/wm/tablet.h).  With no pen
 * present WTInfo(0, 0, NULL) is 0, which programs read as "no Wintab", and
 * WTOpen fails.  With one, there is one device with two cursors (0 the pen
 * tip, 1 its eraser), X and Y 0-65535 (Y up, as on a tablet), pressure
 * 0-1023 and three buttons (the tip and two barrel buttons).
 *
 * A context opened with WTOpen gets the packets that come while it is
 * enabled and one of this process's windows is in front, in its own queue
 * (WTQueueSizeSet), mapped from the tablet to its output extents and laid
 * out as its lcPktData asks (lcPktMode: buttons and pressure relative to
 * the last packet); with CXO_MESSAGES its window is posted WT_PACKET for
 * each, WT_PROXIMITY when the pen comes near or leaves, and with
 * CXO_CSRMESSAGES WT_CSRCHANGE when the pen turns over to its eraser.  A
 * thread of the DLL waits for the desktop's packets while a context is
 * open.
 */
#include <windows.h>
#include <winternl.h>

#define WTAPI __declspec(dllexport)

void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);

/* ---- the spec's types and numbers ---------------------------------- */
typedef DWORD WTPKT;
typedef DWORD FIX32;
typedef struct Ctx *HCTX;
typedef struct { LONG axMin, axMax; UINT axUnits; FIX32 axResolution; } AXIS;

#define LCNAMELEN 40
typedef struct {
    char  lcName[LCNAMELEN];
    UINT  lcOptions, lcStatus, lcLocks, lcMsgBase, lcDevice, lcPktRate;
    WTPKT lcPktData, lcPktMode, lcMoveMask;
    DWORD lcBtnDnMask, lcBtnUpMask;
    LONG  lcInOrgX, lcInOrgY, lcInOrgZ, lcInExtX, lcInExtY, lcInExtZ;
    LONG  lcOutOrgX, lcOutOrgY, lcOutOrgZ, lcOutExtX, lcOutExtY, lcOutExtZ;
    FIX32 lcSensX, lcSensY, lcSensZ;
    BOOL  lcSysMode;
    int   lcSysOrgX, lcSysOrgY, lcSysExtX, lcSysExtY;
    FIX32 lcSysSensX, lcSysSensY;
} LOGCONTEXTA;
typedef struct {
    WCHAR lcName[LCNAMELEN];
    UINT  lcOptions, lcStatus, lcLocks, lcMsgBase, lcDevice, lcPktRate;
    WTPKT lcPktData, lcPktMode, lcMoveMask;
    DWORD lcBtnDnMask, lcBtnUpMask;
    LONG  lcInOrgX, lcInOrgY, lcInOrgZ, lcInExtX, lcInExtY, lcInExtZ;
    LONG  lcOutOrgX, lcOutOrgY, lcOutOrgZ, lcOutExtX, lcOutExtY, lcOutExtZ;
    FIX32 lcSensX, lcSensY, lcSensZ;
    BOOL  lcSysMode;
    int   lcSysOrgX, lcSysOrgY, lcSysExtX, lcSysExtY;
    FIX32 lcSysSensX, lcSysSensY;
} LOGCONTEXTW;

#define WTI_INTERFACE   1
#define WTI_STATUS      2
#define WTI_DEFCONTEXT  3
#define WTI_DEFSYSCTX   4
#define WTI_DEVICES     100
#define WTI_CURSORS     200
#define WTI_EXTENSIONS  300
#define WTI_DDCTXS      400
#define WTI_DSCTXS      500

#define IFC_WINTABID 1
#define IFC_SPECVERSION 2
#define IFC_IMPLVERSION 3
#define IFC_NDEVICES 4
#define IFC_NCURSORS 5
#define IFC_NCONTEXTS 6
#define IFC_CTXOPTIONS 7
#define IFC_CTXSAVESIZE 8
#define IFC_NEXTENSIONS 9
#define IFC_NMANAGERS 10

#define STA_CONTEXTS 1
#define STA_SYSCTXS 2
#define STA_PKTRATE 3
#define STA_PKTDATA 4
#define STA_MANAGERS 5
#define STA_SYSTEM 6
#define STA_BUTTONUSE 7
#define STA_SYSBTNUSE 8

#define DVC_NAME 1
#define DVC_HARDWARE 2
#define DVC_NCSRTYPES 3
#define DVC_FIRSTCSR 4
#define DVC_PKTRATE 5
#define DVC_PKTDATA 6
#define DVC_PKTMODE 7
#define DVC_CSRDATA 8
#define DVC_XMARGIN 9
#define DVC_YMARGIN 10
#define DVC_ZMARGIN 11
#define DVC_X 12
#define DVC_Y 13
#define DVC_Z 14
#define DVC_NPRESSURE 15
#define DVC_TPRESSURE 16
#define DVC_ORIENTATION 17
#define DVC_ROTATION 18
#define DVC_PNPID 19

#define CSR_NAME 1
#define CSR_ACTIVE 2
#define CSR_PKTDATA 3
#define CSR_BUTTONS 4
#define CSR_BUTTONBITS 5
#define CSR_BTNNAMES 6
#define CSR_BUTTONMAP 7
#define CSR_SYSBTNMAP 8
#define CSR_NPBUTTON 9
#define CSR_NPBTNMARKS 10
#define CSR_NPRESPONSE 11
#define CSR_TPBUTTON 12
#define CSR_TPBTNMARKS 13
#define CSR_TPRESPONSE 14
#define CSR_PHYSID 15
#define CSR_MODE 16
#define CSR_MINPKTDATA 17
#define CSR_MINBUTTONS 18
#define CSR_CAPABILITIES 19
#define CSR_TYPE 20

#define PK_CONTEXT          0x0001
#define PK_STATUS           0x0002
#define PK_TIME             0x0004
#define PK_CHANGED          0x0008
#define PK_SERIAL_NUMBER    0x0010
#define PK_CURSOR           0x0020
#define PK_BUTTONS          0x0040
#define PK_X                0x0080
#define PK_Y                0x0100
#define PK_Z                0x0200
#define PK_NORMAL_PRESSURE  0x0400
#define PK_TANGENT_PRESSURE 0x0800
#define PK_ORIENTATION      0x1000
#define PK_ROTATION         0x2000
#define PK_ALL              0x3FFF

#define CXO_SYSTEM      0x0001
#define CXO_PEN         0x0002
#define CXO_MESSAGES    0x0004
#define CXO_CSRMESSAGES 0x0008
#define CXS_DISABLED    0x0001
#define CXS_OBSCURED    0x0002
#define CXS_ONTOP       0x0004
#define HWC_HARDPROX    0x0004
#define TU_NONE         0
#define TU_INCHES       1
#define TU_CIRCLE       3
#define TBN_NONE        0
#define TBN_UP          1
#define TBN_DOWN        2
#define TPS_PROXIMITY   0x0001
#define TPS_QUEUE_ERR   0x0002
#define TPS_INVERT      0x0010
#define CRC_INVERT      0x0004

#define WT_DEFBASE      0x7FF0
#define WT_PACKET       0
#define WT_CTXOPEN      1
#define WT_CTXCLOSE     2
#define WT_CTXOVERLAP   4
#define WT_PROXIMITY    5
#define WT_CSRCHANGE    7

/* ---- the desktop's side (kernel/wm/tablet.h) ------------------------ */
#define CTL_TABLET 30
typedef struct {
    UINT32 serial, time;
    INT32  x, y;                     /* 0-65535 across the desktop, y down */
    UINT16 pressure;                 /* 0-1023 */
    UINT8  buttons, flags;           /* flags: 1 in range, 2 eraser */
} KPacket;

static int devices(void) { return (int)NtNovaGuiCtl(0, CTL_TABLET, 0, NULL); }

/* ---- the tablet as Wintab describes it ------------------------------ */
#define EXT        65536             /* tablet units across X and Y */
#define NPRESS     1023
#define NBUTTONS   3
#define CURSORS    2                 /* 0 the pen, 1 its eraser */
#define QDEFAULT   8
#define QMAX       128

static const AXIS g_axis_xy = { 0, EXT - 1, TU_INCHES, 6553u << 16 };   /* a 10-inch-wide tablet */
static const AXIS g_axis_np = { 0, NPRESS, TU_NONE, 0 };
static const AXIS g_axis_none = { 0, 0, TU_NONE, 0 };

/* A packet with every field, before a context picks its own */
typedef struct {
    UINT  serial, status, cursor;
    DWORD time, changed, buttons;    /* buttons: as the context's lcPktMode has them */
    LONG  x, y, z;
    int   npress, tpress;
    int   orient[3], rot[3];
} Pkt;

typedef struct Ctx {
    DWORD        magic;
    struct Ctx  *next;
    HWND         hwnd;
    LOGCONTEXTW  lc;
    Pkt         *q;
    int          qsize, head, n;     /* a ring: n packets from head */
    int          lost;               /* packets dropped from a full queue (TPS_QUEUE_ERR) */
    /* the last packet this context was given (relative fields, changes) */
    DWORD        last_buttons;
    int          last_press, last_cursor, near;
    LONG         last_x, last_y;
} Ctx;
#define CTX_MAGIC 0x78744357        /* "WCtx" */

static CRITICAL_SECTION g_lock;
static Ctx   *g_ctxs;               /* most recently overlapped on top first */
static HANDLE g_thread;
static volatile LONG g_stop;
static UINT32 g_after;              /* the last packet the thread took */

static Ctx *ctx_of(HCTX h)
{
    for (Ctx *c = g_ctxs; c; c = c->next) if (c == h && c->magic == CTX_MAGIC) return c;
    return NULL;
}

/* ---- WTInfo --------------------------------------------------------- */
static void default_context(LOGCONTEXTW *lc, int sys)
{
    static const char a[] = "NovaOS Pen", b[] = "NovaOS Pen (system)";
    memset(lc, 0, sizeof(*lc));
    const char *n = sys ? b : a;
    for (int i = 0; n[i] && i < LCNAMELEN - 1; i++) lc->lcName[i] = (WCHAR)n[i];
    lc->lcOptions = sys ? CXO_SYSTEM : 0;
    lc->lcStatus = CXS_ONTOP;
    lc->lcMsgBase = WT_DEFBASE;
    lc->lcPktRate = 100;
    lc->lcPktData = PK_CONTEXT | PK_STATUS | PK_TIME | PK_CHANGED | PK_SERIAL_NUMBER | PK_CURSOR | PK_BUTTONS |
                    PK_X | PK_Y | PK_NORMAL_PRESSURE;
    lc->lcMoveMask = lc->lcPktData;
    lc->lcBtnDnMask = lc->lcBtnUpMask = 0xFFFFFFFF;
    lc->lcInExtX = lc->lcInExtY = EXT;
    lc->lcOutExtX = lc->lcOutExtY = EXT;
    lc->lcSensX = lc->lcSensY = lc->lcSensZ = 0x10000;
    lc->lcSysOrgX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    lc->lcSysOrgY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    lc->lcSysExtX = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    lc->lcSysExtY = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    lc->lcSysSensX = lc->lcSysSensY = 0x10000;
}

static void ctx_w2a(const LOGCONTEXTW *w, LOGCONTEXTA *a)
{
    for (int i = 0; i < LCNAMELEN; i++) a->lcName[i] = (char)(w->lcName[i] < 0x80 ? w->lcName[i] : '?');
    memcpy(&a->lcOptions, &w->lcOptions, sizeof(*w) - sizeof(w->lcName));
}

static void ctx_a2w(const LOGCONTEXTA *a, LOGCONTEXTW *w)
{
    for (int i = 0; i < LCNAMELEN; i++) w->lcName[i] = (BYTE)a->lcName[i];
    memcpy(&w->lcOptions, &a->lcOptions, sizeof(*w) - sizeof(w->lcName));
}

/* One answer: @data, @size bytes (a string: @str, ANSI or wide by @wide) */
typedef struct { BYTE buf[sizeof(LOGCONTEXTW)]; UINT size; } Answer;

static void put(Answer *r, const void *p, UINT size) { memcpy(r->buf, p, size); r->size = size; }
static void put_uint(Answer *r, UINT v) { put(r, &v, sizeof(v)); }
static void put_str(Answer *r, const char *s, int wide)
{
    UINT n = 0;
    while (s[n]) n++;
    if (wide) { for (UINT i = 0; i <= n; i++) ((WCHAR *)r->buf)[i] = (BYTE)s[i]; r->size = (n + 1) * 2; }
    else { memcpy(r->buf, s, n + 1); r->size = n + 1; }
}

/* WTInfo's answer for (cat, index); 0 if there is none */
static UINT info(UINT cat, UINT idx, int wide, Answer *r)
{
    int ndev = devices() > 0;
    r->size = 0;
    if (cat == 0) return ndev ? sizeof(LOGCONTEXTW) : 0;    /* the largest answer; 0: no Wintab here */
    if (cat == WTI_INTERFACE) {
        switch (idx) {
        case IFC_WINTABID: put_str(r, "NovaOS Wintab", wide); break;
        case IFC_SPECVERSION: { WORD v = 0x0104; put(r, &v, 2); break; }
        case IFC_IMPLVERSION: { WORD v = 0x0100; put(r, &v, 2); break; }
        case IFC_NDEVICES: put_uint(r, (UINT)ndev); break;
        case IFC_NCURSORS: put_uint(r, ndev ? CURSORS : 0); break;
        case IFC_NCONTEXTS: put_uint(r, 64); break;
        case IFC_CTXOPTIONS: put_uint(r, CXO_SYSTEM | CXO_PEN | CXO_MESSAGES | CXO_CSRMESSAGES); break;
        case IFC_CTXSAVESIZE: put_uint(r, sizeof(LOGCONTEXTW)); break;
        case IFC_NEXTENSIONS: case IFC_NMANAGERS: put_uint(r, 0); break;
        }
        return r->size;
    }
    if (cat == WTI_STATUS) {
        int nctx = 0;
        EnterCriticalSection(&g_lock);
        for (Ctx *c = g_ctxs; c; c = c->next) nctx++;
        LeaveCriticalSection(&g_lock);
        switch (idx) {
        case STA_CONTEXTS: case STA_SYSCTXS: put_uint(r, (UINT)nctx); break;
        case STA_PKTRATE: put_uint(r, 100); break;
        case STA_PKTDATA: { WTPKT v = PK_ALL; put(r, &v, sizeof(v)); break; }
        case STA_MANAGERS: case STA_SYSTEM: put_uint(r, 0); break;
        case STA_BUTTONUSE: case STA_SYSBTNUSE: { DWORD v = (1u << NBUTTONS) - 1; put(r, &v, sizeof(v)); break; }
        }
        return r->size;
    }
    if (cat == WTI_DEFCONTEXT || cat == WTI_DEFSYSCTX || (ndev && (cat == WTI_DDCTXS || cat == WTI_DSCTXS))) {
        LOGCONTEXTW lc;
        default_context(&lc, cat == WTI_DEFSYSCTX || cat == WTI_DSCTXS);
        if (idx == 0) {
            if (wide) put(r, &lc, sizeof(lc));
            else { LOGCONTEXTA a; ctx_w2a(&lc, &a); put(r, &a, sizeof(a)); }
        } else if (idx == 1) {                              /* CTX_NAME */
            char n[LCNAMELEN];
            for (int i = 0; i < LCNAMELEN; i++) n[i] = (char)lc.lcName[i];
            put_str(r, n, wide);
        } else if (idx <= 34) {                             /* CTX_OPTIONS..CTX_SYSSENSY: the fields in order */
            put(r, (const BYTE *)&lc.lcOptions + (idx - 2) * 4, 4);
        }
        return r->size;
    }
    if (cat == WTI_DEVICES) {                               /* device 0 only */
        if (!ndev) return 0;
        switch (idx) {
        case DVC_NAME: put_str(r, "NovaOS Pen Tablet", wide); break;
        case DVC_HARDWARE: put_uint(r, HWC_HARDPROX); break;
        case DVC_NCSRTYPES: put_uint(r, CURSORS); break;
        case DVC_FIRSTCSR: put_uint(r, 0); break;
        case DVC_PKTRATE: put_uint(r, 100); break;
        case DVC_PKTDATA: { WTPKT v = PK_ALL; put(r, &v, sizeof(v)); break; }
        case DVC_PKTMODE: { WTPKT v = 0; put(r, &v, sizeof(v)); break; }
        case DVC_CSRDATA: { WTPKT v = 0; put(r, &v, sizeof(v)); break; }
        case DVC_XMARGIN: case DVC_YMARGIN: case DVC_ZMARGIN: put_uint(r, 0); break;
        case DVC_X: case DVC_Y: put(r, &g_axis_xy, sizeof(AXIS)); break;
        case DVC_Z: case DVC_TPRESSURE: put(r, &g_axis_none, sizeof(AXIS)); break;
        case DVC_NPRESSURE: put(r, &g_axis_np, sizeof(AXIS)); break;
        case DVC_ORIENTATION: case DVC_ROTATION: {          /* no tilt or rotation */
            AXIS a[3] = { g_axis_none, g_axis_none, g_axis_none };
            put(r, a, sizeof(a));
            break;
        }
        case DVC_PNPID: put_str(r, "NOVA0001", wide); break;
        }
        return r->size;
    }
    if (cat >= WTI_CURSORS && cat < WTI_CURSORS + CURSORS) {
        if (!ndev) return 0;
        int eraser = cat - WTI_CURSORS == 1;
        switch (idx) {
        case CSR_NAME: put_str(r, eraser ? "Eraser" : "Pressure Stylus", wide); break;
        case CSR_ACTIVE: put_uint(r, 1); break;
        case CSR_PKTDATA: case CSR_MINPKTDATA: { WTPKT v = PK_ALL; put(r, &v, sizeof(v)); break; }
        case CSR_BUTTONS: case CSR_BUTTONBITS: case CSR_MINBUTTONS: { BYTE v = idx == CSR_MINBUTTONS ? 1 : NBUTTONS; put(r, &v, 1); break; }
        case CSR_BTNNAMES: {                                /* NUL-separated, ending in two NULs */
            static const char names[] = "Tip\0Lower Barrel\0Upper Barrel\0";
            if (wide) { for (UINT i = 0; i < sizeof(names); i++) ((WCHAR *)r->buf)[i] = (BYTE)names[i]; r->size = sizeof(names) * 2; }
            else put(r, names, sizeof(names));
            break;
        }
        case CSR_BUTTONMAP: case CSR_SYSBTNMAP: {
            BYTE m[32];
            memset(m, 0, sizeof(m));
            for (int i = 0; i < NBUTTONS; i++) m[i] = (BYTE)i;
            put(r, m, sizeof(m));
            break;
        }
        case CSR_NPBUTTON: { BYTE v = 0; put(r, &v, 1); break; }
        case CSR_NPBTNMARKS: { UINT m[2] = { 0, 1 }; put(r, m, sizeof(m)); break; }
        case CSR_NPRESPONSE: { UINT m[2] = { 0, NPRESS }; put(r, m, sizeof(m)); break; }
        case CSR_TPBUTTON: { BYTE v = 0xFF; put(r, &v, 1); break; }
        case CSR_TPBTNMARKS: case CSR_TPRESPONSE: { UINT m[2] = { 0, 0 }; put(r, m, sizeof(m)); break; }
        case CSR_PHYSID: { DWORD v = 1; put(r, &v, sizeof(v)); break; }
        case CSR_MODE: put_uint(r, (UINT)(cat - WTI_CURSORS)); break;
        case CSR_CAPABILITIES: put_uint(r, CRC_INVERT); break;
        case CSR_TYPE: put_uint(r, eraser ? 0x080A : 0x0802); break;   /* a general stylus, its eraser */
        }
        return r->size;
    }
    return 0;
}

static UINT do_info(UINT cat, UINT idx, LPVOID out, int wide)
{
    static Answer r;                                        /* (under g_lock) */
    EnterCriticalSection(&g_lock);
    UINT n = info(cat, idx, wide, &r);
    if (out && n && cat) memcpy(out, r.buf, n);
    LeaveCriticalSection(&g_lock);
    return n;
}

WTAPI UINT WINAPI WTInfoA(UINT cat, UINT idx, LPVOID out) { return do_info(cat, idx, out, 0); }
WTAPI UINT WINAPI WTInfoW(UINT cat, UINT idx, LPVOID out) { return do_info(cat, idx, out, 1); }

/* ---- packets -------------------------------------------------------- */
/* A tablet coordinate in a context's output space; a negative extent on one
 * side mirrors the axis */
static LONG scale(LONG v, LONG inorg, LONG inext, LONG outorg, LONG outext)
{
    LONG ia = inext < 0 ? -inext : inext, oa = outext < 0 ? -outext : outext;
    if (!ia) return outorg;
    LONG d = v - inorg;
    if (d < 0) d = 0;
    if (d > ia - 1) d = ia - 1;
    if ((inext < 0) != (outext < 0)) d = ia - 1 - d;
    return outorg + (LONG)((long long)d * oa / ia);
}

/* The packet @k as @c gets it */
static Pkt make_packet(Ctx *c, const KPacket *k)
{
    Pkt p;
    memset(&p, 0, sizeof(p));
    p.serial = k->serial;
    p.time = k->time;
    p.cursor = k->flags & 2 ? 1 : 0;
    p.status = (k->flags & 2 ? TPS_INVERT : 0) | (c->lost ? TPS_QUEUE_ERR : 0);
    LONG tx = k->x, ty = EXT - 1 - k->y;                    /* the tablet's Y is up */
    p.x = scale(tx, c->lc.lcInOrgX, c->lc.lcInExtX, c->lc.lcOutOrgX, c->lc.lcOutExtX);
    p.y = scale(ty, c->lc.lcInOrgY, c->lc.lcInExtY, c->lc.lcOutOrgY, c->lc.lcOutExtY);
    p.orient[1] = 900;                                      /* upright */
    DWORD b = k->buttons, before = c->last_buttons;
    if (c->lc.lcPktMode & PK_BUTTONS) {                     /* relative: one button's change a packet */
        DWORD ch = b ^ c->last_buttons;
        p.buttons = TBN_NONE;
        for (int i = 0; i < 32; i++)
            if (ch & (1u << i)) {
                p.buttons = (DWORD)i | (DWORD)(b & (1u << i) ? TBN_DOWN : TBN_UP) << 16;
                c->last_buttons ^= 1u << i;
                break;
            }
    } else {
        p.buttons = b;
        c->last_buttons = b;
    }
    p.npress = c->lc.lcPktMode & PK_NORMAL_PRESSURE ? k->pressure - c->last_press : k->pressure;
    p.changed = (p.x != c->last_x ? PK_X : 0) | (p.y != c->last_y ? PK_Y : 0) |
                (k->pressure != c->last_press ? PK_NORMAL_PRESSURE : 0) |
                ((int)p.cursor != c->last_cursor ? PK_CURSOR : 0) | (c->last_buttons != before ? PK_BUTTONS : 0) |
                PK_SERIAL_NUMBER | PK_TIME;
    c->last_press = k->pressure;
    c->last_x = p.x; c->last_y = p.y;
    return p;
}

/* The packet laid out as @c's lcPktData says: the fields in bit order, each
 * at its natural alignment, as the PACKET struct of pktdef.h is.  Returns
 * the size. */
static UINT layout(const Ctx *c, const Pkt *p, BYTE *out)
{
    WTPKT d = c->lc.lcPktData;
    UINT off = 0, align = 4;
#define FIELD(bit, type, val) \
    if (d & (bit)) { off = (off + sizeof(type) - 1) & ~(UINT)(sizeof(type) - 1); \
                     if (out) { type v_ = (type)(val); memcpy(out + off, &v_, sizeof(type)); } off += sizeof(type); }
    if (d & PK_CONTEXT) align = sizeof(HCTX);
    FIELD(PK_CONTEXT, ULONG_PTR, (ULONG_PTR)c)
    FIELD(PK_STATUS, UINT, p ? p->status : 0)
    FIELD(PK_TIME, DWORD, p ? p->time : 0)
    FIELD(PK_CHANGED, DWORD, p ? p->changed : 0)
    FIELD(PK_SERIAL_NUMBER, UINT, p ? p->serial : 0)
    FIELD(PK_CURSOR, UINT, p ? p->cursor : 0)
    FIELD(PK_BUTTONS, DWORD, p ? p->buttons : 0)
    FIELD(PK_X, LONG, p ? p->x : 0)
    FIELD(PK_Y, LONG, p ? p->y : 0)
    FIELD(PK_Z, LONG, p ? p->z : 0)
    FIELD(PK_NORMAL_PRESSURE, UINT, p ? p->npress : 0)
    FIELD(PK_TANGENT_PRESSURE, UINT, p ? p->tpress : 0)
    if (d & PK_ORIENTATION) { for (int i = 0; i < 3; i++) { if (out) memcpy(out + off, &p->orient[i], 4); off += 4; } }
    if (d & PK_ROTATION) { for (int i = 0; i < 3; i++) { if (out) memcpy(out + off, &p->rot[i], 4); off += 4; } }
#undef FIELD
    return (off + align - 1) & ~(align - 1);
}

static UINT pkt_size(const Ctx *c) { return layout(c, NULL, NULL); }

/* Is one of this process's windows in front (or none at all)? */
static int ours_in_front(void)
{
    HWND f = GetForegroundWindow();
    DWORD pid = 0;
    if (!f) return 1;
    GetWindowThreadProcessId(f, &pid);
    return pid == GetCurrentProcessId();
}

static void post(Ctx *c, UINT msg, WPARAM wp, LPARAM lp)
{
    if (c->lc.lcOptions & CXO_MESSAGES) PostMessageW(c->hwnd, c->lc.lcMsgBase + msg, wp, lp);
}

/* A packet from the desktop for every enabled context (g_lock held) */
static void deliver(const KPacket *k)
{
    int front = ours_in_front();
    for (Ctx *c = g_ctxs; c; c = c->next) {
        if (c->lc.lcStatus & CXS_DISABLED) continue;
        int near = (k->flags & 1) && front;
        if (near != c->near) {                              /* coming near / going away */
            c->near = near;
            post(c, WT_PROXIMITY, (WPARAM)c, MAKELPARAM(near, k->flags & 1));
            if (!near) c->last_cursor = -1;
        }
        if (!near) continue;
        int csr = k->flags & 2 ? 1 : 0;
        int csr_changed = csr != c->last_cursor;
        Pkt p = make_packet(c, k);
        c->last_cursor = csr;
        if (c->n == c->qsize) {                             /* full: the oldest goes */
            c->head = (c->head + 1) % c->qsize;
            c->n--;
            c->lost = 1;
        }
        c->q[(c->head + c->n) % c->qsize] = p;
        c->n++;
        if (csr_changed && (c->lc.lcOptions & CXO_CSRMESSAGES)) post(c, WT_CSRCHANGE, p.serial, (LPARAM)c);
        post(c, WT_PACKET, p.serial, (LPARAM)c);
    }
}

/* The thread that takes the desktop's packets while contexts are open */
static DWORD WINAPI reader(LPVOID unused)
{
    (void)unused;
    struct { UINT32 after, max, wait_ms, newest; KPacket p[32]; } b;
    while (!g_stop) {
        b.after = g_after; b.max = 32; b.wait_ms = 200; b.newest = 0;
        int n = (int)NtNovaGuiCtl(0, CTL_TABLET, 1, &b);
        if (n <= 0) continue;
        EnterCriticalSection(&g_lock);
        for (int i = 0; i < n && i < 32; i++) {
            if (b.p[i].serial <= g_after) continue;
            g_after = b.p[i].serial;
            deliver(&b.p[i]);
        }
        LeaveCriticalSection(&g_lock);
    }
    return 0;
}

/* ---- contexts ------------------------------------------------------- */
static HCTX open_ctx(HWND hwnd, const LOGCONTEXTW *lc, BOOL enable)
{
    if (!lc || devices() <= 0) return NULL;
    Ctx *c = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Ctx));
    Pkt *q = HeapAlloc(GetProcessHeap(), 0, sizeof(Pkt) * QDEFAULT);
    if (!c || !q) { if (c) HeapFree(GetProcessHeap(), 0, c); if (q) HeapFree(GetProcessHeap(), 0, q); return NULL; }
    c->magic = CTX_MAGIC;
    c->hwnd = hwnd;
    c->lc = *lc;
    if (!c->lc.lcMsgBase) c->lc.lcMsgBase = WT_DEFBASE;
    c->lc.lcStatus = (enable ? 0 : CXS_DISABLED) | CXS_ONTOP;
    c->q = q;
    c->qsize = QDEFAULT;
    c->last_cursor = -1;
    EnterCriticalSection(&g_lock);
    for (Ctx *o = g_ctxs; o; o = o->next) o->lc.lcStatus &= ~CXS_ONTOP;
    c->next = g_ctxs;
    g_ctxs = c;
    if (!g_thread) {                                        /* packets from now on */
        struct { UINT32 after, max, wait_ms, newest; } b = { 0, 0, 0, 0 };
        NtNovaGuiCtl(0, CTL_TABLET, 1, &b);
        g_after = b.newest;
        g_stop = 0;
        g_thread = CreateThread(NULL, 0, reader, NULL, 0, NULL);
    }
    LeaveCriticalSection(&g_lock);
    post(c, WT_CTXOPEN, (WPARAM)c, c->lc.lcStatus);
    return c;
}

WTAPI HCTX WINAPI WTOpenW(HWND hwnd, LOGCONTEXTW *lc, BOOL enable) { return open_ctx(hwnd, lc, enable); }
WTAPI HCTX WINAPI WTOpenA(HWND hwnd, LOGCONTEXTA *lc, BOOL enable)
{
    if (!lc) return NULL;
    LOGCONTEXTW w;
    ctx_a2w(lc, &w);
    return open_ctx(hwnd, &w, enable);
}

WTAPI BOOL WINAPI WTClose(HCTX h)
{
    EnterCriticalSection(&g_lock);
    Ctx *c = ctx_of(h);
    if (!c) { LeaveCriticalSection(&g_lock); return FALSE; }
    for (Ctx **pp = &g_ctxs; *pp; pp = &(*pp)->next) if (*pp == c) { *pp = c->next; break; }
    HANDLE t = NULL;
    if (!g_ctxs && g_thread) { t = g_thread; g_thread = NULL; g_stop = 1; }
    LeaveCriticalSection(&g_lock);
    post(c, WT_CTXCLOSE, (WPARAM)c, c->lc.lcStatus);
    if (t) { WaitForSingleObject(t, 2000); CloseHandle(t); }
    c->magic = 0;
    HeapFree(GetProcessHeap(), 0, c->q);
    HeapFree(GetProcessHeap(), 0, c);
    return TRUE;
}

/* Take (or look at) up to @max packets from the front of @c's queue; with
 * @upto, only those numbered up to it */
static int take(HCTX h, int max, LPVOID buf, int remove, UINT first, UINT upto, int ranged)
{
    EnterCriticalSection(&g_lock);
    Ctx *c = ctx_of(h);
    if (!c) { LeaveCriticalSection(&g_lock); return 0; }
    UINT sz = pkt_size(c);
    int got = 0, skip = 0;
    for (int i = 0; i < c->n && got < max; i++) {
        Pkt *p = &c->q[(c->head + i) % c->qsize];
        if (ranged && p->serial < first) { skip = i + 1; continue; }
        if (ranged && p->serial > upto) break;
        if (buf) layout(c, p, (BYTE *)buf + (size_t)got * sz);
        got++;
    }
    if (remove) {
        int k = skip + got;
        c->head = (c->head + k) % c->qsize;
        c->n -= k;
        if (k) c->lost = 0;
    }
    LeaveCriticalSection(&g_lock);
    return got;
}

WTAPI int WINAPI WTPacketsGet(HCTX h, int max, LPVOID buf) { return take(h, max, buf, 1, 0, 0, 0); }
WTAPI int WINAPI WTPacketsPeek(HCTX h, int max, LPVOID buf) { return take(h, max, buf, 0, 0, 0, 0); }

/* The packet numbered @serial: it and every older one leave the queue */
WTAPI BOOL WINAPI WTPacket(HCTX h, UINT serial, LPVOID buf)
{
    EnterCriticalSection(&g_lock);
    Ctx *c = ctx_of(h);
    BOOL found = FALSE;
    if (c) {
        int k = 0;
        for (; k < c->n; k++) {
            Pkt *p = &c->q[(c->head + k) % c->qsize];
            if (p->serial == serial) { if (buf) layout(c, p, buf); found = TRUE; k++; break; }
            if (p->serial > serial) break;
        }
        c->head = (c->head + k) % c->qsize;
        c->n -= k;
        if (found) c->lost = 0;
    }
    LeaveCriticalSection(&g_lock);
    return found;
}

/* The packets numbered first..last (older ones leave the queue too) */
WTAPI int WINAPI WTDataGet(HCTX h, UINT first, UINT last, int max, LPVOID buf, LPINT n)
{
    int got = take(h, max, buf, 1, first, last, 1);
    if (n) *n = got;
    return got;
}

WTAPI int WINAPI WTDataPeek(HCTX h, UINT first, UINT last, int max, LPVOID buf, LPINT n)
{
    int got = take(h, max, buf, 0, first, last, 1);
    if (n) *n = got;
    return got;
}

WTAPI BOOL WINAPI WTQueuePacketsEx(HCTX h, UINT *oldest, UINT *newest)
{
    EnterCriticalSection(&g_lock);
    Ctx *c = ctx_of(h);
    BOOL r = c && c->n;
    if (r) {
        if (oldest) *oldest = c->q[c->head].serial;
        if (newest) *newest = c->q[(c->head + c->n - 1) % c->qsize].serial;
    }
    LeaveCriticalSection(&g_lock);
    return r;
}

/* Wintab 1.0's: the oldest and newest packet numbers packed in a DWORD */
WTAPI DWORD WINAPI WTQueuePackets(HCTX h)
{
    UINT o = 0, n = 0;
    if (!WTQueuePacketsEx(h, &o, &n)) return 0;
    return MAKELONG(o, n);
}

WTAPI int WINAPI WTQueueSizeGet(HCTX h)
{
    EnterCriticalSection(&g_lock);
    Ctx *c = ctx_of(h);
    int n = c ? c->qsize : 0;
    LeaveCriticalSection(&g_lock);
    return n;
}

WTAPI BOOL WINAPI WTQueueSizeSet(HCTX h, int n)
{
    if (n < 1 || n > QMAX) return FALSE;
    Pkt *q = HeapAlloc(GetProcessHeap(), 0, sizeof(Pkt) * (size_t)n);
    if (!q) return FALSE;
    EnterCriticalSection(&g_lock);
    Ctx *c = ctx_of(h);
    if (!c) { LeaveCriticalSection(&g_lock); HeapFree(GetProcessHeap(), 0, q); return FALSE; }
    HeapFree(GetProcessHeap(), 0, c->q);
    c->q = q; c->qsize = n; c->head = c->n = 0;              /* the queue's packets are dropped, as the spec says */
    LeaveCriticalSection(&g_lock);
    return TRUE;
}

WTAPI BOOL WINAPI WTEnable(HCTX h, BOOL on)
{
    EnterCriticalSection(&g_lock);
    Ctx *c = ctx_of(h);
    if (c) {
        if (on) c->lc.lcStatus &= ~CXS_DISABLED;
        else { c->lc.lcStatus |= CXS_DISABLED; c->head = c->n = 0; c->near = 0; }
    }
    LeaveCriticalSection(&g_lock);
    return c != NULL;
}

/* To the top of the overlap order (or the bottom) */
WTAPI BOOL WINAPI WTOverlap(HCTX h, BOOL top)
{
    EnterCriticalSection(&g_lock);
    Ctx *c = ctx_of(h);
    if (c) {
        for (Ctx **pp = &g_ctxs; *pp; pp = &(*pp)->next) if (*pp == c) { *pp = c->next; break; }
        if (top || !g_ctxs) { c->next = g_ctxs; g_ctxs = c; }
        else { Ctx *t = g_ctxs; while (t->next) t = t->next; t->next = c; c->next = NULL; }
        for (Ctx *o = g_ctxs; o; o = o->next) o->lc.lcStatus = (o->lc.lcStatus & ~CXS_ONTOP) | (o == g_ctxs ? CXS_ONTOP : 0);
    }
    LeaveCriticalSection(&g_lock);
    if (c) post(c, WT_CTXOVERLAP, (WPARAM)c, c->lc.lcStatus);
    return c != NULL;
}

WTAPI BOOL WINAPI WTGetW(HCTX h, LOGCONTEXTW *lc)
{
    EnterCriticalSection(&g_lock);
    Ctx *c = ctx_of(h);
    if (c && lc) *lc = c->lc;
    LeaveCriticalSection(&g_lock);
    return c && lc;
}

WTAPI BOOL WINAPI WTGetA(HCTX h, LOGCONTEXTA *lc)
{
    LOGCONTEXTW w;
    if (!lc || !WTGetW(h, &w)) return FALSE;
    ctx_w2a(&w, lc);
    return TRUE;
}

static BOOL set_ctx(HCTX h, const LOGCONTEXTW *lc)
{
    EnterCriticalSection(&g_lock);
    Ctx *c = ctx_of(h);
    if (c && lc) {
        UINT status = c->lc.lcStatus;
        c->lc = *lc;
        c->lc.lcStatus = status;                            /* (the status is not the program's to set) */
        if (!c->lc.lcMsgBase) c->lc.lcMsgBase = WT_DEFBASE;
        c->head = c->n = 0;
    }
    LeaveCriticalSection(&g_lock);
    return c && lc;
}

WTAPI BOOL WINAPI WTSetW(HCTX h, LOGCONTEXTW *lc) { return set_ctx(h, lc); }
WTAPI BOOL WINAPI WTSetA(HCTX h, LOGCONTEXTA *lc)
{
    if (!lc) return FALSE;
    LOGCONTEXTW w;
    ctx_a2w(lc, &w);
    return set_ctx(h, &w);
}

/* No configuration dialogs, extensions or managers */
WTAPI BOOL WINAPI WTConfig(HCTX h, HWND w) { (void)h; (void)w; return FALSE; }
WTAPI BOOL WINAPI WTExtGet(HCTX h, UINT ext, LPVOID data) { (void)h; (void)ext; (void)data; return FALSE; }
WTAPI BOOL WINAPI WTExtSet(HCTX h, UINT ext, LPVOID data) { (void)h; (void)ext; (void)data; return FALSE; }

/* WTSave writes the context's LOGCONTEXTW (IFC_CTXSAVESIZE bytes) */
WTAPI BOOL WINAPI WTSave(HCTX h, LPVOID buf) { return WTGetW(h, buf); }
WTAPI HCTX WINAPI WTRestore(HWND hwnd, LPVOID buf, BOOL enable) { return open_ctx(hwnd, buf, enable); }

typedef void *HMGR;
WTAPI HMGR WINAPI WTMgrOpen(HWND hwnd, UINT base) { (void)hwnd; (void)base; return NULL; }
WTAPI BOOL WINAPI WTMgrClose(HMGR m) { (void)m; return FALSE; }

BOOL WINAPI DllMain(HINSTANCE inst, DWORD why, LPVOID reserved)
{
    (void)inst; (void)reserved;
    if (why == DLL_PROCESS_ATTACH) InitializeCriticalSection(&g_lock);
    return TRUE;
}
