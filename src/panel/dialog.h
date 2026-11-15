/*
 * dialog.h - dark modal dialogs (message/confirm, name input, about) matching the panel theme.
 */
#ifndef E1P_DIALOG_H
#define E1P_DIALOG_H

#include <windows.h>

enum { DLG_OK = 1, DLG_OKCANCEL = 2, DLG_YESNO = 3 };

/* Returns 1 for OK/Yes, 0 for Cancel/No/close. */
int  dlg_message(HWND owner, const wchar_t *title, const wchar_t *text, int buttons, int warn);
/* Text input. validate() returns NULL when the text is acceptable, else the error message to show.
   buf holds the initial text and receives the result. Returns 1 when confirmed. */
typedef const wchar_t *(*DlgValidate)(const wchar_t *text, void *ctx);
int  dlg_input(HWND owner, const wchar_t *title, const wchar_t *prompt, wchar_t *buf, int cch, int maxlen,
               DlgValidate validate, void *ctx);
void dlg_about(HWND owner, const wchar_t *version);
void dlg_register(HINSTANCE hi);

#endif /* E1P_DIALOG_H */
