/*
 * ec.h — the ACPI embedded controller (see ec.c)
 */

#pragma once

#include "../include/types.h"

/* Find the controller (ECDT or PNP0C09) and give uACPI its EmbeddedControl
 * address space and its events.  After the namespace is loaded and before
 * it is initialized (the controller's _REG runs before any _INI). */
void EcProbe(void);
bool EcPresent(void);

/* Run the events the controller has waiting (a GPE edge may go missing);
 * on the acpi thread */
void EcPoll(void);
