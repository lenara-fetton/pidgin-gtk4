/*
 * pidgin4
 *
 * Pidgin is the legal property of its developers, whose names are too numerous
 * to list here.  Please refer to the COPYRIGHT file distributed with this
 * source distribution.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111-1301  USA
 */

/*
 * The pieces of libpurple's uninstalled internal.h that the pidgin/ sources
 * rely on. pidgin4 builds against an installed libpurple, so it cannot use
 * internal.h itself; every pidgin4 .c file includes this header first.
 */
#ifndef _PIDGIN_INTERNAL_H_
#define _PIDGIN_INTERNAL_H_

#include "config.h"

#include <locale.h>
#include <libintl.h>

/* Translations use the "pidgin" domain that libpurple and Pidgin 2 share;
 * the catalogs come from the libpurple prefix (LOCALEDIR). */
#undef _
#define _(String) ((const char *)dgettext(PACKAGE, String))
#undef N_
#define N_(String) (String)
#undef ngettext
#define ngettext(Singular, Plural, Number) \
	((const char *)dngettext(PACKAGE, Singular, Plural, Number))

#define MSG_LEN 2048
#define BUF_LEN MSG_LEN
#define BUF_LONG (BUF_LEN * 2)

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <glib-object.h>
#include <gmodule.h>

#include "glibcompat.h"

#if SIZEOF_TIME_T == 4
#	define PURPLE_TIME_T_MODIFIER "lu"
#elif SIZEOF_TIME_T == 8
#	define PURPLE_TIME_T_MODIFIER "zu"
#else
#	error Unknown size of time_t
#endif

#define purple_strlcpy(dest, src) g_strlcpy(dest, src, sizeof(dest))
#define purple_strlcat(dest, src) g_strlcat(dest, src, sizeof(dest))

#define PURPLE_WEBSITE "https://pidgin.im/"
#define PURPLE_DEVEL_WEBSITE "https://pidgin.im/development/"

#endif /* _PIDGIN_INTERNAL_H_ */
