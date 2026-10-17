/*
 * wdi.h — the WDI (WLAN Device Driver Interface) messages a Wi-Fi miniport
 * and its upper edge exchange, as dot11wdi.h and wditypes.hpp define them
 * (kernel/nt/wdi.c is NovaOS's upper edge, the part of wdiwifi.sys that
 * drives the IHV driver)
 *
 * A WDI command is an NDIS method OID (OID_WDI_*, always NdisRequestMethod)
 * whose buffer is a WDI_MESSAGE_HEADER followed by TLVs; the IHV writes its
 * answer (header and TLVs) into the same buffer.  A task (OID_WDI_TASK_*)
 * finishes later with an NDIS status indication, NDIS_STATUS_WDI_INDICATION_*
 * carrying a header with the task's TransactionId and the result; other
 * indications (a scan's BSS entries) come unsolicited.  Every TLV is a
 * 16-bit type, a 16-bit length and the value, little-endian, with structure
 * fields packed at their own widths (a bool is one byte, an enum four).
 * The numbers are Microsoft's (the WDK headers and WABIModel.xml, the WDI
 * wire model); only what NovaOS sends, reads or its test miniport answers is
 * named.  Shared with tools/drivertest/wditest.c, so only C's own types
 * (after ndis.h).
 */
#pragma once

#define WDI_OID(id)        (0xE4400000u | (id))
#define WDI_INDICATION(id) (0x40050000u | (id))
#define WDI_IS_INDICATION(code) (((unsigned int)(code) & 0xFFFF0000u) == 0x40050000u)

/* message IDs */
#define WDI_TASK_OPEN                        1
#define WDI_TASK_CLOSE                       2
#define WDI_TASK_SCAN                        4
#define WDI_TASK_CONNECT                     6
#define WDI_TASK_DOT11_RESET                 7
#define WDI_TASK_DISCONNECT                  8
#define WDI_TASK_SET_RADIO_STATE             11
#define WDI_TASK_CREATE_PORT                 12
#define WDI_TASK_DELETE_PORT                 13
#define WDI_GET_ADAPTER_CAPABILITIES         37
#define WDI_GET_BSS_ENTRY_LIST               47
#define WDI_INDICATION_SCAN_COMPLETE         60
#define WDI_INDICATION_BSS_ENTRY_LIST        62
#define WDI_INDICATION_CONNECT_COMPLETE      64
#define WDI_INDICATION_RADIO_STATUS          67
#define WDI_INDICATION_CREATE_PORT_COMPLETE  68
#define WDI_INDICATION_DELETE_PORT_COMPLETE  69

#define OID_WDI_TASK_SCAN                    WDI_OID(WDI_TASK_SCAN)
#define OID_WDI_TASK_CREATE_PORT             WDI_OID(WDI_TASK_CREATE_PORT)
#define OID_WDI_TASK_DELETE_PORT             WDI_OID(WDI_TASK_DELETE_PORT)
#define OID_WDI_GET_ADAPTER_CAPABILITIES     WDI_OID(WDI_GET_ADAPTER_CAPABILITIES)

#define NdisRequestMethod 12

#define WDI_PORT_ID_ADAPTER        0xFFFF   /* a command for the adapter, not a port */
#define WDI_TRANSACTION_ID_UNSOLICIT 0      /* an indication no task asked for */
#define WDI_OPERATION_MODE_STA     0x01

/* (the header has no message ID or length: those are the OID's or the
 * status code's, and the buffer's size) */
typedef struct {
    unsigned short      PortId;
    unsigned short      Reserved;
    NDIS_STATUS Status;
    unsigned int      TransactionId;
    unsigned int      IhvSpecificId;
} WDI_MESSAGE_HEADER;

typedef struct __attribute__((packed)) {
    unsigned short      Type, Length;
} WDI_TLV_HEADER;

/* TLV types */
#define WDI_TLV_BSSID                    0x02
#define WDI_TLV_SCAN_MODE                0x06
#define WDI_TLV_SCAN_DWELL_TIME          0x07
#define WDI_TLV_BSS_ENTRY                0x08
#define WDI_TLV_PROBE_RESPONSE_FRAME     0x09
#define WDI_TLV_BEACON_FRAME             0x0A
#define WDI_TLV_BSS_ENTRY_SIGNAL_INFO    0x0B
#define WDI_TLV_INTERFACE_CAPABILITIES   0x0F
#define WDI_TLV_BAND_CAPABILITIES        0x1A
#define WDI_TLV_INTERFACE_ATTRIBUTES     0x21
#define WDI_TLV_STATION_ATTRIBUTES       0x22
#define WDI_TLV_BAND_INFO                0x27
#define WDI_TLV_CREATE_PORT_PARAMETERS   0x28
#define WDI_TLV_PORT_ATTRIBUTES          0x29
#define WDI_TLV_DELETE_PORT_PARAMETERS   0x2A
#define WDI_TLV_BSS_ENTRY_CHANNEL_INFO   0x3A
#define WDI_TLV_SSID                     0x3B
#define WDI_TLV_BSS_ENTRY_AGE_INFO       0xBA
#define WDI_TLV_FIRMWARE_VERSION         0xF4

/* WDI_TLV_SCAN_MODE (WDI_SCAN_MODE) */
#define WDI_SCAN_TYPE_ACTIVE_ONLY   1
#define WDI_SCAN_TYPE_PASSIVE_ONLY  2
#define WDI_SCAN_TYPE_AUTO          3
#define WDI_SCAN_TRIGGER_MANUAL     1
typedef struct __attribute__((packed)) {
    unsigned char  ScanRepeatCount;
    unsigned int AllowedScanType;
    unsigned char  LiveUpdatesNeeded;
    unsigned int ScanTrigger;
} WDI_SCAN_MODE;

/* WDI_TLV_SCAN_DWELL_TIME (milliseconds) */
typedef struct __attribute__((packed)) {
    unsigned int ActiveChannelDwellTime, PassiveChannelDwellTime, MaximumScanTime;
} WDI_SCAN_DWELL_TIME;

/* WDI_TLV_BSS_ENTRY_SIGNAL_INFO */
typedef struct __attribute__((packed)) {
    int  RSSI;            /* dBm */
    unsigned int LinkQuality;     /* 0-100 */
} WDI_SIGNAL_INFO;

/* WDI_TLV_BSS_ENTRY_CHANNEL_INFO */
#define WDI_BAND_ID_2400 1
#define WDI_BAND_ID_5000 2
#define WDI_BAND_ID_6000 5
typedef struct __attribute__((packed)) {
    unsigned int ChannelNumber, BandId;
} WDI_BSS_ENTRY_CHANNEL_INFO;

/* WDI_TLV_BAND_CAPABILITIES */
typedef struct __attribute__((packed)) {
    unsigned int BandID;
    unsigned char  BandState;
} WDI_BAND_CAPABILITIES;

/* WDI_TLV_CREATE_PORT_PARAMETERS */
typedef struct __attribute__((packed)) {
    unsigned short OpModeMask;
    unsigned int NdisPortNumber;
} WDI_CREATE_PORT_PARAMETERS;

/* WDI_TLV_PORT_ATTRIBUTES (WDI_CREATE_PORT_COMPLETE_STRUCT) */
typedef struct __attribute__((packed)) {
    unsigned char  MacAddress[6];
    unsigned short PortNumber;
} WDI_PORT_ATTRIBUTES;

/* WDI_TLV_DELETE_PORT_PARAMETERS */
typedef struct __attribute__((packed)) {
    unsigned short PortNumber;
} WDI_DELETE_PORT_PARAMETERS;

/* WDI_TLV_INTERFACE_CAPABILITIES: its first fields, the same in every WDI
 * version (later versions add more booleans after them) */
typedef struct __attribute__((packed)) {
    unsigned int MTUSize, MaxMultiCastListSize;
    unsigned short BackFillSize;
    unsigned char  Address[6];
    unsigned int MaxTxRate, MaxRxRate;    /* kbit/s */
    unsigned char  HardwareRadioState, SoftwareRadioState;
} WDI_INTERFACE_CAPABILITIES;
