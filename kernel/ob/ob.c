/*
 * ob.c — NT Object Manager implementation
 *
 * Design notes:
 *
 * Object layout in memory:
 *   [OBJECT_HEADER][object body]
 *   Users hold a pointer to the body.  The header is accessed via
 *   BODY_TO_OBJECT_HEADER(body).
 *
 * Reference counting:
 *   - ObCreateObject: PointerCount = 1
 *   - ObInsertObject (creates handle): HandleCount++; PointerCount++
 *   - ObReferenceObjectByHandle: PointerCount++
 *   - ObDereferenceObject: PointerCount--; if 0 call Delete + free
 *   - ObCloseHandle: HandleCount--; ObDereferenceObject
 *
 * Handle encoding:
 *   HANDLE = (table_index + 1) × 4
 *   e.g. index 0 → handle 4, index 1 → handle 8
 *   This matches NT's layout (bits 0-1 are flag bits).
 *
 * Namespace:
 *   A tree of OBJECT_DIRECTORY objects.  The root is "\".
 *   Names are stored as UNICODE_STRING (UTF-16LE).
 *   For Phase 2 we only need a handful of directories, so a simple
 *   hash chain within each directory is sufficient.
 */

#include "ob.h"
#include "../mm/vmm.h"
#include "../ke/printf.h"
#include "../lib/string.h"
#include "../include/types.h"
#include "../arch/x86_64/cpu.h"

/* -----------------------------------------------------------------------
 * Spinlock helpers (reused from pmm.c pattern)
 * ----------------------------------------------------------------------- */
static void ob_lock(volatile UINT32 *next, volatile UINT32 *owner)
{
    UINT32 t = __atomic_fetch_add(next, 1, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(owner, __ATOMIC_ACQUIRE) != t) pause_cpu();
}
static void ob_unlock(volatile UINT32 *owner)
{
    __atomic_fetch_add(owner, 1, __ATOMIC_RELEASE);
}

/* -----------------------------------------------------------------------
 * Global object type registry
 * ----------------------------------------------------------------------- */
POBJECT_TYPE ObpDirectoryType;
POBJECT_TYPE ObpProcessType;
POBJECT_TYPE ObpThreadType;
POBJECT_TYPE ObpEventType;
POBJECT_TYPE ObpMutantType;
POBJECT_TYPE ObpSemaphoreType;
POBJECT_TYPE ObpTimerType;
POBJECT_TYPE ObpTokenType;
POBJECT_TYPE ObpFileType;
POBJECT_TYPE ObpKeyType;
POBJECT_TYPE ObpSectionType;
POBJECT_TYPE ObpSymlinkType;

static OBJECT_TYPE ob_type_storage[OB_TYPE_IDX_MAX];
static UINT32      ob_type_count;

/* -----------------------------------------------------------------------
 * NT Namespace root directory
 * ----------------------------------------------------------------------- */
static OBJECT_DIRECTORY ob_root_dir_body;
static OBJECT_HEADER    ob_root_dir_hdr;
static void            *ob_root_directory_object;  /* points to body */

/* Well-known sub-directories (bodies allocated via kmalloc) */
static void *ob_dir_device;
static void *ob_dir_bno;        /* \BaseNamedObjects */
static void *ob_dir_registry;
static void *ob_dir_kernel;

/* -----------------------------------------------------------------------
 * UNICODE_STRING helpers
 * ----------------------------------------------------------------------- */

/* Simple hash for a UNICODE_STRING (case-insensitive, ASCII range only) */
static UINT32 ustr_hash(const UNICODE_STRING *s, UINT32 buckets)
{
    UINT32 h = 2166136261UL;  /* FNV-1a offset basis */
    USHORT len = s->Length / sizeof(WCHAR);
    for (USHORT i = 0; i < len; i++) {
        WCHAR c = s->Buffer[i];
        if (c >= L'a' && c <= L'z') c -= 32;  /* uppercase for case-insensitive */
        h ^= (UINT32)c;
        h *= 16777619UL;
    }
    return h % buckets;
}

static bool ustr_equal_ci(const UNICODE_STRING *a, const UNICODE_STRING *b)
{
    if (a->Length != b->Length) return false;
    USHORT len = a->Length / sizeof(WCHAR);
    for (USHORT i = 0; i < len; i++) {
        WCHAR ca = a->Buffer[i], cb = b->Buffer[i];
        if (ca >= L'a' && ca <= L'z') ca -= 32;
        if (cb >= L'a' && cb <= L'z') cb -= 32;
        if (ca != cb) return false;
    }
    return true;
}

/* Convert a narrow (ASCII) string to a WCHAR buffer (simple zero-extension) */
static void ascii_to_ustr(UNICODE_STRING *out, WCHAR *buf, USHORT buf_len,
                           const char *src)
{
    USHORT i = 0;
    while (*src && i < (USHORT)(buf_len - 1)) {
        buf[i++] = (WCHAR)(unsigned char)*src++;
    }
    buf[i] = 0;
    out->Buffer        = buf;
    out->Length        = (USHORT)(i * sizeof(WCHAR));
    out->MaximumLength = (USHORT)buf_len;
}

/* -----------------------------------------------------------------------
 * Directory operations
 * ----------------------------------------------------------------------- */

/* Insert an object into a directory under a given name. */
static NTSTATUS dir_insert(POBJECT_DIRECTORY dir,
                            const UNICODE_STRING *name, void *object)
{
    UINT32 bucket = ustr_hash(name, OBJ_DIRECTORY_HASH_BUCKETS);

    /* Check for duplicate */
    for (POBJECT_DIRECTORY_ENTRY e = dir->HashBuckets[bucket]; e; e = e->HashNext) {
        if (ustr_equal_ci(&e->Name, name)) return STATUS_ALREADY_EXISTS;
    }

    POBJECT_DIRECTORY_ENTRY entry = kzalloc(sizeof(OBJECT_DIRECTORY_ENTRY));
    if (!entry) return STATUS_NO_MEMORY;

    /* Copy name into the embedded buffer */
    USHORT copy_len = name->Length;
    if (copy_len > (OBJ_NAME_MAX - 1) * sizeof(WCHAR))
        copy_len = (USHORT)((OBJ_NAME_MAX - 1) * sizeof(WCHAR));
    __builtin_memcpy(entry->NameBuf, name->Buffer, copy_len);
    entry->NameBuf[copy_len / sizeof(WCHAR)] = 0;
    entry->Name.Buffer        = entry->NameBuf;
    entry->Name.Length        = copy_len;
    entry->Name.MaximumLength = sizeof(entry->NameBuf);
    entry->Object             = object;
    entry->Directory          = dir;

    entry->HashNext            = dir->HashBuckets[bucket];
    dir->HashBuckets[bucket]   = entry;
    dir->TotalEntries++;

    /* Mark the object header as named */
    BODY_TO_OBJECT_HEADER(object)->Flags |= OB_FLAG_NAMED;
    BODY_TO_OBJECT_HEADER(object)->NameEntry = entry;

    return STATUS_SUCCESS;
}

/* Unlink a named object's entry from its directory and free it. */
static void dir_remove(POBJECT_DIRECTORY_ENTRY entry)
{
    /* (every bucket: a name cut to OBJ_NAME_MAX hashes differently) */
    POBJECT_DIRECTORY dir = entry->Directory;
    for (UINT32 b = 0; b < OBJ_DIRECTORY_HASH_BUCKETS; b++) {
        for (POBJECT_DIRECTORY_ENTRY *pp = &dir->HashBuckets[b]; *pp; pp = &(*pp)->HashNext) {
            if (*pp == entry) {
                *pp = entry->HashNext;
                dir->TotalEntries--;
                kfree(entry);
                return;
            }
        }
    }
}

/* Look up an object in a directory by name. */
static void *dir_lookup(POBJECT_DIRECTORY dir, const UNICODE_STRING *name)
{
    UINT32 bucket = ustr_hash(name, OBJ_DIRECTORY_HASH_BUCKETS);
    for (POBJECT_DIRECTORY_ENTRY e = dir->HashBuckets[bucket]; e; e = e->HashNext) {
        if (ustr_equal_ci(&e->Name, name)) return e->Object;
    }
    return NULL;
}

/* -----------------------------------------------------------------------
 * Object allocation and deallocation
 * ----------------------------------------------------------------------- */

static void ob_free_object(void *object)
{
    POBJECT_HEADER hdr = BODY_TO_OBJECT_HEADER(object);
    kfree(hdr);
}

/* -----------------------------------------------------------------------
 * ObCreateObjectType
 * ----------------------------------------------------------------------- */
NTSTATUS ObCreateObjectType(POBJECT_TYPE type)
{
    if (ob_type_count >= OB_TYPE_IDX_MAX) return STATUS_INSUFFICIENT_RESOURCES;
    type->TypeIndex = ob_type_count++;
    kprintf("[OB] Registered type '%s' (index=%u, body=%u bytes)\n",
            type->Name, type->TypeIndex, type->DefaultBodySize);
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * ObCreateObject
 * ----------------------------------------------------------------------- */
NTSTATUS ObCreateObject(
    POBJECT_TYPE        Type,
    POBJECT_ATTRIBUTES  Attributes,
    UINT32              AdditionalBodySize,
    void              **Object)
{
    if (!Type || !Object) return STATUS_INVALID_PARAMETER;

    UINT32 body_size = Type->DefaultBodySize + AdditionalBodySize;
    UINT32 total     = sizeof(OBJECT_HEADER) + body_size;

    POBJECT_HEADER hdr = kzalloc(total);
    if (!hdr) return STATUS_NO_MEMORY;

    hdr->PointerCount = 1;
    hdr->HandleCount  = 0;
    hdr->Type         = Type;
    hdr->Flags        = 0;
    hdr->NameEntry    = NULL;

    if (Attributes) {
        if (Attributes->Attributes & OBJ_PERMANENT) hdr->Flags |= OB_FLAG_PERMANENT;
        if (Attributes->Attributes & OBJ_KERNEL_HANDLE) hdr->Flags |= OB_FLAG_KERNEL;
    }

    __atomic_fetch_add(&Type->TotalNumberOfObjects, 1, __ATOMIC_RELAXED);

    *Object = OBJECT_HEADER_TO_BODY(hdr);
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * Handle table operations
 * ----------------------------------------------------------------------- */

NTSTATUS ObCreateHandleTable(PHANDLE_TABLE *TableOut)
{
    PHANDLE_TABLE t = kzalloc(sizeof(HANDLE_TABLE));
    if (!t) return STATUS_NO_MEMORY;

    t->Entries = kzalloc(sizeof(HANDLE_TABLE_ENTRY) * HANDLE_TABLE_INITIAL_SIZE);
    if (!t->Entries) { kfree(t); return STATUS_NO_MEMORY; }

    t->Capacity  = HANDLE_TABLE_INITIAL_SIZE;
    t->NextFree  = 0;
    *TableOut    = t;
    return STATUS_SUCCESS;
}

void ObDestroyHandleTable(PHANDLE_TABLE Table)
{
    if (!Table) return;
    for (UINT32 i = 0; i < Table->Capacity; i++) {
        if (Table->Entries[i].Object) {
            ObDereferenceObject(Table->Entries[i].Object);
            Table->Entries[i].Object = NULL;
        }
    }
    kfree(Table->Entries);
    kfree(Table);
}

/* Allocate a handle slot; returns the HANDLE value */
static HANDLE ht_alloc_slot(PHANDLE_TABLE ht, void *object, ACCESS_MASK access)
{
    ob_lock(&ht->LockNext, &ht->LockOwner);

    /* Search from hint */
    UINT32 start = ht->NextFree;
    UINT32 idx   = start;
    bool   found = false;

    /* Two-pass search: from hint to end, then from beginning to hint */
    for (UINT32 pass = 0; pass < 2 && !found; pass++) {
        UINT32 end = (pass == 0) ? ht->Capacity : start;
        UINT32 begin = (pass == 0) ? start : 0;
        for (idx = begin; idx < end; idx++) {
            if (!ht->Entries[idx].Object) { found = true; break; }
        }
    }

    if (!found) {
        /* Grow the table */
        UINT32 new_cap = ht->Capacity + HANDLE_TABLE_GROW_BY;
        HANDLE_TABLE_ENTRY *new_entries =
            kzalloc(sizeof(HANDLE_TABLE_ENTRY) * new_cap);
        if (!new_entries) {
            ob_unlock(&ht->LockOwner);
            return 0;
        }
        __builtin_memcpy(new_entries, ht->Entries,
                         sizeof(HANDLE_TABLE_ENTRY) * ht->Capacity);
        kfree(ht->Entries);
        idx          = ht->Capacity;
        ht->Entries  = new_entries;
        ht->Capacity = new_cap;
    }

    ht->Entries[idx].Object        = object;
    ht->Entries[idx].GrantedAccess = access;
    ht->NextFree                   = idx + 1;

    ob_unlock(&ht->LockOwner);
    return (HANDLE)((UINT64)(idx + 1) * 4);  /* NT-style: (index+1)*4 */
}

static HANDLE_TABLE_ENTRY *ht_get_entry(PHANDLE_TABLE ht, HANDLE h)
{
    if (!h || h == (HANDLE)(ULONG_PTR)-1) return NULL;
    UINT64 idx64 = ((UINT64)h / 4);
    if (idx64 == 0 || idx64 > ht->Capacity) return NULL;
    UINT32 idx = (UINT32)(idx64 - 1);
    if (!ht->Entries[idx].Object) return NULL;
    return &ht->Entries[idx];
}

/* -----------------------------------------------------------------------
 * ObInsertObject
 * ----------------------------------------------------------------------- */

/* Current process's handle table (set by PsInitialize, updated on context switch) */
extern PHANDLE_TABLE PsGetCurrentProcessHandleTable(void);

/* Kernel handle table for handles created with OBJ_KERNEL_HANDLE */
static HANDLE_TABLE ob_kernel_handle_table;

NTSTATUS ObInsertObject(
    void        *Object,
    void        *Process,
    ACCESS_MASK  DesiredAccess,
    UINT32       ObjectPointerBias,
    void       **NewObject,
    HANDLE      *Handle)
{
    (void)ObjectPointerBias;
    if (!Object) return STATUS_INVALID_PARAMETER;

    POBJECT_HEADER hdr = BODY_TO_OBJECT_HEADER(Object);

    /* Map GENERIC_* bits via the type's generic mapping (a type that
     * leaves a mapping out keeps that generic bit as it is) */
    ACCESS_MASK access = DesiredAccess;
    const POBJECT_TYPE ty = hdr->Type;
    const struct { ACCESS_MASK bit; ULONG map; } gm[] = {
        { GENERIC_READ,    ty->GenericRead    },
        { GENERIC_WRITE,   ty->GenericWrite   },
        { GENERIC_EXECUTE, ty->GenericExecute },
        { GENERIC_ALL,     ty->GenericAll     },
    };
    for (unsigned i = 0; i < sizeof(gm) / sizeof(gm[0]); i++)
        if ((access & gm[i].bit) && gm[i].map)
            access = (access & ~gm[i].bit) | gm[i].map;

    __atomic_fetch_add(&hdr->HandleCount,  1, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&hdr->PointerCount, 1, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&hdr->Type->TotalNumberOfHandles, 1, __ATOMIC_RELAXED);

    if (Handle) {
        PHANDLE_TABLE ht;
        if (Process) {
            /* Phase 3: obtain process's handle table */
            ht = PsGetCurrentProcessHandleTable();
        } else {
            ht = PsGetCurrentProcessHandleTable();
        }

        if (!ht) {
            /* Fall back to kernel handle table */
            ht = &ob_kernel_handle_table;
        }

        HANDLE h = ht_alloc_slot(ht, Object, access);
        if (!h) {
            __atomic_fetch_sub(&hdr->HandleCount,  1, __ATOMIC_SEQ_CST);
            __atomic_fetch_sub(&hdr->PointerCount, 1, __ATOMIC_SEQ_CST);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        *Handle = h;
    }

    if (NewObject) *NewObject = Object;
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * ObReferenceObjectByHandle
 * ----------------------------------------------------------------------- */
NTSTATUS ObReferenceObjectByHandle(
    HANDLE       Handle,
    ACCESS_MASK  DesiredAccess,
    POBJECT_TYPE ExpectedType,
    void        *Process,
    void       **Object,
    ACCESS_MASK *GrantedAccess)
{
    (void)Process; (void)DesiredAccess;
    if (!Object) return STATUS_INVALID_PARAMETER;

    /* Kernel pseudo-handles */
    extern void *PsGetCurrentProcess(void);
    extern void *PsGetCurrentThread(void);
    if ((ULONG_PTR)Handle == (ULONG_PTR)-1) {  /* NtCurrentProcess() */
        *Object = PsGetCurrentProcess();
        if (*Object) ObReferenceObject(*Object);
        if (GrantedAccess) *GrantedAccess = PROCESS_ALL_ACCESS;
        return STATUS_SUCCESS;
    }
    if ((ULONG_PTR)Handle == (ULONG_PTR)-2) {  /* NtCurrentThread() */
        *Object = PsGetCurrentThread();
        if (*Object) ObReferenceObject(*Object);
        if (GrantedAccess) *GrantedAccess = THREAD_ALL_ACCESS;
        return STATUS_SUCCESS;
    }

    PHANDLE_TABLE ht = PsGetCurrentProcessHandleTable();
    if (!ht) ht = &ob_kernel_handle_table;

    HANDLE_TABLE_ENTRY *entry = ht_get_entry(ht, Handle);
    if (!entry) return STATUS_INVALID_HANDLE;

    void *obj = entry->Object;
    POBJECT_HEADER hdr = BODY_TO_OBJECT_HEADER(obj);

    if (ExpectedType && hdr->Type != ExpectedType)
        return STATUS_OBJECT_TYPE_MISMATCH;

    ObReferenceObject(obj);
    *Object = obj;
    if (GrantedAccess) *GrantedAccess = entry->GrantedAccess;
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * ObReferenceObject / ObDereferenceObject
 * ----------------------------------------------------------------------- */
void ObReferenceObject(void *Object)
{
    if (!Object) return;
    POBJECT_HEADER hdr = BODY_TO_OBJECT_HEADER(Object);
    __atomic_fetch_add(&hdr->PointerCount, 1, __ATOMIC_SEQ_CST);
}

void ObDereferenceObject(void *Object)
{
    if (!Object) return;
    POBJECT_HEADER hdr = BODY_TO_OBJECT_HEADER(Object);
    LONG new_count = __atomic_sub_fetch(&hdr->PointerCount, 1, __ATOMIC_SEQ_CST);

    if (new_count == 0) {
        /* Call the type's Delete callback */
        if (hdr->Type && hdr->Type->Operations.Delete) {
            hdr->Type->Operations.Delete(Object);
        }
        /* Remove from namespace if named */
        if (hdr->NameEntry) dir_remove(hdr->NameEntry);
        __atomic_fetch_sub(&hdr->Type->TotalNumberOfObjects, 1, __ATOMIC_RELAXED);
        ob_free_object(Object);
    }
}

/* -----------------------------------------------------------------------
 * ObCloseHandle
 * ----------------------------------------------------------------------- */
NTSTATUS ObCloseHandle(HANDLE Handle, void *Process)
{
    (void)Process;
    PHANDLE_TABLE ht = PsGetCurrentProcessHandleTable();
    if (!ht) ht = &ob_kernel_handle_table;

    HANDLE_TABLE_ENTRY *entry = ht_get_entry(ht, Handle);
    if (!entry) return STATUS_INVALID_HANDLE;

    void *obj = entry->Object;
    POBJECT_HEADER hdr = BODY_TO_OBJECT_HEADER(obj);

    /* Call the type's Close callback */
    if (hdr->Type && hdr->Type->Operations.Close) {
        hdr->Type->Operations.Close(Process, obj,
                                    entry->GrantedAccess, 1);
    }

    /* Clear the handle slot */
    ob_lock(&ht->LockNext, &ht->LockOwner);
    entry->Object        = NULL;
    entry->GrantedAccess = 0;
    ob_unlock(&ht->LockOwner);

    __atomic_fetch_sub(&hdr->HandleCount, 1, __ATOMIC_SEQ_CST);
    __atomic_fetch_sub(&hdr->Type->TotalNumberOfHandles, 1, __ATOMIC_RELAXED);
    ObDereferenceObject(obj);
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * ObReferenceObjectByName
 * ----------------------------------------------------------------------- */
NTSTATUS ObReferenceObjectByName(
    PUNICODE_STRING ObjectName,
    UINT32          Attributes,
    POBJECT_TYPE    ObjectType,
    ACCESS_MASK     DesiredAccess,
    void          **Object)
{
    (void)Attributes; (void)DesiredAccess;
    if (!ObjectName || !Object) return STATUS_INVALID_PARAMETER;

    /* Walk the path components starting from root */
    POBJECT_DIRECTORY cur_dir = &ob_root_dir_body;
    WCHAR  *p   = ObjectName->Buffer;
    USHORT  remaining = ObjectName->Length / sizeof(WCHAR);

    if (remaining > 0 && p[0] == L'\\') { p++; remaining--; }  /* skip leading \ */

    void *found = NULL;

    while (remaining > 0) {
        /* Find the next component (up to the next '\') */
        USHORT comp_len = 0;
        while (comp_len < remaining && p[comp_len] != L'\\') comp_len++;

        UNICODE_STRING component;
        component.Buffer        = p;
        component.Length        = (USHORT)(comp_len * sizeof(WCHAR));
        component.MaximumLength = component.Length;

        found = dir_lookup(cur_dir, &component);
        if (!found) return STATUS_OBJECT_NAME_NOT_FOUND;

        p         += comp_len;
        remaining -= comp_len;
        if (remaining > 0 && p[0] == L'\\') { p++; remaining--; }

        /* If there are more components, found must be a directory */
        if (remaining > 0) {
            POBJECT_HEADER hdr = BODY_TO_OBJECT_HEADER(found);
            if (hdr->Type != ObpDirectoryType)
                return STATUS_OBJECT_PATH_INVALID;
            cur_dir = (POBJECT_DIRECTORY)found;
        }
    }

    if (!found) return STATUS_OBJECT_NAME_NOT_FOUND;

    POBJECT_HEADER hdr = BODY_TO_OBJECT_HEADER(found);
    if (ObjectType && hdr->Type != ObjectType)
        return STATUS_OBJECT_TYPE_MISMATCH;

    ObReferenceObject(found);
    *Object = found;
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * ObGetRootDirectory / ObGetDirectory
 * ----------------------------------------------------------------------- */
POBJECT_DIRECTORY ObGetRootDirectory(void)
{
    return &ob_root_dir_body;
}

POBJECT_DIRECTORY ObGetDirectory(const char *ascii_path)
{
    if (!ascii_path || ascii_path[0] != '\\') return NULL;

    const char *p = ascii_path + 1;  /* skip leading \ */

    if (strcmp(p, "Device") == 0)          return (POBJECT_DIRECTORY)ob_dir_device;
    if (strcmp(p, "BaseNamedObjects") == 0) return (POBJECT_DIRECTORY)ob_dir_bno;
    if (strcmp(p, "Registry") == 0)         return (POBJECT_DIRECTORY)ob_dir_registry;
    if (strcmp(p, "KernelObjects") == 0)    return (POBJECT_DIRECTORY)ob_dir_kernel;
    return NULL;
}

/* -----------------------------------------------------------------------
 * Helper: create a named directory object and insert it into a parent dir
 * ----------------------------------------------------------------------- */
static NTSTATUS create_directory(POBJECT_DIRECTORY parent_dir,
                                  const char *ascii_name, void **dir_out)
{
    OBJECT_ATTRIBUTES attr = { sizeof(OBJECT_ATTRIBUTES) };
    void *dir_obj;
    NTSTATUS s = ObCreateObject(ObpDirectoryType, &attr, 0, &dir_obj);
    if (!NT_SUCCESS(s)) return s;

    WCHAR wname[64];
    UNICODE_STRING uname;
    ascii_to_ustr(&uname, wname, 64, ascii_name);

    s = dir_insert(parent_dir, &uname, dir_obj);
    if (!NT_SUCCESS(s)) { ObDereferenceObject(dir_obj); return s; }

    /* Permanent objects don't get auto-deleted when pointer count reaches 0 */
    BODY_TO_OBJECT_HEADER(dir_obj)->Flags |= OB_FLAG_PERMANENT;

    if (dir_out) *dir_out = dir_obj;
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * Built-in type callbacks (minimal — types override as needed)
 * ----------------------------------------------------------------------- */
static void directory_delete(void *obj) { (void)obj; }

static OBJECT_TYPE ob_directory_type_storage = {
    .Name            = "Directory",
    .DefaultBodySize = sizeof(OBJECT_DIRECTORY),
    .GenericRead     = 0x00020003,  /* READ_CONTROL | DIRECTORY_QUERY | DIRECTORY_TRAVERSE */
    .GenericWrite    = 0x0002000C,  /* READ_CONTROL | DIRECTORY_CREATE_OBJECT/SUBDIRECTORY */
    .GenericExecute  = 0x00020003,
    .GenericAll      = DIRECTORY_ALL_ACCESS,
    .Operations      = { .Delete = directory_delete },
};

/* -----------------------------------------------------------------------
 * ObInitialize
 * ----------------------------------------------------------------------- */
void ObInitialize(void)
{
    /* Register the Directory type first (needed to create namespace dirs) */
    ObpDirectoryType = &ob_directory_type_storage;
    ObCreateObjectType(ObpDirectoryType);

    /* Set up the root directory object manually (no heap needed for root) */
    __builtin_memset(&ob_root_dir_body, 0, sizeof(ob_root_dir_body));
    __builtin_memset(&ob_root_dir_hdr,  0, sizeof(ob_root_dir_hdr));
    ob_root_dir_hdr.PointerCount = 1;
    ob_root_dir_hdr.Type         = ObpDirectoryType;
    ob_root_dir_hdr.Flags        = OB_FLAG_PERMANENT;
    ob_root_directory_object     = &ob_root_dir_body;

    /* Initialize kernel handle table */
    __builtin_memset(&ob_kernel_handle_table, 0, sizeof(ob_kernel_handle_table));
    ob_kernel_handle_table.Entries =
        kzalloc(sizeof(HANDLE_TABLE_ENTRY) * HANDLE_TABLE_INITIAL_SIZE);
    ob_kernel_handle_table.Capacity = HANDLE_TABLE_INITIAL_SIZE;

    /* Create standard namespace directories */
    create_directory(&ob_root_dir_body, "Device",           &ob_dir_device);
    create_directory(&ob_root_dir_body, "BaseNamedObjects",  &ob_dir_bno);
    create_directory(&ob_root_dir_body, "Registry",          &ob_dir_registry);
    create_directory(&ob_root_dir_body, "KernelObjects",     &ob_dir_kernel);
    create_directory(&ob_root_dir_body, "DosDevices",        NULL);
    create_directory(&ob_root_dir_body, "Windows",           NULL);

    kprintf("[OB] Object manager initialized\n");
    kprintf("[OB] Namespace root: \\  (Device, BaseNamedObjects, Registry, KernelObjects)\n");
}
