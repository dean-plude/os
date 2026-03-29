/*
 * io.h — NT I/O Manager
 *
 * The I/O Manager is the NT executive component responsible for:
 *   - Managing device drivers (DRIVER_OBJECT)
 *   - Managing device objects (DEVICE_OBJECT, attached to drivers)
 *   - Managing open file/device handles (FILE_OBJECT)
 *   - Routing I/O requests via IRP (I/O Request Packet) dispatch
 *
 * IRP flow (simplified):
 *   1. User calls NtCreateFile / NtReadFile / etc.
 *   2. I/O Manager allocates an IRP and a stack location per driver in the stack.
 *   3. IoCallDriver() calls the top driver's dispatch routine.
 *   4. Driver processes the IRP and calls IoCompleteRequest().
 *   5. I/O Manager signals the waiting thread and returns the status.
 *
 * Driver model:
 *   - DriverEntry(DRIVER_OBJECT*, UNICODE_STRING*) initializes the driver.
 *   - Driver sets MajorFunction[IRP_MJ_*] dispatch pointers.
 *   - IoCreateDevice() creates a DEVICE_OBJECT linked to the driver.
 *   - IoCreateSymbolicLink() creates \DosDevices\X: → \Device\Foo links.
 *
 * Phase 3 scope:
 *   - In-kernel drivers only (no ring-3 driver loading).
 *   - Synchronous IRP completion only (no pending / completion ports).
 *   - FILE_OBJECT opened to \Device\Null, \Device\Zero, \Device\Random.
 *   - NtCreateFile, NtReadFile, NtWriteFile, NtClose(file).
 *   - IoCreateDevice, IoCreateSymbolicLink, IoCallDriver, IoCompleteRequest.
 */

#pragma once

#include "../include/types.h"
#include "../ob/ob.h"

/* POINTER_ALIGNMENT is a Windows SDK annotation macro — not needed in our kernel */
#ifndef POINTER_ALIGNMENT
#define POINTER_ALIGNMENT
#endif

/* -----------------------------------------------------------------------
 * IRP Major function codes (subset from wdm.h)
 * ----------------------------------------------------------------------- */
#define IRP_MJ_CREATE                    0x00
#define IRP_MJ_CREATE_NAMED_PIPE         0x01
#define IRP_MJ_CLOSE                     0x02
#define IRP_MJ_READ                      0x03
#define IRP_MJ_WRITE                     0x04
#define IRP_MJ_QUERY_INFORMATION         0x05
#define IRP_MJ_SET_INFORMATION           0x06
#define IRP_MJ_QUERY_EA                  0x07
#define IRP_MJ_SET_EA                    0x08
#define IRP_MJ_FLUSH_BUFFERS             0x09
#define IRP_MJ_QUERY_VOLUME_INFORMATION  0x0A
#define IRP_MJ_SET_VOLUME_INFORMATION    0x0B
#define IRP_MJ_DIRECTORY_CONTROL         0x0C
#define IRP_MJ_FILE_SYSTEM_CONTROL       0x0D
#define IRP_MJ_DEVICE_CONTROL            0x0E
#define IRP_MJ_INTERNAL_DEVICE_CONTROL   0x0F
#define IRP_MJ_SHUTDOWN                  0x10
#define IRP_MJ_LOCK_CONTROL              0x11
#define IRP_MJ_CLEANUP                   0x12
#define IRP_MJ_CREATE_MAILSLOT           0x13
#define IRP_MJ_QUERY_SECURITY            0x14
#define IRP_MJ_SET_SECURITY              0x15
#define IRP_MJ_POWER                     0x16
#define IRP_MJ_SYSTEM_CONTROL            0x17
#define IRP_MJ_DEVICE_CHANGE             0x18
#define IRP_MJ_QUERY_QUOTA               0x19
#define IRP_MJ_SET_QUOTA                 0x1A
#define IRP_MJ_PNP                       0x1B
#define IRP_MJ_MAXIMUM_FUNCTION          0x1B

/* -----------------------------------------------------------------------
 * File access/share/create flags (NtCreateFile)
 * ----------------------------------------------------------------------- */

/* DesiredAccess */
#define FILE_READ_DATA          0x0001
#define FILE_WRITE_DATA         0x0002
#define FILE_APPEND_DATA        0x0004
#define FILE_READ_EA            0x0008
#define FILE_WRITE_EA           0x0010
#define FILE_EXECUTE            0x0020
#define FILE_READ_ATTRIBUTES    0x0080
#define FILE_WRITE_ATTRIBUTES   0x0100
#define FILE_ALL_ACCESS         (STANDARD_RIGHTS_REQUIRED | 0x1FF)
#define FILE_GENERIC_READ       (FILE_READ_DATA | FILE_READ_ATTRIBUTES | \
                                  FILE_READ_EA | SYNCHRONIZE)
#define FILE_GENERIC_WRITE      (FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES | \
                                  FILE_WRITE_EA | FILE_APPEND_DATA | SYNCHRONIZE)
#define FILE_GENERIC_EXECUTE    (FILE_EXECUTE | FILE_READ_ATTRIBUTES | SYNCHRONIZE)

/* ShareAccess */
#define FILE_SHARE_READ    0x0001
#define FILE_SHARE_WRITE   0x0002
#define FILE_SHARE_DELETE  0x0004

/* CreateDisposition */
#define FILE_SUPERSEDE     0x00000000
#define FILE_OPEN          0x00000001
#define FILE_CREATE        0x00000002
#define FILE_OPEN_IF       0x00000003
#define FILE_OVERWRITE     0x00000004
#define FILE_OVERWRITE_IF  0x00000005

/* CreateOptions */
#define FILE_DIRECTORY_FILE            0x00000001
#define FILE_WRITE_THROUGH             0x00000002
#define FILE_SEQUENTIAL_ONLY           0x00000004
#define FILE_NO_INTERMEDIATE_BUFFERING 0x00000008
#define FILE_SYNCHRONOUS_IO_ALERT      0x00000010
#define FILE_SYNCHRONOUS_IO_NONALERT   0x00000020
#define FILE_NON_DIRECTORY_FILE        0x00000040
#define FILE_RANDOM_ACCESS             0x00000800
#define FILE_DELETE_ON_CLOSE           0x00001000

/* IoStatus Information for NtCreateFile */
#define FILE_SUPERSEDED   0
#define FILE_OPENED       1
#define FILE_CREATED      2
#define FILE_OVERWRITTEN  3
#define FILE_EXISTS       4
#define FILE_DOES_NOT_EXIST 5

/* File information class */
typedef enum _FILE_INFORMATION_CLASS {
    FileDirectoryInformation       = 1,
    FileBasicInformation           = 4,
    FileStandardInformation        = 5,
    FileNameInformation            = 9,
    FilePositionInformation        = 14,
    FileEndOfFileInformation       = 20,
} FILE_INFORMATION_CLASS;

typedef struct _FILE_STANDARD_INFORMATION {
    UINT64   AllocationSize;
    UINT64   EndOfFile;
    UINT32   NumberOfLinks;
    bool     DeletePending;
    bool     Directory;
} FILE_STANDARD_INFORMATION;

typedef struct _FILE_POSITION_INFORMATION {
    UINT64   CurrentByteOffset;
} FILE_POSITION_INFORMATION;

typedef struct _FILE_BASIC_INFORMATION {
    UINT64   CreationTime;
    UINT64   LastAccessTime;
    UINT64   LastWriteTime;
    UINT64   ChangeTime;
    UINT32   FileAttributes;
} FILE_BASIC_INFORMATION;

/* -----------------------------------------------------------------------
 * IO_STATUS_BLOCK
 * ----------------------------------------------------------------------- */
typedef struct _IO_STATUS_BLOCK {
    union {
        NTSTATUS Status;
        void    *Pointer;
    };
    ULONG_PTR Information;   /* bytes transferred / creation disposition */
} IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;

/* -----------------------------------------------------------------------
 * Forward declarations
 * ----------------------------------------------------------------------- */
typedef struct _IRP             IRP,            *PIRP;
typedef struct _IO_STACK_LOCATION IO_STACK_LOCATION, *PIO_STACK_LOCATION;
typedef struct _DRIVER_OBJECT   DRIVER_OBJECT,  *PDRIVER_OBJECT;
typedef struct _DEVICE_OBJECT   DEVICE_OBJECT,  *PDEVICE_OBJECT;
typedef struct _FILE_OBJECT     FILE_OBJECT,    *PFILE_OBJECT;

/* -----------------------------------------------------------------------
 * Driver dispatch routine prototype
 * ----------------------------------------------------------------------- */
typedef NTSTATUS (*PDRIVER_DISPATCH)(PDEVICE_OBJECT DeviceObject, PIRP Irp);

typedef NTSTATUS (*PDRIVER_ADD_DEVICE)(PDRIVER_OBJECT, PDEVICE_OBJECT);
typedef void     (*PDRIVER_UNLOAD)(PDRIVER_OBJECT);

/* -----------------------------------------------------------------------
 * IO_STACK_LOCATION — per-IRP per-driver stack entry
 *
 * One of these is allocated per driver in the IRP's device stack.
 * Phase 3: single-level stack (one device, one driver).
 * ----------------------------------------------------------------------- */
struct _IO_STACK_LOCATION {
    UINT8  MajorFunction;
    UINT8  MinorFunction;
    UINT8  Flags;
    UINT8  Control;

    /* Parameters union (subset — add as needed) */
    union {
        /* IRP_MJ_CREATE */
        struct {
            UINT32          SecurityContext;
            UINT32          Options;       /* Hi 8 bits = CreateDisposition */
            USHORT          FileAttributes;
            USHORT          ShareAccess;
            UINT32          EaLength;
            PFILE_OBJECT    FileObject;    /* Target FILE_OBJECT */
            UNICODE_STRING *FileName;      /* Path relative to device (ASCII buf) */
        } Create;

        /* IRP_MJ_READ */
        struct {
            UINT32       Length;
            UINT32       POINTER_ALIGNMENT Key;
            UINT64       ByteOffset;
            PFILE_OBJECT FileObject;
        } Read;

        /* IRP_MJ_WRITE */
        struct {
            UINT32       Length;
            UINT32       POINTER_ALIGNMENT Key;
            UINT64       ByteOffset;
            PFILE_OBJECT FileObject;
        } Write;

        /* IRP_MJ_QUERY_INFORMATION / IRP_MJ_SET_INFORMATION */
        struct {
            UINT32                  Length;
            FILE_INFORMATION_CLASS  FileInformationClass;
            void                   *Buffer;     /* Output buffer */
            PFILE_OBJECT            FileObject;
        } QueryFile;

        /* IRP_MJ_DEVICE_CONTROL */
        struct {
            UINT32  OutputBufferLength;
            UINT32  POINTER_ALIGNMENT InputBufferLength;
            UINT32  POINTER_ALIGNMENT IoControlCode;
            void   *Type3InputBuffer;
        } DeviceIoControl;
    } Parameters;

    PDEVICE_OBJECT DeviceObject;
    PFILE_OBJECT   FileObject;
    void          *CompletionRoutine;  /* NULL in Phase 3 */
    void          *Context;
};

/* -----------------------------------------------------------------------
 * IRP — I/O Request Packet
 *
 * Phase 3 simplification: one stack location, synchronous only.
 * ----------------------------------------------------------------------- */
struct _IRP {
    UINT16  Type;           /* IRP_TYPE_IRP = 6 */
    UINT16  Size;
    UINT32  Flags;

    /* Transfer buffer — for METHOD_BUFFERED: SystemBuffer */
    void   *SystemBuffer;   /* kernel copy of user buffer */
    UINT32  SystemBufferLen;

    /* User-visible I/O status (updated by IoCompleteRequest) */
    IO_STATUS_BLOCK  IoStatus;

    /* Request originator info */
    UINT8   RequestorMode;  /* 0 = KernelMode, 1 = UserMode */

    /* Stack location (Phase 3: one slot) */
    IO_STACK_LOCATION StackLocation;

    /* Linkage for IRP queue */
    struct _IRP *Next;
};

#define IRP_TYPE_IRP    6

/* IRP flags */
#define IRP_SYNCHRONOUS_API         0x00000004
#define IRP_NOCACHE                 0x00000008
#define IRP_PAGING_IO               0x00000010
#define IRP_MOUNT_COMPLETION        0x00000020
#define IRP_SYNCHRONOUS_PAGING_IO   0x00000040

/* -----------------------------------------------------------------------
 * DEVICE_OBJECT
 * ----------------------------------------------------------------------- */
#define FILE_DEVICE_NULL        0x00000015
#define FILE_DEVICE_UNKNOWN     0x00000022
#define FILE_DEVICE_DISK        0x00000007
#define FILE_DEVICE_DISK_FILE_SYSTEM 0x00000008
#define FILE_DEVICE_CD_ROM      0x00000002

#define DO_BUFFERED_IO          0x00000004
#define DO_DIRECT_IO            0x00000010
#define DO_DEVICE_INITIALIZING  0x00000080

struct _DEVICE_OBJECT {
    UINT16          Type;           /* 3 = DEVICE_OBJECT */
    UINT16          Size;
    LONG            ReferenceCount;
    PDRIVER_OBJECT  DriverObject;
    PDEVICE_OBJECT  NextDevice;     /* Next device on the same driver */
    PDEVICE_OBJECT  AttachedDevice;
    PIRP            CurrentIrp;
    UINT32          Flags;          /* DO_* */
    UINT32          Characteristics;
    UINT32          DeviceType;     /* FILE_DEVICE_* */
    UINT32          AlignmentRequirement;
    UNICODE_STRING  DeviceName;     /* \Device\Foo */
    void           *DeviceExtension;
    ULONG_PTR       ReservedForExtension;
};

/* -----------------------------------------------------------------------
 * DRIVER_OBJECT
 * ----------------------------------------------------------------------- */
struct _DRIVER_OBJECT {
    UINT16          Type;           /* 4 = DRIVER_OBJECT */
    UINT16          Size;
    PDEVICE_OBJECT  DeviceObject;   /* Linked list of devices */
    UINT32          Flags;
    UNICODE_STRING  DriverName;     /* \Driver\Foo */
    UNICODE_STRING  HardwareDatabase;
    PDRIVER_ADD_DEVICE AddDevice;
    PDRIVER_UNLOAD  DriverUnload;
    PDRIVER_DISPATCH MajorFunction[IRP_MJ_MAXIMUM_FUNCTION + 1];
};

/* -----------------------------------------------------------------------
 * FILE_OBJECT
 * ----------------------------------------------------------------------- */
struct _FILE_OBJECT {
    UINT16          Type;           /* 5 = FILE_OBJECT */
    UINT16          Size;
    PDEVICE_OBJECT  DeviceObject;
    PVOID           FsContext;      /* File system / driver private data */
    PVOID           FsContext2;
    UNICODE_STRING  FileName;       /* Name relative to device (e.g. \foo.txt) */
    UINT64          CurrentByteOffset;
    UINT32          Flags;
    UINT32          ReadAccess       : 1;
    UINT32          WriteAccess      : 1;
    UINT32          DeleteAccess     : 1;
    UINT32          SharedRead       : 1;
    UINT32          SharedWrite      : 1;
    UINT32          SharedDelete     : 1;
    UINT32          SynchronousIo    : 1;
    UINT32          Waiters;
};

/* FILE_OBJECT type constant */
#define FILE_OBJECT_TYPE    5

/* -----------------------------------------------------------------------
 * IopInvalidDeviceRequest — default dispatch for unimplemented majors
 * ----------------------------------------------------------------------- */
NTSTATUS IopInvalidDeviceRequest(PDEVICE_OBJECT DeviceObject, PIRP Irp);

/* -----------------------------------------------------------------------
 * Public I/O Manager API
 * ----------------------------------------------------------------------- */

/* Initialize the I/O Manager and register built-in devices */
void IoInitialize(void);

/*
 * Create a device object attached to DriverObject.
 * @DeviceName:      Optional name in the namespace (e.g. L"\\Device\\Null")
 * @DeviceType:      FILE_DEVICE_*
 * @DeviceCharacteristics: FILE_* characteristic flags
 * @Exclusive:       Only one handle at a time
 * @DeviceObject:    Receives the new device object
 */
NTSTATUS IoCreateDevice(
    PDRIVER_OBJECT  DriverObject,
    UINT32          DeviceExtensionSize,
    UNICODE_STRING *DeviceName,
    UINT32          DeviceType,
    UINT32          DeviceCharacteristics,
    bool            Exclusive,
    PDEVICE_OBJECT *DeviceObject);

/*
 * Create a symbolic link from SymbolicLinkName → DeviceName.
 * e.g. \DosDevices\NUL → \Device\Null
 */
NTSTATUS IoCreateSymbolicLink(
    UNICODE_STRING *SymbolicLinkName,
    UNICODE_STRING *DeviceName);

/*
 * Delete a device object.
 */
void IoDeleteDevice(PDEVICE_OBJECT DeviceObject);

/*
 * Allocate an IRP with a single stack location.
 * Caller must call IoFreeIrp() when done.
 */
PIRP IoAllocateIrp(UINT8 StackSize);

/*
 * Free an IRP allocated by IoAllocateIrp.
 */
void IoFreeIrp(PIRP Irp);

/*
 * Get the current stack location of an IRP.
 */
static inline PIO_STACK_LOCATION IoGetCurrentIrpStackLocation(PIRP Irp)
{
    return &Irp->StackLocation;
}

/*
 * Complete an IRP (sets IoStatus, wakes up waiter).
 * Phase 3: synchronous only — just sets the status.
 */
void IoCompleteRequest(PIRP Irp, INT8 PriorityBoost);

#define IO_NO_INCREMENT  0
#define IO_READ_INCREMENT  1
#define IO_WRITE_INCREMENT 1

/*
 * Call a driver's dispatch routine for the given IRP.
 */
NTSTATUS IoCallDriver(PDEVICE_OBJECT DeviceObject, PIRP Irp);

/*
 * Open a file/device by name (kernel-internal implementation of
 * NtCreateFile).  Returns a FILE_OBJECT pointer (caller holds a reference).
 *
 * For Phase 3: only device paths (\Device\Null, \Device\Zero) are resolved.
 */
NTSTATUS IoCreateFile(
    HANDLE            *FileHandle,
    ACCESS_MASK        DesiredAccess,
    POBJECT_ATTRIBUTES ObjectAttributes,
    PIO_STATUS_BLOCK   IoStatusBlock,
    UINT64            *AllocationSize,
    UINT32             FileAttributes,
    UINT32             ShareAccess,
    UINT32             CreateDisposition,
    UINT32             CreateOptions,
    void              *EaBuffer,
    UINT32             EaLength);

/*
 * Perform a synchronous read on a file/device.
 */
NTSTATUS IoReadFile(
    HANDLE           FileHandle,
    HANDLE           Event,
    void            *ApcRoutine,
    void            *ApcContext,
    PIO_STATUS_BLOCK IoStatusBlock,
    void            *Buffer,
    UINT32           Length,
    UINT64          *ByteOffset,
    UINT32          *Key);

/*
 * Perform a synchronous write on a file/device.
 */
NTSTATUS IoWriteFile(
    HANDLE           FileHandle,
    HANDLE           Event,
    void            *ApcRoutine,
    void            *ApcContext,
    PIO_STATUS_BLOCK IoStatusBlock,
    void            *Buffer,
    UINT32           Length,
    UINT64          *ByteOffset,
    UINT32          *Key);

/*
 * Query file information.
 */
NTSTATUS IoQueryInformationFile(
    HANDLE                 FileHandle,
    PIO_STATUS_BLOCK       IoStatusBlock,
    void                  *FileInformation,
    UINT32                 Length,
    FILE_INFORMATION_CLASS FileInformationClass);

/*
 * Lookup a device by name (returns DEVICE_OBJECT*, caller holds reference).
 */
PDEVICE_OBJECT IoGetDeviceObjectByName(UNICODE_STRING *DeviceName);

/*
 * Global I/O manager state
 */
extern POBJECT_TYPE ObpFileType;
