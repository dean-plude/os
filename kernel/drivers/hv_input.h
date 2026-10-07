/*
 * hv_input.h — Hyper-V's synthetic keyboard and mouse (VMBus channels)
 */

#pragma once

#include "vmbus.h"

/* vmbus.c opens these channels when the host offers them */
extern const VmbusDriver HvKeyboardDriver;
extern const VmbusDriver HvMouseDriver;
