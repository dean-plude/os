/*
 * ndr_types.c — NDR for one type at a time: the buffer size it needs, its
 * marshaling into the buffer, its unmarshaling out of it and freeing what
 * an unmarshal allocated, all read from MIDL's type format strings.
 *
 * Supported: the base types, pointers (ref, unique, full treated as
 * unique), simple, conformant and complex structures, fixed, conformant,
 * conformant-varying and complex arrays, strings, interface pointers
 * (through ole32's CoMarshalInterface, so the pointer arrives as a proxy)
 * and user-marshaled types (BSTR, VARIANT: the routines the stub
 * descriptor names).  Unions, pipes, transmit_as/represent_as and context
 * handles are not, and fail with RPC_X_BAD_STUB_DATA.
 *
 * Pointers inside structures and arrays are deferred as NDR wants: the
 * flat part first (a nonzero referent ID per non-null pointer), the data
 * pointed to after it, breadth first.  Both ends here are NovaOS's, but
 * keeping NDR's order means a 32-bit caller (whose MIDL format strings
 * copy structures as blocks) and a 64-bit server (whose format strings
 * walk them member by member) agree.
 */
#include "ndr.h"

enum { M_SIZE, M_MARSHAL, M_UNMARSHAL, M_FREE };

void ndr_fail(PMIDL_STUB_MESSAGE sm, long status)
{
    if (!NDR_ERR(sm)) NDR_ERR(sm) = status ? status : RPC_X_BAD_STUB_DATA;
}

/* ---- the buffer -------------------------------------------------------- */
void ndr_align(PMIDL_STUB_MESSAGE sm, unsigned n)
{
    if (NDR_FAILED(sm) || n < 2) return;
    ULONG_PTR off = (ULONG_PTR)(sm->Buffer - sm->BufferStart);
    ULONG_PTR to = (off + n - 1) & ~(ULONG_PTR)(n - 1);
    if (sm->BufferStart + to > sm->BufferEnd) { ndr_fail(sm, RPC_X_BAD_STUB_DATA); return; }
    while (off < to) sm->BufferStart[off++] = 0;
    sm->Buffer = sm->BufferStart + to;
}
void ndr_size_align(PMIDL_STUB_MESSAGE sm, unsigned n)
{
    if (n >= 2) sm->BufferLength = (sm->BufferLength + n - 1) & ~(unsigned long)(n - 1);
}
unsigned char *ndr_take(PMIDL_STUB_MESSAGE sm, unsigned long n)
{
    if (NDR_FAILED(sm)) return NULL;
    if (n > (ULONG_PTR)(sm->BufferEnd - sm->Buffer)) { ndr_fail(sm, RPC_X_BAD_STUB_DATA); return NULL; }
    unsigned char *p = sm->Buffer;
    sm->Buffer += n;
    return p;
}
void ndr_put(PMIDL_STUB_MESSAGE sm, const void *p, unsigned long n)
{
    unsigned char *d = ndr_take(sm, n);
    if (d && n) memcpy(d, p, n);
}
void ndr_put32(PMIDL_STUB_MESSAGE sm, unsigned long v)
{
    ndr_align(sm, 4);
    ndr_put(sm, &v, 4);
}
unsigned long ndr_get32(PMIDL_STUB_MESSAGE sm)
{
    ndr_align(sm, 4);
    unsigned char *p = ndr_take(sm, 4);
    return p ? rd32(p) : 0;
}
void *ndr_alloc(PMIDL_STUB_MESSAGE sm, size_t n)
{
    if (!n) n = 1;
    void *p = sm->pfnAllocate ? sm->pfnAllocate(n) : NdrOleAllocate(n);
    if (!p) { ndr_fail(sm, RPC_S_OUT_OF_MEMORY); return NULL; }
    memset(p, 0, n);
    return p;
}
void ndr_free(PMIDL_STUB_MESSAGE sm, void *p)
{
    if (!p) return;
    if (sm->pfnFree) sm->pfnFree(p); else NdrOleFree(p);
}
RPCRTAPI void *RPC_ENTRY NdrAllocate(PMIDL_STUB_MESSAGE sm, size_t n) { return ndr_alloc(sm, n); }

/* ---- base types -------------------------------------------------------- */
unsigned ndr_base_size(unsigned char fc)
{
    switch (fc) {
    case FC_BYTE: case FC_CHAR: case FC_SMALL: case FC_USMALL: return 1;
    case FC_WCHAR: case FC_SHORT: case FC_USHORT: case FC_ENUM16: return 2;
    case FC_LONG: case FC_ULONG: case FC_FLOAT: case FC_ENUM32: case FC_ERROR_STATUS_T:
    case FC_INT3264: case FC_UINT3264: case FC_IGNORE: return 4;
    case FC_HYPER: case FC_DOUBLE: return 8;
    }
    return 0;
}
unsigned ndr_base_memsize(unsigned char fc)
{
    switch (fc) {
    case FC_ENUM16: return 4;
    case FC_INT3264: case FC_UINT3264: case FC_IGNORE: return sizeof(void *);
    }
    return ndr_base_size(fc);
}
void ndr_base_size_type(PMIDL_STUB_MESSAGE sm, unsigned char fc)
{
    unsigned n = ndr_base_size(fc);
    ndr_size_align(sm, n);
    sm->BufferLength += n;
}
void ndr_base_marshal(PMIDL_STUB_MESSAGE sm, const unsigned char *mem, unsigned char fc)
{
    unsigned n = ndr_base_size(fc);
    if (!n) { ndr_fail(sm, RPC_X_BAD_STUB_DATA); return; }
    ndr_align(sm, n);
    if (fc == FC_ENUM16) {
        int v = *(const int *)mem;
        if (v < 0 || v > 0x7fff) { ndr_fail(sm, RPC_X_ENUM_VALUE_OUT_OF_RANGE); return; }
        unsigned short s = (unsigned short)v;
        ndr_put(sm, &s, 2);
    } else if (fc == FC_IGNORE) {
        ndr_put32(sm, 0);
    } else
        ndr_put(sm, mem, n);                       /* INT3264: the low half, as Windows sends it */
}
void ndr_base_unmarshal(PMIDL_STUB_MESSAGE sm, unsigned char *mem, unsigned char fc)
{
    unsigned n = ndr_base_size(fc);
    if (!n) { ndr_fail(sm, RPC_X_BAD_STUB_DATA); return; }
    ndr_align(sm, n);
    unsigned char *p = ndr_take(sm, n);
    if (!p) return;
    switch (fc) {
    case FC_ENUM16: *(int *)mem = (int)rd16(p); break;
    case FC_INT3264: *(LONG_PTR *)mem = (LONG_PTR)(LONG)rd32(p); break;
    case FC_UINT3264: *(ULONG_PTR *)mem = (ULONG_PTR)rd32(p); break;
    case FC_IGNORE: *(void **)mem = NULL; break;
    default: memcpy(mem, p, n);
    }
}

int ndr_is_pointer(unsigned char fc) { return fc >= FC_RP && fc <= FC_FP; }

static unsigned corr_len(PMIDL_STUB_MESSAGE sm) { return sm->fHasNewCorrDesc ? 6 : 4; }

/* ---- correlation (size_is, length_is, iid_is) ------------------------- */
PFORMAT_STRING ndr_conformance(PMIDL_STUB_MESSAGE sm, unsigned char *base, PFORMAT_STRING d, ULONG_PTR *out)
{
    PFORMAT_STRING next = d + corr_len(sm);
    unsigned char kind = d[0] & 0xf0, type = d[0] & 0x0f, op = d[1];
    short ofs = rds16(d + 2);
    *out = 0;
    if (rd32(d) == 0xffffffffUL) { *out = (ULONG_PTR)-1; return next; }     /* none */
    if (kind == FC_CONSTANT_CONFORMANCE) { *out = (ULONG_PTR)op << 16 | rd16(d + 2); return next; }
    unsigned char *p = kind == FC_TOP_LEVEL_CONFORMANCE ? sm->StackTop
                     : kind == FC_POINTER_CONFORMANCE ? sm->Memory : base;
    if (!p) { ndr_fail(sm, RPC_X_BAD_STUB_DATA); return next; }
    if (op == FC_CALLBACK) {
        unsigned char *old = sm->StackTop;
        if (kind != FC_TOP_LEVEL_CONFORMANCE) sm->StackTop = p;
        if (sm->StubDesc && sm->StubDesc->apfnExprEval) sm->StubDesc->apfnExprEval[(unsigned short)ofs](sm);
        sm->StackTop = old;
        *out = sm->MaxCount;
        return next;
    }
    p += ofs;
    if (op == FC_DEREFERENCE) {
        p = *(unsigned char **)p;
        if (!p) { ndr_fail(sm, RPC_X_NULL_REF_POINTER); return next; }
    }
    LONG_PTR v;
    switch (type) {
    case FC_BYTE: case FC_CHAR: case FC_USMALL: v = *(unsigned char *)p; break;
    case FC_SMALL: v = *(signed char *)p; break;
    case FC_WCHAR: case FC_USHORT: v = *(unsigned short *)p; break;
    case FC_SHORT: v = *(short *)p; break;
    case FC_ULONG: v = (LONG_PTR)*(unsigned long *)p; break;
    case FC_HYPER: v = *(LONG_PTR *)p; break;                    /* a pointer-sized value (iid_is) */
    default: v = *(long *)p; break;
    }
    switch (op) {
    case FC_DIV_2: v /= 2; break;
    case FC_MULT_2: v *= 2; break;
    case FC_ADD_1: v += 1; break;
    case FC_SUB_1: v -= 1; break;
    }
    *out = (ULONG_PTR)v;
    return next;
}

/* ---- deferred pointees ------------------------------------------------- */
typedef struct { unsigned char *mem, *base; PFORMAT_STRING f; } DItem;
typedef struct { DItem *items; int n, cap, head; } DQueue;
#define QUEUE(sm) ((DQueue *)(sm)->pPointerQueueState)

static void defer(PMIDL_STUB_MESSAGE sm, unsigned char *mem, PFORMAT_STRING f)
{
    DQueue *q = QUEUE(sm);
    if (!q) { ndr_fail(sm, RPC_S_INTERNAL_ERROR); return; }
    if (q->n == q->cap) {
        int cap = q->cap ? q->cap * 2 : 16;
        DItem *n = HeapAlloc(GetProcessHeap(), 0, cap * sizeof *n);
        if (!n) { ndr_fail(sm, RPC_S_OUT_OF_MEMORY); return; }
        if (q->n) memcpy(n, q->items, q->n * sizeof *n);
        HeapFree(GetProcessHeap(), 0, q->items);
        q->items = n;
        q->cap = cap;
    }
    q->items[q->n].mem = mem;
    q->items[q->n].base = sm->Memory;
    q->items[q->n].f = f;
    q->n++;
}

static void type_op(PMIDL_STUB_MESSAGE sm, int mode, unsigned char *mem, unsigned char **pmem, PFORMAT_STRING f, int alloc);

/* what a pointer description @f points to */
static PFORMAT_STRING pointee_fmt(PFORMAT_STRING f)
{
    return f[1] & FC_SIMPLE_POINTER ? f + 2 : f + 2 + rds16(f + 2);
}

/* the data behind a (non-null) pointer: @ptr for size/marshal/free, @loc
 * (where the pointer lives) for unmarshal */
static void pointee_op(PMIDL_STUB_MESSAGE sm, int mode, unsigned char *ptr, unsigned char **loc, PFORMAT_STRING f, int alloc)
{
    PFORMAT_STRING d = pointee_fmt(f);
    if (f[1] & FC_SIMPLE_POINTER) {
        switch (mode) {
        case M_SIZE: ndr_base_size_type(sm, d[0]); break;
        case M_MARSHAL: ndr_base_marshal(sm, ptr, d[0]); break;
        case M_UNMARSHAL:
            if (!*loc || alloc) *loc = ndr_alloc(sm, ndr_base_memsize(d[0]));
            if (*loc) ndr_base_unmarshal(sm, *loc, d[0]);
            break;
        }
    } else if (f[1] & FC_POINTER_DEREF) {
        if (mode == M_UNMARSHAL) {
            if (!*loc || alloc) *loc = ndr_alloc(sm, sizeof(void *));
            if (*loc) type_op(sm, mode, NULL, (unsigned char **)*loc, d, alloc);
        } else
            type_op(sm, mode, *(unsigned char **)ptr, NULL, d, 0);
    } else
        type_op(sm, mode, ptr, loc, d, alloc);
}

/* a pointer: @embedded ones (in structures and arrays) defer their data;
 * @flat ones were copied with their structure's block (their referent ID
 * is the pointer value itself, as 32-bit MIDL lays such structures out) */
static void ptr_op(PMIDL_STUB_MESSAGE sm, int mode, unsigned char *ptr, unsigned char **loc, PFORMAT_STRING f,
                   int embedded, int flat, int alloc)
{
    unsigned char type = f[0];
    if (NDR_FAILED(sm)) return;
    if (!ndr_is_pointer(type)) { ndr_fail(sm, RPC_X_BAD_STUB_DATA); return; }
    switch (mode) {
    case M_SIZE:
        if (type != FC_RP && !flat) { ndr_size_align(sm, 4); sm->BufferLength += 4; }
        if (!ptr) return;
        break;
    case M_MARSHAL:
        if (type == FC_RP && !ptr) { ndr_fail(sm, RPC_X_NULL_REF_POINTER); return; }
        if (type != FC_RP && !flat) ndr_put32(sm, ptr ? 0x20000u + 4u * ++sm->UniquePtrCount : 0);
        if (!ptr) return;
        break;
    case M_UNMARSHAL: {
        ULONG_PTR id = 1;
        if (flat) id = *(ULONG_PTR *)loc;
        else if (type != FC_RP) id = ndr_get32(sm);
        if (flat || embedded) *loc = NULL;              /* the data arrives later, in memory we allocate */
        if (!id) { *loc = NULL; return; }
        break;
    }
    case M_FREE:
        if (!ptr) return;
        pointee_op(sm, M_FREE, ptr, NULL, f, 0);
        if (!(f[1] & FC_DONT_FREE)) ndr_free(sm, ptr);
        return;
    }
    if (embedded) defer(sm, mode == M_UNMARSHAL ? (unsigned char *)loc : ptr, f);
    else pointee_op(sm, mode, ptr, loc, f, alloc);
}

/* a pointer layout (FC_PP ... FC_END) over memory @base; @count repeats
 * the variable part (conformant arrays) */
static void layout_op(PMIDL_STUB_MESSAGE sm, int mode, unsigned char *base, PFORMAT_STRING p, ULONG_PTR count)
{
    if (!base || p[0] != FC_PP) return;
    p += 2;
    while (*p != FC_END && !NDR_FAILED(sm)) {
        ULONG_PTR iters, inc, arr, n;
        PFORMAT_STRING q;
        if (*p == FC_NO_REPEAT) {
            iters = 1; inc = 0; arr = 0; n = 1; q = p + 2;
        } else if (*p == FC_FIXED_REPEAT) {
            iters = rd16(p + 2); inc = rd16(p + 4); arr = rd16(p + 6); n = rd16(p + 8); q = p + 10;
        } else if (*p == FC_VARIABLE_REPEAT) {
            iters = count; inc = rd16(p + 2); arr = rd16(p + 4); n = rd16(p + 6); q = p + 8;
        } else { ndr_fail(sm, RPC_X_BAD_STUB_DATA); return; }
        for (ULONG_PTR i = 0; i < iters; i++)
            for (ULONG_PTR j = 0; j < n; j++) {
                unsigned char *field = base + arr + i * inc + rds16(q + j * 8);
                ptr_op(sm, mode, *(unsigned char **)field, (unsigned char **)field, q + j * 8 + 4, 1, 1, 1);
            }
        p = q + n * 8;
    }
}

/* where a structure's or array's pointer layout starts, if it has one */
static PFORMAT_STRING find_layout(PFORMAT_STRING p) { return p[0] == FC_PP ? p : NULL; }

/* ---- structures -------------------------------------------------------- */
static unsigned long array_elem_memsize(PMIDL_STUB_MESSAGE sm, PFORMAT_STRING a);
static void array_op(PMIDL_STUB_MESSAGE sm, int mode, unsigned char *mem, unsigned char **pmem, PFORMAT_STRING f,
                     int alloc, int in_struct);

/* FC_STRUCT, FC_PSTRUCT, FC_CSTRUCT, FC_CPSTRUCT: a block, then a
 * conformant array (C*), then the layout's pointers */
static void block_struct_op(PMIDL_STUB_MESSAGE sm, int mode, unsigned char *mem, unsigned char **pmem, PFORMAT_STRING f, int alloc)
{
    unsigned align = f[1] + 1, size = rd16(f + 2);
    int conformant = f[0] == FC_CSTRUCT || f[0] == FC_CPSTRUCT;
    PFORMAT_STRING arr = conformant ? f + 4 + rds16(f + 4) : NULL;
    PFORMAT_STRING lay = find_layout(f + (conformant ? 6 : 4));
    ULONG_PTR count = 0;
    unsigned long esize = arr ? rd16(arr + 2) : 0;
    unsigned char *old = sm->Memory;
    if (arr && (mode == M_SIZE || mode == M_MARSHAL || mode == M_FREE))
        ndr_conformance(sm, mem + size, arr + 4, &count);
    switch (mode) {
    case M_SIZE:
        if (arr) { ndr_size_align(sm, 4); sm->BufferLength += 4; }
        ndr_size_align(sm, align);
        sm->BufferLength += size + count * esize;
        break;
    case M_MARSHAL:
        if (arr) ndr_put32(sm, (unsigned long)count);
        ndr_align(sm, align);
        ndr_put(sm, mem, size + count * esize);
        break;
    case M_UNMARSHAL: {
        if (arr) count = ndr_get32(sm);
        ndr_align(sm, align);
        unsigned char *src = ndr_take(sm, size + count * esize);
        if (!src) return;
        if (!*pmem || alloc) *pmem = ndr_alloc(sm, size + count * esize);
        if (!*pmem) return;
        memcpy(*pmem, src, size + count * esize);
        mem = *pmem;
        break;
    }
    }
    sm->Memory = mem;
    if (lay) layout_op(sm, mode, mem, lay, count);
    if (arr) {                                  /* pointers in the array's own layout */
        PFORMAT_STRING alay = find_layout(arr + 4 + corr_len(sm));
        if (alay) layout_op(sm, mode, mem + size, alay, count);
    }
    sm->Memory = old;
}

/* FC_BOGUS_STRUCT: member by member */
static void bogus_struct_op(PMIDL_STUB_MESSAGE sm, int mode, unsigned char *mem, unsigned char **pmem, PFORMAT_STRING f, int alloc)
{
    unsigned align = f[1] + 1, size = rd16(f + 2);
    PFORMAT_STRING arr = rd16(f + 4) ? f + 4 + rds16(f + 4) : NULL;
    PFORMAT_STRING ptrs = rd16(f + 6) ? f + 6 + rds16(f + 6) : NULL;
    ULONG_PTR count = 0;
    unsigned long esize = arr ? array_elem_memsize(sm, arr) : 0;
    if (arr) {
        if (mode == M_SIZE) { ndr_size_align(sm, 4); sm->BufferLength += 4; }
        else if (mode == M_UNMARSHAL) count = ndr_get32(sm);
        else {
            PFORMAT_STRING cd = arr[0] == FC_C_CSTRING || arr[0] == FC_C_WSTRING ? NULL : arr + 4;
            if (cd) ndr_conformance(sm, mem + size, cd, &count);
            if (mode == M_MARSHAL) ndr_put32(sm, (unsigned long)count);
        }
    }
    if (mode == M_UNMARSHAL) {
        if (!*pmem || alloc) *pmem = ndr_alloc(sm, size + count * esize);
        if (!*pmem) return;
        mem = *pmem;
    }
    if (mode == M_SIZE) ndr_size_align(sm, align);
    else if (mode != M_FREE) ndr_align(sm, align);
    unsigned char *old = sm->Memory;
    sm->Memory = mem;
    unsigned char *field = mem;
    PFORMAT_STRING p = f + 8;
    while (*p != FC_END && !NDR_FAILED(sm)) {
        unsigned char c = *p;
        if (ndr_base_size(c)) {
            switch (mode) {
            case M_SIZE: ndr_base_size_type(sm, c); break;
            case M_MARSHAL: ndr_base_marshal(sm, field, c); break;
            case M_UNMARSHAL: ndr_base_unmarshal(sm, field, c); break;
            }
            field += ndr_base_memsize(c);
            p++;
        } else if (c == FC_POINTER) {
            if (!ptrs) { ndr_fail(sm, RPC_X_BAD_STUB_DATA); break; }
            ptr_op(sm, mode, *(unsigned char **)field, (unsigned char **)field, ptrs, 1, 0, 1);
            ptrs += 4;
            field += sizeof(void *);
            p++;
        } else if (c == FC_EMBEDDED_COMPLEX) {
            field += p[1];
            PFORMAT_STRING t = p + 2 + rds16(p + 2);
            if (ndr_is_pointer(t[0]))                 /* (older MIDL: a pointer member described in line) */
                ptr_op(sm, mode, *(unsigned char **)field, (unsigned char **)field, t, 1, 0, 1);
            else if (mode == M_UNMARSHAL) {
                unsigned char *at = field;
                type_op(sm, mode, NULL, &at, t, 0);
            } else
                type_op(sm, mode, field, NULL, t, 0);
            field += ndr_memsize(sm, t);
            p += 4;
        } else if (c >= FC_ALIGNM2 && c <= FC_ALIGNM8) {
            ULONG_PTR a = c == FC_ALIGNM2 ? 2 : c == FC_ALIGNM4 ? 4 : 8, o = field - mem;
            field = mem + ((o + a - 1) & ~(a - 1));
            p++;
        } else if (c >= FC_STRUCTPAD1 && c <= FC_STRUCTPAD7) {
            field += c - FC_STRUCTPAD1 + 1;
            p++;
        } else if (c == FC_PAD) {
            p++;
        } else {
            ndr_fail(sm, RPC_X_BAD_STUB_DATA);
            break;
        }
    }
    if (arr && !NDR_FAILED(sm)) {
        sm->MaxCount = count;
        if (mode == M_UNMARSHAL) {
            unsigned char *at = mem + size;
            array_op(sm, mode, NULL, &at, arr, 0, 1);
        } else
            array_op(sm, mode, mem + size, NULL, arr, 0, 1);
    }
    sm->Memory = old;
}

/* ---- arrays ------------------------------------------------------------ */
/* the element description of a complex array, after its descriptors */
static PFORMAT_STRING bogus_elem(PMIDL_STUB_MESSAGE sm, PFORMAT_STRING a) { return a + 4 + 2 * corr_len(sm); }

static unsigned long elem_memsize(PMIDL_STUB_MESSAGE sm, PFORMAT_STRING e)
{
    if (e[0] == FC_EMBEDDED_COMPLEX) return ndr_memsize(sm, e + 2 + rds16(e + 2));
    if (ndr_is_pointer(e[0])) return sizeof(void *);
    return ndr_memsize(sm, e);
}
static unsigned long array_elem_memsize(PMIDL_STUB_MESSAGE sm, PFORMAT_STRING a)
{
    switch (a[0]) {
    case FC_CARRAY: case FC_CVARRAY: return rd16(a + 2);
    case FC_BOGUS_ARRAY: return elem_memsize(sm, bogus_elem(sm, a));
    case FC_C_WSTRING: return 2;
    case FC_C_CSTRING: return 1;
    }
    return 0;
}

/* one complex-array element at @at */
static void elem_op(PMIDL_STUB_MESSAGE sm, int mode, unsigned char *at, PFORMAT_STRING e)
{
    if (e[0] == FC_EMBEDDED_COMPLEX) e = e + 2 + rds16(e + 2);
    if (ndr_is_pointer(e[0]))
        ptr_op(sm, mode, *(unsigned char **)at, (unsigned char **)at, e, 1, 0, 1);
    else if (ndr_base_size(e[0])) {
        switch (mode) {
        case M_SIZE: ndr_base_size_type(sm, e[0]); break;
        case M_MARSHAL: ndr_base_marshal(sm, at, e[0]); break;
        case M_UNMARSHAL: ndr_base_unmarshal(sm, at, e[0]); break;
        }
    } else if (mode == M_UNMARSHAL) {
        unsigned char *p = at;
        type_op(sm, mode, NULL, &p, e, 0);
    } else
        type_op(sm, mode, at, NULL, e, 0);
}

/* fixed, conformant, conformant-varying and complex arrays; @in_struct:
 * the enclosing structure handled the conformance (sm->MaxCount) */
static void array_op(PMIDL_STUB_MESSAGE sm, int mode, unsigned char *mem, unsigned char **pmem, PFORMAT_STRING f,
                     int alloc, int in_struct)
{
    unsigned align = f[1] + 1;
    unsigned char fc = f[0];
    ULONG_PTR count, offset = 0, actual;
    unsigned long esize;
    PFORMAT_STRING cd = NULL, vd = NULL, after, elem = NULL;
    switch (fc) {
    case FC_SMFARRAY: count = rd16(f + 2); esize = 1; after = f + 4; break;
    case FC_LGFARRAY: count = rd32(f + 2); esize = 1; after = f + 6; break;
    case FC_CARRAY: esize = rd16(f + 2); cd = f + 4; after = cd + corr_len(sm); count = 0; break;
    case FC_CVARRAY: esize = rd16(f + 2); cd = f + 4; vd = cd + corr_len(sm); after = vd + corr_len(sm); count = 0; break;
    case FC_BOGUS_ARRAY:
        count = rd16(f + 2);
        cd = f + 4; vd = cd + corr_len(sm);
        elem = bogus_elem(sm, f);
        esize = elem_memsize(sm, elem);
        if (rd32(cd) == 0xffffffffUL) cd = NULL;
        if (rd32(vd) == 0xffffffffUL) vd = NULL;
        after = elem;
        break;
    default: ndr_fail(sm, RPC_X_BAD_STUB_DATA); return;
    }
    /* how many elements, and which of them travel */
    if (cd) {
        if (in_struct) count = sm->MaxCount;
        else if (mode == M_SIZE) { ndr_size_align(sm, 4); sm->BufferLength += 4; }
        else if (mode == M_UNMARSHAL) count = ndr_get32(sm);
        if (!in_struct && mode != M_UNMARSHAL) {
            ndr_conformance(sm, sm->Memory, cd, &count);
            if (mode == M_MARSHAL) ndr_put32(sm, (unsigned long)count);
        }
    }
    actual = count;
    if (vd) {
        if (mode == M_SIZE) { ndr_size_align(sm, 4); sm->BufferLength += 8; ndr_conformance(sm, sm->Memory, vd, &actual); }
        else if (mode == M_UNMARSHAL) { offset = ndr_get32(sm); actual = ndr_get32(sm); }
        else {
            ndr_conformance(sm, sm->Memory, vd, &actual);
            if (mode == M_MARSHAL) { ndr_put32(sm, 0); ndr_put32(sm, (unsigned long)actual); }
        }
        if (offset > count || actual > count - offset) { ndr_fail(sm, RPC_X_INVALID_BOUND); return; }
    }
    if (NDR_FAILED(sm)) return;
    if (count > 0x10000000UL / (esize ? esize : 1)) { ndr_fail(sm, RPC_X_INVALID_BOUND); return; }
    if (mode == M_UNMARSHAL) {
        if (!*pmem || alloc) *pmem = ndr_alloc(sm, count * esize);
        if (!*pmem) return;
        mem = *pmem;
    }
    if (!mem && actual) { ndr_fail(sm, RPC_X_NULL_REF_POINTER); return; }
    unsigned char *old = sm->Memory;
    if (fc == FC_BOGUS_ARRAY) {
        if (mode == M_SIZE) ndr_size_align(sm, align);
        else if (mode != M_FREE) ndr_align(sm, align);
        for (ULONG_PTR i = offset; i < offset + actual && !NDR_FAILED(sm); i++)
            elem_op(sm, mode, mem + i * esize, elem);
    } else {
        unsigned long bytes = (unsigned long)(actual * esize);
        switch (mode) {
        case M_SIZE: ndr_size_align(sm, align); sm->BufferLength += bytes; break;
        case M_MARSHAL: ndr_align(sm, align); ndr_put(sm, mem + offset * esize, bytes); break;
        case M_UNMARSHAL: {
            ndr_align(sm, align);
            unsigned char *src = ndr_take(sm, bytes);
            if (src) memcpy(mem + offset * esize, src, bytes);
            break;
        }
        }
        PFORMAT_STRING lay = find_layout(after);
        if (lay) layout_op(sm, mode, mem + offset * esize, lay, actual);
    }
    sm->Memory = old;
}

/* ---- strings ----------------------------------------------------------- */
static void string_op(PMIDL_STUB_MESSAGE sm, int mode, unsigned char *mem, unsigned char **pmem, PFORMAT_STRING f, int alloc)
{
    unsigned cs = f[0] == FC_C_WSTRING || f[0] == FC_WSTRING ? 2 : 1;
    int conformant = f[0] == FC_C_CSTRING || f[0] == FC_C_WSTRING;
    ULONG_PTR max = conformant ? 0 : rd16(f + 2), len = 0;
    if (mode == M_FREE) return;
    if (mode != M_UNMARSHAL) {
        if (!mem) { ndr_fail(sm, RPC_X_NULL_REF_POINTER); return; }
        if (cs == 2) while (((const WCHAR *)mem)[len]) len++;
        else while (mem[len]) len++;
        len++;
        if (conformant) {
            max = len;
            if (f[1] == FC_STRING_SIZED) ndr_conformance(sm, sm->Memory, f + 2, &max);
        }
        if (len > max) { ndr_fail(sm, RPC_X_INVALID_BOUND); return; }
    }
    switch (mode) {
    case M_SIZE:
        ndr_size_align(sm, 4);
        sm->BufferLength += (conformant ? 12 : 8) + (unsigned long)(len * cs);
        break;
    case M_MARSHAL:
        if (conformant) ndr_put32(sm, (unsigned long)max);
        ndr_put32(sm, 0);
        ndr_put32(sm, (unsigned long)len);
        ndr_put(sm, mem, (unsigned long)(len * cs));
        break;
    case M_UNMARSHAL: {
        if (conformant) max = ndr_get32(sm);
        ULONG_PTR off = ndr_get32(sm);
        len = ndr_get32(sm);
        if (off || !len || len > max || max > 0x10000000UL) { ndr_fail(sm, RPC_X_INVALID_BOUND); return; }
        unsigned char *src = ndr_take(sm, (unsigned long)(len * cs));
        if (!src) return;
        if (!*pmem || alloc) *pmem = ndr_alloc(sm, (conformant && f[1] != FC_STRING_SIZED ? len : max) * cs);
        if (!*pmem) return;
        memcpy(*pmem, src, len * cs);
        memset(*pmem + (len - 1) * cs, 0, cs);           /* always terminated */
        break;
    }
    }
}

/* ---- interface pointers, through ole32 --------------------------------- */
/* an IStream over part of the NDR buffer */
typedef struct { IStream s; unsigned char *base; ULONG pos, len, cap; } BufStream;
static HRESULT STDMETHODCALLTYPE bs_qi(IStream *This, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IStream) || IsEqualIID(riid, &IID_ISequentialStream)) {
        *ppv = This;
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE bs_addref(IStream *This) { (void)This; return 2; }
static ULONG STDMETHODCALLTYPE bs_release(IStream *This) { (void)This; return 1; }
static HRESULT STDMETHODCALLTYPE bs_read(IStream *This, void *pv, ULONG cb, ULONG *got)
{
    BufStream *s = (BufStream *)This;
    ULONG n = s->pos < s->len ? s->len - s->pos : 0;
    if (n > cb) n = cb;
    memcpy(pv, s->base + s->pos, n);
    s->pos += n;
    if (got) *got = n;
    return n == cb ? S_OK : S_FALSE;
}
static HRESULT STDMETHODCALLTYPE bs_write(IStream *This, const void *pv, ULONG cb, ULONG *put)
{
    BufStream *s = (BufStream *)This;
    if (put) *put = 0;
    if (cb > s->cap - s->pos) return (HRESULT)0x80030070L;          /* STG_E_MEDIUMFULL */
    memcpy(s->base + s->pos, pv, cb);
    s->pos += cb;
    if (s->pos > s->len) s->len = s->pos;
    if (put) *put = cb;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE bs_seek(IStream *This, LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER *pos)
{
    BufStream *s = (BufStream *)This;
    LONGLONG to = (origin == 1 ? s->pos : origin == 2 ? s->len : 0) + move.QuadPart;
    if (to < 0 || to > s->cap) return (HRESULT)0x80030019L;          /* STG_E_INVALIDFUNCTION */
    s->pos = (ULONG)to;
    if (pos) pos->QuadPart = s->pos;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE bs_setsize(IStream *This, ULARGE_INTEGER n) { (void)This; (void)n; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE bs_copyto(IStream *This, IStream *to, ULARGE_INTEGER cb, ULARGE_INTEGER *r, ULARGE_INTEGER *w)
{ (void)This; (void)to; (void)cb; (void)r; (void)w; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE bs_commit(IStream *This, DWORD f) { (void)This; (void)f; return S_OK; }
static HRESULT STDMETHODCALLTYPE bs_revert(IStream *This) { (void)This; return S_OK; }
static HRESULT STDMETHODCALLTYPE bs_lock(IStream *This, ULARGE_INTEGER o, ULARGE_INTEGER n, DWORD t)
{ (void)This; (void)o; (void)n; (void)t; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE bs_stat(IStream *This, STATSTG *st, DWORD flags)
{
    (void)flags;
    memset(st, 0, sizeof *st);
    st->type = 2;                                   /* STGTY_STREAM */
    st->cbSize.QuadPart = ((BufStream *)This)->len;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE bs_clone(IStream *This, IStream **out) { (void)This; *out = NULL; return E_NOTIMPL; }
static const IStreamVtbl bs_vtbl = {
    bs_qi, bs_addref, bs_release, bs_read, bs_write, bs_seek, bs_setsize, bs_copyto,
    bs_commit, bs_revert, bs_lock, bs_lock, bs_stat, bs_clone,
};

static const IID *ip_iid(PMIDL_STUB_MESSAGE sm, PFORMAT_STRING f)
{
    if (f[1] == FC_CONSTANT_IID) return (const IID *)(f + 2);
    ULONG_PTR v = 0;
    ndr_conformance(sm, sm->Memory, f + 2, &v);
    if (!v) ndr_fail(sm, RPC_X_NULL_REF_POINTER);
    return (const IID *)v;
}

static void ip_op(PMIDL_STUB_MESSAGE sm, int mode, unsigned char *mem, unsigned char **pmem, PFORMAT_STRING f)
{
    const Ole32Fns *ole = ndr_ole32();
    if (!ole) { ndr_fail(sm, RPC_S_INTERNAL_ERROR); return; }
    switch (mode) {
    case M_SIZE: {
        ndr_size_align(sm, 4);
        sm->BufferLength += 4;
        if (!mem) return;
        const IID *iid = ip_iid(sm, f);
        ULONG n = 0;
        if (!iid || FAILED(ole->CoGetMarshalSizeMax(&n, iid, (IUnknown *)mem, sm->dwDestContext, sm->pvDestContext, 0))) n = 1024;
        sm->BufferLength += 4 + n;
        return;
    }
    case M_MARSHAL: {
        ndr_put32(sm, mem ? 0x20000u + 4u * ++sm->UniquePtrCount : 0);
        if (!mem) return;
        const IID *iid = ip_iid(sm, f);
        unsigned char *lenp = ndr_take(sm, 4);
        if (!lenp || !iid) return;
        BufStream s = { { &bs_vtbl }, sm->Buffer, 0, 0, (ULONG)(sm->BufferEnd - sm->Buffer) };
        HRESULT hr = ole->CoMarshalInterface(&s.s, iid, (IUnknown *)mem, sm->dwDestContext, sm->pvDestContext, 0);
        if (FAILED(hr)) { ndr_fail(sm, hr); return; }
        memcpy(lenp, &s.len, 4);
        sm->Buffer += s.len;
        return;
    }
    case M_UNMARSHAL: {
        if (!ndr_get32(sm)) { *pmem = NULL; return; }
        ULONG len = ndr_get32(sm);
        unsigned char *data = ndr_take(sm, len);
        if (!data) return;
        BufStream s = { { &bs_vtbl }, data, 0, len, len };
        *pmem = NULL;
        HRESULT hr = ole->CoUnmarshalInterface(&s.s, &IID_NULL, (void **)pmem);
        if (FAILED(hr)) { *pmem = NULL; ndr_fail(sm, hr); }
        return;
    }
    case M_FREE:
        if (mem) ((IUnknown *)mem)->lpVtbl->Release((IUnknown *)mem);
        return;
    }
}

/* ---- user-marshaled types (BSTR, VARIANT, ...) ------------------------- */
static void user_op(PMIDL_STUB_MESSAGE sm, int mode, unsigned char *mem, unsigned char **pmem, PFORMAT_STRING f, int alloc)
{
    unsigned char flags = f[1];
    unsigned align = (flags & 0x0f) + 1;
    const USER_MARSHAL_ROUTINE_QUADRUPLE *q = sm->StubDesc && sm->StubDesc->aUserMarshalQuadruple
        ? &sm->StubDesc->aUserMarshalQuadruple[rd16(f + 2)] : NULL;
    if (!q) { ndr_fail(sm, RPC_X_BAD_STUB_DATA); return; }
    USER_MARSHAL_CB cb;
    memset(&cb, 0, sizeof cb);
    cb.Flags = (NDR_LOCAL_DATA_REPRESENTATION << 16) | (sm->dwDestContext & 0xffff);
    cb.pStubMsg = sm;
    cb.Signature = USER_MARSHAL_CB_SIGNATURE;
    cb.pFormat = f;
    cb.pTypeFormat = f + 8 + rds16(f + 8);
    switch (mode) {
    case M_SIZE:
        cb.CBType = USER_MARSHAL_CB_BUFFER_SIZE;
        if (flags & USER_MARSHAL_POINTER) { ndr_size_align(sm, 4); sm->BufferLength += 4; }
        ndr_size_align(sm, align);
        sm->BufferLength = q->pfnBufferSize(&cb.Flags, sm->BufferLength, mem);
        break;
    case M_MARSHAL: {
        cb.CBType = USER_MARSHAL_CB_MARSHALL;
        if (flags & USER_MARSHAL_POINTER) ndr_put32(sm, USER_MARSHAL_PTR_PREFIX);
        ndr_align(sm, align);
        if (NDR_FAILED(sm)) return;
        unsigned char *end = q->pfnMarshall(&cb.Flags, sm->Buffer, mem);
        if (!end || end > sm->BufferEnd) { ndr_fail(sm, RPC_X_BAD_STUB_DATA); return; }
        sm->Buffer = end;
        break;
    }
    case M_UNMARSHAL: {
        cb.CBType = USER_MARSHAL_CB_UNMARSHALL;
        if ((flags & USER_MARSHAL_POINTER) && ndr_get32(sm) != USER_MARSHAL_PTR_PREFIX) { ndr_fail(sm, RPC_X_BAD_STUB_DATA); return; }
        ndr_align(sm, align);
        if (NDR_FAILED(sm)) return;
        if (!*pmem || alloc) *pmem = ndr_alloc(sm, rd16(f + 4));
        if (!*pmem) return;
        unsigned char *end = q->pfnUnmarshall(&cb.Flags, sm->Buffer, *pmem);
        if (!end || end > sm->BufferEnd) { ndr_fail(sm, RPC_X_BAD_STUB_DATA); return; }
        sm->Buffer = end;
        break;
    }
    case M_FREE:
        cb.CBType = USER_MARSHAL_CB_FREE;
        if (mem) q->pfnFree(&cb.Flags, mem);
        break;
    }
}

/* ---- dispatch ---------------------------------------------------------- */
static void type_op(PMIDL_STUB_MESSAGE sm, int mode, unsigned char *mem, unsigned char **pmem, PFORMAT_STRING f, int alloc)
{
    if (NDR_FAILED(sm)) return;
    unsigned char fc = f[0];
    if (fc == FC_RANGE) fc = f[1] & 0x0f;
    if (ndr_base_size(fc)) {
        switch (mode) {
        case M_SIZE: ndr_base_size_type(sm, fc); break;
        case M_MARSHAL: ndr_base_marshal(sm, mem, fc); break;
        case M_UNMARSHAL:
            if (!*pmem || alloc) *pmem = ndr_alloc(sm, ndr_base_memsize(fc));
            if (*pmem) ndr_base_unmarshal(sm, *pmem, fc);
            break;
        }
        return;
    }
    switch (fc) {
    case FC_RP: case FC_UP: case FC_OP: case FC_FP:
        ptr_op(sm, mode, mem, pmem, f, 0, 0, alloc);
        break;
    case FC_IP:
        ip_op(sm, mode, mem, pmem, f);
        break;
    case FC_STRUCT: case FC_PSTRUCT: case FC_CSTRUCT: case FC_CPSTRUCT:
        block_struct_op(sm, mode, mem, pmem, f, alloc);
        break;
    case FC_BOGUS_STRUCT:
        bogus_struct_op(sm, mode, mem, pmem, f, alloc);
        break;
    case FC_SMFARRAY: case FC_LGFARRAY: case FC_CARRAY: case FC_CVARRAY: case FC_BOGUS_ARRAY:
        array_op(sm, mode, mem, pmem, f, alloc, 0);
        break;
    case FC_C_CSTRING: case FC_C_WSTRING: case FC_CSTRING: case FC_WSTRING:
        string_op(sm, mode, mem, pmem, f, alloc);
        break;
    case FC_USER_MARSHAL:
        user_op(sm, mode, mem, pmem, f, alloc);
        break;
    default:
        ndr_fail(sm, RPC_X_BAD_STUB_DATA);
    }
}

/* one top-level type, then everything its embedded pointers deferred */
static void top_op(PMIDL_STUB_MESSAGE sm, int mode, unsigned char *mem, unsigned char **pmem, PFORMAT_STRING f, int alloc)
{
    DQueue q = { 0 }, *old = QUEUE(sm);
    unsigned char *oldmem = sm->Memory;
    sm->pPointerQueueState = (struct NDR_POINTER_QUEUE_STATE *)&q;
    type_op(sm, mode, mem, pmem, f, alloc);
    while (q.head < q.n && !NDR_FAILED(sm)) {
        DItem it = q.items[q.head++];
        sm->Memory = it.base;
        if (mode == M_UNMARSHAL) pointee_op(sm, mode, NULL, (unsigned char **)it.mem, it.f, 1);
        else pointee_op(sm, mode, it.mem, NULL, it.f, 0);
    }
    HeapFree(GetProcessHeap(), 0, q.items);
    sm->pPointerQueueState = (struct NDR_POINTER_QUEUE_STATE *)old;
    sm->Memory = oldmem;
}

void ndr_size_type(PMIDL_STUB_MESSAGE sm, unsigned char *mem, PFORMAT_STRING f) { top_op(sm, M_SIZE, mem, NULL, f, 0); }
void ndr_marshal_type(PMIDL_STUB_MESSAGE sm, unsigned char *mem, PFORMAT_STRING f) { top_op(sm, M_MARSHAL, mem, NULL, f, 0); }
void ndr_unmarshal_type(PMIDL_STUB_MESSAGE sm, unsigned char **pmem, PFORMAT_STRING f, int alloc) { top_op(sm, M_UNMARSHAL, NULL, pmem, f, alloc); }
void ndr_free_type(PMIDL_STUB_MESSAGE sm, unsigned char *mem, PFORMAT_STRING f) { top_op(sm, M_FREE, mem, NULL, f, 0); }

unsigned long ndr_memsize(PMIDL_STUB_MESSAGE sm, PFORMAT_STRING f)
{
    unsigned char fc = f[0];
    if (fc == FC_RANGE) fc = f[1] & 0x0f;
    if (ndr_base_size(fc)) return ndr_base_memsize(fc);
    switch (fc) {
    case FC_RP: case FC_UP: case FC_OP: case FC_FP: case FC_IP: return sizeof(void *);
    case FC_STRUCT: case FC_PSTRUCT: case FC_CSTRUCT: case FC_CPSTRUCT: case FC_BOGUS_STRUCT:
    case FC_SMFARRAY: return rd16(f + 2);
    case FC_LGFARRAY: return rd32(f + 2);
    case FC_USER_MARSHAL: return rd16(f + 4);
    case FC_CSTRING: return rd16(f + 2);
    case FC_WSTRING: return 2u * rd16(f + 2);
    case FC_BOGUS_ARRAY: {
        PFORMAT_STRING cd = f + 4;
        if (rd32(cd) != 0xffffffffUL || !rd16(f + 2)) return 0;
        return rd16(f + 2) * elem_memsize(sm, bogus_elem(sm, f));
    }
    }
    return 0;
}

/* ---- the documented per-type entry points (for /Os stubs) -------------- */
#define NDR_ENTRY_POINTS(name) \
    RPCRTAPI unsigned char *RPC_ENTRY Ndr##name##Marshall(PMIDL_STUB_MESSAGE m, unsigned char *mem, PFORMAT_STRING f) \
    { ndr_marshal_type(m, mem, f); if (NDR_FAILED(m)) RpcRaiseException((RPC_STATUS)NDR_ERR(m)); return NULL; } \
    RPCRTAPI unsigned char *RPC_ENTRY Ndr##name##Unmarshall(PMIDL_STUB_MESSAGE m, unsigned char **mem, PFORMAT_STRING f, unsigned char alloc) \
    { ndr_unmarshal_type(m, mem, f, alloc); if (NDR_FAILED(m)) RpcRaiseException((RPC_STATUS)NDR_ERR(m)); return NULL; } \
    RPCRTAPI void RPC_ENTRY Ndr##name##BufferSize(PMIDL_STUB_MESSAGE m, unsigned char *mem, PFORMAT_STRING f) \
    { ndr_size_type(m, mem, f); } \
    RPCRTAPI unsigned long RPC_ENTRY Ndr##name##MemorySize(PMIDL_STUB_MESSAGE m, PFORMAT_STRING f) \
    { return ndr_memsize(m, f); } \
    RPCRTAPI void RPC_ENTRY Ndr##name##Free(PMIDL_STUB_MESSAGE m, unsigned char *mem, PFORMAT_STRING f) \
    { ndr_free_type(m, mem, f); }
NDR_ENTRY_POINTS(Pointer)
NDR_ENTRY_POINTS(SimpleStruct)
NDR_ENTRY_POINTS(ConformantStruct)
NDR_ENTRY_POINTS(ComplexStruct)
NDR_ENTRY_POINTS(FixedArray)
NDR_ENTRY_POINTS(ConformantArray)
NDR_ENTRY_POINTS(ConformantVaryingArray)
NDR_ENTRY_POINTS(ComplexArray)
NDR_ENTRY_POINTS(ConformantString)
NDR_ENTRY_POINTS(InterfacePointer)
NDR_ENTRY_POINTS(UserMarshal)

RPCRTAPI void RPC_ENTRY NdrSimpleTypeMarshall(PMIDL_STUB_MESSAGE m, unsigned char *mem, unsigned char fc) { ndr_base_marshal(m, mem, fc); }
RPCRTAPI void RPC_ENTRY NdrSimpleTypeUnmarshall(PMIDL_STUB_MESSAGE m, unsigned char *mem, unsigned char fc) { ndr_base_unmarshal(m, mem, fc); }
/* NovaOS only speaks little-endian NDR with IEEE floats: nothing to convert */
RPCRTAPI void RPC_ENTRY NdrConvert(PMIDL_STUB_MESSAGE m, PFORMAT_STRING f) { (void)m; (void)f; }
RPCRTAPI void RPC_ENTRY NdrConvert2(PMIDL_STUB_MESSAGE m, PFORMAT_STRING f, long n) { (void)m; (void)f; (void)n; }
