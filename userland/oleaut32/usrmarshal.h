/* usrmarshal.h — the wire form of BSTRs, VARIANTs, SAFEARRAYs and
 * interface pointers (usrmarshal.c), for psdispatch.c */
#pragma once
#include <oleauto.h>

/* sizing when buf is NULL (size counts up), else writing at buf; reading
 * from buf, never past end (when set) */
typedef struct {
    BYTE *buf, *start, *end;
    ULONG size;
    ULONG ctx;                  /* the destination context (MSHCTX_*) */
    int fail;
    HRESULT hr;                 /* why an interface pointer did not unmarshal */
} WireBuf;

void wire_bstr(WireBuf *w, BSTR b);
BSTR wire_get_bstr(WireBuf *r);
void wire_iface(WireBuf *w, IUnknown *p, REFIID iid);
IUnknown *wire_get_iface(WireBuf *r, REFIID iid);
void wire_safearray(WireBuf *w, SAFEARRAY *a);
SAFEARRAY *wire_get_safearray(WireBuf *r);
void wire_variant(WireBuf *w, const VARIANT *v);
void wire_get_variant(WireBuf *r, VARIANT *v);
void wire_free_variant(VARIANT *v);

/* fields, aligned from the value's start */
void wb_align(WireBuf *w, unsigned n);
void wb_put(WireBuf *w, const void *p, ULONG n);
void wb_u32(WireBuf *w, ULONG v);
void rb_align(WireBuf *r, unsigned n);
int rb_get(WireBuf *r, void *p, ULONG n);
ULONG rb_u32(WireBuf *r);
