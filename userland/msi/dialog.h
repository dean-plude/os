/*
 * dialog.h — the package's own dialogs (the Dialog, Control and
 * ControlEvent tables), shown by the InstallUISequence at full UI level
 */
#pragma once
#include "engine.h"

typedef struct Dlg Dlg;

/* The package has dialogs to show */
bool  dlg_available(MsiDb *db);
Dlg  *dlg_create(Inst *in);
void  dlg_destroy(Dlg *d);
bool  dlg_exists(Dlg *d, const char *name);
/* Show a dialog: a modal one until it ends (0, or MSI_ERROR_USEREXIT when
 * the user cancelled); a modeless one (the progress dialog) stays up and
 * returns at once */
int   dlg_run(Dlg *d, const char *name);
/* Engine events for the controls subscribed to them (EventMapping):
 * "ActionText", "ActionData" (@text), "SetProgress" (@a of @b) */
void  dlg_event(Dlg *d, const char *event, const char *text, int a, int b);
void  dlg_pump(Dlg *d);
/* The user cancelled from the modeless dialog */
bool  dlg_cancelled(Dlg *d);
/* The top dialog window, the owner of message boxes */
HWND  dlg_window(Dlg *d);
/* A property changed: refresh the conditions and values of the controls */
void  dlg_property_changed(Dlg *d, const char *prop);
