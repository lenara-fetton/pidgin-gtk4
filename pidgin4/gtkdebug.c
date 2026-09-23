/* PLACEHOLDER */
#include "pidgin-internal.h"
#include "pidgin.h"
#include "gtkdebug.h"
void pidgin_debug_init(void) { }
void pidgin_debug_uninit(void) { }
void *pidgin_debug_get_handle(void) { static int h; return &h; }
void pidgin_debug_window_show(void) { }
void pidgin_debug_window_hide(void) { }
PurpleDebugUiOps *pidgin_debug_get_ui_ops(void) { return NULL; }
