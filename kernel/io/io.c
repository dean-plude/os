/*
 * io.c — NT I/O Manager implementation (Phase 3)
 *
 * Built-in devices registered at IoInitialize():
 *
 *   \Device\Null   — reads return 0 bytes, writes succeed silently
 *   \Device\Zero   — reads fill buffer with zeros, writes succeed silently
 *   \Device\Kmsg   — reads return kernel log, writes post to kprintf
 *
 * IRP dispatch:
 *   IoCallDriver() → DeviceObject->DriverObject->MajorFunction[major](dev, irp)
 *   Handler calls IoCompleteRequest() to finish the IRP.
 *
 * FILE_OBJECT lifecycle:
 *   NtCreateFile → IoCreateFile → find DEVICE_OBJECT → alloc FILE_OBJECT →
 *     dispatch IRP_MJ_CREATE → return HANDLE to FILE_OBJECT
 *   NtReadFile → FILE_OBJECT → DEVICE_OBJECT → IRP_MJ_READ
 *   NtClose → IRP_MJ_CLEANUP + IRP_MJ_CLOSE → free FILE_OBJECT
 */

#include "io.h"
#include "../ob/ob.h"
#include "../mm/vmm.h"
#include "../ke/printf.h"
#include "../lib/string.h"
#include "../include/types.h"
#include "../arch/x86_64/cpu.h"

/* -----------------------------------------------------------------------
 * FILE_OBJECT type (registered with Object Manager)
 * ----------------------------------------------------------------------- */
POBJECT_TYPE ObpFileType;

static void file_object_delete(void *obj)
{
    PFILE_OBJECT fo = (PFILE_OBJECT)obj;
    /* Send IRP_MJ_CLOSE to the device */
    if (fo->DeviceObject) {
        PIRP irp = IoAllocateIrp(1);
        if (irp) {
            PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(irp);
            sl->MajorFunction = IRP_MJ_CLOSE;
            sl->FileObject    = fo;
            IoCallDriver(fo->DeviceObject, irp);
            IoFreeIrp(irp);
        }
    }
    /* Free the FileName buffer if it was allocated */
    if (fo->FileName.Buffer) kfree(fo->FileName.Buffer);
}

static OBJECT_TYPE io_file_type_storage = {
    .Name            = "File",
    .DefaultBodySize = sizeof(FILE_OBJECT),
    .GenericAll      = FILE_ALL_ACCESS,
    .Operations      = { .Delete = file_object_delete },
};

/* -----------------------------------------------------------------------
 * Device registry — simple linked list of all created devices
 * ----------------------------------------------------------------------- */
#define IO_MAX_DEVICES 64
static PDEVICE_OBJECT io_device_list;          /* singly-linked via NextDevice */
static volatile UINT32 io_lock_next, io_lock_owner;

static void io_lock(void)
{
    UINT32 t = __atomic_fetch_add(&io_lock_next, 1, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&io_lock_owner, __ATOMIC_ACQUIRE) != t)
        __asm__ volatile("pause");
}
static void io_unlock(void)
{
    __atomic_fetch_add(&io_lock_owner, 1, __ATOMIC_RELEASE);
}

/* -----------------------------------------------------------------------
 * IopInvalidDeviceRequest — default dispatch stub
 * ----------------------------------------------------------------------- */
NTSTATUS IopInvalidDeviceRequest(PDEVICE_OBJECT dev, PIRP irp)
{
    (void)dev;
    irp->IoStatus.Status      = STATUS_INVALID_DEVICE_REQUEST;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_INVALID_DEVICE_REQUEST;
}

/* -----------------------------------------------------------------------
 * IoAllocateIrp / IoFreeIrp
 * ----------------------------------------------------------------------- */
PIRP IoAllocateIrp(UINT8 StackSize)
{
    (void)StackSize;  /* Phase 3: always one stack location */
    PIRP irp = kzalloc(sizeof(IRP));
    if (!irp) return NULL;
    irp->Type = IRP_TYPE_IRP;
    irp->Size = (UINT16)sizeof(IRP);
    return irp;
}

void IoFreeIrp(PIRP Irp)
{
    if (Irp) kfree(Irp);
}

/* -----------------------------------------------------------------------
 * IoCompleteRequest
 * Phase 3: synchronous only — status is already set by the driver.
 * ----------------------------------------------------------------------- */
void IoCompleteRequest(PIRP Irp, INT8 PriorityBoost)
{
    (void)PriorityBoost;
    /* Nothing more to do in Phase 3 (no async completion ports) */
    (void)Irp;
}

/* -----------------------------------------------------------------------
 * IoCallDriver
 * ----------------------------------------------------------------------- */
NTSTATUS IoCallDriver(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(Irp);
    sl->DeviceObject = DeviceObject;

    UINT8 major = sl->MajorFunction;
    if (major > IRP_MJ_MAXIMUM_FUNCTION)
        return IopInvalidDeviceRequest(DeviceObject, Irp);

    PDRIVER_DISPATCH dispatch =
        DeviceObject->DriverObject->MajorFunction[major];
    if (!dispatch)
        return IopInvalidDeviceRequest(DeviceObject, Irp);

    return dispatch(DeviceObject, Irp);
}

/* -----------------------------------------------------------------------
 * IoCreateDevice
 * ----------------------------------------------------------------------- */
NTSTATUS IoCreateDevice(
    PDRIVER_OBJECT  DriverObject,
    UINT32          DeviceExtensionSize,
    UNICODE_STRING *DeviceName,
    UINT32          DeviceType,
    UINT32          DeviceCharacteristics,
    bool            Exclusive,
    PDEVICE_OBJECT *DeviceObject)
{
    (void)Exclusive; (void)DeviceCharacteristics;

    UINT32 total_size = (UINT32)(sizeof(DEVICE_OBJECT) + DeviceExtensionSize);
    PDEVICE_OBJECT dev = kzalloc(total_size);
    if (!dev) return STATUS_NO_MEMORY;

    dev->Type         = 3;   /* DEVICE_OBJECT */
    dev->Size         = (UINT16)total_size;
    dev->DriverObject = DriverObject;
    dev->DeviceType   = DeviceType;
    dev->Flags        = DO_DEVICE_INITIALIZING;
    dev->DeviceExtension = DeviceExtensionSize ?
        (void *)((uintptr_t)dev + sizeof(DEVICE_OBJECT)) : NULL;

    /* Copy device name */
    if (DeviceName && DeviceName->Length) {
        UINT32 bufbytes = DeviceName->Length + sizeof(WCHAR);
        dev->DeviceName.Buffer = kmalloc(bufbytes);
        if (!dev->DeviceName.Buffer) { kfree(dev); return STATUS_NO_MEMORY; }
        __builtin_memcpy(dev->DeviceName.Buffer, DeviceName->Buffer, DeviceName->Length);
        dev->DeviceName.Buffer[DeviceName->Length / sizeof(WCHAR)] = 0;
        dev->DeviceName.Length        = DeviceName->Length;
        dev->DeviceName.MaximumLength = (USHORT)bufbytes;
    }

    /* Link onto driver's device list */
    dev->NextDevice         = DriverObject->DeviceObject;
    DriverObject->DeviceObject = dev;

    /* Register in global device list */
    io_lock();
    dev->AttachedDevice = io_device_list;
    io_device_list      = dev;
    io_unlock();

    if (DeviceObject) *DeviceObject = dev;

    kprintf("[IO] Created device '%.*S' type=0x%x\n",
            (int)(dev->DeviceName.Length / 2),
            dev->DeviceName.Buffer,
            DeviceType);
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * IoDeleteDevice
 * ----------------------------------------------------------------------- */
void IoDeleteDevice(PDEVICE_OBJECT DeviceObject)
{
    io_lock();
    /* Remove from global list */
    PDEVICE_OBJECT *pp = &io_device_list;
    while (*pp) {
        if (*pp == DeviceObject) { *pp = DeviceObject->AttachedDevice; break; }
        pp = &(*pp)->AttachedDevice;
    }
    io_unlock();

    if (DeviceObject->DeviceName.Buffer)
        kfree(DeviceObject->DeviceName.Buffer);
    kfree(DeviceObject);
}

/* -----------------------------------------------------------------------
 * IoCreateSymbolicLink
 * Phase 3 stub: logs the link but doesn't create an OB symlink object.
 * Phase 4 will create a real \DosDevices\ entry.
 * ----------------------------------------------------------------------- */
NTSTATUS IoCreateSymbolicLink(
    UNICODE_STRING *SymbolicLinkName,
    UNICODE_STRING *DeviceName)
{
    kprintf("[IO] Symlink '%.*S' -> '%.*S'\n",
            (int)(SymbolicLinkName->Length / 2), SymbolicLinkName->Buffer,
            (int)(DeviceName->Length / 2),       DeviceName->Buffer);
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * IoGetDeviceObjectByName
 * ----------------------------------------------------------------------- */
PDEVICE_OBJECT IoGetDeviceObjectByName(UNICODE_STRING *DeviceName)
{
    io_lock();
    PDEVICE_OBJECT dev = io_device_list;
    while (dev) {
        if (dev->DeviceName.Length == DeviceName->Length) {
            /* Case-insensitive compare */
            bool match = true;
            UINT32 nch = DeviceName->Length / sizeof(WCHAR);
            for (UINT32 i = 0; i < nch; i++) {
                WCHAR a = dev->DeviceName.Buffer[i];
                WCHAR b = DeviceName->Buffer[i];
                if (a >= 'a' && a <= 'z') a -= 32;
                if (b >= 'a' && b <= 'z') b -= 32;
                if (a != b) { match = false; break; }
            }
            if (match) {
                __atomic_fetch_add(&dev->ReferenceCount, 1, __ATOMIC_SEQ_CST);
                io_unlock();
                return dev;
            }
        }
        dev = dev->AttachedDevice;
    }
    io_unlock();
    return NULL;
}

/* -----------------------------------------------------------------------
 * Built-in Null device
 * ----------------------------------------------------------------------- */
static DRIVER_OBJECT io_null_driver;
static DEVICE_OBJECT io_null_device;

static NTSTATUS null_dispatch_create(PDEVICE_OBJECT dev, PIRP irp)
{
    (void)dev;
    irp->IoStatus.Status      = STATUS_SUCCESS;
    irp->IoStatus.Information = FILE_OPENED;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS null_dispatch_close(PDEVICE_OBJECT dev, PIRP irp)
{
    (void)dev;
    irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS null_dispatch_read(PDEVICE_OBJECT dev, PIRP irp)
{
    (void)dev;
    irp->IoStatus.Status      = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;   /* 0 bytes read = EOF */
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS null_dispatch_write(PDEVICE_OBJECT dev, PIRP irp)
{
    (void)dev;
    PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(irp);
    irp->IoStatus.Status      = STATUS_SUCCESS;
    irp->IoStatus.Information = sl->Parameters.Write.Length;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * Built-in Zero device
 * ----------------------------------------------------------------------- */
static DRIVER_OBJECT io_zero_driver;
static DEVICE_OBJECT io_zero_device;

static NTSTATUS zero_dispatch_read(PDEVICE_OBJECT dev, PIRP irp)
{
    (void)dev;
    PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(irp);
    if (irp->SystemBuffer && sl->Parameters.Read.Length)
        __builtin_memset(irp->SystemBuffer, 0, sl->Parameters.Read.Length);
    irp->IoStatus.Status      = STATUS_SUCCESS;
    irp->IoStatus.Information = sl->Parameters.Read.Length;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * Built-in Kmsg device (kernel log read-back)
 * Phase 3 stub: writes go to kprintf, reads return STATUS_END_OF_FILE.
 * ----------------------------------------------------------------------- */
static DRIVER_OBJECT io_kmsg_driver;

static NTSTATUS kmsg_dispatch_write(PDEVICE_OBJECT dev, PIRP irp)
{
    (void)dev;
    PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(irp);
    UINT32 len = sl->Parameters.Write.Length;
    if (irp->SystemBuffer && len) {
        /* Print as ASCII (truncate at 512 bytes) */
        char buf[513];
        UINT32 copy = len < 512 ? len : 512;
        __builtin_memcpy(buf, irp->SystemBuffer, copy);
        buf[copy] = '\0';
        kprintf("[KMSG] %s", buf);
    }
    irp->IoStatus.Status      = STATUS_SUCCESS;
    irp->IoStatus.Information = len;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * Helper: initialize a static DRIVER_OBJECT with all dispatch slots set to
 * IopInvalidDeviceRequest.
 * ----------------------------------------------------------------------- */
static void io_init_driver(DRIVER_OBJECT *drv, const char *name)
{
    __builtin_memset(drv, 0, sizeof(*drv));
    drv->Type = 4;
    drv->Size = sizeof(DRIVER_OBJECT);
    for (int i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
        drv->MajorFunction[i] = IopInvalidDeviceRequest;
    /* Store driver name as ASCII in DriverName.Buffer (static) */
    (void)name;
}

/* Helper: wire a device object statically (no heap needed for built-ins) */
static void io_wire_device(DEVICE_OBJECT *dev, DRIVER_OBJECT *drv,
                            UINT32 type, UINT32 flags)
{
    __builtin_memset(dev, 0, sizeof(*dev));
    dev->Type         = 3;
    dev->Size         = sizeof(DEVICE_OBJECT);
    dev->DriverObject = drv;
    dev->DeviceType   = type;
    dev->Flags        = flags;
    dev->NextDevice   = drv->DeviceObject;
    drv->DeviceObject = dev;
    dev->AttachedDevice = io_device_list;
    io_device_list      = dev;
}

/* Helper: set the DeviceName from a static WCHAR literal */
static void io_set_device_name(DEVICE_OBJECT *dev,
                                 WCHAR *buf, UINT32 chars)
{
    dev->DeviceName.Buffer        = buf;
    dev->DeviceName.Length        = (USHORT)(chars * sizeof(WCHAR));
    dev->DeviceName.MaximumLength = (USHORT)((chars + 1) * sizeof(WCHAR));
}

/* Static name buffers for built-in devices */
static WCHAR io_null_name[] = L"\\Device\\Null";
static WCHAR io_zero_name[] = L"\\Device\\Zero";
static WCHAR io_kmsg_name[] = L"\\Device\\Kmsg";
static DEVICE_OBJECT io_kmsg_device;

/* -----------------------------------------------------------------------
 * IoInitialize
 * ----------------------------------------------------------------------- */
void IoInitialize(void)
{
    /* Register FILE object type */
    ObpFileType = &io_file_type_storage;
    ObCreateObjectType(ObpFileType);

    /* ---- Null device ---- */
    io_init_driver(&io_null_driver, "Null");
    io_null_driver.MajorFunction[IRP_MJ_CREATE]  = null_dispatch_create;
    io_null_driver.MajorFunction[IRP_MJ_CLOSE]   = null_dispatch_close;
    io_null_driver.MajorFunction[IRP_MJ_CLEANUP] = null_dispatch_close;
    io_null_driver.MajorFunction[IRP_MJ_READ]    = null_dispatch_read;
    io_null_driver.MajorFunction[IRP_MJ_WRITE]   = null_dispatch_write;

    io_wire_device(&io_null_device, &io_null_driver,
                    FILE_DEVICE_NULL, DO_BUFFERED_IO);
    io_set_device_name(&io_null_device, io_null_name, 12);

    /* ---- Zero device ---- */
    io_init_driver(&io_zero_driver, "Zero");
    io_zero_driver.MajorFunction[IRP_MJ_CREATE]  = null_dispatch_create;
    io_zero_driver.MajorFunction[IRP_MJ_CLOSE]   = null_dispatch_close;
    io_zero_driver.MajorFunction[IRP_MJ_CLEANUP] = null_dispatch_close;
    io_zero_driver.MajorFunction[IRP_MJ_READ]    = zero_dispatch_read;
    io_zero_driver.MajorFunction[IRP_MJ_WRITE]   = null_dispatch_write;

    io_wire_device(&io_zero_device, &io_zero_driver,
                    FILE_DEVICE_NULL, DO_BUFFERED_IO);
    io_set_device_name(&io_zero_device, io_zero_name, 12);

    /* ---- Kmsg device ---- */
    io_init_driver(&io_kmsg_driver, "Kmsg");
    io_kmsg_driver.MajorFunction[IRP_MJ_CREATE]  = null_dispatch_create;
    io_kmsg_driver.MajorFunction[IRP_MJ_CLOSE]   = null_dispatch_close;
    io_kmsg_driver.MajorFunction[IRP_MJ_CLEANUP] = null_dispatch_close;
    io_kmsg_driver.MajorFunction[IRP_MJ_READ]    = null_dispatch_read;
    io_kmsg_driver.MajorFunction[IRP_MJ_WRITE]   = kmsg_dispatch_write;

    io_wire_device(&io_kmsg_device, &io_kmsg_driver,
                    FILE_DEVICE_UNKNOWN, DO_BUFFERED_IO);
    io_set_device_name(&io_kmsg_device, io_kmsg_name, 12);

    kprintf("[IO] I/O Manager initialized\n");
    kprintf("[IO] Devices: \\Device\\Null  \\Device\\Zero  \\Device\\Kmsg\n");
}

/* -----------------------------------------------------------------------
 * IoCreateFile — NT-internal CreateFile implementation
 * ----------------------------------------------------------------------- */
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
    UINT32             EaLength)
{
    (void)AllocationSize; (void)FileAttributes; (void)ShareAccess;
    (void)CreateOptions; (void)EaBuffer; (void)EaLength;

    if (!ObjectAttributes || !ObjectAttributes->ObjectName)
        return STATUS_INVALID_PARAMETER;

    /* Look up device by name */
    PDEVICE_OBJECT dev = IoGetDeviceObjectByName(ObjectAttributes->ObjectName);
    if (!dev) return STATUS_OBJECT_NAME_NOT_FOUND;

    /* Allocate a FILE_OBJECT via the Object Manager */
    OBJECT_ATTRIBUTES attr;
    __builtin_memset(&attr, 0, sizeof(attr));
    attr.Length     = sizeof(OBJECT_ATTRIBUTES);
    attr.Attributes = OBJ_KERNEL_HANDLE;

    void *obj;
    NTSTATUS s = ObCreateObject(ObpFileType, &attr, 0, &obj);
    if (!NT_SUCCESS(s)) return s;

    PFILE_OBJECT fo = (PFILE_OBJECT)obj;
    fo->Type         = FILE_OBJECT_TYPE;
    fo->Size         = sizeof(FILE_OBJECT);
    fo->DeviceObject = dev;
    fo->SynchronousIo = 1;
    fo->ReadAccess   = (DesiredAccess & (FILE_READ_DATA | GENERIC_READ)) ? 1 : 0;
    fo->WriteAccess  = (DesiredAccess & (FILE_WRITE_DATA | GENERIC_WRITE)) ? 1 : 0;

    /* Build and dispatch IRP_MJ_CREATE */
    PIRP irp = IoAllocateIrp(1);
    if (!irp) { ObDereferenceObject(obj); return STATUS_NO_MEMORY; }

    PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(irp);
    sl->MajorFunction    = IRP_MJ_CREATE;
    sl->FileObject        = fo;
    sl->Parameters.Create.Options =
        (UINT32)(CreateDisposition << 24) | (CreateOptions & 0x00FFFFFF);

    irp->RequestorMode = 1; /* UserMode */

    s = IoCallDriver(dev, irp);
    ULONG_PTR info = irp->IoStatus.Information;
    IoFreeIrp(irp);

    if (!NT_SUCCESS(s)) { ObDereferenceObject(obj); return s; }

    if (IoStatusBlock) {
        IoStatusBlock->Status      = s;
        IoStatusBlock->Information = info;
    }

    /* Insert into handle table */
    HANDLE h = 0;
    s = ObInsertObject(obj, NULL, DesiredAccess, 0, NULL, &h);
    if (!NT_SUCCESS(s)) { ObDereferenceObject(obj); return s; }

    if (FileHandle) *FileHandle = h;
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * IoReadFile
 * ----------------------------------------------------------------------- */
NTSTATUS IoReadFile(
    HANDLE           FileHandle,
    HANDLE           Event,
    void            *ApcRoutine,
    void            *ApcContext,
    PIO_STATUS_BLOCK IoStatusBlock,
    void            *Buffer,
    UINT32           Length,
    UINT64          *ByteOffset,
    UINT32          *Key)
{
    (void)Event; (void)ApcRoutine; (void)ApcContext; (void)Key;

    void *obj;
    NTSTATUS s = ObReferenceObjectByHandle(FileHandle, FILE_READ_DATA,
                                            ObpFileType, NULL, &obj, NULL);
    if (!NT_SUCCESS(s)) return s;

    PFILE_OBJECT fo = (PFILE_OBJECT)obj;

    PIRP irp = IoAllocateIrp(1);
    if (!irp) { ObDereferenceObject(obj); return STATUS_NO_MEMORY; }

    irp->SystemBuffer    = Buffer;
    irp->SystemBufferLen = Length;

    PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(irp);
    sl->MajorFunction              = IRP_MJ_READ;
    sl->FileObject                 = fo;
    sl->Parameters.Read.Length     = Length;
    sl->Parameters.Read.ByteOffset = ByteOffset ? *ByteOffset
                                                 : fo->CurrentByteOffset;

    s = IoCallDriver(fo->DeviceObject, irp);
    UINT64 bytes = irp->IoStatus.Information;
    IoFreeIrp(irp);

    if (NT_SUCCESS(s)) fo->CurrentByteOffset += bytes;

    if (IoStatusBlock) {
        IoStatusBlock->Status      = s;
        IoStatusBlock->Information = (ULONG_PTR)bytes;
    }

    ObDereferenceObject(obj);
    return s;
}

/* -----------------------------------------------------------------------
 * IoWriteFile
 * ----------------------------------------------------------------------- */
NTSTATUS IoWriteFile(
    HANDLE           FileHandle,
    HANDLE           Event,
    void            *ApcRoutine,
    void            *ApcContext,
    PIO_STATUS_BLOCK IoStatusBlock,
    void            *Buffer,
    UINT32           Length,
    UINT64          *ByteOffset,
    UINT32          *Key)
{
    (void)Event; (void)ApcRoutine; (void)ApcContext; (void)Key;

    void *obj;
    NTSTATUS s = ObReferenceObjectByHandle(FileHandle, FILE_WRITE_DATA,
                                            ObpFileType, NULL, &obj, NULL);
    if (!NT_SUCCESS(s)) return s;

    PFILE_OBJECT fo = (PFILE_OBJECT)obj;

    PIRP irp = IoAllocateIrp(1);
    if (!irp) { ObDereferenceObject(obj); return STATUS_NO_MEMORY; }

    irp->SystemBuffer    = Buffer;
    irp->SystemBufferLen = Length;

    PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(irp);
    sl->MajorFunction               = IRP_MJ_WRITE;
    sl->FileObject                  = fo;
    sl->Parameters.Write.Length     = Length;
    sl->Parameters.Write.ByteOffset = ByteOffset ? *ByteOffset
                                                  : fo->CurrentByteOffset;

    s = IoCallDriver(fo->DeviceObject, irp);
    UINT64 bytes = irp->IoStatus.Information;
    IoFreeIrp(irp);

    if (NT_SUCCESS(s)) fo->CurrentByteOffset += bytes;

    if (IoStatusBlock) {
        IoStatusBlock->Status      = s;
        IoStatusBlock->Information = (ULONG_PTR)bytes;
    }

    ObDereferenceObject(obj);
    return s;
}

/* -----------------------------------------------------------------------
 * IoQueryInformationFile
 * ----------------------------------------------------------------------- */
NTSTATUS IoQueryInformationFile(
    HANDLE                 FileHandle,
    PIO_STATUS_BLOCK       IoStatusBlock,
    void                  *FileInformation,
    UINT32                 Length,
    FILE_INFORMATION_CLASS FileInformationClass)
{
    void *obj;
    NTSTATUS s = ObReferenceObjectByHandle(FileHandle, FILE_READ_ATTRIBUTES,
                                            ObpFileType, NULL, &obj, NULL);
    if (!NT_SUCCESS(s)) return s;

    PFILE_OBJECT fo = (PFILE_OBJECT)obj;
    (void)fo;

    switch (FileInformationClass) {
    case FileStandardInformation:
        if (Length < sizeof(FILE_STANDARD_INFORMATION)) {
            s = STATUS_BUFFER_TOO_SMALL;
        } else {
            FILE_STANDARD_INFORMATION *fi = FileInformation;
            __builtin_memset(fi, 0, sizeof(*fi));
            fi->NumberOfLinks = 1;
            s = STATUS_SUCCESS;
        }
        break;

    case FilePositionInformation:
        if (Length < sizeof(FILE_POSITION_INFORMATION)) {
            s = STATUS_BUFFER_TOO_SMALL;
        } else {
            FILE_POSITION_INFORMATION *fi = FileInformation;
            fi->CurrentByteOffset = fo->CurrentByteOffset;
            s = STATUS_SUCCESS;
        }
        break;

    case FileBasicInformation:
        if (Length < sizeof(FILE_BASIC_INFORMATION)) {
            s = STATUS_BUFFER_TOO_SMALL;
        } else {
            __builtin_memset(FileInformation, 0, sizeof(FILE_BASIC_INFORMATION));
            s = STATUS_SUCCESS;
        }
        break;

    default:
        s = STATUS_INVALID_INFO_CLASS;
        break;
    }

    if (IoStatusBlock) {
        IoStatusBlock->Status      = s;
        IoStatusBlock->Information = 0;
    }

    ObDereferenceObject(obj);
    return s;
}
