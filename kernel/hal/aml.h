/*
 * aml.h — the ACPI namespace: AML run by uACPI (third_party/uacpi)
 *
 * acpi.c reads what it can straight from the tables; what the firmware
 * only describes in AML (batteries, AC adapters, control-method power
 * buttons, lids, thermal zones, PCI interrupt routing, wake devices, \_PTS
 * and \_WAK) goes through the interpreter here.  It loads on its own
 * kernel thread after boot.  The SCI is a real interrupt (through the I/O
 * APIC) whose handler wakes that thread to run the GPE and Notify work it
 * queued; without an I/O APIC the thread polls the events instead.
 */

#pragma once

#include "../include/types.h"

/* Start the "acpi" thread, which loads the namespace.  After
 * AcpiInitialize. */
void AmlInitialize(void);
/* The namespace is loaded and events are being handled */
bool AmlReady(void);

/* Was a power button (fixed or control-method) pressed since the last
 * call?  Only meaningful once AmlReady(). */
bool AmlPowerButtonPressed(void);

/* Around S3: \_PTS before the CPUs stop (wake GPEs only), \_WAK and the
 * runtime GPEs after.  No-ops before the namespace is loaded. */
void AmlPrepareSleep(void);
void AmlWake(void);

/* All batteries together, as Windows reports them (SYSTEM_BATTERY_STATE) */
typedef struct {
    bool   ac_online;
    bool   battery_present;
    bool   charging, discharging;
    UINT32 max_capacity;               /* mWh (last full charge) */
    UINT32 remaining_capacity;         /* mWh */
    INT32  rate;                       /* mW: negative while discharging */
    UINT32 estimated_time;             /* seconds left; 0xFFFFFFFF unknown */
    UINT32 alert_low, alert_warning;   /* mWh */
} AmlBatteryState;

/* The latest reading (refreshed every few seconds and on notifications);
 * on mains with no battery until the namespace is loaded. */
void AmlGetBatteryState(AmlBatteryState *out);

/* The lid (PNP0C0D): present, and closed as last read */
bool AmlLidPresent(void);
bool AmlLidClosed(void);
/* Did the lid close since the last call?  (The desktop sleeps then.) */
bool AmlLidClosedEvent(void);

/* Thermal zones: temperatures and trip points in tenths of a kelvin (0:
 * none), the polling period in tenths of a second (0: notifications only) */
typedef struct {
    char   name[40];
    UINT32 temp, passive, hot, critical, period;
    bool   cooling;                    /* at or above the passive trip point */
    UINT32 stamp;                      /* bumped when a reading changes */
} AmlThermalZone;
int AmlThermalZones(AmlThermalZone *out, int max);

/* What a zone's temperature asks for: AML_THERMAL_SLEEP (it reached _HOT)
 * or AML_THERMAL_SHUTDOWN (_CRT); AML_THERMAL_NONE.  Taken by the desktop. */
#define AML_THERMAL_NONE     0
#define AML_THERMAL_SLEEP    1
#define AML_THERMAL_SHUTDOWN 2
int AmlThermalRequest(void);

/* PCI interrupt routing (_PRT of the root bridge): the GSI that pin @pin
 * (0 = INTA) of device @dev on bus 0 is wired to, and how it triggers */
bool AmlPciIrq(UINT8 dev, UINT8 pin, UINT32 *gsi, bool *level, bool *low);

/* The SCI is an interrupt (false: polled) */
bool AmlSciIsInterrupt(void);
