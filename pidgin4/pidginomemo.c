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
 * OMEMO fingerprints and trust over the core-omemo plugin's IPC
 * (doc/PIDGIN-UPGRADE.md, M8 "Landed (OMEMO)"):
 *   omemo-list-devices(account, jid or NULL for our own) -> GList of
 *     GHashTable (device-id, fingerprint, trust, active, session, own)
 *   omemo-set-trust(account, jid, guint device id, state) -> gboolean
 *   omemo-own-fingerprint(account) -> char *
 * The plugin has no call that lists every JID, so the window asks for our
 * own devices, then for each buddy of the account (and the JID typed in
 * the filter), and shows the JIDs that have devices.
 */
#include "pidgin-internal.h"
#include "pidgin.h"

#include "account.h"
#include "blist.h"
#include "debug.h"
#include "notify.h"
#include "plugin.h"
#include "signals.h"

#include "gtkplugin.h"
#include "gtkutils.h"
#include "pidginconvmeta.h"
#include "pidginomemo.h"
#include "pidginselftest.h"

#define OMEMO_PLUGIN_ID "core-omemo"

static const char *const trust_states[] = {
	"undecided", "trusted", "verified", "untrusted", NULL
};
static const char *const trust_labels[] = {
	N_("Undecided"), N_("Trusted"), N_("Verified"), N_("Untrusted"), NULL
};

/**************************************************************************
 * IPC
 **************************************************************************/

static PurplePlugin *test_plugin = NULL;

void
pidgin_omemo_set_plugin_for_tests(PurplePlugin *plugin)
{
	test_plugin = plugin;
}

static PurplePlugin *
omemo_plugin(void)
{
	PurplePlugin *plugin = test_plugin ? test_plugin
	                                   : purple_plugins_find_with_id(OMEMO_PLUGIN_ID);

	return (plugin != NULL && purple_plugin_is_loaded(plugin)) ? plugin : NULL;
}

gboolean
pidgin_omemo_is_available(void)
{
	return omemo_plugin() != NULL;
}

/* The command exists with @nparams parameters (the contract above). */
static PurplePlugin *
ipc_plugin(const char *command, int nparams)
{
	PurplePlugin *plugin = omemo_plugin();
	PurpleValue *ret = NULL, **params = NULL;
	int num = -1;

	if (plugin == NULL ||
	    !purple_plugin_ipc_get_params(plugin, command, &ret, &num, &params) ||
	    num != nparams) {
		if (plugin != NULL)
			purple_debug_warning("omemo-ui", "IPC %s is missing or differs\n",
			                     command);
		return NULL;
	}
	return plugin;
}

static GList *
ipc_list_devices(PurpleAccount *account, const char *jid)
{
	PurplePlugin *plugin = ipc_plugin("omemo-list-devices", 2);
	gboolean ok = FALSE;
	GList *list;

	if (plugin == NULL)
		return NULL;
	list = purple_plugin_ipc_call(plugin, "omemo-list-devices", &ok, account, jid);
	return ok ? list : NULL;
}

static char *
ipc_own_fingerprint(PurpleAccount *account)
{
	PurplePlugin *plugin = ipc_plugin("omemo-own-fingerprint", 1);
	gboolean ok = FALSE;
	char *fp;

	if (plugin == NULL)
		return NULL;
	fp = purple_plugin_ipc_call(plugin, "omemo-own-fingerprint", &ok, account);
	return ok ? fp : NULL;
}

static gboolean
ipc_set_trust(PurpleAccount *account, const char *jid, guint device_id,
              const char *state)
{
	PurplePlugin *plugin = ipc_plugin("omemo-set-trust", 4);
	gboolean ok = FALSE;
	gpointer ret;

	if (plugin == NULL)
		return FALSE;
	ret = purple_plugin_ipc_call(plugin, "omemo-set-trust", &ok, account, jid,
	                             device_id, state);
	return ok && GPOINTER_TO_INT(ret);
}

/* 8 groups of 8 hex digits (from whatever the plugin returns). */
static char *
format_fingerprint(const char *fp)
{
	GString *str;
	int n = 0;

	if (fp == NULL || *fp == '\0')
		return NULL;
	str = g_string_new(NULL);
	for (; *fp != '\0'; fp++) {
		if (!g_ascii_isxdigit(*fp))
			continue;
		if (n > 0 && n % 8 == 0)
			g_string_append_c(str, ' ');
		g_string_append_c(str, g_ascii_tolower(*fp));
		n++;
	}
	return g_string_free(str, FALSE);
}

/**************************************************************************
 * Rows
 **************************************************************************/

#define PIDGIN_TYPE_OMEMO_DEVICE (pidgin_omemo_device_get_type())
G_DECLARE_FINAL_TYPE(PidginOmemoDevice, pidgin_omemo_device, PIDGIN, OMEMO_DEVICE, GObject)

struct _PidginOmemoDevice {
	GObject parent;
	char *jid;
	guint device_id;
	char *fingerprint;      /* formatted, or NULL */
	char *trust;
	gboolean active;
	gboolean session;
	gboolean own;
};

G_DEFINE_TYPE(PidginOmemoDevice, pidgin_omemo_device, G_TYPE_OBJECT)

static void
pidgin_omemo_device_finalize(GObject *obj)
{
	PidginOmemoDevice *dev = PIDGIN_OMEMO_DEVICE(obj);

	g_free(dev->jid);
	g_free(dev->fingerprint);
	g_free(dev->trust);
	G_OBJECT_CLASS(pidgin_omemo_device_parent_class)->finalize(obj);
}

static void
pidgin_omemo_device_class_init(PidginOmemoDeviceClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = pidgin_omemo_device_finalize;
}

static void
pidgin_omemo_device_init(PidginOmemoDevice *dev)
{
}

static PidginOmemoDevice *
device_new(const char *jid, GHashTable *h)
{
	PidginOmemoDevice *dev = g_object_new(PIDGIN_TYPE_OMEMO_DEVICE, NULL);
	const char *id = g_hash_table_lookup(h, "device-id");

	dev->jid = g_strdup(jid);
	dev->device_id = id ? (guint)g_ascii_strtoull(id, NULL, 10) : 0;
	dev->fingerprint = format_fingerprint(g_hash_table_lookup(h, "fingerprint"));
	dev->trust = g_strdup(g_hash_table_lookup(h, "trust"));
	dev->active = purple_strequal(g_hash_table_lookup(h, "active"), "1");
	dev->session = purple_strequal(g_hash_table_lookup(h, "session"), "1");
	dev->own = purple_strequal(g_hash_table_lookup(h, "own"), "1");
	return dev;
}

/**************************************************************************
 * The window
 **************************************************************************/

typedef struct {
	GtkWidget *window;
	GtkWidget *unavailable;         /* the "not loaded" bar */
	GtkWidget *body;                /* everything greyed without the plugin */
	GtkWidget *account_dropdown;
	GtkWidget *own_fp;
	GtkWidget *filter_entry;
	GtkWidget *columnview;
	GtkWidget *status;
	GtkWidget *omemo2_note;         /* round 2: contacts that use OMEMO 2 */
	GString *omemo2_jids;
	GListStore *store;
	GtkCustomFilter *filter;
	guint n_jids;
} OmemoWindow;

/* XEP-0384 v0.8+ ("OMEMO 2"); the plugin speaks the legacy
 * eu.siacs.conversations.axolotl only. */
#define OMEMO2_NS "urn:xmpp:omemo:2"

static OmemoWindow *omemo_window = NULL;
static int handle;
static PurplePlugin *connected_plugin = NULL;

static gboolean
xmpp_account_filter(PurpleAccount *account)
{
	return purple_strequal(purple_account_get_protocol_id(account), "prpl-jabber");
}

static PurpleAccount *
selected_account(void)
{
	PurpleAccount *account;

	if (omemo_window == NULL)
		return NULL;
	account = pidgin_account_dropdown_get_selected(omemo_window->account_dropdown);
	return (account != NULL && g_list_find(purple_accounts_get_all(), account)) ?
		account : NULL;
}

static void
add_devices(PurpleAccount *account, const char *jid, const char *shown_jid,
            GHashTable *seen)
{
	GList *devices = ipc_list_devices(account, jid), *l;

	if (devices != NULL)
		omemo_window->n_jids++;
	/* devices, but their messages came as OMEMO 2 (XEP-0380 meta) */
	if (devices != NULL && jid != NULL &&
	    pidgin_conv_meta_saw_encryption(account, jid, OMEMO2_NS)) {
		if (omemo_window->omemo2_jids->len > 0)
			g_string_append(omemo_window->omemo2_jids, ", ");
		g_string_append(omemo_window->omemo2_jids, shown_jid);
	}
	for (l = devices; l != NULL; l = l->next) {
		PidginOmemoDevice *dev = device_new(shown_jid, l->data);
		char *key = g_strdup_printf("%s/%u", shown_jid, dev->device_id);

		if (!g_hash_table_contains(seen, key)) {
			g_hash_table_add(seen, key);
			g_list_store_append(omemo_window->store, dev);
		} else {
			g_free(key);
		}
		g_object_unref(dev);
	}
	g_list_free_full(devices, (GDestroyNotify)g_hash_table_unref);
}

static char *
bare_jid(const char *jid)
{
	const char *slash = strchr(jid, '/');

	return slash ? g_strndup(jid, slash - jid) : g_strdup(jid);
}

static void
refresh(void)
{
	PurpleAccount *account;
	gboolean available;
	GHashTable *seen, *jids;
	GSList *buddies, *l;
	const char *typed;
	char *fp, *text, *own_jid;

	if (omemo_window == NULL)
		return;

	available = pidgin_omemo_is_available();
	gtk_widget_set_visible(omemo_window->unavailable, !available);
	gtk_widget_set_sensitive(omemo_window->body, available);
	g_list_store_remove_all(omemo_window->store);
	omemo_window->n_jids = 0;
	g_string_truncate(omemo_window->omemo2_jids, 0);
	gtk_widget_set_visible(omemo_window->omemo2_note, FALSE);

	account = selected_account();
	if (!available || account == NULL) {
		gtk_label_set_text(GTK_LABEL(omemo_window->own_fp), "");
		gtk_label_set_text(GTK_LABEL(omemo_window->status),
			account == NULL && available ? _("No XMPP account.") : "");
		return;
	}

	fp = ipc_own_fingerprint(account);
	text = format_fingerprint(fp);
	gtk_label_set_text(GTK_LABEL(omemo_window->own_fp),
		text ? text : _("(not known yet)"));
	g_free(text);
	g_free(fp);

	seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	jids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

	own_jid = bare_jid(purple_account_get_username(account));
	add_devices(account, NULL, own_jid, seen);
	g_hash_table_add(jids, own_jid);

	buddies = purple_find_buddies(account, NULL);
	for (l = buddies; l != NULL; l = l->next) {
		char *jid = bare_jid(purple_buddy_get_name(l->data));

		if (g_hash_table_contains(jids, jid)) {
			g_free(jid);
			continue;
		}
		add_devices(account, jid, jid, seen);
		g_hash_table_add(jids, jid);
	}
	g_slist_free(buddies);

	/* A JID typed in the filter that is not a buddy (e.g. from a
	 * conversation window). */
	typed = gtk_editable_get_text(GTK_EDITABLE(omemo_window->filter_entry));
	if (typed != NULL && strchr(typed, '@') != NULL && strchr(typed, ' ') == NULL) {
		char *jid = bare_jid(typed);

		if (!g_hash_table_contains(jids, jid))
			add_devices(account, jid, jid, seen);
		g_free(jid);
	}

	g_hash_table_destroy(jids);
	g_hash_table_destroy(seen);

	text = g_strdup_printf(ngettext("%u device of %u contact", "%u devices of %u contacts",
		g_list_model_get_n_items(G_LIST_MODEL(omemo_window->store))),
		g_list_model_get_n_items(G_LIST_MODEL(omemo_window->store)),
		omemo_window->n_jids);
	gtk_label_set_text(GTK_LABEL(omemo_window->status), text);
	g_free(text);
	if (omemo_window->omemo2_jids->len > 0) {
		text = g_strdup_printf(_("OMEMO 2 (%s), which isn't supported, is used by: %s. "
		                         "Messages encrypted with it can't be read here, "
		                         "whatever the trust below."),
		                       OMEMO2_NS, omemo_window->omemo2_jids->str);
		gtk_label_set_text(GTK_LABEL(omemo_window->omemo2_note), text);
		gtk_widget_set_visible(omemo_window->omemo2_note, TRUE);
		g_free(text);
	}
	gtk_filter_changed(GTK_FILTER(omemo_window->filter), GTK_FILTER_CHANGE_DIFFERENT);
}

static gboolean
filter_func(gpointer item, gpointer data)
{
	PidginOmemoDevice *dev = item;
	const char *text;
	char *needle, *hay;
	gboolean match;

	if (omemo_window == NULL)
		return TRUE;
	text = gtk_editable_get_text(GTK_EDITABLE(omemo_window->filter_entry));
	if (text == NULL || *text == '\0')
		return TRUE;
	needle = g_utf8_casefold(text, -1);
	hay = g_utf8_casefold(dev->jid, -1);
	match = strstr(hay, needle) != NULL;
	g_free(needle);
	g_free(hay);
	return match;
}

static void
filter_changed_cb(GtkEditable *editable, gpointer data)
{
	gtk_filter_changed(GTK_FILTER(omemo_window->filter), GTK_FILTER_CHANGE_DIFFERENT);
}

static void
filter_activate_cb(GtkEntry *entry, gpointer data)
{
	/* Enter: also ask the plugin about a JID that is not a buddy. */
	refresh();
}

static void
account_changed_cb(GObject *dropdown, GParamSpec *pspec, gpointer data)
{
	refresh();
}

static void
refresh_cb(GtkWidget *button, gpointer data)
{
	refresh();
}

static void
plugins_cb(GtkWidget *button, gpointer data)
{
	pidgin_plugin_dialog_show();
}

static void
close_cb(GtkWidget *button, gpointer data)
{
	gtk_window_destroy(GTK_WINDOW(omemo_window->window));
}

static void
window_destroy_cb(GtkWidget *window, gpointer data)
{
	OmemoWindow *win = omemo_window;

	if (win == NULL)
		return;
	omemo_window = NULL;
	g_clear_object(&win->store);
	g_clear_object(&win->filter);
	g_string_free(win->omemo2_jids, TRUE);
	g_free(win);
}

/* Cells */

static void
label_setup_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	GtkWidget *label = gtk_label_new(NULL);

	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	if (GPOINTER_TO_INT(data)) {
		gtk_widget_add_css_class(label, "monospace");
		gtk_label_set_selectable(GTK_LABEL(label), TRUE);
		gtk_label_set_wrap(GTK_LABEL(label), TRUE);
		gtk_label_set_max_width_chars(GTK_LABEL(label), 36);
	} else {
		gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
	}
	gtk_list_item_set_child(li, label);
}

static void
jid_bind_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	PidginOmemoDevice *dev = gtk_list_item_get_item(li);

	gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(li)), dev->jid);
}

static void
id_bind_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	PidginOmemoDevice *dev = gtk_list_item_get_item(li);
	char *text = dev->own ?
		g_strdup_printf(_("%u (this device)"), dev->device_id) :
		g_strdup_printf("%u", dev->device_id);

	gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(li)), text);
	g_free(text);
}

static void
fp_bind_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	PidginOmemoDevice *dev = gtk_list_item_get_item(li);

	gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(li)),
		dev->fingerprint ? dev->fingerprint : _("(fingerprint not known yet)"));
}

static void
state_bind_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	PidginOmemoDevice *dev = gtk_list_item_get_item(li);
	char *text = g_strdup_printf("%s%s", dev->active ? _("active") : _("inactive"),
		dev->session ? _(", session") : "");

	gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(li)), text);
	g_free(text);
}

static int
trust_index(const char *trust)
{
	int i;

	for (i = 0; trust_states[i] != NULL; i++)
		if (purple_strequal(trust, trust_states[i]))
			return i;
	return 0;
}

static void
trust_changed_cb(GtkDropDown *dropdown, GParamSpec *pspec, gpointer data)
{
	PidginOmemoDevice *dev = g_object_get_data(G_OBJECT(dropdown), "pidgin-device");
	guint sel = gtk_drop_down_get_selected(dropdown);
	PurpleAccount *account = selected_account();
	const char *state;

	if (dev == NULL || sel >= G_N_ELEMENTS(trust_states) - 1)
		return;
	state = trust_states[sel];
	if (purple_strequal(state, dev->trust))
		return;
	if (account == NULL || !ipc_set_trust(account, dev->jid, dev->device_id, state)) {
		purple_notify_error(NULL, _("OMEMO Fingerprints"),
		                    _("Could not change the trust of this device."), NULL);
		/* Put it back. */
		g_signal_handlers_block_by_func(dropdown, trust_changed_cb, NULL);
		gtk_drop_down_set_selected(dropdown, trust_index(dev->trust));
		g_signal_handlers_unblock_by_func(dropdown, trust_changed_cb, NULL);
		return;
	}
	g_free(dev->trust);
	dev->trust = g_strdup(state);
}

static void
trust_setup_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	const char *labels[G_N_ELEMENTS(trust_labels)];
	GtkWidget *dropdown;
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(trust_labels); i++)
		labels[i] = trust_labels[i] ? _(trust_labels[i]) : NULL;
	dropdown = gtk_drop_down_new_from_strings(labels);
	gtk_accessible_update_property(GTK_ACCESSIBLE(dropdown),
		GTK_ACCESSIBLE_PROPERTY_LABEL, _("Trust"), -1);
	g_signal_connect(dropdown, "notify::selected", G_CALLBACK(trust_changed_cb), NULL);
	gtk_list_item_set_child(li, dropdown);
}

static void
trust_bind_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	PidginOmemoDevice *dev = gtk_list_item_get_item(li);
	GtkWidget *dropdown = gtk_list_item_get_child(li);

	g_signal_handlers_block_by_func(dropdown, trust_changed_cb, NULL);
	g_object_set_data_full(G_OBJECT(dropdown), "pidgin-device", g_object_ref(dev),
	                       g_object_unref);
	gtk_drop_down_set_selected(GTK_DROP_DOWN(dropdown), trust_index(dev->trust));
	gtk_widget_set_sensitive(dropdown, !dev->own);
	g_signal_handlers_unblock_by_func(dropdown, trust_changed_cb, NULL);
}

static void
trust_unbind_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	g_object_set_data(G_OBJECT(gtk_list_item_get_child(li)), "pidgin-device", NULL);
}

static void
add_column(GtkWidget *columnview, const char *title, GCallback setup,
           gpointer setup_data, GCallback bind, GCallback unbind, gboolean expand)
{
	GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
	GtkColumnViewColumn *column;

	g_signal_connect(factory, "setup", setup, setup_data);
	g_signal_connect(factory, "bind", bind, NULL);
	if (unbind != NULL)
		g_signal_connect(factory, "unbind", unbind, NULL);
	column = gtk_column_view_column_new(title, factory);
	gtk_column_view_column_set_expand(column, expand);
	gtk_column_view_column_set_resizable(column, TRUE);
	gtk_column_view_append_column(GTK_COLUMN_VIEW(columnview), column);
	g_object_unref(column);
}

static PurpleAccount *
first_xmpp_account(void)
{
	GList *l;

	for (l = purple_accounts_get_all(); l != NULL; l = l->next)
		if (xmpp_account_filter(l->data))
			return l->data;
	return NULL;
}

static void
create_window(PurpleAccount *account)
{
	OmemoWindow *win;
	GtkWidget *content, *bar, *label, *button, *grid, *sw, *hbox;
	GtkFilterListModel *filtered;
	GtkNoSelection *selection;

	omemo_window = win = g_new0(OmemoWindow, 1);
	win->window = pidgin_dialog_new(_("OMEMO Fingerprints"), NULL,
	                                "omemo", TRUE);
	gtk_window_set_default_size(GTK_WINDOW(win->window), 820, 480);
	g_signal_connect(win->window, "destroy", G_CALLBACK(window_destroy_cb), NULL);
	content = pidgin_dialog_get_content_area(win->window);

	/* Shown while the plugin is not loaded. */
	bar = win->unavailable = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PIDGIN_HIG_BORDER);
	gtk_box_append(GTK_BOX(bar), gtk_image_new_from_icon_name("dialog-information-symbolic"));
	label = gtk_label_new(_("The OMEMO plugin is not loaded."));
	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_label_set_wrap(GTK_LABEL(label), TRUE);
	gtk_widget_set_hexpand(label, TRUE);
	gtk_box_append(GTK_BOX(bar), label);
	button = gtk_button_new_with_mnemonic(_("_Plugins..."));
	g_signal_connect(button, "clicked", G_CALLBACK(plugins_cb), NULL);
	gtk_box_append(GTK_BOX(bar), button);
	gtk_box_append(GTK_BOX(content), bar);

	win->body = gtk_box_new(GTK_ORIENTATION_VERTICAL, PIDGIN_HIG_BOX_SPACE);
	gtk_widget_set_vexpand(win->body, TRUE);
	gtk_box_append(GTK_BOX(content), win->body);

	grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(grid), PIDGIN_HIG_BOX_SPACE);
	gtk_grid_set_column_spacing(GTK_GRID(grid), PIDGIN_HIG_BORDER);
	gtk_box_append(GTK_BOX(win->body), grid);

	label = gtk_label_new_with_mnemonic(_("_Account:"));
	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_grid_attach(GTK_GRID(grid), label, 0, 0, 1, 1);
	win->account_dropdown = pidgin_account_dropdown_new(
		account ? account : first_xmpp_account(), TRUE,
		(PurpleFilterAccountFunc)xmpp_account_filter, NULL);
	gtk_widget_set_hexpand(win->account_dropdown, TRUE);
	gtk_grid_attach(GTK_GRID(grid), win->account_dropdown, 1, 0, 1, 1);
	pidgin_set_accessible_label(win->account_dropdown, label);

	label = gtk_label_new(_("Your fingerprint:"));
	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_grid_attach(GTK_GRID(grid), label, 0, 1, 1, 1);
	win->own_fp = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(win->own_fp), 0.0);
	gtk_label_set_selectable(GTK_LABEL(win->own_fp), TRUE);
	gtk_widget_add_css_class(win->own_fp, "monospace");
	gtk_grid_attach(GTK_GRID(grid), win->own_fp, 1, 1, 1, 1);

	label = gtk_label_new_with_mnemonic(_("_Contact:"));
	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_grid_attach(GTK_GRID(grid), label, 0, 2, 1, 1);
	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PIDGIN_HIG_BOX_SPACE);
	win->filter_entry = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(win->filter_entry),
		_("Filter by JID; Enter looks up a JID that is not a buddy"));
	gtk_widget_set_hexpand(win->filter_entry, TRUE);
	g_signal_connect(win->filter_entry, "changed", G_CALLBACK(filter_changed_cb), NULL);
	g_signal_connect(win->filter_entry, "activate", G_CALLBACK(filter_activate_cb), NULL);
	gtk_box_append(GTK_BOX(hbox), win->filter_entry);
	button = gtk_button_new_with_mnemonic(_("_Refresh"));
	g_signal_connect(button, "clicked", G_CALLBACK(refresh_cb), NULL);
	gtk_box_append(GTK_BOX(hbox), button);
	gtk_grid_attach(GTK_GRID(grid), hbox, 1, 2, 1, 1);
	pidgin_set_accessible_label(win->filter_entry, label);

	win->store = g_list_store_new(PIDGIN_TYPE_OMEMO_DEVICE);
	win->filter = gtk_custom_filter_new(filter_func, NULL, NULL);
	filtered = gtk_filter_list_model_new(G_LIST_MODEL(g_object_ref(win->store)),
	                                     GTK_FILTER(g_object_ref(win->filter)));
	selection = gtk_no_selection_new(G_LIST_MODEL(filtered));
	win->columnview = gtk_column_view_new(GTK_SELECTION_MODEL(selection));
	gtk_column_view_set_show_column_separators(GTK_COLUMN_VIEW(win->columnview), FALSE);
	add_column(win->columnview, _("JID"), G_CALLBACK(label_setup_cb), GINT_TO_POINTER(FALSE),
	           G_CALLBACK(jid_bind_cb), NULL, TRUE);
	add_column(win->columnview, _("Device"), G_CALLBACK(label_setup_cb),
	           GINT_TO_POINTER(FALSE), G_CALLBACK(id_bind_cb), NULL, FALSE);
	add_column(win->columnview, _("Fingerprint"), G_CALLBACK(label_setup_cb),
	           GINT_TO_POINTER(TRUE), G_CALLBACK(fp_bind_cb), NULL, TRUE);
	add_column(win->columnview, _("State"), G_CALLBACK(label_setup_cb),
	           GINT_TO_POINTER(FALSE), G_CALLBACK(state_bind_cb), NULL, FALSE);
	add_column(win->columnview, _("Trust"), G_CALLBACK(trust_setup_cb), NULL,
	           G_CALLBACK(trust_bind_cb), G_CALLBACK(trust_unbind_cb), FALSE);
	sw = pidgin_make_scrollable(win->columnview, GTK_POLICY_AUTOMATIC,
	                            GTK_POLICY_AUTOMATIC, -1, 240);
	gtk_widget_set_vexpand(sw, TRUE);
	gtk_box_append(GTK_BOX(win->body), sw);

	win->status = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(win->status), 0.0);
	gtk_widget_add_css_class(win->status, "dim-label");
	gtk_box_append(GTK_BOX(win->body), win->status);

	win->omemo2_jids = g_string_new(NULL);
	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PIDGIN_HIG_BOX_SPACE);
	gtk_box_append(GTK_BOX(hbox), gtk_image_new_from_icon_name("dialog-warning-symbolic"));
	label = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_label_set_wrap(GTK_LABEL(label), TRUE);
	gtk_widget_set_hexpand(label, TRUE);
	gtk_widget_set_name(label, "pidgin-omemo2-note");
	gtk_box_append(GTK_BOX(hbox), label);
	gtk_box_append(GTK_BOX(win->body), hbox);
	win->omemo2_note = label;
	g_object_bind_property(label, "visible", hbox, "visible", G_BINDING_SYNC_CREATE);
	gtk_widget_set_visible(label, FALSE);

	pidgin_dialog_add_button(win->window, _("_Close"), G_CALLBACK(close_cb), NULL);

	g_signal_connect(win->account_dropdown, "notify::selected",
	                 G_CALLBACK(account_changed_cb), NULL);
}

void
pidgin_omemo_show_fingerprints(PurpleAccount *account, const char *jid)
{
	if (account != NULL && !xmpp_account_filter(account))
		account = NULL;

	if (omemo_window == NULL)
		create_window(account);
	else if (account != NULL)
		pidgin_account_dropdown_set_selected(omemo_window->account_dropdown, account);

	if (jid != NULL) {
		char *bare = bare_jid(jid);
		gtk_editable_set_text(GTK_EDITABLE(omemo_window->filter_entry), bare);
		g_free(bare);
	}
	refresh();
	pidgin_window_set_secondary(GTK_WINDOW(omemo_window->window));
	gtk_window_present(GTK_WINDOW(omemo_window->window));
}

/**************************************************************************
 * Signals
 **************************************************************************/

static void
new_device_cb(PurpleAccount *account, const char *jid, guint device_id,
              const char *fingerprint, gpointer data)
{
	if (omemo_window != NULL && account == selected_account())
		refresh();
}

static void
connect_plugin(PurplePlugin *plugin)
{
	if (connected_plugin == plugin)
		return;
	connected_plugin = plugin;
	purple_signal_connect(plugin, "omemo-new-device", &handle,
	                      PURPLE_CALLBACK(new_device_cb), NULL);
}

static void
plugin_load_cb(PurplePlugin *plugin, gpointer data)
{
	if (!purple_strequal(purple_plugin_get_id(plugin), OMEMO_PLUGIN_ID))
		return;
	connect_plugin(plugin);
	refresh();
}

static void
plugin_unload_cb(PurplePlugin *plugin, gpointer data)
{
	if (!purple_strequal(purple_plugin_get_id(plugin), OMEMO_PLUGIN_ID))
		return;
	/* The plugin unregistered its signals (and our handler) already. */
	connected_plugin = NULL;
	refresh();
}

void
pidgin_omemo_init(void)
{
	PurplePlugin *plugin;

	purple_signal_connect(purple_plugins_get_handle(), "plugin-load", &handle,
	                      PURPLE_CALLBACK(plugin_load_cb), NULL);
	purple_signal_connect(purple_plugins_get_handle(), "plugin-unload", &handle,
	                      PURPLE_CALLBACK(plugin_unload_cb), NULL);
	if ((plugin = omemo_plugin()) != NULL)
		connect_plugin(plugin);
}

void
pidgin_omemo_uninit(void)
{
	purple_signals_disconnect_by_handle(&handle);
	connected_plugin = NULL;
	if (omemo_window != NULL)
		gtk_window_destroy(GTK_WINDOW(omemo_window->window));
}

/**************************************************************************
 * Selftest
 **************************************************************************/

#define MODULE "omemo"

void
pidgin_omemo_selftest(void)
{
	gboolean available = pidgin_omemo_is_available();

	pidgin_selftest_log(MODULE, "plugin %s", available ? "loaded" : "not loaded");
	pidgin_omemo_show_fingerprints(NULL, NULL);
	pidgin_selftest_iterate(150);
	if (omemo_window == NULL) {
		pidgin_selftest_fail(MODULE, "the window did not open");
		return;
	}
	if (gtk_widget_get_visible(omemo_window->unavailable) == available)
		pidgin_selftest_fail(MODULE, "the not-loaded bar is wrong");
	if (gtk_widget_get_sensitive(omemo_window->body) != available)
		pidgin_selftest_fail(MODULE, "the body sensitivity is wrong");
	pidgin_selftest_log(MODULE, "%s; own fingerprint: %s",
		gtk_label_get_text(GTK_LABEL(omemo_window->status)),
		gtk_label_get_text(GTK_LABEL(omemo_window->own_fp)));

	/* With a JID, as the conversation window calls it. */
	pidgin_omemo_show_fingerprints(NULL, "someone@example.com/resource");
	pidgin_selftest_iterate(50);
	if (!purple_strequal(gtk_editable_get_text(GTK_EDITABLE(omemo_window->filter_entry)),
	                     "someone@example.com"))
		pidgin_selftest_fail(MODULE, "the JID filter was not set");

	gtk_window_destroy(GTK_WINDOW(omemo_window->window));
	pidgin_selftest_iterate(50);
	if (omemo_window != NULL)
		pidgin_selftest_fail(MODULE, "the window did not close");
}
