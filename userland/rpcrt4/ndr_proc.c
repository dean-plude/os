/*
 * ndr_proc.c — whole calls from Oicf procedure format strings.
 *
 * Client: a stubless proxy's vtable slot is one of ndr_stubless_thunks,
 * which hands the caller's arguments (and, on x64, the float registers)
 * to client_call: it sizes, marshals the [in] parameters, sends the call
 * through the proxy's IRpcChannelBuffer and unmarshals the [out] ones and
 * the return value.
 *
 * Server: NdrStubCall2 unmarshals the [in] parameters into an argument
 * block shaped like the method's stack, calls the method through the
 * server object's vtable (ndr_invoke builds the real call), marshals the
 * [out] parameters and the return value and frees what it allocated.
 */
#include "ndr.h"
#include "ndr_ole.h"

typedef struct {
    unsigned procnum, stack_size, nparams, client_buf, server_buf;
    unsigned char oi, opt, flags2;
    unsigned short fpmask;
    PFORMAT_STRING params;
} Proc;

/* @indirect: widl describes a top-level [string] pointer parameter
 * (WCHAR **) by its pointee's type (FC_OP or FC_UP to the string, with a
 * server allocation size) instead of MIDL's ref pointer to it, so the
 * stack slot holds where that pointer lives */
typedef struct { unsigned short attr, offset; PFORMAT_STRING type; int indirect; } Param;

static int parse_proc(PFORMAT_STRING p, Proc *pr)
{
    memset(pr, 0, sizeof *pr);
    unsigned char handle = p[0];
    pr->oi = p[1];
    p += 2;
    if (pr->oi & Oi_HAS_RPCFLAGS) p += 4;
    pr->procnum = rd16(p);
    pr->stack_size = rd16(p + 2);
    p += 4;
    if (!handle) {                                 /* an explicit handle's description */
        if (p[0] == FC_BIND_PRIMITIVE) p += 4;
        else if (p[0] == FC_BIND_GENERIC || p[0] == FC_BIND_CONTEXT) p += 6;
        else return 0;
    }
    pr->client_buf = rd16(p);
    pr->server_buf = rd16(p + 2);
    pr->opt = p[4];
    pr->nparams = p[5];
    p += 6;
    if (pr->opt & 0x40) {                           /* Oi2 extensions */
        unsigned size = p[0];
        pr->flags2 = p[1];
#ifdef _WIN64
        if (size >= 10) pr->fpmask = rd16(p + 8);
#endif
        p += size;
    }
    pr->params = p;
    return 1;
}

static void get_param(const Proc *pr, const MIDL_STUB_DESC *desc, unsigned i, Param *out)
{
    PFORMAT_STRING q = pr->params + 6 * i;
    out->attr = rd16(q);
    out->offset = rd16(q + 2);
    out->type = out->attr & PA_BASETYPE ? q + 4 : desc->pFormatTypes + rd16(q + 4);
    out->indirect = !(out->attr & (PA_BASETYPE | PA_SIMPLEREF | PA_BYVALUE)) && (out->attr >> 13)
                    && (out->type[0] == FC_UP || out->type[0] == FC_OP || out->type[0] == FC_FP);
}

/* a parameter's value as the Ndr*Marshall routines take it */
static unsigned char *param_mem(PMIDL_STUB_MESSAGE sm, const Param *p, unsigned char *slot)
{
    if (p->attr & PA_BYVALUE) {
#ifdef _WIN64
        unsigned long n = ndr_memsize(sm, p->type);
        if (n != 1 && n != 2 && n != 4 && n != 8) return *(unsigned char **)slot;   /* passed by reference */
#endif
        (void)sm;
        return slot;
    }
    if (p->indirect) {
        unsigned char **at = *(unsigned char ***)slot;
        return at ? *at : NULL;
    }
    return *(unsigned char **)slot;
}
/* where an [out] parameter's value is unmarshaled to */
static unsigned char **param_loc(const Param *p, unsigned char *slot)
{
    return p->indirect ? *(unsigned char ***)slot : (unsigned char **)slot;
}

static HRESULT status_hr(LONG_PTR e)
{
    long v = (long)e;
    return v < 0 ? (HRESULT)v : v ? HRESULT_FROM_WIN32(v) : S_OK;
}

static void init_sm(PMIDL_STUB_MESSAGE sm, PRPC_MESSAGE msg, const MIDL_STUB_DESC *desc, int client)
{
    memset(sm, 0, sizeof *sm);
    sm->RpcMsg = msg;
    sm->StubDesc = desc;
    sm->IsClient = (unsigned char)client;
    sm->pfnAllocate = desc->pfnAllocate;
    sm->pfnFree = desc->pfnFree;
    msg->RpcInterfaceInformation = desc->RpcInterfaceInformation;
    msg->DataRepresentation = NDR_LOCAL_DATA_REPRESENTATION;
}

/* zero what an [out]-only parameter points to, so a failed call leaves
 * NULLs behind and unique pointers inside it start empty */
static void clear_out(PMIDL_STUB_MESSAGE sm, const Param *p, unsigned char *slot)
{
    unsigned char *target = *(unsigned char **)slot;
    if (!target) return;
    unsigned long n;
    if (p->attr & PA_BASETYPE) n = ndr_base_memsize(p->type[0]);
    else if (p->indirect) n = sizeof(void *);
    else if (p->attr & PA_SIMPLEREF) n = ndr_memsize(sm, p->type);
    else if (ndr_is_pointer(p->type[0])) {
        PFORMAT_STRING f = p->type;
        if (f[1] & FC_POINTER_DEREF) n = sizeof(void *);
        else if (f[1] & FC_SIMPLE_POINTER) n = ndr_base_memsize(f[2]);
        else n = ndr_memsize(sm, f + 2 + rds16(f + 2));
    } else n = 0;
    if (n) memset(target, 0, n);
}

/* the client side of one call: @args is the caller's argument block */
static LONG_PTR client_call(const MIDL_STUBLESS_PROXY_INFO *info, unsigned method, unsigned char *args, unsigned *stack_size)
{
    const MIDL_STUB_DESC *desc = info->pStubDesc;
    PFORMAT_STRING fmt = info->ProcFormatString + info->FormatStringOffset[method];
    Proc pr;
    if (!parse_proc(fmt, &pr)) return RPC_X_BAD_STUB_DATA;
    if (stack_size) {                              /* what the callee pops: the return value's slot is counted but not passed */
        Param r;
        *stack_size = pr.stack_size;
        for (unsigned i = 0; i < pr.nparams; i++) {
            get_param(&pr, desc, i, &r);
            if (r.attr & PA_RETURN) *stack_size = r.offset;
        }
    }
    StdProxy *px = PROXY_FROM_IFACE(*(void **)args);
    IRpcChannelBuffer *chan = px->chan;
    if (!chan) return CO_E_OBJNOTCONNECTED;
    chan->lpVtbl->AddRef(chan);

    RPC_MESSAGE msg;
    MIDL_STUB_MESSAGE sm;
    memset(&msg, 0, sizeof msg);
    init_sm(&sm, &msg, desc, 1);
    sm.StackTop = args;
    sm.fHasNewCorrDesc = (pr.flags2 & 1) != 0;
    sm.pRpcChannelBuffer = chan;
    chan->lpVtbl->GetDestCtx(chan, &sm.dwDestContext, &sm.pvDestContext);

    LONG_PTR ret = 0;
    HRESULT hr = S_OK;
    Param p;
    for (unsigned i = 0; i < pr.nparams; i++) {
        get_param(&pr, desc, i, &p);
        if ((p.attr & (PA_OUT | PA_IN | PA_RETURN)) == PA_OUT) clear_out(&sm, &p, args + p.offset);
    }
    /* size */
    sm.BufferLength = 0;
    for (unsigned i = 0; i < pr.nparams; i++) {
        get_param(&pr, desc, i, &p);
        if (!(p.attr & PA_IN)) continue;
        unsigned char *slot = args + p.offset;
        if (p.attr & PA_BASETYPE) ndr_base_size_type(&sm, p.type[0]);
        else ndr_size_type(&sm, param_mem(&sm, &p, slot), p.type);
        sm.BufferLength += 8;
    }
    if (NDR_FAILED(&sm)) { hr = status_hr(NDR_ERR(&sm)); goto done; }
    msg.BufferLength = sm.BufferLength + 16;
    msg.ProcNum = pr.procnum;
    hr = chan->lpVtbl->GetBuffer(chan, (RPCOLEMESSAGE *)&msg, px->iid);
    if (FAILED(hr)) goto done;
    sm.Buffer = sm.BufferStart = msg.Buffer;
    sm.BufferEnd = sm.Buffer + msg.BufferLength;
    /* marshal */
    for (unsigned i = 0; i < pr.nparams && !NDR_FAILED(&sm); i++) {
        get_param(&pr, desc, i, &p);
        if (!(p.attr & PA_IN)) continue;
        unsigned char *slot = args + p.offset;
        if (p.attr & PA_BASETYPE) ndr_base_marshal(&sm, p.attr & PA_SIMPLEREF ? *(unsigned char **)slot : slot, p.type[0]);
        else ndr_marshal_type(&sm, param_mem(&sm, &p, slot), p.type);
    }
    if (NDR_FAILED(&sm)) {
        hr = status_hr(NDR_ERR(&sm));
        chan->lpVtbl->FreeBuffer(chan, (RPCOLEMESSAGE *)&msg);
        goto done;
    }
    msg.BufferLength = (unsigned)(sm.Buffer - sm.BufferStart);
    ULONG status = 0;
    hr = chan->lpVtbl->SendReceive(chan, (RPCOLEMESSAGE *)&msg, &status);
    if (FAILED(hr)) {
        chan->lpVtbl->FreeBuffer(chan, (RPCOLEMESSAGE *)&msg);
        goto done;
    }
    /* unmarshal */
    sm.Buffer = sm.BufferStart = msg.Buffer;
    sm.BufferEnd = sm.Buffer + msg.BufferLength;
    sm.UniquePtrCount = 0;
    for (unsigned i = 0; i < pr.nparams && !NDR_FAILED(&sm); i++) {
        get_param(&pr, desc, i, &p);
        unsigned char *slot = args + p.offset;
        if (p.attr & PA_RETURN) {
            if (p.attr & PA_BASETYPE) ndr_base_unmarshal(&sm, (unsigned char *)&ret, p.type[0]);
        } else if (p.attr & PA_OUT) {
            if (p.attr & PA_BASETYPE) {
                if (*(unsigned char **)slot) ndr_base_unmarshal(&sm, *(unsigned char **)slot, p.type[0]);
                else ndr_fail(&sm, RPC_X_NULL_REF_POINTER);
            } else if (param_loc(&p, slot))
                ndr_unmarshal_type(&sm, param_loc(&p, slot), p.type, 0);
            else ndr_fail(&sm, RPC_X_NULL_REF_POINTER);
        }
    }
    if (NDR_FAILED(&sm)) hr = status_hr(NDR_ERR(&sm));
    chan->lpVtbl->FreeBuffer(chan, (RPCOLEMESSAGE *)&msg);
done:
    chan->lpVtbl->Release(chan);
    if (FAILED(hr)) {
        for (unsigned i = 0; i < pr.nparams; i++) {               /* failed: [out] parameters come back empty */
            get_param(&pr, desc, i, &p);
            if ((p.attr & (PA_IN | PA_OUT | PA_RETURN)) == PA_OUT) clear_out(&sm, &p, args + p.offset);
        }
        return hr;
    }
    return ret;
}

/* ---- stubless proxy entry points (called by the thunks below) ---------- */
#ifdef _WIN64
__attribute__((used)) LONG_PTR ndr_stubless_x64(ULONG_PTR *args, const double *xmm, unsigned method)
{
    StdProxy *px = PROXY_FROM_IFACE((void *)args[0]);
    const MIDL_STUBLESS_PROXY_INFO *info = px->info;
    Proc pr;
    if (parse_proc(info->ProcFormatString + info->FormatStringOffset[method], &pr))
        for (unsigned i = 1; i < 4; i++) {          /* floats in xmm1-3 never reached the integer registers */
            unsigned k = (pr.fpmask >> (2 * i)) & 3;
            if (k == 1) memcpy(&args[i], &xmm[i - 1], 4);
            else if (k == 2) memcpy(&args[i], &xmm[i - 1], 8);
        }
    return client_call(info, method, (unsigned char *)args, NULL);
}

/* ndr_stubless_thunks + 16 * n: method n.  ndr_forward_thunks + 16 * n:
 * method n of the delegated base interface's proxy */
__asm__(
    ".text\n"
    ".p2align 4\n"
    "ndr_stubless_common:\n"
    "  movq %rcx, 8(%rsp)\n"
    "  movq %rdx, 16(%rsp)\n"
    "  movq %r8, 24(%rsp)\n"
    "  movq %r9, 32(%rsp)\n"
    "  subq $0x48, %rsp\n"
    "  movsd %xmm1, 0x20(%rsp)\n"
    "  movsd %xmm2, 0x28(%rsp)\n"
    "  movsd %xmm3, 0x30(%rsp)\n"
    "  leaq 0x50(%rsp), %rcx\n"
    "  leaq 0x20(%rsp), %rdx\n"
    "  movl %r10d, %r8d\n"
    "  callq ndr_stubless_x64\n"
    "  addq $0x48, %rsp\n"
    "  retq\n"
    ".p2align 4\n"
    "ndr_forward_common:\n"
    "  movq " NDR_STR(PROXY_BASE_OFFSET) "(%rcx), %rcx\n"
    "  movq (%rcx), %rax\n"
    "  jmpq *(%rax,%r10,8)\n"
    ".p2align 4\n"
    ".globl ndr_stubless_thunks\n"
    "ndr_stubless_thunks:\n"
    ".set ndr_i, 0\n"
    ".rept " NDR_STR(NDR_MAX_METHODS) "\n"
    "  .p2align 4\n"
    "  movl $ndr_i, %r10d\n"
    "  jmp ndr_stubless_common\n"
    "  .set ndr_i, ndr_i + 1\n"
    ".endr\n"
    ".p2align 4\n"
    ".globl ndr_forward_thunks\n"
    "ndr_forward_thunks:\n"
    ".set ndr_i, 0\n"
    ".rept " NDR_STR(NDR_MAX_FORWARD) "\n"
    "  .p2align 4\n"
    "  movl $ndr_i, %r10d\n"
    "  jmp ndr_forward_common\n"
    "  .set ndr_i, ndr_i + 1\n"
    ".endr\n"
    /* ndr_invoke(fn, args, bytes): the call with @args as its stack and
     * the first four in rcx/rdx/r8/r9 and xmm0-3.  It has unwind data so
     * an exception in the server method reaches call_server's handler */
    ".p2align 4\n"
    ".globl ndr_invoke\n"
    ".def ndr_invoke; .scl 2; .type 32; .endef\n"
    "ndr_invoke:\n"
    ".seh_proc ndr_invoke\n"
    "  pushq %rbp\n"
    ".seh_pushreg %rbp\n"
    "  pushq %rsi\n"
    ".seh_pushreg %rsi\n"
    "  pushq %rdi\n"
    ".seh_pushreg %rdi\n"
    "  movq %rsp, %rbp\n"
    ".seh_setframe %rbp, 0\n"
    ".seh_endprologue\n"
    "  movq %rcx, %rax\n"
    "  movq %rdx, %rsi\n"
    "  movq %r8, %rcx\n"
    "  addq $15, %rcx\n"
    "  andq $-16, %rcx\n"
    "  cmpq $32, %rcx\n"
    "  jae 1f\n"
    "  movq $32, %rcx\n"
    "1:\n"
    "  subq %rcx, %rsp\n"
    "  andq $-16, %rsp\n"
    "  movq %rsp, %rdi\n"
    "  shrq $3, %rcx\n"
    "  rep movsq\n"
    "  movq %rax, %r10\n"
    "  movq (%rsp), %rcx\n"
    "  movq 8(%rsp), %rdx\n"
    "  movq 16(%rsp), %r8\n"
    "  movq 24(%rsp), %r9\n"
    "  movsd (%rsp), %xmm0\n"
    "  movsd 8(%rsp), %xmm1\n"
    "  movsd 16(%rsp), %xmm2\n"
    "  movsd 24(%rsp), %xmm3\n"
    "  callq *%r10\n"
    "  leaq 0(%rbp), %rsp\n"
    "  popq %rdi\n"
    "  popq %rsi\n"
    "  popq %rbp\n"
    "  retq\n"
    ".seh_endproc\n");
#else
__attribute__((used)) LONG_PTR __cdecl ndr_stubless_x86(void **args, unsigned method, unsigned *stack_size)
{
    StdProxy *px = PROXY_FROM_IFACE(args[0]);
    return client_call(px->info, method, (unsigned char *)args, stack_size);
}

__asm__(
    ".text\n"
    ".p2align 4\n"
    "_ndr_stubless_common:\n"
    "  pushl %ebp\n"
    "  movl %esp, %ebp\n"
    "  subl $4, %esp\n"
    "  movl $0, -4(%ebp)\n"
    "  leal -4(%ebp), %ecx\n"
    "  pushl %ecx\n"
    "  pushl %eax\n"
    "  leal 8(%ebp), %ecx\n"
    "  pushl %ecx\n"
    "  calll _ndr_stubless_x86\n"
    "  addl $12, %esp\n"
    "  movl -4(%ebp), %ecx\n"
    "  movl %ebp, %esp\n"
    "  popl %ebp\n"
    "  popl %edx\n"
    "  addl %ecx, %esp\n"
    "  jmpl *%edx\n"
    ".p2align 4\n"
    "_ndr_forward_common:\n"
    "  movl 4(%esp), %ecx\n"
    "  movl " NDR_STR(PROXY_BASE_OFFSET) "(%ecx), %ecx\n"
    "  movl %ecx, 4(%esp)\n"
    "  movl (%ecx), %ecx\n"
    "  jmpl *(%ecx,%eax,4)\n"
    ".p2align 4\n"
    ".globl _ndr_stubless_thunks\n"
    "_ndr_stubless_thunks:\n"
    ".set ndr_i, 0\n"
    ".rept " NDR_STR(NDR_MAX_METHODS) "\n"
    "  .p2align 4\n"
    "  movl $ndr_i, %eax\n"
    "  jmp _ndr_stubless_common\n"
    "  .set ndr_i, ndr_i + 1\n"
    ".endr\n"
    ".p2align 4\n"
    ".globl _ndr_forward_thunks\n"
    "_ndr_forward_thunks:\n"
    ".set ndr_i, 0\n"
    ".rept " NDR_STR(NDR_MAX_FORWARD) "\n"
    "  .p2align 4\n"
    "  movl $ndr_i, %eax\n"
    "  jmp _ndr_forward_common\n"
    "  .set ndr_i, ndr_i + 1\n"
    ".endr\n"
    ".p2align 4\n"
    ".globl _ndr_invoke\n"
    "_ndr_invoke:\n"
    "  pushl %ebp\n"
    "  movl %esp, %ebp\n"
    "  pushl %esi\n"
    "  pushl %edi\n"
    "  movl 16(%ebp), %ecx\n"
    "  movl 12(%ebp), %esi\n"
    "  addl $3, %ecx\n"
    "  andl $-4, %ecx\n"
    "  subl %ecx, %esp\n"
    "  andl $-16, %esp\n"
    "  movl %esp, %edi\n"
    "  shrl $2, %ecx\n"
    "  rep movsl\n"
    "  calll *8(%ebp)\n"
    "  leal -8(%ebp), %esp\n"
    "  popl %edi\n"
    "  popl %esi\n"
    "  popl %ebp\n"
    "  retl\n");
#endif

/* NdrClientCall2 for an object method called from a (non-stubless) proxy:
 * the argument after the format string is the address of the arguments */
RPCRTAPI CLIENT_CALL_RETURN RPC_VAR_ENTRY NdrClientCall2(PMIDL_STUB_DESC desc, PFORMAT_STRING fmt, ...)
{
    CLIENT_CALL_RETURN r;
    va_list ap;
    va_start(ap, fmt);
    unsigned char *args = va_arg(ap, unsigned char *);
    va_end(ap);
    Proc pr;
    r.Simple = RPC_X_BAD_STUB_DATA;
    if (!parse_proc(fmt, &pr) || !(pr.oi & Oi_OBJECT_PROC)) return r;     /* (no RPC without COM here) */
    MIDL_STUBLESS_PROXY_INFO info = { desc, fmt, NULL, NULL, 0, NULL };
    static const unsigned short zero = 0;
    info.FormatStringOffset = &zero - 0;
    r.Simple = client_call(&info, 0, args, NULL);
    return r;
}

/* ---- the server side ---------------------------------------------------- */
extern LONGLONG __cdecl ndr_invoke(void *fn, void *args, ULONG_PTR bytes);

static LONGLONG call_server(void *fn, unsigned char *args, unsigned bytes, long *fault)
{
    LONGLONG r = 0;
    __try {
        r = ndr_invoke(fn, args, bytes);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *fault = RPC_E_SERVERFAULT;
    }
    return r;
}

/* where an [out]-only parameter's data goes while the method runs */
static void alloc_out(PMIDL_STUB_MESSAGE sm, const Param *p, unsigned char *slot)
{
    unsigned long n = 0;
    PFORMAT_STRING t = p->type;
    if (p->attr & PA_BASETYPE) n = ndr_base_memsize(t[0]);
    else if (p->indirect) n = sizeof(void *);
    else if (!(p->attr & PA_SIMPLEREF) && ndr_is_pointer(t[0])) {
        if (t[1] & FC_POINTER_DEREF) n = sizeof(void *);
        else if (t[1] & FC_SIMPLE_POINTER) n = ndr_base_memsize(t[2]);
        else t = t + 2 + rds16(t + 2);
    }
    if (!n) n = ndr_memsize(sm, t);
    if (!n) {                                      /* sized by another parameter */
        ULONG_PTR count = 0;
        unsigned long esize = 0;
        switch (t[0]) {
        case FC_CARRAY: case FC_CVARRAY: esize = rd16(t + 2); ndr_conformance(sm, NULL, t + 4, &count); break;
        case FC_C_WSTRING: case FC_C_CSTRING:
            esize = t[0] == FC_C_WSTRING ? 2 : 1;
            if (t[1] == FC_STRING_SIZED) ndr_conformance(sm, NULL, t + 2, &count);
            break;
        case FC_BOGUS_ARRAY:
            esize = 0;
            break;
        }
        n = (unsigned long)(count * esize);
        if (!n) n = sizeof(void *);
    }
    *(unsigned char **)slot = ndr_alloc(sm, n);
}

static long stub_call(IRpcStubBuffer *This, IRpcChannelBuffer *chan, PRPC_MESSAGE msg)
{
    CStdStubBuffer *sb = (CStdStubBuffer *)This;
    const CInterfaceStubHeader *hdr = STUB_HEADER(This);
    const MIDL_SERVER_INFO *si = hdr->pServerInfo;
    const MIDL_STUB_DESC *desc = si->pStubDesc;
    unsigned method = msg->ProcNum;
    if (method >= hdr->DispatchTableCount) return RPC_S_PROCNUM_OUT_OF_RANGE;
    if (!sb->pvServerObject) return CO_E_OBJNOTCONNECTED;
    Proc pr;
    if (!parse_proc(si->ProcString + si->FmtStringOffset[method], &pr)) return RPC_X_BAD_STUB_DATA;
    unsigned char *args = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, pr.stack_size + 64);
    if (!args) return E_OUTOFMEMORY;
    *(void **)args = sb->pvServerObject;

    MIDL_STUB_MESSAGE sm;
    init_sm(&sm, msg, desc, 0);
    sm.StackTop = args;
    sm.fHasNewCorrDesc = (pr.flags2 & 1) != 0;
    sm.pRpcChannelBuffer = chan;
    chan->lpVtbl->GetDestCtx(chan, &sm.dwDestContext, &sm.pvDestContext);
    sm.Buffer = sm.BufferStart = msg->Buffer;
    sm.BufferEnd = sm.Buffer + msg->BufferLength;

    Param p;
    long fault = 0;
    /* unmarshal */
    for (unsigned i = 0; i < pr.nparams && !NDR_FAILED(&sm); i++) {
        get_param(&pr, desc, i, &p);
        unsigned char *slot = args + p.offset;
        if (p.attr & PA_RETURN) continue;
        if (p.attr & PA_IN) {
            if (p.attr & PA_BASETYPE) {
                if (p.attr & PA_SIMPLEREF) {
                    *(unsigned char **)slot = ndr_alloc(&sm, ndr_base_memsize(p.type[0]));
                    if (*(unsigned char **)slot) ndr_base_unmarshal(&sm, *(unsigned char **)slot, p.type[0]);
                } else
                    ndr_base_unmarshal(&sm, slot, p.type[0]);
            } else if (p.attr & PA_BYVALUE) {
                unsigned char *at = slot;
#ifdef _WIN64
                unsigned long n = ndr_memsize(&sm, p.type);
                if (n != 1 && n != 2 && n != 4 && n != 8) at = *(unsigned char **)slot = ndr_alloc(&sm, n);
#endif
                if (at) ndr_unmarshal_type(&sm, &at, p.type, 0);
            } else if (p.indirect) {
                unsigned char **at = ndr_alloc(&sm, sizeof(void *));
                *(unsigned char ***)slot = at;
                if (at) ndr_unmarshal_type(&sm, at, p.type, 1);
            } else
                ndr_unmarshal_type(&sm, (unsigned char **)slot, p.type, 1);
        } else if (p.attr & PA_OUT)
            alloc_out(&sm, &p, slot);
    }
    if (!NDR_FAILED(&sm)) {
        void *fn = (*(void ***)sb->pvServerObject)[method];
        LONGLONG r = call_server(fn, args, pr.stack_size, &fault);
        for (unsigned i = 0; i < pr.nparams; i++) {
            get_param(&pr, desc, i, &p);
            if (p.attr & PA_RETURN) memcpy(args + p.offset, &r, p.attr & PA_BASETYPE ? ndr_base_memsize(p.type[0]) : sizeof(void *));
        }
    }
    if (!NDR_FAILED(&sm) && !fault) {
        /* size and marshal the results */
        sm.BufferLength = 0;
        for (unsigned i = 0; i < pr.nparams; i++) {
            get_param(&pr, desc, i, &p);
            if (!(p.attr & (PA_OUT | PA_RETURN))) continue;
            unsigned char *slot = args + p.offset;
            if (p.attr & PA_BASETYPE) ndr_base_size_type(&sm, p.type[0]);
            else ndr_size_type(&sm, param_mem(&sm, &p, slot), p.type);
            sm.BufferLength += 8;
        }
        msg->BufferLength = sm.BufferLength + 16;
        HRESULT hr = NDR_FAILED(&sm) ? E_FAIL : chan->lpVtbl->GetBuffer(chan, (RPCOLEMESSAGE *)msg, hdr->piid);
        if (FAILED(hr)) fault = hr;
        else {
            sm.Buffer = sm.BufferStart = msg->Buffer;
            sm.BufferEnd = sm.Buffer + msg->BufferLength;
            sm.UniquePtrCount = 0;
            for (unsigned i = 0; i < pr.nparams && !NDR_FAILED(&sm); i++) {
                get_param(&pr, desc, i, &p);
                if (!(p.attr & (PA_OUT | PA_RETURN))) continue;
                unsigned char *slot = args + p.offset;
                if (p.attr & PA_BASETYPE)
                    ndr_base_marshal(&sm, (p.attr & PA_SIMPLEREF) && !(p.attr & PA_RETURN) ? *(unsigned char **)slot : slot, p.type[0]);
                else ndr_marshal_type(&sm, param_mem(&sm, &p, slot), p.type);
            }
            msg->BufferLength = (unsigned)(sm.Buffer - sm.BufferStart);
        }
    }
    long err = NDR_ERR(&sm);
    /* free what the call allocated (and release the interfaces it handled) */
    NDR_ERR(&sm) = 0;
    for (unsigned i = 0; i < pr.nparams; i++) {
        get_param(&pr, desc, i, &p);
        unsigned char *slot = args + p.offset;
        if (p.attr & PA_RETURN) continue;
        if (p.attr & PA_BASETYPE) {
            if (p.attr & PA_SIMPLEREF) ndr_free(&sm, *(unsigned char **)slot);
        } else if (p.attr & PA_BYVALUE) {
            unsigned char *m = param_mem(&sm, &p, slot);
            ndr_free_type(&sm, m, p.type);
            if (m != slot) ndr_free(&sm, m);
        } else if (p.indirect) {
            unsigned char **at = *(unsigned char ***)slot;
            if (at) ndr_free_type(&sm, *at, p.type);
            ndr_free(&sm, at);
        } else {
            unsigned char *m = *(unsigned char **)slot;
            ndr_free_type(&sm, m, p.type);
            if (p.attr & PA_SIMPLEREF) ndr_free(&sm, m);
        }
    }
    HeapFree(GetProcessHeap(), 0, args);
    if (err) return status_hr(err);
    return fault;
}

/* the status of the last NdrStubCall2 / forwarded call made through a
 * stub's dispatch table (which returns nothing): CStdStubBuffer_Invoke
 * reads it */
__declspec(thread) long ndr_dispatch_status;

RPCRTAPI long RPC_ENTRY NdrStubCall2(struct IRpcStubBuffer *This, struct IRpcChannelBuffer *chan, PRPC_MESSAGE msg,
                                     unsigned long *phase)
{
    if (phase) *phase = 0;
    long r = stub_call(This, chan, msg);
    ndr_dispatch_status = r;
    return r;
}

HRESULT ndr_invoke_stub(IRpcStubBuffer *This, RPCOLEMESSAGE *msg, IRpcChannelBuffer *chan)
{
    const CInterfaceStubHeader *hdr = STUB_HEADER(This);
    if (msg->iMethod >= hdr->DispatchTableCount) return (HRESULT)0x80010104L;   /* RPC_E_INVALIDMETHOD */
    PRPC_STUB_FUNCTION fn = hdr->pDispatchTable ? hdr->pDispatchTable[msg->iMethod] : NULL;
    if (!fn) return status_hr(stub_call(This, chan, (PRPC_MESSAGE)msg));
    DWORD phase = 0;
    ndr_dispatch_status = 0;
    __try {
        fn(This, chan, (PRPC_MESSAGE)msg, &phase);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ndr_dispatch_status = (long)GetExceptionCode();
    }
    return status_hr(ndr_dispatch_status);
}

/* a call to one of the delegated base interface's methods: the base
 * interface's own stub runs it */
RPCRTAPI void __RPC_STUB NdrStubForwardingFunction(IRpcStubBuffer *This, IRpcChannelBuffer *chan, PRPC_MESSAGE msg, DWORD *phase)
{
    (void)phase;
    IRpcStubBuffer *base = DELEGATED_BASE_STUB(This);
    ndr_dispatch_status = base ? base->lpVtbl->Invoke(base, (RPCOLEMESSAGE *)msg, chan) : CO_E_OBJNOTCONNECTED;
}
