/* PLACEHOLDER: being ported from pidgin/gtkaccount.c. */
#include "pidgin-internal.h"
#include "pidgin.h"
#include "gtkaccount.h"

void pidgin_accounts_window_show(void) { }
void pidgin_accounts_window_hide(void) { }
void pidgin_account_dialog_show(PidginAccountDialogType type, PurpleAccount *account) { }
PurpleAccountUiOps *pidgin_accounts_get_ui_ops(void) { return NULL; }
void *pidgin_account_get_handle(void) { static int handle; return &handle; }
void pidgin_account_init(void) { }
void pidgin_account_uninit(void) { }
