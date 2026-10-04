/*
 * hidpi.h — HID report parsing (hid.dll's HidP_* calls), as the Windows SDK has it
 */
#pragma once
#include <windows.h>

#ifndef HIDAPI_
#define HIDAPI_ __declspec(dllimport)
#endif

typedef USHORT *PUSHORT;
typedef CHAR *PCHAR;
typedef USHORT USAGE, *PUSAGE;
typedef LONG NTSTATUS_HIDP;
typedef struct _HIDP_PREPARSED_DATA *PHIDP_PREPARSED_DATA;

typedef enum _HIDP_REPORT_TYPE { HidP_Input, HidP_Output, HidP_Feature } HIDP_REPORT_TYPE;

typedef struct _USAGE_AND_PAGE { USAGE Usage; USAGE UsagePage; } USAGE_AND_PAGE, *PUSAGE_AND_PAGE;

#define HIDP_STATUS_SUCCESS                 ((LONG)0x00110000)
#define HIDP_STATUS_NULL                    ((LONG)0x80110001)
#define HIDP_STATUS_INVALID_PREPARSED_DATA  ((LONG)0xC0110001)
#define HIDP_STATUS_INVALID_REPORT_TYPE     ((LONG)0xC0110002)
#define HIDP_STATUS_INVALID_REPORT_LENGTH   ((LONG)0xC0110003)
#define HIDP_STATUS_USAGE_NOT_FOUND         ((LONG)0xC0110004)
#define HIDP_STATUS_VALUE_OUT_OF_RANGE      ((LONG)0xC0110005)
#define HIDP_STATUS_BAD_LOG_PHY_VALUES      ((LONG)0xC0110006)
#define HIDP_STATUS_BUFFER_TOO_SMALL        ((LONG)0xC0110007)
#define HIDP_STATUS_INTERNAL_ERROR          ((LONG)0xC0110008)
#define HIDP_STATUS_I8042_TRANS_UNKNOWN     ((LONG)0xC0110009)
#define HIDP_STATUS_INCOMPATIBLE_REPORT_ID  ((LONG)0xC011000A)
#define HIDP_STATUS_NOT_VALUE_ARRAY         ((LONG)0xC011000B)
#define HIDP_STATUS_IS_VALUE_ARRAY          ((LONG)0xC011000C)
#define HIDP_STATUS_DATA_INDEX_NOT_FOUND    ((LONG)0xC011000D)
#define HIDP_STATUS_DATA_INDEX_OUT_OF_RANGE ((LONG)0xC011000E)
#define HIDP_STATUS_BUTTON_NOT_PRESSED      ((LONG)0xC011000F)
#define HIDP_STATUS_REPORT_DOES_NOT_EXIST   ((LONG)0xC0110010)
#define HIDP_STATUS_NOT_IMPLEMENTED         ((LONG)0xC0110020)

typedef struct _HIDP_CAPS {
    USAGE Usage, UsagePage;
    USHORT InputReportByteLength, OutputReportByteLength, FeatureReportByteLength;
    USHORT Reserved[17];
    USHORT NumberLinkCollectionNodes;
    USHORT NumberInputButtonCaps, NumberInputValueCaps, NumberInputDataIndices;
    USHORT NumberOutputButtonCaps, NumberOutputValueCaps, NumberOutputDataIndices;
    USHORT NumberFeatureButtonCaps, NumberFeatureValueCaps, NumberFeatureDataIndices;
} HIDP_CAPS, *PHIDP_CAPS;

typedef struct _HIDP_BUTTON_CAPS {
    USAGE UsagePage;
    UCHAR ReportID;
    BOOLEAN IsAlias;
    USHORT BitField;
    USHORT LinkCollection;
    USAGE LinkUsage, LinkUsagePage;
    BOOLEAN IsRange, IsStringRange, IsDesignatorRange, IsAbsolute;
    USHORT ReportCount;
    USHORT Reserved2;
    ULONG Reserved[9];
    union {
        struct { USAGE UsageMin, UsageMax; USHORT StringMin, StringMax; USHORT DesignatorMin, DesignatorMax; USHORT DataIndexMin, DataIndexMax; } Range;
        struct { USAGE Usage, Reserved1; USHORT StringIndex, Reserved2; USHORT DesignatorIndex, Reserved3; USHORT DataIndex, Reserved4; } NotRange;
    };
} HIDP_BUTTON_CAPS, *PHIDP_BUTTON_CAPS;

typedef struct _HIDP_VALUE_CAPS {
    USAGE UsagePage;
    UCHAR ReportID;
    BOOLEAN IsAlias;
    USHORT BitField;
    USHORT LinkCollection;
    USAGE LinkUsage, LinkUsagePage;
    BOOLEAN IsRange, IsStringRange, IsDesignatorRange, IsAbsolute;
    BOOLEAN HasNull;
    UCHAR Reserved;
    USHORT BitSize;
    USHORT ReportCount;
    USHORT Reserved2[5];
    ULONG UnitsExp, Units;
    LONG LogicalMin, LogicalMax;
    LONG PhysicalMin, PhysicalMax;
    union {
        struct { USAGE UsageMin, UsageMax; USHORT StringMin, StringMax; USHORT DesignatorMin, DesignatorMax; USHORT DataIndexMin, DataIndexMax; } Range;
        struct { USAGE Usage, Reserved1; USHORT StringIndex, Reserved2; USHORT DesignatorIndex, Reserved3; USHORT DataIndex, Reserved4; } NotRange;
    };
} HIDP_VALUE_CAPS, *PHIDP_VALUE_CAPS;

typedef struct _HIDP_LINK_COLLECTION_NODE {
    USAGE LinkUsage, LinkUsagePage;
    USHORT Parent, NumberOfChildren, NextSibling, FirstChild;
    ULONG CollectionType : 8;
    ULONG IsAlias : 1;
    ULONG Reserved : 23;
    PVOID UserContext;
} HIDP_LINK_COLLECTION_NODE, *PHIDP_LINK_COLLECTION_NODE;

typedef struct _HIDP_DATA {
    USHORT DataIndex;
    USHORT Reserved;
    union { ULONG RawValue; BOOLEAN On; };
} HIDP_DATA, *PHIDP_DATA;

typedef struct _HIDP_UNKNOWN_TOKEN { UCHAR Token; UCHAR Reserved[3]; ULONG BitField; } HIDP_UNKNOWN_TOKEN, *PHIDP_UNKNOWN_TOKEN;
typedef struct _HIDP_EXTENDED_ATTRIBUTES {
    UCHAR NumGlobalUnknowns;
    UCHAR Reserved[3];
    PHIDP_UNKNOWN_TOKEN GlobalUnknowns;
    ULONG Data[1];
} HIDP_EXTENDED_ATTRIBUTES, *PHIDP_EXTENDED_ATTRIBUTES;

HIDAPI_ LONG WINAPI HidP_GetCaps(PHIDP_PREPARSED_DATA p, PHIDP_CAPS caps);
HIDAPI_ LONG WINAPI HidP_GetLinkCollectionNodes(PHIDP_LINK_COLLECTION_NODE nodes, PULONG n, PHIDP_PREPARSED_DATA p);
HIDAPI_ LONG WINAPI HidP_GetButtonCaps(HIDP_REPORT_TYPE t, PHIDP_BUTTON_CAPS caps, PUSHORT n, PHIDP_PREPARSED_DATA p);
HIDAPI_ LONG WINAPI HidP_GetSpecificButtonCaps(HIDP_REPORT_TYPE t, USAGE page, USHORT link, USAGE usage, PHIDP_BUTTON_CAPS caps, PUSHORT n, PHIDP_PREPARSED_DATA p);
HIDAPI_ LONG WINAPI HidP_GetValueCaps(HIDP_REPORT_TYPE t, PHIDP_VALUE_CAPS caps, PUSHORT n, PHIDP_PREPARSED_DATA p);
HIDAPI_ LONG WINAPI HidP_GetSpecificValueCaps(HIDP_REPORT_TYPE t, USAGE page, USHORT link, USAGE usage, PHIDP_VALUE_CAPS caps, PUSHORT n, PHIDP_PREPARSED_DATA p);
HIDAPI_ LONG WINAPI HidP_GetExtendedAttributes(HIDP_REPORT_TYPE t, USHORT index, PHIDP_PREPARSED_DATA p, PHIDP_EXTENDED_ATTRIBUTES a, PULONG n);
HIDAPI_ LONG WINAPI HidP_InitializeReportForID(HIDP_REPORT_TYPE t, UCHAR id, PHIDP_PREPARSED_DATA p, PCHAR report, ULONG len);
HIDAPI_ LONG WINAPI HidP_GetData(HIDP_REPORT_TYPE t, PHIDP_DATA data, PULONG n, PHIDP_PREPARSED_DATA p, PCHAR report, ULONG len);
HIDAPI_ LONG WINAPI HidP_SetData(HIDP_REPORT_TYPE t, PHIDP_DATA data, PULONG n, PHIDP_PREPARSED_DATA p, PCHAR report, ULONG len);
HIDAPI_ ULONG WINAPI HidP_MaxDataListLength(HIDP_REPORT_TYPE t, PHIDP_PREPARSED_DATA p);
HIDAPI_ LONG WINAPI HidP_GetUsages(HIDP_REPORT_TYPE t, USAGE page, USHORT link, PUSAGE list, PULONG n, PHIDP_PREPARSED_DATA p, PCHAR report, ULONG len);
HIDAPI_ LONG WINAPI HidP_GetUsagesEx(HIDP_REPORT_TYPE t, USHORT link, PUSAGE_AND_PAGE list, PULONG n, PHIDP_PREPARSED_DATA p, PCHAR report, ULONG len);
HIDAPI_ LONG WINAPI HidP_SetUsages(HIDP_REPORT_TYPE t, USAGE page, USHORT link, PUSAGE list, PULONG n, PHIDP_PREPARSED_DATA p, PCHAR report, ULONG len);
HIDAPI_ LONG WINAPI HidP_UnsetUsages(HIDP_REPORT_TYPE t, USAGE page, USHORT link, PUSAGE list, PULONG n, PHIDP_PREPARSED_DATA p, PCHAR report, ULONG len);
HIDAPI_ ULONG WINAPI HidP_MaxUsageListLength(HIDP_REPORT_TYPE t, USAGE page, PHIDP_PREPARSED_DATA p);
HIDAPI_ LONG WINAPI HidP_GetUsageValue(HIDP_REPORT_TYPE t, USAGE page, USHORT link, USAGE usage, PULONG value, PHIDP_PREPARSED_DATA p, PCHAR report, ULONG len);
HIDAPI_ LONG WINAPI HidP_GetScaledUsageValue(HIDP_REPORT_TYPE t, USAGE page, USHORT link, USAGE usage, PLONG value, PHIDP_PREPARSED_DATA p, PCHAR report, ULONG len);
HIDAPI_ LONG WINAPI HidP_GetUsageValueArray(HIDP_REPORT_TYPE t, USAGE page, USHORT link, USAGE usage, PCHAR value, USHORT vlen, PHIDP_PREPARSED_DATA p, PCHAR report, ULONG len);
HIDAPI_ LONG WINAPI HidP_SetUsageValue(HIDP_REPORT_TYPE t, USAGE page, USHORT link, USAGE usage, ULONG value, PHIDP_PREPARSED_DATA p, PCHAR report, ULONG len);
HIDAPI_ LONG WINAPI HidP_SetScaledUsageValue(HIDP_REPORT_TYPE t, USAGE page, USHORT link, USAGE usage, LONG value, PHIDP_PREPARSED_DATA p, PCHAR report, ULONG len);
HIDAPI_ LONG WINAPI HidP_SetUsageValueArray(HIDP_REPORT_TYPE t, USAGE page, USHORT link, USAGE usage, PCHAR value, USHORT vlen, PHIDP_PREPARSED_DATA p, PCHAR report, ULONG len);
HIDAPI_ LONG WINAPI HidP_UsageListDifference(PUSAGE prev, PUSAGE cur, PUSAGE brk, PUSAGE make, ULONG n);
HIDAPI_ LONG WINAPI HidP_UsageAndPageListDifference(PUSAGE_AND_PAGE prev, PUSAGE_AND_PAGE cur, PUSAGE_AND_PAGE brk, PUSAGE_AND_PAGE make, ULONG n);

#define HidP_GetButtons(t, page, link, list, n, p, r, l) HidP_GetUsages(t, page, link, list, n, p, r, l)
#define HidP_GetButtonsEx(t, link, list, n, p, r, l) HidP_GetUsagesEx(t, link, list, n, p, r, l)
#define HidP_SetButtons(t, page, link, list, n, p, r, l) HidP_SetUsages(t, page, link, list, n, p, r, l)
#define HidP_UnsetButtons(t, page, link, list, n, p, r, l) HidP_UnsetUsages(t, page, link, list, n, p, r, l)
#define HidP_MaxButtonListLength(t, page, p) HidP_MaxUsageListLength(t, page, p)
