/*
 * ndis.h — the NDIS 6 structures network miniports see, in Windows' x64
 * layout (kernel/nt/ndis.c is NovaOS's ndis.sys)
 *
 * A miniport (netkvm.sys, an Intel or Realtek driver) reads and writes
 * these directly: the NET_BUFFER_LIST it sends and indicates, the
 * attributes it hands NdisMSetMiniportAttributes, the characteristics it
 * registers.  Offsets are Microsoft's, from the WDK's public documentation
 * and checked below against the layouts in a released miniport's own
 * debug information (netkvm.pdb, virtio-win 0.1.302).  Only what step 30.9
 * uses is named; the rest is reserved space at the right size.
 */
#pragma once

#include "wdm.h"

typedef int NDIS_STATUS;

#define NDIS_STATUS_SUCCESS              ((NDIS_STATUS)0x00000000)
#define NDIS_STATUS_PENDING              ((NDIS_STATUS)0x00000103)
#define NDIS_STATUS_NOT_ACCEPTED         ((NDIS_STATUS)0x00010003)
#define NDIS_STATUS_FAILURE              ((NDIS_STATUS)0xC0000001)
#define NDIS_STATUS_INVALID_PARAMETER    ((NDIS_STATUS)0xC000000D)
#define NDIS_STATUS_RESOURCES            ((NDIS_STATUS)0xC000009A)
#define NDIS_STATUS_NOT_SUPPORTED        ((NDIS_STATUS)0xC00000BB)
#define NDIS_STATUS_INVALID_LENGTH       ((NDIS_STATUS)0xC0010014)
#define NDIS_STATUS_BUFFER_TOO_SHORT     ((NDIS_STATUS)0xC0010016)
#define NDIS_STATUS_INVALID_OID          ((NDIS_STATUS)0xC0010017)
#define NDIS_STATUS_MEDIA_CONNECT        ((NDIS_STATUS)0x4001000B)
#define NDIS_STATUS_MEDIA_DISCONNECT     ((NDIS_STATUS)0x4001000C)
#define NDIS_STATUS_LINK_STATE           ((NDIS_STATUS)0x40010017)

/* NDIS_OBJECT_HEADER and the object types NovaOS reads or writes */
typedef struct {
    unsigned char  Type, Revision;
    unsigned short Size;
} NDIS_OBJECT_HEADER;

#define NDIS_OBJECT_TYPE_DEFAULT                                  0x80
#define NDIS_OBJECT_TYPE_MINIPORT_INIT_PARAMETERS                 0x81
#define NDIS_OBJECT_TYPE_SG_DMA_DESCRIPTION                       0x83
#define NDIS_OBJECT_TYPE_MINIPORT_INTERRUPT                       0x84
#define NDIS_OBJECT_TYPE_MINIPORT_DRIVER_CHARACTERISTICS          0x8A
#define NDIS_OBJECT_TYPE_OID_REQUEST                              0x96
#define NDIS_OBJECT_TYPE_STATUS_INDICATION                        0x98
#define NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES 0x9E
#define NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES      0x9F
#define NDIS_OBJECT_TYPE_RESTART_GENERAL_ATTRIBUTES               0xA7

/* NET_BUFFER: one frame, described by a chain of MDLs */
typedef struct _NET_BUFFER NET_BUFFER;
struct _NET_BUFFER {
    NET_BUFFER        *Next;
    NT_MDL            *CurrentMdl;
    unsigned int       CurrentMdlOffset;
    unsigned int       DataLength __attribute__((aligned(8)));
    NT_MDL            *MdlChain;
    unsigned int       DataOffset;
    unsigned short     ChecksumBias __attribute__((aligned(8)));
    unsigned short     Reserved;
    void              *NdisPoolHandle;
    void              *NdisReserved[2] __attribute__((aligned(16)));
    void              *ProtocolReserved[6] __attribute__((aligned(16)));
    void              *MiniportReserved[4];
    long long          DataPhysicalAddress;
    void              *ScatterGatherList;   /* (or SharedMemoryInfo) */
} __attribute__((aligned(16)));

typedef struct _NET_BUFFER_LIST_CONTEXT {
    struct _NET_BUFFER_LIST_CONTEXT *Next;
    unsigned short     Size, Offset;
    unsigned char      ContextData[] __attribute__((aligned(16)));
} NET_BUFFER_LIST_CONTEXT;

#define NET_BUFFER_LIST_INFO_COUNT 27       /* MaxNetBufferListInfo on Windows 11 */

/* NET_BUFFER_LIST: a packet's frames and their out-of-band information */
typedef struct _NET_BUFFER_LIST NET_BUFFER_LIST;
struct _NET_BUFFER_LIST {
    NET_BUFFER_LIST   *Next;
    NET_BUFFER        *FirstNetBuffer;
    NET_BUFFER_LIST_CONTEXT *Context;
    NET_BUFFER_LIST   *ParentNetBufferList;
    void              *NdisPoolHandle;
    void              *NdisReserved[2] __attribute__((aligned(16)));
    void              *ProtocolReserved[4] __attribute__((aligned(16)));
    void              *MiniportReserved[2] __attribute__((aligned(16)));
    void              *Scratch;
    void              *SourceHandle;
    unsigned int       NblFlags;
    int                ChildRefCount;
    unsigned int       Flags;
    NDIS_STATUS        Status;
    void              *NetBufferListInfo[NET_BUFFER_LIST_INFO_COUNT];
} __attribute__((aligned(16)));

typedef struct {
    NDIS_OBJECT_HEADER Header;
    unsigned char      ProtocolId, fAllocateNetBuffer;
    unsigned short     ContextSize;
    unsigned int       PoolTag, DataSize;
} NET_BUFFER_LIST_POOL_PARAMETERS;

/* NDIS_MINIPORT_DRIVER_CHARACTERISTICS: the miniport's handlers */
typedef struct {
    NDIS_OBJECT_HEADER Header;
    unsigned char      MajorNdisVersion, MinorNdisVersion, MajorDriverVersion, MinorDriverVersion;
    unsigned int       Flags;
    void              *SetOptionsHandler, *InitializeHandlerEx, *HaltHandlerEx, *UnloadHandler, *PauseHandler,
                      *RestartHandler, *OidRequestHandler, *SendNetBufferListsHandler, *ReturnNetBufferListsHandler,
                      *CancelSendHandler, *CheckForHangHandlerEx, *ResetHandlerEx, *DevicePnPEventNotifyHandler,
                      *ShutdownHandlerEx, *CancelOidRequestHandler, *DirectOidRequestHandler,
                      *CancelDirectOidRequestHandler, *SynchronousOidRequestHandler;
} NDIS_MINIPORT_DRIVER_CHARACTERISTICS;

/* NDIS_MINIPORT_PNP_CHARACTERISTICS (NdisSetOptionalHandlers) */
#define NDIS_OBJECT_TYPE_MINIPORT_PNP_CHARACTERISTICS 0x92
typedef struct {
    NDIS_OBJECT_HEADER Header;
    void              *MiniportAddDeviceHandler, *MiniportRemoveDeviceHandler,
                      *MiniportFilterResourceRequirementsHandler, *MiniportStartDeviceHandler;
    unsigned int       Flags;
} NDIS_MINIPORT_PNP_CHARACTERISTICS;

typedef struct {
    NDIS_OBJECT_HEADER Header;
    unsigned int       Flags;
    NT_CM_PARTIAL_RESOURCE_LIST *AllocatedResources;
    void              *IMDeviceInstanceContext, *MiniportAddDeviceContext;
    unsigned int       IfIndex;
    unsigned long long NetLuid;
    void              *DefaultPortAuthStates, *PciDeviceCustomProperties;
} NDIS_MINIPORT_INIT_PARAMETERS;

typedef struct {
    NDIS_OBJECT_HEADER Header;
    unsigned int       Flags, PauseReason;
} NDIS_MINIPORT_PAUSE_PARAMETERS;

typedef struct {
    NDIS_OBJECT_HEADER Header;
    void              *RestartAttributes;
    unsigned int       Flags;
} NDIS_MINIPORT_RESTART_PARAMETERS;

/* NdisMSetMiniportAttributes' attribute blocks (Header.Type says which) */
typedef struct {
    NDIS_OBJECT_HEADER Header;
    void              *MiniportAdapterContext;
    unsigned int       AttributeFlags, CheckForHangTimeInSeconds;
    int                InterfaceType;
} NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES;

typedef struct {
    NDIS_OBJECT_HEADER Header;
    unsigned int       Flags;
    int                MediaType, PhysicalMediumType;
    unsigned int       MtuSize;
    unsigned long long MaxXmitLinkSpeed, XmitLinkSpeed, MaxRcvLinkSpeed, RcvLinkSpeed;
    int                MediaConnectState, MediaDuplexState;
    unsigned int       LookaheadSize;
    void              *PowerManagementCapabilities;
    unsigned int       MacOptions, SupportedPacketFilters, MaxMulticastListSize;
    unsigned short     MacAddressLength;
    unsigned char      PermanentMacAddress[32], CurrentMacAddress[32];
    void              *RecvScaleCapabilities;
    int                AccessType, DirectionType, ConnectionType;
    unsigned short     IfType;
    unsigned char      IfConnectorPresent;
    unsigned int       SupportedStatistics, SupportedPauseFunctions, DataBackFillSize, ContextBackFillSize;
    unsigned int      *SupportedOidList;
    unsigned int       SupportedOidListLength, AutoNegotiationFlags;
    void              *PowerManagementCapabilitiesEx;
} NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES;

/* (AddDevice's: MiniportAddDeviceContext, the context its later calls get) */
typedef struct {
    NDIS_OBJECT_HEADER Header;
    void              *MiniportAddDeviceContext;
    unsigned int       Flags;
} NDIS_MINIPORT_ADD_DEVICE_REGISTRATION_ATTRIBUTES;

/* WDI (dot11wdi.h): a Wi-Fi miniport's lower edge.  NdisMRegisterWdiMiniportDriver
 * takes these handlers beside an NDIS_MINIPORT_DRIVER_CHARACTERISTICS that
 * has no initialize, halt, pause, restart or send handlers: on Windows the
 * WDI upper edge (wdiwifi.sys) is the NDIS miniport and drives the IHV's
 * through these.  (The header types are what Intel's AX211 driver fills
 * in and what NovaOS hands it.) */
#define NDIS_OBJECT_TYPE_MINIPORT_WDI_CHARACTERISTICS 0xC0
#define NDIS_OBJECT_TYPE_WDI_INIT_PARAMETERS          0xC1
typedef struct {
    NDIS_OBJECT_HEADER Header;
    unsigned int       WdiVersion;
    void              *AllocateAdapterHandler, *FreeAdapterHandler, *OpenAdapterHandler, *CloseAdapterHandler,
                      *StartOperationHandler, *StopOperationHandler, *PostPauseHandler, *PostRestartHandler,
                      *HangDiagnoseHandler, *TalTxRxInitializeHandler, *TalTxRxDeinitializeHandler,
                      *LeIdleNotificationHandler, *LeCancelIdleNotificationHandler;
} NDIS_MINIPORT_DRIVER_WDI_CHARACTERISTICS;

/* What the upper edge gives the IHV driver's MiniportWdiAllocateAdapter:
 * the routines it completes its open and close with */
typedef struct {
    NDIS_OBJECT_HEADER Header;
    unsigned int       WdiVersion;
    void              *OpenAdapterCompleteHandler, *CloseAdapterCompleteHandler, *UeIdleNotificationConfirm,
                      *UeIdleNotificationComplete;
} NDIS_WDI_INIT_PARAMETERS;

#define NdisMediaStateUnknown   0
#define MediaConnectStateConnected    1
#define MediaConnectStateDisconnected 2

typedef struct {
    NDIS_OBJECT_HEADER Header;
    int                MediaConnectState, MediaDuplexState;
    unsigned long long XmitLinkSpeed, RcvLinkSpeed;
    int                PauseFunctions;
    unsigned int       AutoNegotiationFlags;
} NDIS_LINK_STATE;

/* NDIS_MINIPORT_INTERRUPT_CHARACTERISTICS (NdisMRegisterInterruptEx) */
#define NDIS_CONNECT_LINE_BASED     2
#define NDIS_CONNECT_MESSAGE_BASED  3
typedef struct {
    NDIS_OBJECT_HEADER Header;
    void              *InterruptHandler, *InterruptDpcHandler, *DisableInterruptHandler, *EnableInterruptHandler;
    unsigned char      MsiSupported, MsiSyncWithAllMessages;
    void              *MessageInterruptHandler, *MessageInterruptDpcHandler, *DisableMessageInterruptHandler,
                      *EnableMessageInterruptHandler;
    int                InterruptType;
    void              *MessageInfoTable;
} NDIS_MINIPORT_INTERRUPT_CHARACTERISTICS;

/* What an interrupt DPC is handed: how many packets it may indicate */
typedef struct {
    unsigned int       MaxNblsToIndicate, MoreNblsPending;
} NDIS_RECEIVE_THROTTLE_PARAMETERS;
#define NDIS_INDICATE_ALL_NBLS (~0u)

/* NDIS_SG_DMA_DESCRIPTION (NdisMRegisterScatterGatherDma) */
#define NDIS_SG_DMA_64_BIT_ADDRESS 0x00000001
typedef struct {
    NDIS_OBJECT_HEADER Header;
    unsigned int       Flags, MaximumPhysicalMapping;
    void              *ProcessSGListHandler, *SharedMemAllocateCompleteHandler;
    unsigned int       ScatterGatherListSize;
    NT_DEVICE_OBJECT  *PdoOverride;
} NDIS_SG_DMA_DESCRIPTION;

/* NDIS_OID_REQUEST: a query, a set or a method request of one OID */
#define NdisRequestQueryInformation 0
#define NdisRequestSetInformation   1
#define OID_GEN_CURRENT_PACKET_FILTER 0x0001010E
#define OID_GEN_CURRENT_LOOKAHEAD     0x0001010F
#define NDIS_PACKET_TYPE_DIRECTED      0x01
#define NDIS_PACKET_TYPE_MULTICAST     0x02
#define NDIS_PACKET_TYPE_ALL_MULTICAST 0x04
#define NDIS_PACKET_TYPE_BROADCAST     0x08
typedef struct {
    NDIS_OBJECT_HEADER Header;
    int                RequestType;
    unsigned int       PortNumber, Timeout;
    void              *RequestId, *RequestHandle;
    union {
        struct {
            unsigned int   Oid;
            void          *InformationBuffer;
            unsigned int   InformationBufferLength, BytesWritten, BytesNeeded;
        } QUERY_INFORMATION;
        struct {
            unsigned int   Oid;
            void          *InformationBuffer;
            unsigned int   InformationBufferLength, BytesRead, BytesNeeded;
        } SET_INFORMATION;
        struct {                        /* (NdisRequestMethod: WDI's commands) */
            unsigned int   Oid;
            void          *InformationBuffer;
            unsigned int   InputBufferLength, OutputBufferLength, MethodId, BytesWritten, BytesRead, BytesNeeded;
        } METHOD_INFORMATION;
        unsigned char  Space[40];
    } DATA;
    unsigned char      NdisReserved[16 * sizeof(void *)];
    unsigned char      MiniportReserved[2 * sizeof(void *)];
    unsigned char      SourceReserved[2 * sizeof(void *)];
    unsigned char      SupportedRevision, Reserved1;
    unsigned short     Reserved2;
    unsigned int       SwitchId, VPortId, Flags;
} NDIS_OID_REQUEST;

typedef struct {
    NDIS_OBJECT_HEADER Header;
    void              *SourceHandle;
    unsigned int       PortNumber;
    NDIS_STATUS        StatusCode;
    unsigned int       Flags;
    void              *DestinationHandle, *RequestId, *StatusBuffer;
    unsigned int       StatusBufferSize;
    NT_GUID            Guid;
    void              *NdisReserved[4];
} NDIS_STATUS_INDICATION;

/* NdisOpenConfigurationEx and NdisReadConfiguration */
typedef struct {
    NDIS_OBJECT_HEADER Header;
    void              *NdisHandle;
    unsigned int       Flags;
} NDIS_CONFIGURATION_OBJECT;

#define NdisParameterInteger    0
#define NdisParameterHexInteger 1
#define NdisParameterString     2
#define NdisParameterMultiString 3
#define NdisParameterBinary     4
typedef struct {
    int                ParameterType;
    union {
        unsigned int      IntegerData;
        NT_UNICODE_STRING StringData;
        struct { unsigned short Length; void *Buffer; } BinaryData;
    } ParameterData;
} NDIS_CONFIGURATION_PARAMETER;

/* NdisRegisterDeviceEx */
typedef struct {
    NDIS_OBJECT_HEADER Header;
    NT_UNICODE_STRING *DeviceName, *SymbolicName;
    NT_DRIVER_DISPATCH *MajorFunctions;
    unsigned int       ExtensionSize;
    NT_UNICODE_STRING *DefaultSDDLString;
    NT_GUID           *DeviceClassGuid;
} NDIS_DEVICE_OBJECT_ATTRIBUTES;

/* NDIS_RW_LOCK_EX is opaque to a driver; LOCK_STATE_EX is its own */
typedef struct {
    unsigned char      OldIrql, LockState, Flags;
} LOCK_STATE_EX;
#define NDIS_RWL_AT_DISPATCH_LEVEL 1

/* Flags on the send, return, receive and send-complete paths */
#define NDIS_SEND_FLAGS_DISPATCH_LEVEL          0x01
#define NDIS_RETURN_FLAGS_DISPATCH_LEVEL        0x01
#define NDIS_RECEIVE_FLAGS_DISPATCH_LEVEL       0x01
#define NDIS_RECEIVE_FLAGS_RESOURCES            0x02
#define NDIS_SEND_COMPLETE_FLAGS_DISPATCH_LEVEL 0x01

#define NDIS_SG_LIST_WRITE_TO_DEVICE 0x0001

#define NDIS_OFFSET(t, f) __builtin_offsetof(t, f)
_Static_assert(sizeof(NET_BUFFER) == 176 && NDIS_OFFSET(NET_BUFFER, DataLength) == 24 &&
               NDIS_OFFSET(NET_BUFFER, DataOffset) == 40 && NDIS_OFFSET(NET_BUFFER, NdisPoolHandle) == 56 &&
               NDIS_OFFSET(NET_BUFFER, ProtocolReserved) == 80 && NDIS_OFFSET(NET_BUFFER, MiniportReserved) == 128 &&
               NDIS_OFFSET(NET_BUFFER, ScatterGatherList) == 168, "NET_BUFFER");
_Static_assert(sizeof(NET_BUFFER_LIST) == 368 && NDIS_OFFSET(NET_BUFFER_LIST, NdisPoolHandle) == 32 &&
               NDIS_OFFSET(NET_BUFFER_LIST, NdisReserved) == 48 && NDIS_OFFSET(NET_BUFFER_LIST, ProtocolReserved) == 64 &&
               NDIS_OFFSET(NET_BUFFER_LIST, MiniportReserved) == 96 && NDIS_OFFSET(NET_BUFFER_LIST, Scratch) == 112 &&
               NDIS_OFFSET(NET_BUFFER_LIST, NblFlags) == 128 && NDIS_OFFSET(NET_BUFFER_LIST, Status) == 140 &&
               NDIS_OFFSET(NET_BUFFER_LIST, NetBufferListInfo) == 144, "NET_BUFFER_LIST");
_Static_assert(sizeof(NET_BUFFER_LIST_CONTEXT) == 16 && sizeof(NET_BUFFER_LIST_POOL_PARAMETERS) == 16, "NBL context, pool");
_Static_assert(sizeof(NDIS_MINIPORT_DRIVER_CHARACTERISTICS) == 160 &&
               NDIS_OFFSET(NDIS_MINIPORT_DRIVER_CHARACTERISTICS, InitializeHandlerEx) == 24 &&
               NDIS_OFFSET(NDIS_MINIPORT_DRIVER_CHARACTERISTICS, SendNetBufferListsHandler) == 72 &&
               NDIS_OFFSET(NDIS_MINIPORT_DRIVER_CHARACTERISTICS, SynchronousOidRequestHandler) == 152, "DRIVER_CHARACTERISTICS");
_Static_assert(sizeof(NDIS_MINIPORT_PNP_CHARACTERISTICS) == 48 && sizeof(NDIS_MINIPORT_INIT_PARAMETERS) == 64 &&
               NDIS_OFFSET(NDIS_MINIPORT_INIT_PARAMETERS, NetLuid) == 40, "PNP_CHARACTERISTICS, INIT_PARAMETERS");
_Static_assert(sizeof(NDIS_MINIPORT_PAUSE_PARAMETERS) == 12 && sizeof(NDIS_MINIPORT_RESTART_PARAMETERS) == 24,
               "PAUSE/RESTART_PARAMETERS");
_Static_assert(sizeof(NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES) == 32 &&
               sizeof(NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES) == 224 &&
               NDIS_OFFSET(NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES, MediaConnectState) == 56 &&
               NDIS_OFFSET(NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES, MacAddressLength) == 92 &&
               NDIS_OFFSET(NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES, CurrentMacAddress) == 126 &&
               NDIS_OFFSET(NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES, IfType) == 180 &&
               NDIS_OFFSET(NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES, SupportedOidList) == 200, "ADAPTER attributes");
_Static_assert(sizeof(NDIS_LINK_STATE) == 40 && sizeof(NDIS_MINIPORT_INTERRUPT_CHARACTERISTICS) == 96 &&
               NDIS_OFFSET(NDIS_MINIPORT_INTERRUPT_CHARACTERISTICS, MsiSupported) == 40 &&
               NDIS_OFFSET(NDIS_MINIPORT_INTERRUPT_CHARACTERISTICS, InterruptType) == 80, "LINK_STATE, INTERRUPT");
_Static_assert(sizeof(NDIS_SG_DMA_DESCRIPTION) == 48 && NDIS_OFFSET(NDIS_SG_DMA_DESCRIPTION, ScatterGatherListSize) == 32,
               "SG_DMA_DESCRIPTION");
_Static_assert(sizeof(NDIS_OID_REQUEST) == 248 && NDIS_OFFSET(NDIS_OID_REQUEST, DATA) == 32 &&
               NDIS_OFFSET(NDIS_OID_REQUEST, NdisReserved) == 72 && NDIS_OFFSET(NDIS_OID_REQUEST, Flags) == 244,
               "OID_REQUEST");
_Static_assert(NDIS_OFFSET(NDIS_OID_REQUEST, DATA.METHOD_INFORMATION.InputBufferLength) == 48 &&
               NDIS_OFFSET(NDIS_OID_REQUEST, DATA.METHOD_INFORMATION.BytesWritten) == 60, "OID_REQUEST method");
_Static_assert(sizeof(NDIS_STATUS_INDICATION) == 112 && NDIS_OFFSET(NDIS_STATUS_INDICATION, StatusBuffer) == 48 &&
               NDIS_OFFSET(NDIS_STATUS_INDICATION, Guid) == 60, "STATUS_INDICATION");
_Static_assert(sizeof(NDIS_CONFIGURATION_OBJECT) == 24 && sizeof(NDIS_CONFIGURATION_PARAMETER) == 24 &&
               sizeof(NDIS_DEVICE_OBJECT_ATTRIBUTES) == 56, "CONFIGURATION, DEVICE_OBJECT_ATTRIBUTES");
_Static_assert(sizeof(NDIS_MINIPORT_DRIVER_WDI_CHARACTERISTICS) == 0x70 && sizeof(NDIS_WDI_INIT_PARAMETERS) == 40,
               "WDI_CHARACTERISTICS, WDI_INIT_PARAMETERS");
