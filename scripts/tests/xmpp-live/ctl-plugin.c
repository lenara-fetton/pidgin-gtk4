/*
 * ctl-plugin.c: a test-only libpurple plugin for the live XMPP scenario.
 *
 * run.sh builds it against the prefix's libpurple and loads it into each
 * pidgin4 instance of the throwaway profiles.  Every 250 ms it looks for
 * the file named by $XMPP_LIVE_CTL, runs each of its lines as a command
 * on the instance's (only) account, deletes the file and logs the results
 * to the -d log as "xmpp-live-ctl: ...".  It gives drive.sh a way to use
 * libpurple API and prpl IPC that has no UI yet (blocking, reporting,
 * invisibility, idle), without screen coordinates.
 *
 * Commands:
 *   block JID | unblock JID      purple_privacy_deny_add/remove (not local)
 *   report JID [abuse] [TEXT]    IPC report-spam
 *   modes                        IPC privacy-modes
 *   caps                         IPC status-invisible-supported and
 *                                report-spam-supported
 *   status ID                    purple_account_set_status(ID)
 *   idle SECONDS                 account idle since SECONDS ago (0: not idle)
 *   send JID TEXT                an IM, as if typed
 *   sendfile JID PATH            serv_send_file (HTTP upload when available)
 *   buddy-idle JID               the buddy's idle state as libpurple sees it
 *   join ROOM@SERVER NICK        serv_join_chat, as from the buddy list
 */
#define PURPLE_PLUGINS

#include <string.h>
#include <stdlib.h>
#include <glib.h>
#include <glib/gstdio.h>

#include "account.h"
#include "blist.h"
#include "conversation.h"
#include "debug.h"
#include "plugin.h"
#include "privacy.h"
#include "server.h"
#include "status.h"
#include "version.h"

static guint timer = 0;

static PurpleAccount *
the_account(void)
{
	GList *l = purple_accounts_get_all();
	return l ? l->data : NULL;
}

static PurplePlugin *
jabber(void)
{
	return purple_plugins_find_with_id("prpl-jabber");
}

static void
run(PurpleAccount *account, char *line)
{
	char **argv = g_strsplit(g_strstrip(line), " ", 3);
	const char *cmd = argv[0], *a1 = argv[0] ? argv[1] : NULL;
	const char *a2 = a1 ? argv[2] : NULL;

	if (cmd == NULL || *cmd == '\0') {
		g_strfreev(argv);
		return;
	}
	purple_debug_info("xmpp-live-ctl", "> %s\n", line);

	if (purple_strequal(cmd, "block") && a1) {
		purple_privacy_deny_add(account, a1, FALSE);
	} else if (purple_strequal(cmd, "unblock") && a1) {
		purple_privacy_deny_remove(account, a1, FALSE);
	} else if (purple_strequal(cmd, "report") && a1) {
		gboolean abuse = purple_strequal(a2, "abuse");
		gboolean ok = FALSE;
		gboolean sent = GPOINTER_TO_INT(purple_plugin_ipc_call(jabber(),
				"report-spam", &ok, account, a1,
				abuse ? NULL : a2, (guint)abuse));
		purple_debug_info("xmpp-live-ctl", "report-spam: %s (dispatched %d)\n",
		                  sent ? "sent" : "not sent", ok);
	} else if (purple_strequal(cmd, "modes")) {
		GList *modes = purple_plugin_ipc_call(jabber(), "privacy-modes", NULL,
		                                      account), *l;
		GString *s = g_string_new("");
		for (l = modes; l; l = l->next)
			g_string_append_printf(s, " %s", (char *)l->data);
		purple_debug_info("xmpp-live-ctl", "privacy-modes:%s\n", s->str);
		g_string_free(s, TRUE);
		g_list_free_full(modes, g_free);
	} else if (purple_strequal(cmd, "caps")) {
		purple_debug_info("xmpp-live-ctl", "status-invisible-supported: %d, "
			"report-spam-supported: %d\n",
			GPOINTER_TO_INT(purple_plugin_ipc_call(jabber(),
				"status-invisible-supported", NULL, account)),
			GPOINTER_TO_INT(purple_plugin_ipc_call(jabber(),
				"report-spam-supported", NULL, account)));
	} else if (purple_strequal(cmd, "status") && a1) {
		purple_account_set_status(account, a1, TRUE, NULL);
	} else if (purple_strequal(cmd, "idle") && a1) {
		int secs = atoi(a1);
		purple_presence_set_idle(purple_account_get_presence(account),
		                         secs > 0, secs > 0 ? time(NULL) - secs : 0);
	} else if (purple_strequal(cmd, "send") && a1 && a2) {
		PurpleConversation *conv = purple_conversation_new(PURPLE_CONV_TYPE_IM,
		                                                   account, a1);
		purple_conv_im_send(PURPLE_CONV_IM(conv), a2);
	} else if (purple_strequal(cmd, "sendfile") && a1 && a2) {
		serv_send_file(purple_account_get_connection(account), a1, a2);
	} else if (purple_strequal(cmd, "join") && a1 && a2 && strchr(a1, '@')) {
		GHashTable *comps = g_hash_table_new_full(g_str_hash, g_str_equal,
		                                          NULL, g_free);
		const char *at = strchr(a1, '@');
		g_hash_table_insert(comps, "room", g_strndup(a1, at - a1));
		g_hash_table_insert(comps, "server", g_strdup(at + 1));
		g_hash_table_insert(comps, "handle", g_strdup(a2));
		serv_join_chat(purple_account_get_connection(account), comps);
		g_hash_table_destroy(comps);
	} else if (purple_strequal(cmd, "buddy-idle") && a1) {
		PurpleBuddy *b = purple_find_buddy(account, a1);
		PurplePresence *p = b ? purple_buddy_get_presence(b) : NULL;
		time_t since = p ? purple_presence_get_idle_time(p) : 0;
		purple_debug_info("xmpp-live-ctl", "buddy-idle %s: %s, %ld s\n", a1,
		                  p && purple_presence_is_idle(p) ? "idle" : "not idle",
		                  p && since ? (long)(time(NULL) - since) : -1L);
	} else {
		purple_debug_warning("xmpp-live-ctl", "unknown command: %s\n", line);
	}
	g_strfreev(argv);
}

static gboolean
poll_cb(gpointer data)
{
	const char *path = g_getenv("XMPP_LIVE_CTL");
	PurpleAccount *account = the_account();
	char *contents = NULL, *taken, **lines, **l;

	if (path == NULL || account == NULL || !purple_account_is_connected(account))
		return TRUE;
	/* rename first: drive.sh may write the next file meanwhile */
	taken = g_strconcat(path, ".taken", NULL);
	if (g_rename(path, taken) != 0) {
		g_free(taken);
		return TRUE;
	}
	if (g_file_get_contents(taken, &contents, NULL, NULL)) {
		lines = g_strsplit(contents, "\n", -1);
		for (l = lines; *l; l++)
			run(account, *l);
		g_strfreev(lines);
		g_free(contents);
	}
	g_unlink(taken);
	g_free(taken);
	return TRUE;
}

static gboolean
plugin_load(PurplePlugin *plugin)
{
	timer = purple_timeout_add(250, poll_cb, NULL);
	return TRUE;
}

static gboolean
plugin_unload(PurplePlugin *plugin)
{
	if (timer)
		purple_timeout_remove(timer);
	timer = 0;
	return TRUE;
}

static PurplePluginInfo info = {
	PURPLE_PLUGIN_MAGIC, PURPLE_MAJOR_VERSION, PURPLE_MINOR_VERSION,
	PURPLE_PLUGIN_STANDARD, NULL, 0, NULL, PURPLE_PRIORITY_DEFAULT,
	"core-xmpplive-ctl", "xmpp-live control", "1.0",
	"Test-only command file for scripts/tests/xmpp-live",
	"Test-only command file for scripts/tests/xmpp-live",
	NULL, NULL,
	plugin_load, plugin_unload, NULL, NULL, NULL, NULL, NULL,
	NULL, NULL, NULL, NULL
};

static void
init_plugin(PurplePlugin *plugin)
{
}

PURPLE_INIT_PLUGIN(xmpplive_ctl, init_plugin, info)
