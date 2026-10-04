/*
 * hid.dll — Human Interface Device class support: HID device handles
 * (HidD_*) and report parsing (HidP_*)
 *
 * NovaOS's HID devices are its game controllers (novapad.h), listed by
 * setupapi and cfgmgr32 under the HID interface class and by Raw Input,
 * and opened with CreateFile on their paths (kernel/um/um_hid.c): a read
 * gets the next input report.  HidD_* ask the kernel which controller a
 * handle reads for its attributes, strings, report descriptor (as
 * preparsed data, novahidp.h) and latest input report.  Output and
 * feature reports are not supported (none of the controllers has any
 * the kernel would send): those calls fail.
 *
 * HidP_* read the preparsed data, from here or from Raw Input's
 * RIDI_PREPARSEDDATA alike, and a report: the capabilities, the buttons
 * down (variable one-bit fields, and array fields holding usage indices),
 * values raw or scaled to the physical range, the data list (every value,
 * every button down, by data index), and building reports with
 * HidP_Set*.  A report starts with its report ID byte and must be the
 * report type's length, as Windows checks.
 */
#define HIDAPI_ __declspec(dllexport)
#include <hidsdi.h>
#include <novahidp.h>

#ifndef ERROR_INVALID_USER_BUFFER
#define ERROR_INVALID_USER_BUFFER 1784
#endif

/* ---------------------------------------------------------------------------
 * HidD_*: devices
 * ------------------------------------------------------------------------- */

HIDAPI_ void WINAPI HidD_GetHidGuid(LPGUID g)
{
    static const GUID hid = { 0x4d1e55b2, 0xf16f, 0x11cf, { 0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 } };
    if (g) *g = hid;
}

/* The controller @dev reads (its slot), or -1 with the error set */
static int dev_slot(HANDLE dev, NovaPadInfo *info)
{
    int slot = dev && dev != INVALID_HANDLE_VALUE ? nova_pad_handle_slot(dev) : -1;
    if (slot < 0 || !nova_pad_info(slot, info)) {
        SetLastError(slot < 0 && dev && dev != INVALID_HANDLE_VALUE ? ERROR_INVALID_FUNCTION : ERROR_INVALID_HANDLE);
        return -1;
    }
    return slot;
}

HIDAPI_ BOOLEAN WINAPI HidD_GetAttributes(HANDLE dev, PHIDD_ATTRIBUTES a)
{
    NovaPadInfo i;
    if (dev_slot(dev, &i) < 0) return FALSE;
    if (!a) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    a->Size = sizeof(*a);
    a->VendorID = i.vid;
    a->ProductID = i.pid;
    a->VersionNumber = i.kind == NOVA_PAD_HID ? 0x0100 : 0x0114;
    return TRUE;
}

HIDAPI_ BOOLEAN WINAPI HidD_GetPreparsedData(HANDLE dev, PHIDP_PREPARSED_DATA *p)
{
    NovaPadInfo i;
    int slot = dev_slot(dev, &i);
    if (p) *p = NULL;
    if (slot < 0) return FALSE;
    if (!p) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    UINT n = nova_pad_preparsed(slot, NULL, 0);
    void *b = n ? LocalAlloc(LMEM_FIXED, n) : NULL;
    if (!b || nova_pad_preparsed(slot, b, n) != n) {
        if (b) LocalFree(b);
        SetLastError(n ? ERROR_NOT_ENOUGH_MEMORY : ERROR_INVALID_FUNCTION);
        return FALSE;
    }
    *p = b;
    return TRUE;
}

HIDAPI_ BOOLEAN WINAPI HidD_FreePreparsedData(PHIDP_PREPARSED_DATA p)
{
    if (!nova_hidp_check(p)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    LocalFree(p);
    return TRUE;
}

HIDAPI_ BOOLEAN WINAPI HidD_FlushQueue(HANDLE dev)
{
    NovaPadInfo i;
    if (dev_slot(dev, &i) < 0) return FALSE;
    return nova_pad_flush(dev);
}

/* Input buffers: the kernel keeps 32 reports per handle */
HIDAPI_ BOOLEAN WINAPI HidD_GetNumInputBuffers(HANDLE dev, PULONG n)
{
    NovaPadInfo i;
    if (dev_slot(dev, &i) < 0) return FALSE;
    if (!n) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    *n = 32;
    return TRUE;
}

HIDAPI_ BOOLEAN WINAPI HidD_SetNumInputBuffers(HANDLE dev, ULONG n)
{
    NovaPadInfo i;
    if (dev_slot(dev, &i) < 0) return FALSE;
    if (n < 2 || n > 512) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    return TRUE;                                       /* (the kernel keeps 32 whatever is asked) */
}

/* The latest input report with the ID in buf[0] */
HIDAPI_ BOOLEAN WINAPI HidD_GetInputReport(HANDLE dev, PVOID buf, ULONG len)
{
    NovaPadInfo i;
    int slot = dev_slot(dev, &i);
    if (slot < 0) return FALSE;
    if (!buf || !len) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    NovaPadRaw r;
    BYTE *b = buf;
    if (!nova_pad_last(slot, b[0], &r)) {              /* (nothing yet: the controller is at rest) */
        WORD in_len = 0;
        BYTE d[NOVA_PAD_DESC_MAX];
        nova_pad_descriptor(slot, d, &in_len);
        if (b[0] || len < in_len) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
        memset(b, 0, in_len);
        return TRUE;
    }
    if (len < r.len) { SetLastError(ERROR_INVALID_USER_BUFFER); return FALSE; }
    memcpy(b, r.data, r.len);
    return TRUE;
}

static BOOLEAN no_report(HANDLE dev, PVOID buf, ULONG len)
{
    NovaPadInfo i;
    if (dev_slot(dev, &i) < 0) return FALSE;
    SetLastError(!buf || !len ? ERROR_INVALID_PARAMETER : ERROR_INVALID_FUNCTION);
    return FALSE;
}

HIDAPI_ BOOLEAN WINAPI HidD_SetOutputReport(HANDLE dev, PVOID buf, ULONG len) { return no_report(dev, buf, len); }
HIDAPI_ BOOLEAN WINAPI HidD_GetFeature(HANDLE dev, PVOID buf, ULONG len) { return no_report(dev, buf, len); }
HIDAPI_ BOOLEAN WINAPI HidD_SetFeature(HANDLE dev, PVOID buf, ULONG len) { return no_report(dev, buf, len); }
HIDAPI_ BOOLEAN WINAPI HidD_GetPhysicalDescriptor(HANDLE dev, PVOID buf, ULONG len) { return no_report(dev, buf, len); }
HIDAPI_ BOOLEAN WINAPI HidD_GetConfiguration(HANDLE dev, PVOID cfg, ULONG len) { return no_report(dev, cfg, len); }
HIDAPI_ BOOLEAN WINAPI HidD_SetConfiguration(HANDLE dev, PVOID cfg, ULONG len) { return no_report(dev, cfg, len); }

/* A string into @buf (@len bytes, UTF-16 with its NUL) */
static BOOLEAN put_string(PVOID buf, ULONG len, const char *s)
{
    if (!buf) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    ULONG n = (ULONG)strlen(s);
    if ((n + 1) * 2 > len) { SetLastError(ERROR_INVALID_USER_BUFFER); return FALSE; }
    WCHAR *w = buf;
    for (ULONG k = 0; k <= n; k++) w[k] = (BYTE)s[k];
    return TRUE;
}

/* What Windows' Xbox driver calls its HID side; a HID controller's own product string */
HIDAPI_ BOOLEAN WINAPI HidD_GetProductString(HANDLE dev, PVOID buf, ULONG len)
{
    NovaPadInfo i;
    if (dev_slot(dev, &i) < 0) return FALSE;
    return put_string(buf, len, i.kind == NOVA_PAD_XBOX360 ? "Controller (XBOX 360 For Windows)" :
                                i.kind == NOVA_PAD_XBOXONE ? "Controller (Xbox One For Windows)" : i.name);
}

/* The kernel keeps no manufacturer or serial number strings */
HIDAPI_ BOOLEAN WINAPI HidD_GetManufacturerString(HANDLE dev, PVOID buf, ULONG len) { return no_report(dev, buf, len); }
HIDAPI_ BOOLEAN WINAPI HidD_GetSerialNumberString(HANDLE dev, PVOID buf, ULONG len) { return no_report(dev, buf, len); }
HIDAPI_ BOOLEAN WINAPI HidD_GetIndexedString(HANDLE dev, ULONG index, PVOID buf, ULONG len) { (void)index; return no_report(dev, buf, len); }

/* ---------------------------------------------------------------------------
 * HidP_*: capabilities
 * ------------------------------------------------------------------------- */

#define PP(p) const NovaHidP *pp = nova_hidp_check(p); if (!pp) return HIDP_STATUS_INVALID_PREPARSED_DATA
/* The caps calls also report no caps */
#define PP_CAPS(p, n) const NovaHidP *pp = nova_hidp_check(p); \
    if (!pp) { if (n) *(n) = 0; return HIDP_STATUS_INVALID_PREPARSED_DATA; }

HIDAPI_ LONG WINAPI HidP_GetCaps(PHIDP_PREPARSED_DATA p, PHIDP_CAPS caps)
{
    PP(p);
    if (!caps) return HIDP_STATUS_NULL;
    memset(caps, 0, sizeof(*caps));
    caps->Usage = pp->usage;
    caps->UsagePage = pp->page;
    caps->InputReportByteLength = pp->rlen[0];
    caps->OutputReportByteLength = pp->rlen[1];
    caps->FeatureReportByteLength = pp->rlen[2];
    caps->NumberLinkCollectionNodes = pp->nlinks;
    USHORT *b[3] = { &caps->NumberInputButtonCaps, &caps->NumberOutputButtonCaps, &caps->NumberFeatureButtonCaps };
    USHORT *v[3] = { &caps->NumberInputValueCaps, &caps->NumberOutputValueCaps, &caps->NumberFeatureValueCaps };
    for (int k = 0; k < pp->ncaps; k++) {
        const NovaHidCap *c = &pp->caps[k];
        (*(c->button ? b : v)[c->rtype])++;
    }
    caps->NumberInputDataIndices = pp->ndata[0];
    caps->NumberOutputDataIndices = pp->ndata[1];
    caps->NumberFeatureDataIndices = pp->ndata[2];
    return HIDP_STATUS_SUCCESS;
}

HIDAPI_ LONG WINAPI HidP_GetLinkCollectionNodes(PHIDP_LINK_COLLECTION_NODE nodes, PULONG n, PHIDP_PREPARSED_DATA p)
{
    PP(p);
    if (!n) return HIDP_STATUS_NULL;
    ULONG have = pp->nlinks, room = *n;
    *n = have;
    if (!nodes || room < have) return HIDP_STATUS_BUFFER_TOO_SMALL;
    const NovaHidLink *l = nova_hidp_links(pp);
    for (ULONG k = 0; k < have; k++) {
        memset(&nodes[k], 0, sizeof(nodes[k]));
        nodes[k].LinkUsage = l[k].usage;
        nodes[k].LinkUsagePage = l[k].page;
        nodes[k].Parent = l[k].parent;
        nodes[k].NumberOfChildren = l[k].nchildren;
        nodes[k].NextSibling = l[k].next;
        nodes[k].FirstChild = l[k].first;
        nodes[k].CollectionType = l[k].type;
    }
    return HIDP_STATUS_SUCCESS;
}

static BOOL type_ok(HIDP_REPORT_TYPE t) { return (unsigned)t <= HidP_Feature; }

/* Whether cap @c answers a request for @page/@link/@usage (0: any) */
static BOOL cap_matches(const NovaHidCap *c, USAGE page, USHORT link, USAGE usage)
{
    if (page && c->page != page) return FALSE;
    if (link && c->link != link) return FALSE;
    if (usage && (usage < c->umin || usage > c->umax)) return FALSE;
    return TRUE;
}

/* What button and value caps share */
typedef struct {
    USAGE UsagePage; UCHAR ReportID; BOOLEAN IsAlias; USHORT BitField; USHORT LinkCollection;
    USAGE LinkUsage, LinkUsagePage; BOOLEAN IsRange, IsStringRange, IsDesignatorRange, IsAbsolute;
} CapHead;

static void cap_head(const NovaHidP *pp, const NovaHidCap *c, CapHead *h)
{
    const NovaHidLink *l = &nova_hidp_links(pp)[c->link < pp->nlinks ? c->link : 0];
    h->UsagePage = c->page;
    h->ReportID = c->id;
    h->IsAlias = FALSE;
    h->BitField = c->bitfield;
    h->LinkCollection = c->link;
    h->LinkUsage = l->usage;
    h->LinkUsagePage = l->page;
    h->IsRange = c->range;
    h->IsStringRange = h->IsDesignatorRange = FALSE;
    h->IsAbsolute = c->absolute;
}

HIDAPI_ LONG WINAPI HidP_GetSpecificButtonCaps(HIDP_REPORT_TYPE t, USAGE page, USHORT link, USAGE usage,
                                               PHIDP_BUTTON_CAPS caps, PUSHORT n, PHIDP_PREPARSED_DATA p)
{
    PP_CAPS(p, n);
    if (!type_ok(t)) return HIDP_STATUS_INVALID_REPORT_TYPE;
    if (!n) return HIDP_STATUS_NULL;
    USHORT room = caps ? *n : 0, got = 0;
    for (int k = 0; k < pp->ncaps; k++) {
        const NovaHidCap *c = &pp->caps[k];
        if (c->rtype != t || !c->button || !cap_matches(c, page, link, usage)) continue;
        if (got < room) {
            HIDP_BUTTON_CAPS *b = &caps[got];
            memset(b, 0, sizeof(*b));
            cap_head(pp, c, (CapHead *)b);
            b->ReportCount = c->count;
            if (c->range) {
                b->Range.UsageMin = c->umin; b->Range.UsageMax = c->umax;
                b->Range.DataIndexMin = c->dmin; b->Range.DataIndexMax = c->dmax;
            } else {
                b->NotRange.Usage = c->umin;
                b->NotRange.DataIndex = c->dmin;
            }
        }
        got++;
    }
    *n = got < room ? got : room;
    if (!got) return HIDP_STATUS_USAGE_NOT_FOUND;
    return got > room ? HIDP_STATUS_BUFFER_TOO_SMALL : HIDP_STATUS_SUCCESS;
}

HIDAPI_ LONG WINAPI HidP_GetButtonCaps(HIDP_REPORT_TYPE t, PHIDP_BUTTON_CAPS caps, PUSHORT n, PHIDP_PREPARSED_DATA p)
{
    return HidP_GetSpecificButtonCaps(t, 0, 0, 0, caps, n, p);
}

HIDAPI_ LONG WINAPI HidP_GetSpecificValueCaps(HIDP_REPORT_TYPE t, USAGE page, USHORT link, USAGE usage,
                                              PHIDP_VALUE_CAPS caps, PUSHORT n, PHIDP_PREPARSED_DATA p)
{
    PP_CAPS(p, n);
    if (!type_ok(t)) return HIDP_STATUS_INVALID_REPORT_TYPE;
    if (!n) return HIDP_STATUS_NULL;
    USHORT room = caps ? *n : 0, got = 0;
    for (int k = 0; k < pp->ncaps; k++) {
        const NovaHidCap *c = &pp->caps[k];
        if (c->rtype != t || c->button || !cap_matches(c, page, link, usage)) continue;
        if (got < room) {
            HIDP_VALUE_CAPS *v = &caps[got];
            memset(v, 0, sizeof(*v));
            cap_head(pp, c, (CapHead *)v);
            v->HasNull = c->has_null;
            v->BitSize = c->size;
            v->ReportCount = c->count;
            v->UnitsExp = c->unit_exp;
            v->Units = c->units;
            v->LogicalMin = c->lmin; v->LogicalMax = c->lmax;
            v->PhysicalMin = c->pmin; v->PhysicalMax = c->pmax;
            if (c->range) {
                v->Range.UsageMin = c->umin; v->Range.UsageMax = c->umax;
                v->Range.DataIndexMin = c->dmin; v->Range.DataIndexMax = c->dmax;
            } else {
                v->NotRange.Usage = c->umin;
                v->NotRange.DataIndex = c->dmin;
            }
        }
        got++;
    }
    *n = got < room ? got : room;
    if (!got) return HIDP_STATUS_USAGE_NOT_FOUND;
    return got > room ? HIDP_STATUS_BUFFER_TOO_SMALL : HIDP_STATUS_SUCCESS;
}

HIDAPI_ LONG WINAPI HidP_GetValueCaps(HIDP_REPORT_TYPE t, PHIDP_VALUE_CAPS caps, PUSHORT n, PHIDP_PREPARSED_DATA p)
{
    return HidP_GetSpecificValueCaps(t, 0, 0, 0, caps, n, p);
}

/* No item NovaOS's parser does not know is kept */
HIDAPI_ LONG WINAPI HidP_GetExtendedAttributes(HIDP_REPORT_TYPE t, USHORT index, PHIDP_PREPARSED_DATA p,
                                               PHIDP_EXTENDED_ATTRIBUTES a, PULONG n)
{
    PP(p);
    if (!type_ok(t)) return HIDP_STATUS_INVALID_REPORT_TYPE;
    if (index >= pp->ndata[t]) return HIDP_STATUS_DATA_INDEX_NOT_FOUND;
    if (!n) return HIDP_STATUS_NULL;
    ULONG need = (ULONG)__builtin_offsetof(HIDP_EXTENDED_ATTRIBUTES, Data);
    if (!a || *n < need) { *n = need; return HIDP_STATUS_BUFFER_TOO_SMALL; }
    memset(a, 0, need);
    *n = need;
    return HIDP_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * HidP_*: reports
 * ------------------------------------------------------------------------- */

static ULONG get_bits(const BYTE *r, ULONG len, ULONG bit, ULONG size)
{
    ULONG v = 0;
    for (ULONG k = 0; k < size && k < 32; k++) {
        ULONG at = bit + k;
        if (at / 8 >= len) break;
        if (r[at / 8] & (1u << (at % 8))) v |= 1u << k;
    }
    return v;
}

static void put_bits(BYTE *r, ULONG len, ULONG bit, ULONG size, ULONG v)
{
    for (ULONG k = 0; k < size && k < 32; k++) {
        ULONG at = bit + k;
        if (at / 8 >= len) break;
        if (v & (1u << k)) r[at / 8] |= (BYTE)(1u << (at % 8));
        else r[at / 8] &= (BYTE)~(1u << (at % 8));
    }
}

/* Field @k of cap @c in @report (bits counted after the ID byte) */
static ULONG field(const NovaHidCap *c, const BYTE *report, ULONG len, int k)
{
    return get_bits(report + 1, len - 1, (ULONG)c->bit + (ULONG)k * c->size, c->size);
}

static void set_field(const NovaHidCap *c, BYTE *report, ULONG len, int k, ULONG v)
{
    put_bits(report + 1, len - 1, (ULONG)c->bit + (ULONG)k * c->size, c->size, v);
}

static LONG sign(const NovaHidCap *c, ULONG v)
{
    if (c->lmin < 0 && c->size < 32 && (v & (1u << (c->size - 1)))) v |= ~0u << c->size;
    return (LONG)v;
}

/* The checks every report call makes: type, preparsed data, length */
static LONG report_ok(const NovaHidP *pp, HIDP_REPORT_TYPE t, const void *report, ULONG len)
{
    if (!type_ok(t)) return HIDP_STATUS_INVALID_REPORT_TYPE;
    if (!report) return HIDP_STATUS_NULL;
    if (!pp->rlen[t] || len != pp->rlen[t]) return HIDP_STATUS_INVALID_REPORT_LENGTH;
    return HIDP_STATUS_SUCCESS;
}

/* The usage an array field's value @v names in cap @c, or 0 */
static USAGE array_usage(const NovaHidCap *c, ULONG v)
{
    LONG i = (LONG)v - c->lmin - c->aofs;
    if (c->lmin < 0 || (LONG)v < c->lmin || (LONG)v > c->lmax || i < 0 || i > c->umax - c->umin) return 0;
    return (USAGE)(c->umin + i);
}

/* Buttons down: each (usage, page) of the caps for @page (0 any) and
 * @link in @report into @list (Ex: USAGE_AND_PAGE) */
static LONG get_usages(const NovaHidP *pp, HIDP_REPORT_TYPE t, USAGE page, USHORT link, void *list, BOOL ex,
                       PULONG n, const BYTE *report, ULONG len)
{
    LONG st = report_ok(pp, t, report, len);
    if (st != HIDP_STATUS_SUCCESS) return st;
    if (!n) return HIDP_STATUS_NULL;
    ULONG room = list ? *n : 0, got = 0;
    BOOL any = FALSE, other_id = FALSE;
    for (int k = 0; k < pp->ncaps; k++) {
        const NovaHidCap *c = &pp->caps[k];
        if (c->rtype != t || !c->button || !cap_matches(c, page, link, 0)) continue;
        if (c->id != report[0]) { other_id = TRUE; continue; }
        any = TRUE;
        for (int f = 0; f < c->count; f++) {
            ULONG v = field(c, report, len, f);
            USAGE u;
            if (c->array) u = array_usage(c, v);
            else u = v ? (USAGE)(c->range ? c->umin + f : c->umin) : 0;
            if (!u || u > c->umax) continue;
            if (got < room) {
                if (ex) { ((USAGE_AND_PAGE *)list)[got].Usage = u; ((USAGE_AND_PAGE *)list)[got].UsagePage = c->page; }
                else ((USAGE *)list)[got] = u;
            }
            got++;
        }
    }
    if (!any) return other_id ? HIDP_STATUS_INCOMPATIBLE_REPORT_ID : HIDP_STATUS_USAGE_NOT_FOUND;
    *n = got;
    return got > room ? HIDP_STATUS_BUFFER_TOO_SMALL : HIDP_STATUS_SUCCESS;
}

HIDAPI_ LONG WINAPI HidP_GetUsages(HIDP_REPORT_TYPE t, USAGE page, USHORT link, PUSAGE list, PULONG n,
                                   PHIDP_PREPARSED_DATA p, PCHAR report, ULONG len)
{
    PP(p);
    return get_usages(pp, t, page, link, list, FALSE, n, (const BYTE *)report, len);
}

HIDAPI_ LONG WINAPI HidP_GetUsagesEx(HIDP_REPORT_TYPE t, USHORT link, PUSAGE_AND_PAGE list, PULONG n,
                                     PHIDP_PREPARSED_DATA p, PCHAR report, ULONG len)
{
    PP(p);
    return get_usages(pp, t, 0, link, list, TRUE, n, (const BYTE *)report, len);
}

HIDAPI_ ULONG WINAPI HidP_MaxUsageListLength(HIDP_REPORT_TYPE t, USAGE page, PHIDP_PREPARSED_DATA p)
{
    const NovaHidP *pp = nova_hidp_check(p);
    if (!pp || !type_ok(t)) return 0;
    ULONG n = 0;
    for (int k = 0; k < pp->ncaps; k++) {
        const NovaHidCap *c = &pp->caps[k];
        if (c->rtype == t && c->button && (!page || c->page == page)) n += c->count;
    }
    return n;
}

/* Set (@on) or clear the buttons in @list in @report */
static LONG set_usages(const NovaHidP *pp, HIDP_REPORT_TYPE t, USAGE page, USHORT link, const USAGE *list,
                       PULONG n, BYTE *report, ULONG len, BOOL on)
{
    LONG st = report_ok(pp, t, report, len);
    if (st != HIDP_STATUS_SUCCESS) return st;
    if (!n || (!list && *n)) return HIDP_STATUS_NULL;
    for (ULONG i = 0; i < *n; i++) {
        USAGE u = list[i];
        if (!u) continue;
        LONG why = HIDP_STATUS_USAGE_NOT_FOUND;
        BOOL done = FALSE;
        for (int k = 0; k < pp->ncaps && !done; k++) {
            const NovaHidCap *c = &pp->caps[k];
            if (c->rtype != t || !c->button || !cap_matches(c, page, link, u)) continue;
            if (c->id != report[0]) { why = HIDP_STATUS_INCOMPATIBLE_REPORT_ID; continue; }
            if (!c->array) {
                for (int f = 0; f < c->count; f++)
                    if ((c->range ? c->umin + f : c->umin) == u) set_field(c, report, len, f, on ? 1 : 0);
                done = TRUE;
                continue;
            }
            ULONG idx = (ULONG)(c->lmin + c->aofs + (u - c->umin));
            if (on) {
                int slot = -1;
                for (int f = 0; f < c->count && slot < 0; f++) if (array_usage(c, field(c, report, len, f)) == u) slot = f;
                for (int f = 0; f < c->count && slot < 0; f++) if (!array_usage(c, field(c, report, len, f))) slot = f;
                if (slot < 0) { why = HIDP_STATUS_BUFFER_TOO_SMALL; continue; }
                set_field(c, report, len, slot, idx);
            } else {
                BOOL was = FALSE;
                for (int f = 0; f < c->count; f++)
                    if (array_usage(c, field(c, report, len, f)) == u) { set_field(c, report, len, f, 0); was = TRUE; }
                if (!was) { why = HIDP_STATUS_BUTTON_NOT_PRESSED; continue; }
            }
            done = TRUE;
        }
        if (!done) { *n = i; return why; }
    }
    return HIDP_STATUS_SUCCESS;
}

HIDAPI_ LONG WINAPI HidP_SetUsages(HIDP_REPORT_TYPE t, USAGE page, USHORT link, PUSAGE list, PULONG n,
                                   PHIDP_PREPARSED_DATA p, PCHAR report, ULONG len)
{
    PP(p);
    return set_usages(pp, t, page, link, list, n, (BYTE *)report, len, TRUE);
}

HIDAPI_ LONG WINAPI HidP_UnsetUsages(HIDP_REPORT_TYPE t, USAGE page, USHORT link, PUSAGE list, PULONG n,
                                     PHIDP_PREPARSED_DATA p, PCHAR report, ULONG len)
{
    PP(p);
    return set_usages(pp, t, page, link, list, n, (BYTE *)report, len, FALSE);
}

/* The value cap and field for @page/@link/@usage in report @id */
static LONG find_value(const NovaHidP *pp, HIDP_REPORT_TYPE t, USAGE page, USHORT link, USAGE usage, BYTE id,
                       const NovaHidCap **out, int *f)
{
    LONG why = HIDP_STATUS_USAGE_NOT_FOUND;
    for (int k = 0; k < pp->ncaps; k++) {
        const NovaHidCap *c = &pp->caps[k];
        if (c->rtype != t || c->button || !cap_matches(c, page, link, usage)) continue;
        if (c->id != id) { why = HIDP_STATUS_INCOMPATIBLE_REPORT_ID; continue; }
        *out = c;
        *f = c->range ? usage - c->umin : 0;
        return HIDP_STATUS_SUCCESS;
    }
    return why;
}

HIDAPI_ LONG WINAPI HidP_GetUsageValue(HIDP_REPORT_TYPE t, USAGE page, USHORT link, USAGE usage, PULONG value,
                                       PHIDP_PREPARSED_DATA p, PCHAR report, ULONG len)
{
    PP(p);
    LONG st = report_ok(pp, t, report, len);
    if (st != HIDP_STATUS_SUCCESS) return st;
    if (!value) return HIDP_STATUS_NULL;
    const NovaHidCap *c;
    int f;
    if ((st = find_value(pp, t, page, link, usage, (BYTE)report[0], &c, &f)) != HIDP_STATUS_SUCCESS) return st;
    if (!c->range && c->count > 1) return HIDP_STATUS_IS_VALUE_ARRAY;
    *value = field(c, (const BYTE *)report, len, f);
    return HIDP_STATUS_SUCCESS;
}

HIDAPI_ LONG WINAPI HidP_GetScaledUsageValue(HIDP_REPORT_TYPE t, USAGE page, USHORT link, USAGE usage, PLONG value,
                                             PHIDP_PREPARSED_DATA p, PCHAR report, ULONG len)
{
    PP(p);
    LONG st = report_ok(pp, t, report, len);
    if (st != HIDP_STATUS_SUCCESS) return st;
    if (!value) return HIDP_STATUS_NULL;
    const NovaHidCap *c;
    int f;
    if ((st = find_value(pp, t, page, link, usage, (BYTE)report[0], &c, &f)) != HIDP_STATUS_SUCCESS) return st;
    if (!c->range && c->count > 1) return HIDP_STATUS_IS_VALUE_ARRAY;
    if (c->lmin >= c->lmax || c->pmin >= c->pmax) return HIDP_STATUS_BAD_LOG_PHY_VALUES;
    LONG v = sign(c, field(c, (const BYTE *)report, len, f));
    if (v < c->lmin || v > c->lmax) {
        *value = 0;
        return c->has_null ? HIDP_STATUS_NULL : HIDP_STATUS_VALUE_OUT_OF_RANGE;
    }
    *value = (LONG)(((LONGLONG)(v - c->lmin) * ((LONGLONG)c->pmax - c->pmin)) / ((LONGLONG)c->lmax - c->lmin) + c->pmin);
    return HIDP_STATUS_SUCCESS;
}

HIDAPI_ LONG WINAPI HidP_SetUsageValue(HIDP_REPORT_TYPE t, USAGE page, USHORT link, USAGE usage, ULONG value,
                                       PHIDP_PREPARSED_DATA p, PCHAR report, ULONG len)
{
    PP(p);
    LONG st = report_ok(pp, t, report, len);
    if (st != HIDP_STATUS_SUCCESS) return st;
    const NovaHidCap *c;
    int f;
    if ((st = find_value(pp, t, page, link, usage, (BYTE)report[0], &c, &f)) != HIDP_STATUS_SUCCESS) return st;
    if (!c->range && c->count > 1) return HIDP_STATUS_IS_VALUE_ARRAY;
    set_field(c, (BYTE *)report, len, f, value);
    return HIDP_STATUS_SUCCESS;
}

HIDAPI_ LONG WINAPI HidP_SetScaledUsageValue(HIDP_REPORT_TYPE t, USAGE page, USHORT link, USAGE usage, LONG value,
                                             PHIDP_PREPARSED_DATA p, PCHAR report, ULONG len)
{
    PP(p);
    LONG st = report_ok(pp, t, report, len);
    if (st != HIDP_STATUS_SUCCESS) return st;
    const NovaHidCap *c;
    int f;
    if ((st = find_value(pp, t, page, link, usage, (BYTE)report[0], &c, &f)) != HIDP_STATUS_SUCCESS) return st;
    if (!c->range && c->count > 1) return HIDP_STATUS_IS_VALUE_ARRAY;
    if (c->lmin >= c->lmax || c->pmin >= c->pmax) return HIDP_STATUS_BAD_LOG_PHY_VALUES;
    if (value < c->pmin || value > c->pmax) return HIDP_STATUS_VALUE_OUT_OF_RANGE;
    LONG v = (LONG)(((LONGLONG)(value - c->pmin) * ((LONGLONG)c->lmax - c->lmin)) / ((LONGLONG)c->pmax - c->pmin) + c->lmin);
    set_field(c, (BYTE *)report, len, f, (ULONG)v);
    return HIDP_STATUS_SUCCESS;
}

/* A value array (a usage with several fields), packed as the report packs it */
static LONG value_array(HIDP_REPORT_TYPE t, USAGE page, USHORT link, USAGE usage, PCHAR value, USHORT vlen,
                        PHIDP_PREPARSED_DATA p, PCHAR report, ULONG len, BOOL set)
{
    PP(p);
    LONG st = report_ok(pp, t, report, len);
    if (st != HIDP_STATUS_SUCCESS) return st;
    if (!value) return HIDP_STATUS_NULL;
    const NovaHidCap *c;
    int f;
    if ((st = find_value(pp, t, page, link, usage, (BYTE)report[0], &c, &f)) != HIDP_STATUS_SUCCESS) return st;
    if (c->range || c->count < 2) return HIDP_STATUS_NOT_VALUE_ARRAY;
    if ((ULONG)vlen * 8 < (ULONG)c->size * c->count) return HIDP_STATUS_BUFFER_TOO_SMALL;
    for (int k = 0; k < c->count; k++) {
        if (set) set_field(c, (BYTE *)report, len, k, get_bits((BYTE *)value, vlen, (ULONG)k * c->size, c->size));
        else put_bits((BYTE *)value, vlen, (ULONG)k * c->size, c->size, field(c, (const BYTE *)report, len, k));
    }
    return HIDP_STATUS_SUCCESS;
}

HIDAPI_ LONG WINAPI HidP_GetUsageValueArray(HIDP_REPORT_TYPE t, USAGE page, USHORT link, USAGE usage, PCHAR value,
                                            USHORT vlen, PHIDP_PREPARSED_DATA p, PCHAR report, ULONG len)
{
    return value_array(t, page, link, usage, value, vlen, p, report, len, FALSE);
}

HIDAPI_ LONG WINAPI HidP_SetUsageValueArray(HIDP_REPORT_TYPE t, USAGE page, USHORT link, USAGE usage, PCHAR value,
                                            USHORT vlen, PHIDP_PREPARSED_DATA p, PCHAR report, ULONG len)
{
    return value_array(t, page, link, usage, value, vlen, p, report, len, TRUE);
}

/* Every value and every button down, by data index */
HIDAPI_ LONG WINAPI HidP_GetData(HIDP_REPORT_TYPE t, PHIDP_DATA data, PULONG n, PHIDP_PREPARSED_DATA p,
                                 PCHAR report, ULONG len)
{
    PP(p);
    LONG st = report_ok(pp, t, report, len);
    if (st != HIDP_STATUS_SUCCESS) return st;
    if (!n) return HIDP_STATUS_NULL;
    const BYTE *r = (const BYTE *)report;
    ULONG room = data ? *n : 0, got = 0;
    BOOL any = FALSE;
    for (int k = 0; k < pp->ncaps; k++) {
        const NovaHidCap *c = &pp->caps[k];
        if (c->rtype != t || c->id != r[0]) continue;
        any = TRUE;
        for (int f = 0; f < c->count; f++) {
            ULONG v = field(c, r, len, f);
            USHORT index;
            if (c->button) {
                USAGE u = c->array ? array_usage(c, v) : v ? (USAGE)(c->range ? c->umin + f : c->umin) : 0;
                if (!u || u > c->umax) continue;
                index = (USHORT)(c->dmin + (c->range ? u - c->umin : 0));
                v = TRUE;
            } else {
                if (!c->range && f) break;                     /* (a value array: by HidP_GetUsageValueArray) */
                if (c->range && f > c->umax - c->umin) break;
                index = (USHORT)(c->dmin + (c->range ? f : 0));
            }
            if (got < room) {
                data[got].DataIndex = index;
                data[got].Reserved = 0;
                data[got].RawValue = v;
            }
            got++;
        }
    }
    *n = got < room ? got : room;
    if (!any) return HIDP_STATUS_INCOMPATIBLE_REPORT_ID;
    if (got > room) { *n = got; return HIDP_STATUS_BUFFER_TOO_SMALL; }
    return HIDP_STATUS_SUCCESS;
}

HIDAPI_ LONG WINAPI HidP_SetData(HIDP_REPORT_TYPE t, PHIDP_DATA data, PULONG n, PHIDP_PREPARSED_DATA p,
                                 PCHAR report, ULONG len)
{
    PP(p);
    LONG st = report_ok(pp, t, report, len);
    if (st != HIDP_STATUS_SUCCESS) return st;
    if (!n || (!data && *n)) return HIDP_STATUS_NULL;
    for (ULONG i = 0; i < *n; i++) {
        BOOL done = FALSE;
        LONG why = HIDP_STATUS_DATA_INDEX_NOT_FOUND;
        for (int k = 0; k < pp->ncaps && !done; k++) {
            const NovaHidCap *c = &pp->caps[k];
            if (c->rtype != t || data[i].DataIndex < c->dmin || data[i].DataIndex > c->dmax) continue;
            if (c->id != (BYTE)report[0]) { why = HIDP_STATUS_INCOMPATIBLE_REPORT_ID; continue; }
            USAGE u = (USAGE)(c->umin + (data[i].DataIndex - c->dmin));
            if (c->button) {
                ULONG one = 1;
                st = set_usages(pp, t, c->page, c->link, &u, &one, (BYTE *)report, len, data[i].On);
                if (st != HIDP_STATUS_SUCCESS && st != HIDP_STATUS_BUTTON_NOT_PRESSED) { *n = i; return st; }
            } else set_field(c, (BYTE *)report, len, c->range ? u - c->umin : 0, data[i].RawValue);
            done = TRUE;
        }
        if (!done) { *n = i; return why; }
    }
    return HIDP_STATUS_SUCCESS;
}

HIDAPI_ ULONG WINAPI HidP_MaxDataListLength(HIDP_REPORT_TYPE t, PHIDP_PREPARSED_DATA p)
{
    const NovaHidP *pp = nova_hidp_check(p);
    return pp && type_ok(t) ? pp->ndata[t] : 0;
}

/* A report of @id with nothing set: zeros, and the null value where a
 * value has one */
HIDAPI_ LONG WINAPI HidP_InitializeReportForID(HIDP_REPORT_TYPE t, UCHAR id, PHIDP_PREPARSED_DATA p, PCHAR report, ULONG len)
{
    PP(p);
    if (!type_ok(t)) return HIDP_STATUS_INVALID_REPORT_TYPE;
    if (!report) return HIDP_STATUS_NULL;
    if (!pp->rlen[t] || len != pp->rlen[t]) return HIDP_STATUS_INVALID_REPORT_LENGTH;
    BOOL found = FALSE;
    for (int k = 0; k < pp->ncaps; k++) if (pp->caps[k].rtype == t && pp->caps[k].id == id) found = TRUE;
    if (!found) return HIDP_STATUS_REPORT_DOES_NOT_EXIST;
    memset(report, 0, len);
    report[0] = (CHAR)id;
    for (int k = 0; k < pp->ncaps; k++) {
        const NovaHidCap *c = &pp->caps[k];
        if (c->rtype != t || c->id != id || c->button || !c->has_null) continue;
        ULONG null = (ULONG)(c->lmin > 0 ? 0 : c->lmax + 1);    /* a value outside the logical range */
        for (int f = 0; f < c->count; f++) set_field(c, (BYTE *)report, len, f, null);
    }
    return HIDP_STATUS_SUCCESS;
}

/* What went up (in @prev, not @cur) and what went down */
HIDAPI_ LONG WINAPI HidP_UsageListDifference(PUSAGE prev, PUSAGE cur, PUSAGE brk, PUSAGE make, ULONG n)
{
    ULONG nb = 0, nm = 0;
    for (ULONG i = 0; i < n && prev[i]; i++) {
        BOOL in = FALSE;
        for (ULONG j = 0; j < n && cur[j]; j++) if (cur[j] == prev[i]) in = TRUE;
        if (!in) brk[nb++] = prev[i];
    }
    for (ULONG i = 0; i < n && cur[i]; i++) {
        BOOL in = FALSE;
        for (ULONG j = 0; j < n && prev[j]; j++) if (prev[j] == cur[i]) in = TRUE;
        if (!in) make[nm++] = cur[i];
    }
    while (nb < n) brk[nb++] = 0;
    while (nm < n) make[nm++] = 0;
    return HIDP_STATUS_SUCCESS;
}

HIDAPI_ LONG WINAPI HidP_UsageAndPageListDifference(PUSAGE_AND_PAGE prev, PUSAGE_AND_PAGE cur, PUSAGE_AND_PAGE brk,
                                                    PUSAGE_AND_PAGE make, ULONG n)
{
    ULONG nb = 0, nm = 0;
    for (ULONG i = 0; i < n && prev[i].Usage; i++) {
        BOOL in = FALSE;
        for (ULONG j = 0; j < n && cur[j].Usage; j++) if (cur[j].Usage == prev[i].Usage && cur[j].UsagePage == prev[i].UsagePage) in = TRUE;
        if (!in) brk[nb++] = prev[i];
    }
    for (ULONG i = 0; i < n && cur[i].Usage; i++) {
        BOOL in = FALSE;
        for (ULONG j = 0; j < n && prev[j].Usage; j++) if (prev[j].Usage == cur[i].Usage && prev[j].UsagePage == cur[i].UsagePage) in = TRUE;
        if (!in) make[nm++] = cur[i];
    }
    while (nb < n) { brk[nb].Usage = brk[nb].UsagePage = 0; nb++; }
    while (nm < n) { make[nm].Usage = make[nm].UsagePage = 0; nm++; }
    return HIDP_STATUS_SUCCESS;
}

/* Keyboards are not HID devices of NovaOS's */
HIDAPI_ LONG WINAPI HidP_TranslateUsagesToI8042ScanCodes(PUSAGE list, ULONG n, int action, PVOID modifiers,
                                                         PVOID insert, PVOID ctx)
{
    (void)list; (void)n; (void)action; (void)modifiers; (void)insert; (void)ctx;
    return HIDP_STATUS_I8042_TRANS_UNKNOWN;
}
