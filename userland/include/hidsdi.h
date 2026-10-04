/*
 * hidsdi.h — HID device calls (hid.dll's HidD_*), as the Windows SDK has it
 */
#pragma once
#include <windows.h>
#include <hidpi.h>

typedef struct _HIDD_ATTRIBUTES {
    ULONG Size;
    USHORT VendorID, ProductID, VersionNumber;
} HIDD_ATTRIBUTES, *PHIDD_ATTRIBUTES;

HIDAPI_ void WINAPI HidD_GetHidGuid(LPGUID g);
HIDAPI_ BOOLEAN WINAPI HidD_GetAttributes(HANDLE dev, PHIDD_ATTRIBUTES a);
HIDAPI_ BOOLEAN WINAPI HidD_GetPreparsedData(HANDLE dev, PHIDP_PREPARSED_DATA *p);
HIDAPI_ BOOLEAN WINAPI HidD_FreePreparsedData(PHIDP_PREPARSED_DATA p);
HIDAPI_ BOOLEAN WINAPI HidD_FlushQueue(HANDLE dev);
HIDAPI_ BOOLEAN WINAPI HidD_GetNumInputBuffers(HANDLE dev, PULONG n);
HIDAPI_ BOOLEAN WINAPI HidD_SetNumInputBuffers(HANDLE dev, ULONG n);
HIDAPI_ BOOLEAN WINAPI HidD_GetInputReport(HANDLE dev, PVOID buf, ULONG len);
HIDAPI_ BOOLEAN WINAPI HidD_SetOutputReport(HANDLE dev, PVOID buf, ULONG len);
HIDAPI_ BOOLEAN WINAPI HidD_GetFeature(HANDLE dev, PVOID buf, ULONG len);
HIDAPI_ BOOLEAN WINAPI HidD_SetFeature(HANDLE dev, PVOID buf, ULONG len);
HIDAPI_ BOOLEAN WINAPI HidD_GetProductString(HANDLE dev, PVOID buf, ULONG len);
HIDAPI_ BOOLEAN WINAPI HidD_GetManufacturerString(HANDLE dev, PVOID buf, ULONG len);
HIDAPI_ BOOLEAN WINAPI HidD_GetSerialNumberString(HANDLE dev, PVOID buf, ULONG len);
HIDAPI_ BOOLEAN WINAPI HidD_GetIndexedString(HANDLE dev, ULONG index, PVOID buf, ULONG len);
HIDAPI_ BOOLEAN WINAPI HidD_GetPhysicalDescriptor(HANDLE dev, PVOID buf, ULONG len);
HIDAPI_ BOOLEAN WINAPI HidD_GetConfiguration(HANDLE dev, PVOID cfg, ULONG len);
HIDAPI_ BOOLEAN WINAPI HidD_SetConfiguration(HANDLE dev, PVOID cfg, ULONG len);
