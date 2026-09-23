/* PLACEHOLDER */
#include "pidgin-internal.h"
#include "pidgin.h"
#include "connection.h"
#include "gtkconn.h"
PurpleConnectionUiOps *pidgin_connections_get_ui_ops(void) { return NULL; }
void *pidgin_connection_get_handle(void) { static int h; return &h; }
void pidgin_connection_init(void) { }
void pidgin_connection_uninit(void) { }
