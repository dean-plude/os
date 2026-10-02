/*
 * engine.h — what the installation engine (install.c) offers the handle
 * API (api.c), the dialogs (dialog.c) and custom actions: the running
 * installation's properties, conditions, formatting, folders, feature
 * and component states, actions and messages.
 */
#pragma once
#include <windows.h>
#include "msi_int.h"

typedef struct Inst Inst;

/* Win32 codes the headers lack */
#ifndef ERROR_INSTALL_FAILURE
#define ERROR_INSTALL_FAILURE  1603
#endif
#ifndef ERROR_INVALID_DATATYPE
#define ERROR_INVALID_DATATYPE 1804
#endif
#ifndef ERROR_READ_FAULT
#define ERROR_READ_FAULT       30
#endif

/* MSIRUNMODE_* */
#define RUNMODE_ADMIN          0
#define RUNMODE_ADVERTISE      1
#define RUNMODE_MAINTENANCE    2
#define RUNMODE_ROLLBACKENABLED 3
#define RUNMODE_LOGENABLED     4
#define RUNMODE_OPERATIONS     5
#define RUNMODE_REBOOTATEND    6
#define RUNMODE_REBOOTNOW      7
#define RUNMODE_CABINET        8
#define RUNMODE_SOURCESHORTNAMES 9
#define RUNMODE_TARGETSHORTNAMES 10
#define RUNMODE_WINDOWS9X      12
#define RUNMODE_ZAWENABLED     13
#define RUNMODE_SCHEDULED      16
#define RUNMODE_ROLLBACK       17
#define RUNMODE_COMMIT         18

/* INSTALLMESSAGE_* (high byte of the message type) */
#define IMSG_FATALEXIT      0x00000000
#define IMSG_ERROR          0x01000000
#define IMSG_WARNING        0x02000000
#define IMSG_USER           0x03000000
#define IMSG_INFO           0x04000000
#define IMSG_FILESINUSE     0x05000000
#define IMSG_RESOLVESOURCE  0x06000000
#define IMSG_OUTOFDISKSPACE 0x07000000
#define IMSG_ACTIONSTART    0x08000000
#define IMSG_ACTIONDATA     0x09000000
#define IMSG_PROGRESS       0x0A000000
#define IMSG_COMMONDATA     0x0B000000
#define IMSG_INITIALIZE     0x0C000000
#define IMSG_TERMINATE      0x0D000000
#define IMSG_SHOWDIALOG     0x0E000000

/* INSTALLSTATE_* */
#define ISTATE_UNKNOWN   (-1)
#define ISTATE_ADVERTISED 1
#define ISTATE_ABSENT    2
#define ISTATE_LOCAL     3
#define ISTATE_SOURCE    4
#define ISTATE_DEFAULT   5

const char *eng_get_prop(Inst *in, const char *name);
void        eng_set_prop(Inst *in, const char *name, const char *value);
MsiDb      *eng_db(Inst *in);
bool        eng_condition(Inst *in, const char *cond);
/* Formatted text; @rec (may be NULL) answers [1], [2]... */
void        eng_format(Inst *in, const MsiRec *rec, const char *s, char *out, int cap);
int         eng_target_path(Inst *in, const char *folder, char *out, int cap);
int         eng_source_path(Inst *in, const char *folder, char *out, int cap);
int         eng_set_target_path(Inst *in, const char *folder, const char *path);
int         eng_do_action(Inst *in, const char *action);
int         eng_sequence(Inst *in, const char *table);
bool        eng_get_mode(Inst *in, int mode);
int         eng_set_mode(Inst *in, int mode, bool state);
int         eng_feature_state(Inst *in, const char *feature, int *installed, int *action);
int         eng_set_feature_state(Inst *in, const char *feature, int state);
int         eng_component_state(Inst *in, const char *comp, int *installed, int *action);
int         eng_set_component_state(Inst *in, const char *comp, int state);
int         eng_set_install_level(Inst *in, int level);
/* MsiProcessMessage: logging, the UI's action text and progress, message
 * boxes; returns IDOK/IDCANCEL/... or 0 when nothing handled it */
int         eng_message(Inst *in, int type, const MsiRec *rec);
void        eng_log(Inst *in, const char *fmt, ...);

/* api.c: handles for the engine's own use (custom actions, dialogs) */
unsigned    api_session_handle(Inst *in);
void        api_close_handles_since(unsigned mark);
unsigned    api_handle_mark(void);
/* Run a DLL custom action in a custom-action server process of the DLL's
 * architecture (32-bit DLLs in SysWOW64's msiexec, as Windows does) */
int         api_run_dll_action(Inst *in, const WCHAR *dll, const char *entry, bool *crashed);
/* Wait for a process while serving the UI; returns its exit code */
DWORD       api_wait_process(Inst *in, HANDLE proc);
/* ui.c / dialog.c pump */
void        eng_pump(Inst *in);
