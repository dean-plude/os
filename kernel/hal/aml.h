/*
 * aml.h — the ACPI namespace: AML run by uACPI (third_party/uacpi)
 *
 * acpi.c reads what it can straight from the tables; what the firmware
 * only describes in AML (batteries, AC adapters, control-method power
 * buttons, \_PTS and \_WAK) goes through the interpreter here.  It loads
 * on its own kernel thread after boot, which then also stands in for the
 * SCI: it polls the ACPI events and runs the GPE and Notify work they
 * queue (device interrupts stay off in NovaOS).
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
