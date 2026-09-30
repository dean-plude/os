/* ui.h — the installer's progress window */
#pragma once
#include <stdbool.h>

typedef struct MsiUi MsiUi;

MsiUi *msiui_create(void);
/* Show the window for @product ("Installing ..." / "Removing ...") */
void   msiui_begin(MsiUi *ui, const char *product, bool removing);
void   msiui_status(MsiUi *ui, const char *text);        /* "Copying new files: x" */
void   msiui_progress(MsiUi *ui, int done, int total);
bool   msiui_cancelled(MsiUi *ui);                       /* pumps messages */
/* Close the window; with @message_box, tell the user the result (@err on
 * failure).  Frees the UI. */
void   msiui_end(MsiUi *ui, int result, bool message_box, const char *err);
