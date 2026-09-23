/* PLACEHOLDER: being ported from pidgin/gtkrequest.c. */
#include "pidgin-internal.h"
#include "pidgin.h"
#include "gtkrequest.h"

static PurpleRequestUiOps ops = { NULL };

PurpleRequestUiOps *
pidgin_request_get_ui_ops(void)
{
	return &ops;
}
