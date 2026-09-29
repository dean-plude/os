/*
 * dpa.c — dynamic arrays (DSA of fixed-size items, DPA of pointers)
 */
#include "cc.h"

/* ---- DSA: a growable array of fixed-size items ---- */
typedef struct { int count, cap, size, grow; BYTE *items; } DSA;

CC DSA *WINAPI DSA_Create(int size, int grow)
{
    DSA *d = LocalAlloc(LMEM_ZEROINIT, sizeof(*d));
    if (d) { d->size = size; d->grow = grow > 0 ? grow : 8; }
    return d;
}

CC int WINAPI DSA_InsertItem(DSA *d, int i, const void *item)
{
    if (!d) return -1;
    if (i < 0 || i > d->count) i = d->count;
    if (d->count == d->cap) {
        int cap = d->cap + d->grow;
        BYTE *n = d->items ? HeapReAlloc(GetProcessHeap(), 0, d->items, (SIZE_T)cap * d->size) : HeapAlloc(GetProcessHeap(), 0, (SIZE_T)cap * d->size);
        if (!n) return -1;
        d->items = n; d->cap = cap;
    }
    BYTE *at = d->items + (SIZE_T)i * d->size;
    for (int k = (d->count - i) * d->size - 1; k >= 0; k--) at[d->size + k] = at[k];
    for (int k = 0; k < d->size; k++) at[k] = ((const BYTE *)item)[k];
    d->count++;
    return i;
}

CC void *WINAPI DSA_GetItemPtr(DSA *d, int i) { return d && i >= 0 && i < d->count ? d->items + (SIZE_T)i * d->size : 0; }
CC BOOL WINAPI DSA_GetItem(DSA *d, int i, void *out)
{
    BYTE *p = DSA_GetItemPtr(d, i);
    if (!p) return FALSE;
    for (int k = 0; k < d->size; k++) ((BYTE *)out)[k] = p[k];
    return TRUE;
}
CC BOOL WINAPI DSA_SetItem(DSA *d, int i, const void *item)
{
    if (!d || i < 0) return FALSE;
    while (i >= d->count) if (DSA_InsertItem(d, d->count, item) < 0) return FALSE;
    BYTE *p = d->items + (SIZE_T)i * d->size;
    for (int k = 0; k < d->size; k++) p[k] = ((const BYTE *)item)[k];
    return TRUE;
}
CC BOOL WINAPI DSA_DeleteItem(DSA *d, int i)
{
    if (!d || i < 0 || i >= d->count) return FALSE;
    BYTE *at = d->items + (SIZE_T)i * d->size;
    for (int k = 0; k < (d->count - i - 1) * d->size; k++) at[k] = at[d->size + k];
    d->count--;
    return TRUE;
}
CC BOOL WINAPI DSA_DeleteAllItems(DSA *d) { if (!d) return FALSE; d->count = 0; return TRUE; }
CC BOOL WINAPI DSA_Destroy(DSA *d) { if (!d) return TRUE; if (d->items) HeapFree(GetProcessHeap(), 0, d->items); LocalFree(d); return TRUE; }

/* ---- DPA: a growable array of pointers ---- */
CC DSA *WINAPI DPA_Create(int grow) { return DSA_Create(sizeof(void *), grow); }
CC int WINAPI DPA_InsertPtr(DSA *d, int i, void *p) { return DSA_InsertItem(d, i, &p); }
CC void *WINAPI DPA_GetPtr(DSA *d, INT_PTR i) { void **p = DSA_GetItemPtr(d, (int)i); return p ? *p : 0; }
CC BOOL WINAPI DPA_SetPtr(DSA *d, int i, void *p) { return DSA_SetItem(d, i, &p); }
CC void *WINAPI DPA_DeletePtr(DSA *d, int i) { void *p = DPA_GetPtr(d, i); return DSA_DeleteItem(d, i) ? p : 0; }
CC BOOL WINAPI DPA_DeleteAllPtrs(DSA *d) { return DSA_DeleteAllItems(d); }
CC BOOL WINAPI DPA_Destroy(DSA *d) { return DSA_Destroy(d); }
CC int WINAPI DPA_GetPtrIndex(DSA *d, const void *p)
{
    for (int i = 0; d && i < d->count; i++) if (((void **)d->items)[i] == p) return i;
    return -1;
}
typedef int (CALLBACK *PFNDACOMPARE)(void *a, void *b, LPARAM l);
CC BOOL WINAPI DPA_Sort(DSA *d, PFNDACOMPARE cmp, LPARAM l)
{
    if (!d) return FALSE;
    void **v = (void **)d->items;
    for (int i = 1; i < d->count; i++) {                   /* insertion sort: stable, lists are small */
        void *x = v[i];
        int j = i - 1;
        while (j >= 0 && cmp(v[j], x, l) > 0) { v[j + 1] = v[j]; j--; }
        v[j + 1] = x;
    }
    return TRUE;
}
typedef int (CALLBACK *PFNDAENUMCALLBACK)(void *p, void *data);
CC void WINAPI DPA_EnumCallback(DSA *d, PFNDAENUMCALLBACK fn, void *data)
{
    for (int i = 0; d && i < d->count; i++) if (!fn(((void **)d->items)[i], data)) break;
}
CC void WINAPI DPA_DestroyCallback(DSA *d, PFNDAENUMCALLBACK fn, void *data) { DPA_EnumCallback(d, fn, data); DPA_Destroy(d); }
