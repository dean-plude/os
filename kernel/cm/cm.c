/*
 * cm.c — NT Configuration Manager (in-memory registry)
 *
 * Phase 2 implementation: all data lives in kernel heap.  Persistence
 * (writing to an NTFS hive file) is deferred to Phase 3.
 *
 * Design:
 *   - Key tree: CM_KEY_NODE linked via child_head/sibling pointers.
 *   - Values: CM_VALUE singly-linked list hanging off each node.
 *   - Namespace integration: the root key \Registry is a CM_KEY_BODY
 *     inserted as an NT object under ObGetRootDirectory() at the path
 *     \Registry.  Sub-keys are found by walking CM_KEY_NODE trees
 *     (not the NT namespace), matching Windows behaviour.
 *   - Lookup: NtOpenKey walks from \Registry down using the path
 *     components after the root prefix.
 *   - Thread safety: a single ticket spinlock (cm_lock) guards all
 *     mutations.
 *
 * String comparison: registry key names are case-insensitive (like NT).
 * We perform case-folding on ASCII letters only (Phase 2 simplification).
 */

#include "cm.h"
#include "../ob/ob.h"
#include "../mm/vmm.h"
#include "../ke/printf.h"
#include "../lib/string.h"
#include "../include/types.h"

/* -----------------------------------------------------------------------
 * Key object type
 * ----------------------------------------------------------------------- */
static void key_delete(void *obj)
{
    /* The CM_KEY_BODY doesn't own the CM_KEY_NODE — the tree does.
     * Just clear the pointer so dangling uses are obvious. */
    PCM_KEY_BODY body = (PCM_KEY_BODY)obj;
    body->Node = NULL;
}

static OBJECT_TYPE cm_key_type_storage = {
    .Name            = "Key",
    .DefaultBodySize = sizeof(CM_KEY_BODY),
    .GenericRead     = 0x00020019,  /* KEY_READ */
    .GenericWrite    = 0x00020006,  /* KEY_WRITE */
    .GenericExecute  = 0x00020019,  /* KEY_EXECUTE (= KEY_READ) */
    .GenericAll      = KEY_ALL_ACCESS,
    .Operations      = { .Delete = key_delete },
};

/* -----------------------------------------------------------------------
 * Globals
 * ----------------------------------------------------------------------- */
static CM_KEY_NODE *cm_root_node;   /* \Registry node */

static volatile UINT32 cm_lock_next, cm_lock_owner;

static void cm_lock(void)
{
    UINT32 t = __atomic_fetch_add(&cm_lock_next, 1, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&cm_lock_owner, __ATOMIC_ACQUIRE) != t)
        __asm__ volatile ("pause");
}
static void cm_unlock(void)
{
    __atomic_fetch_add(&cm_lock_owner, 1, __ATOMIC_RELEASE);
}

/* -----------------------------------------------------------------------
 * ASCII case-insensitive comparison of WCHAR strings
 * (both strings are NUL-terminated WCHAR arrays)
 * ----------------------------------------------------------------------- */
static int wchar_icmp(const WCHAR *a, UINT32 alen,
                      const WCHAR *b, UINT32 blen)
{
    if (alen != blen) return (int)alen - (int)blen;
    for (UINT32 i = 0; i < alen; i++) {
        WCHAR ca = a[i], cb = b[i];
        /* ASCII fold */
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return (int)ca - (int)cb;
    }
    return 0;
}

/* Convert an ASCII string (with optional L prefix handling) to WCHAR buf.
 * Returns the number of WCHAR characters written (no NUL). */
static UINT32 ascii_to_wchar(const char *src, WCHAR *dst, UINT32 dstmax)
{
    UINT32 n = 0;
    while (*src && n < dstmax) {
        dst[n++] = (WCHAR)(unsigned char)*src++;
    }
    return n;
}

/* Convert a UNICODE_STRING to an internal WCHAR buffer (no NUL appended).
 * Returns number of WCHAR chars copied. */
static UINT32 unicode_to_wbuf(const UNICODE_STRING *us, WCHAR *dst, UINT32 dstmax)
{
    UINT32 chars = us->Length / sizeof(WCHAR);
    if (chars > dstmax) chars = dstmax;
    for (UINT32 i = 0; i < chars; i++) dst[i] = us->Buffer[i];
    return chars;
}

/* -----------------------------------------------------------------------
 * Key node allocation
 * ----------------------------------------------------------------------- */
static CM_KEY_NODE *cm_alloc_node(const WCHAR *name, UINT32 namelen,
                                   CM_KEY_NODE *parent)
{
    CM_KEY_NODE *n = kzalloc(sizeof(CM_KEY_NODE));
    if (!n) return NULL;
    n->parent   = parent;
    n->NameLen  = (namelen < CM_NAME_MAX) ? namelen : CM_NAME_MAX - 1;
    for (UINT32 i = 0; i < n->NameLen; i++) n->Name[i] = name[i];
    return n;
}

/* -----------------------------------------------------------------------
 * Find a direct child by name
 * ----------------------------------------------------------------------- */
static CM_KEY_NODE *cm_find_child(CM_KEY_NODE *parent,
                                   const WCHAR *name, UINT32 namelen)
{
    CM_KEY_NODE *c = parent->child_head;
    while (c) {
        if (wchar_icmp(c->Name, c->NameLen, name, namelen) == 0)
            return c;
        c = c->sibling;
    }
    return NULL;
}

/* -----------------------------------------------------------------------
 * Append a child node
 * ----------------------------------------------------------------------- */
static void cm_add_child(CM_KEY_NODE *parent, CM_KEY_NODE *child)
{
    child->sibling      = parent->child_head;
    parent->child_head  = child;
    parent->subkey_count++;
}

/* -----------------------------------------------------------------------
 * Walk a path like "Machine\SYSTEM\CurrentControlSet" starting from
 * the node *start*.  On success returns the node and sets *out_node.
 * path_wchar / path_len cover the remainder after the root prefix.
 * ----------------------------------------------------------------------- */
static NTSTATUS cm_walk_path(CM_KEY_NODE *start,
                              const WCHAR *path, UINT32 pathlen,
                              bool create_missing,
                              CM_KEY_NODE **out_node)
{
    CM_KEY_NODE *cur = start;
    UINT32 i = 0;

    /* Skip leading backslash if present */
    if (pathlen > 0 && path[0] == L'\\') { i++; }

    while (i < pathlen) {
        /* Find the next backslash (component separator) */
        UINT32 j = i;
        while (j < pathlen && path[j] != L'\\') j++;

        UINT32 complen = j - i;
        if (complen == 0) { i = j + 1; continue; }

        CM_KEY_NODE *child = cm_find_child(cur, path + i, complen);
        if (!child) {
            if (!create_missing) return STATUS_OBJECT_NAME_NOT_FOUND;
            child = cm_alloc_node(path + i, complen, cur);
            if (!child) return STATUS_NO_MEMORY;
            cm_add_child(cur, child);
        }
        cur = child;
        i = j + 1;
    }

    *out_node = cur;
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * Strip the "\Registry" prefix from an absolute path.
 * Returns pointer into the WCHAR buffer past the prefix.
 * Returns NULL if the path doesn't start with \Registry.
 * ----------------------------------------------------------------------- */
static const WCHAR *cm_strip_root_prefix(const WCHAR *path, UINT32 pathlen,
                                          UINT32 *out_remaining)
{
    /* "\Registry" = 9 chars */
    static const WCHAR root_prefix[] = { L'\\', L'R', L'e', L'g', L'i',
                                          L's', L't', L'r', L'y' };
    const UINT32 pfxlen = 9;

    if (pathlen < pfxlen) return NULL;
    for (UINT32 i = 0; i < pfxlen; i++) {
        WCHAR a = path[i], b = root_prefix[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return NULL;
    }
    /* After prefix: either end of string or '\' separator */
    if (pathlen > pfxlen && path[pfxlen] != L'\\') return NULL;
    *out_remaining = (pathlen > pfxlen) ? pathlen - pfxlen - 1 : 0;
    return (pathlen > pfxlen) ? path + pfxlen + 1 : path + pfxlen;
}

/* -----------------------------------------------------------------------
 * Resolve an ObjectAttributes path to a CM_KEY_NODE.
 * The path must be absolute (rooted at \Registry).
 * ----------------------------------------------------------------------- */
static NTSTATUS cm_resolve_path(POBJECT_ATTRIBUTES attr, bool create_missing,
                                  CM_KEY_NODE **out_node)
{
    if (!attr || !attr->ObjectName || !attr->ObjectName->Buffer)
        return STATUS_INVALID_PARAMETER;

    const WCHAR *path    = attr->ObjectName->Buffer;
    UINT32       pathlen = attr->ObjectName->Length / sizeof(WCHAR);

    UINT32 remaining = 0;
    const WCHAR *sub = cm_strip_root_prefix(path, pathlen, &remaining);
    if (!sub) return STATUS_OBJECT_PATH_NOT_FOUND;

    if (remaining == 0) {
        *out_node = cm_root_node;
        return STATUS_SUCCESS;
    }

    return cm_walk_path(cm_root_node, sub, remaining, create_missing, out_node);
}

/* -----------------------------------------------------------------------
 * Value lookup in a node
 * ----------------------------------------------------------------------- */
static CM_VALUE *cm_find_value(CM_KEY_NODE *node,
                                const WCHAR *name, UINT32 namelen)
{
    CM_VALUE *v = node->values;
    while (v) {
        if (wchar_icmp(v->Name, v->NameLen, name, namelen) == 0) return v;
        v = v->next;
    }
    return NULL;
}

/* -----------------------------------------------------------------------
 * Create or update a value on a node (internal, lock must be held)
 * ----------------------------------------------------------------------- */
static NTSTATUS cm_set_value_internal(CM_KEY_NODE *node,
                                       const WCHAR *name, UINT32 namelen,
                                       UINT32 type,
                                       const void *data, UINT32 datalen)
{
    if (datalen > CM_VALUE_DATA_MAX) return STATUS_BUFFER_OVERFLOW;

    CM_VALUE *v = cm_find_value(node, name, namelen);
    if (!v) {
        v = kzalloc(sizeof(CM_VALUE));
        if (!v) return STATUS_NO_MEMORY;
        v->NameLen = (namelen < CM_NAME_MAX) ? namelen : CM_NAME_MAX - 1;
        for (UINT32 i = 0; i < v->NameLen; i++) v->Name[i] = name[i];
        v->next      = node->values;
        node->values = v;
        node->value_count++;
    }

    v->Type    = type;
    v->DataLen = datalen;
    if (datalen && data)
        __builtin_memcpy(v->Data, data, datalen);

    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * CmInitialize — build the initial key tree
 * ----------------------------------------------------------------------- */

/* Helper to create a key hierarchy from ASCII path (called during init) */
static CM_KEY_NODE *cm_ensure_path_ascii(const char *path)
{
    /* Convert the path (e.g. "Machine\\SYSTEM") to WCHAR and walk */
    WCHAR wpath[512];
    UINT32 wlen = ascii_to_wchar(path, wpath, 511);
    CM_KEY_NODE *node = NULL;
    cm_walk_path(cm_root_node, wpath, wlen, true, &node);
    return node;
}

/* Helper to set a DWORD value (called during init) */
static void cm_init_dword(CM_KEY_NODE *node, const char *valname, UINT32 val)
{
    WCHAR wname[64];
    UINT32 wlen = ascii_to_wchar(valname, wname, 63);
    cm_set_value_internal(node, wname, wlen, REG_DWORD, &val, sizeof(UINT32));
}

/* Helper to set a REG_SZ value from ASCII */
static void cm_init_sz(CM_KEY_NODE *node, const char *valname, const char *value)
{
    WCHAR wname[64];
    UINT32 wnamelen = ascii_to_wchar(valname, wname, 63);

    /* Convert value string to WCHAR */
    WCHAR wval[CM_VALUE_DATA_MAX / sizeof(WCHAR)];
    UINT32 wvallen = ascii_to_wchar(value, wval, CM_VALUE_DATA_MAX / sizeof(WCHAR) - 1);
    wval[wvallen++] = 0; /* NUL terminator */

    cm_set_value_internal(node, wname, wnamelen, REG_SZ,
                           wval, (UINT32)(wvallen * sizeof(WCHAR)));
}

void CmInitialize(void)
{
    /* Register the Key object type */
    ObpKeyType = &cm_key_type_storage;
    ObCreateObjectType(ObpKeyType);

    /* Allocate the root \Registry node */
    WCHAR root_name[] = { L'R', L'e', L'g', L'i', L's', L't', L'r', L'y' };
    cm_root_node = cm_alloc_node(root_name, 8, NULL);
    if (!cm_root_node) {
        kprintf("[CM] FATAL: out of memory for root node\n");
        for (;;) {}
    }

    /* Create standard hive roots */
    cm_ensure_path_ascii("Machine");
    cm_ensure_path_ascii("User");
    cm_ensure_path_ascii("User\\Default");

    /* Populate HKLM\SYSTEM\CurrentControlSet\Control */
    CM_KEY_NODE *control = cm_ensure_path_ascii(
        "Machine\\SYSTEM\\CurrentControlSet\\Control");
    if (control) {
        cm_init_sz(control,   "SystemBootDevice", "\\Device\\HarddiskVolume1");
        cm_init_sz(control,   "SystemRoot",       "\\Windows");
        cm_init_dword(control,"CurrentMajorVersionNumber", 10);
        cm_init_dword(control,"CurrentMinorVersionNumber", 0);
        cm_init_dword(control,"CurrentBuildNumber",        19041);
    }

    /* HKLM\SYSTEM\CurrentControlSet\Services */
    cm_ensure_path_ascii("Machine\\SYSTEM\\CurrentControlSet\\Services");

    /* HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion */
    CM_KEY_NODE *winver = cm_ensure_path_ascii(
        "Machine\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion");
    if (winver) {
        cm_init_sz(winver,    "CurrentVersion",    "10.0");
        cm_init_sz(winver,    "ProductName",       "NovaOS");
        cm_init_sz(winver,    "EditionID",         "Professional");
        cm_init_dword(winver, "CurrentMajorVersionNumber", 10);
        cm_init_dword(winver, "CurrentMinorVersionNumber", 0);
        cm_init_dword(winver, "CurrentBuildNumber",        19041);
        cm_init_dword(winver, "UBR",                       1);
    }

    /* HKLM\HARDWARE\DESCRIPTION\System */
    CM_KEY_NODE *hw = cm_ensure_path_ascii(
        "Machine\\HARDWARE\\DESCRIPTION\\System");
    if (hw) {
        cm_init_sz(hw, "Identifier", "AT/AT COMPATIBLE");
        cm_init_sz(hw, "SystemBiosDate", "01/01/2024");
    }

    kprintf("[CM] Configuration Manager initialized\n");
}

/* -----------------------------------------------------------------------
 * NtOpenKey
 * ----------------------------------------------------------------------- */
NTSTATUS NtOpenKey(
    HANDLE             *KeyHandle,
    ACCESS_MASK         DesiredAccess,
    POBJECT_ATTRIBUTES  ObjectAttributes)
{
    cm_lock();

    CM_KEY_NODE *node = NULL;
    NTSTATUS s = cm_resolve_path(ObjectAttributes, false, &node);
    if (!NT_SUCCESS(s)) { cm_unlock(); return s; }

    /* Create a CM_KEY_BODY object */
    OBJECT_ATTRIBUTES attr = { sizeof(OBJECT_ATTRIBUTES),
                                .Attributes = OBJ_KERNEL_HANDLE };
    void *obj;
    s = ObCreateObject(ObpKeyType, &attr, 0, &obj);
    if (!NT_SUCCESS(s)) { cm_unlock(); return s; }

    PCM_KEY_BODY body = (PCM_KEY_BODY)obj;
    body->Node  = node;
    body->Flags = 0;

    cm_unlock();

    /* Insert into current process handle table */
    HANDLE h = 0;
    s = ObInsertObject(obj, NULL, DesiredAccess, 0, NULL, &h);
    if (!NT_SUCCESS(s)) { ObDereferenceObject(obj); return s; }

    if (KeyHandle) *KeyHandle = h;
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * NtCreateKey
 * ----------------------------------------------------------------------- */
NTSTATUS NtCreateKey(
    HANDLE             *KeyHandle,
    ACCESS_MASK         DesiredAccess,
    POBJECT_ATTRIBUTES  ObjectAttributes,
    UINT32              TitleIndex,
    UNICODE_STRING     *Class,
    UINT32              CreateOptions,
    UINT32             *Disposition)
{
    (void)TitleIndex; (void)Class; (void)CreateOptions;

    cm_lock();

    CM_KEY_NODE *node = NULL;
    NTSTATUS s = cm_resolve_path(ObjectAttributes, false, &node);
    UINT32 disp = REG_OPENED_EXISTING_KEY;

    if (s == STATUS_OBJECT_NAME_NOT_FOUND) {
        /* Need to create the leaf key */
        if (!ObjectAttributes || !ObjectAttributes->ObjectName)
            { cm_unlock(); return STATUS_INVALID_PARAMETER; }

        const WCHAR *full   = ObjectAttributes->ObjectName->Buffer;
        UINT32       flen   = ObjectAttributes->ObjectName->Length / sizeof(WCHAR);

        /* Find the parent path (everything up to the last '\') */
        UINT32 last_sep = flen;
        while (last_sep > 0 && full[last_sep - 1] != L'\\') last_sep--;
        if (last_sep == 0) { cm_unlock(); return STATUS_OBJECT_PATH_NOT_FOUND; }

        /* Resolve parent */
        WCHAR parent_path[512];
        UINT32 plen = (flen < 512) ? flen : 511;
        for (UINT32 i = 0; i < plen; i++) parent_path[i] = full[i];

        /* Strip \Registry prefix from parent path */
        UINT32 parent_remaining = 0;
        const WCHAR *parent_sub = cm_strip_root_prefix(parent_path,
                                                         last_sep - 1,
                                                         &parent_remaining);
        CM_KEY_NODE *parent = cm_root_node;
        if (parent_sub && parent_remaining > 0) {
            s = cm_walk_path(cm_root_node, parent_sub, parent_remaining,
                              false, &parent);
            if (!NT_SUCCESS(s)) { cm_unlock(); return STATUS_OBJECT_PATH_NOT_FOUND; }
        }

        /* Create the leaf */
        const WCHAR *leaf_name = full + last_sep;
        UINT32       leaf_len  = flen - last_sep;
        node = cm_alloc_node(leaf_name, leaf_len, parent);
        if (!node) { cm_unlock(); return STATUS_NO_MEMORY; }
        cm_add_child(parent, node);
        disp = REG_CREATED_NEW_KEY;
        s    = STATUS_SUCCESS;
    }

    if (!NT_SUCCESS(s)) { cm_unlock(); return s; }

    /* Wrap in a CM_KEY_BODY object */
    OBJECT_ATTRIBUTES attr = { sizeof(OBJECT_ATTRIBUTES),
                                .Attributes = OBJ_KERNEL_HANDLE };
    void *obj;
    s = ObCreateObject(ObpKeyType, &attr, 0, &obj);
    if (!NT_SUCCESS(s)) { cm_unlock(); return s; }

    PCM_KEY_BODY body = (PCM_KEY_BODY)obj;
    body->Node  = node;
    body->Flags = 0;
    cm_unlock();

    HANDLE h = 0;
    s = ObInsertObject(obj, NULL, DesiredAccess, 0, NULL, &h);
    if (!NT_SUCCESS(s)) { ObDereferenceObject(obj); return s; }

    if (KeyHandle) *KeyHandle = h;
    if (Disposition) *Disposition = disp;
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * NtQueryValueKey
 * ----------------------------------------------------------------------- */
NTSTATUS NtQueryValueKey(
    HANDLE                      KeyHandle,
    UNICODE_STRING             *ValueName,
    KEY_VALUE_INFORMATION_CLASS KeyValueInformationClass,
    void                       *KeyValueInformation,
    UINT32                      Length,
    UINT32                     *ResultLength)
{
    void *obj;
    NTSTATUS s = ObReferenceObjectByHandle(KeyHandle, KEY_QUERY_VALUE,
                                            ObpKeyType, NULL, &obj, NULL);
    if (!NT_SUCCESS(s)) return s;

    PCM_KEY_BODY body = (PCM_KEY_BODY)obj;

    /* Convert value name to WCHAR buf */
    WCHAR wname[CM_NAME_MAX];
    UINT32 wnamelen = unicode_to_wbuf(ValueName, wname, CM_NAME_MAX - 1);

    cm_lock();
    CM_VALUE *v = cm_find_value(body->Node, wname, wnamelen);
    if (!v) {
        cm_unlock();
        ObDereferenceObject(obj);
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    NTSTATUS result = STATUS_SUCCESS;

    if (KeyValueInformationClass == KeyValuePartialInformation) {
        UINT32 needed = (UINT32)(__builtin_offsetof(KEY_VALUE_PARTIAL_INFORMATION, Data)
                                 + v->DataLen);
        if (ResultLength) *ResultLength = needed;
        if (Length < needed) {
            result = STATUS_BUFFER_TOO_SMALL;
        } else if (KeyValueInformation) {
            KEY_VALUE_PARTIAL_INFORMATION *out = KeyValueInformation;
            out->TitleIndex = 0;
            out->Type       = v->Type;
            out->DataLength = v->DataLen;
            __builtin_memcpy(out->Data, v->Data, v->DataLen);
        }
    } else if (KeyValueInformationClass == KeyValueBasicInformation) {
        UINT32 needed = (UINT32)(__builtin_offsetof(KEY_VALUE_BASIC_INFORMATION, Name)
                                 + v->NameLen * sizeof(WCHAR));
        if (ResultLength) *ResultLength = needed;
        if (Length < needed) {
            result = STATUS_BUFFER_TOO_SMALL;
        } else if (KeyValueInformation) {
            KEY_VALUE_BASIC_INFORMATION *out = KeyValueInformation;
            out->TitleIndex = 0;
            out->Type       = v->Type;
            out->NameLength = v->NameLen * sizeof(WCHAR);
            __builtin_memcpy(out->Name, v->Name, v->NameLen * sizeof(WCHAR));
        }
    } else {
        result = STATUS_INVALID_PARAMETER;
    }

    cm_unlock();
    ObDereferenceObject(obj);
    return result;
}

/* -----------------------------------------------------------------------
 * NtSetValueKey
 * ----------------------------------------------------------------------- */
NTSTATUS NtSetValueKey(
    HANDLE          KeyHandle,
    UNICODE_STRING *ValueName,
    UINT32          TitleIndex,
    UINT32          Type,
    void           *Data,
    UINT32          DataSize)
{
    (void)TitleIndex;

    void *obj;
    NTSTATUS s = ObReferenceObjectByHandle(KeyHandle, KEY_SET_VALUE,
                                            ObpKeyType, NULL, &obj, NULL);
    if (!NT_SUCCESS(s)) return s;

    PCM_KEY_BODY body = (PCM_KEY_BODY)obj;

    WCHAR wname[CM_NAME_MAX];
    UINT32 wnamelen = unicode_to_wbuf(ValueName, wname, CM_NAME_MAX - 1);

    cm_lock();
    s = cm_set_value_internal(body->Node, wname, wnamelen, Type, Data, DataSize);
    cm_unlock();

    ObDereferenceObject(obj);
    return s;
}

/* -----------------------------------------------------------------------
 * NtDeleteValueKey
 * ----------------------------------------------------------------------- */
NTSTATUS NtDeleteValueKey(
    HANDLE          KeyHandle,
    UNICODE_STRING *ValueName)
{
    void *obj;
    NTSTATUS s = ObReferenceObjectByHandle(KeyHandle, KEY_SET_VALUE,
                                            ObpKeyType, NULL, &obj, NULL);
    if (!NT_SUCCESS(s)) return s;

    PCM_KEY_BODY body = (PCM_KEY_BODY)obj;

    WCHAR wname[CM_NAME_MAX];
    UINT32 wnamelen = unicode_to_wbuf(ValueName, wname, CM_NAME_MAX - 1);

    cm_lock();
    CM_VALUE **pp = &body->Node->values;
    while (*pp) {
        if (wchar_icmp((*pp)->Name, (*pp)->NameLen, wname, wnamelen) == 0) {
            CM_VALUE *del = *pp;
            *pp = del->next;
            body->Node->value_count--;
            kfree(del);
            cm_unlock();
            ObDereferenceObject(obj);
            return STATUS_SUCCESS;
        }
        pp = &(*pp)->next;
    }
    cm_unlock();
    ObDereferenceObject(obj);
    return STATUS_OBJECT_NAME_NOT_FOUND;
}

/* -----------------------------------------------------------------------
 * NtDeleteKey
 * ----------------------------------------------------------------------- */
NTSTATUS NtDeleteKey(HANDLE KeyHandle)
{
    void *obj;
    NTSTATUS s = ObReferenceObjectByHandle(KeyHandle, KEY_ALL_ACCESS,
                                            ObpKeyType, NULL, &obj, NULL);
    if (!NT_SUCCESS(s)) return s;

    PCM_KEY_BODY body = (PCM_KEY_BODY)obj;
    CM_KEY_NODE *node = body->Node;

    cm_lock();
    if (node->child_head) {
        cm_unlock();
        ObDereferenceObject(obj);
        return STATUS_CANNOT_DELETE; /* Has subkeys */
    }

    /* Unlink from parent */
    if (node->parent) {
        CM_KEY_NODE **pp = &node->parent->child_head;
        while (*pp) {
            if (*pp == node) { *pp = node->sibling; break; }
            pp = &(*pp)->sibling;
        }
        node->parent->subkey_count--;
    }

    /* Free all values */
    CM_VALUE *v = node->values;
    while (v) {
        CM_VALUE *nxt = v->next;
        kfree(v);
        v = nxt;
    }
    cm_unlock();

    kfree(node);
    ObDereferenceObject(obj);
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * NtEnumerateKey
 * ----------------------------------------------------------------------- */
NTSTATUS NtEnumerateKey(
    HANDLE                 KeyHandle,
    UINT32                 Index,
    KEY_INFORMATION_CLASS  KeyInformationClass,
    void                  *KeyInformation,
    UINT32                 Length,
    UINT32                *ResultLength)
{
    (void)KeyInformationClass;

    void *obj;
    NTSTATUS s = ObReferenceObjectByHandle(KeyHandle, KEY_ENUMERATE_SUB_KEYS,
                                            ObpKeyType, NULL, &obj, NULL);
    if (!NT_SUCCESS(s)) return s;

    PCM_KEY_BODY body = (PCM_KEY_BODY)obj;

    cm_lock();
    CM_KEY_NODE *child = body->Node->child_head;
    for (UINT32 i = 0; i < Index && child; i++) child = child->sibling;

    if (!child) {
        cm_unlock();
        ObDereferenceObject(obj);
        return STATUS_NO_MORE_ENTRIES;
    }

    UINT32 needed = (UINT32)(__builtin_offsetof(KEY_BASIC_INFORMATION, Name)
                             + child->NameLen * sizeof(WCHAR));
    if (ResultLength) *ResultLength = needed;

    if (Length < needed) {
        cm_unlock();
        ObDereferenceObject(obj);
        return STATUS_BUFFER_TOO_SMALL;
    }

    if (KeyInformation) {
        KEY_BASIC_INFORMATION *out = KeyInformation;
        out->LastWriteTime = 0;
        out->TitleIndex    = 0;
        out->NameLength    = child->NameLen * sizeof(WCHAR);
        __builtin_memcpy(out->Name, child->Name, child->NameLen * sizeof(WCHAR));
    }

    cm_unlock();
    ObDereferenceObject(obj);
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * NtEnumerateValueKey
 * ----------------------------------------------------------------------- */
NTSTATUS NtEnumerateValueKey(
    HANDLE                      KeyHandle,
    UINT32                      Index,
    KEY_VALUE_INFORMATION_CLASS KeyValueInformationClass,
    void                       *KeyValueInformation,
    UINT32                      Length,
    UINT32                     *ResultLength)
{
    void *obj;
    NTSTATUS s = ObReferenceObjectByHandle(KeyHandle, KEY_QUERY_VALUE,
                                            ObpKeyType, NULL, &obj, NULL);
    if (!NT_SUCCESS(s)) return s;

    PCM_KEY_BODY body = (PCM_KEY_BODY)obj;

    cm_lock();
    CM_VALUE *v = body->Node->values;
    for (UINT32 i = 0; i < Index && v; i++) v = v->next;

    if (!v) {
        cm_unlock();
        ObDereferenceObject(obj);
        return STATUS_NO_MORE_ENTRIES;
    }

    NTSTATUS result = STATUS_SUCCESS;

    if (KeyValueInformationClass == KeyValueBasicInformation) {
        UINT32 needed = (UINT32)(__builtin_offsetof(KEY_VALUE_BASIC_INFORMATION, Name)
                                 + v->NameLen * sizeof(WCHAR));
        if (ResultLength) *ResultLength = needed;
        if (Length < needed) {
            result = STATUS_BUFFER_TOO_SMALL;
        } else if (KeyValueInformation) {
            KEY_VALUE_BASIC_INFORMATION *out = KeyValueInformation;
            out->TitleIndex = 0;
            out->Type       = v->Type;
            out->NameLength = v->NameLen * sizeof(WCHAR);
            __builtin_memcpy(out->Name, v->Name, v->NameLen * sizeof(WCHAR));
        }
    } else if (KeyValueInformationClass == KeyValuePartialInformation) {
        UINT32 needed = (UINT32)(__builtin_offsetof(KEY_VALUE_PARTIAL_INFORMATION, Data)
                                 + v->DataLen);
        if (ResultLength) *ResultLength = needed;
        if (Length < needed) {
            result = STATUS_BUFFER_TOO_SMALL;
        } else if (KeyValueInformation) {
            KEY_VALUE_PARTIAL_INFORMATION *out = KeyValueInformation;
            out->TitleIndex = 0;
            out->Type       = v->Type;
            out->DataLength = v->DataLen;
            __builtin_memcpy(out->Data, v->Data, v->DataLen);
        }
    } else {
        result = STATUS_INVALID_PARAMETER;
    }

    cm_unlock();
    ObDereferenceObject(obj);
    return result;
}
