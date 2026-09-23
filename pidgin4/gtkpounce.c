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
 * Buddy pounces: port of pidgin/gtkpounce.c. The manager is a
 * GtkColumnView, the editor a plain window; the pounce handler and its
 * five actions behave as in Pidgin 2, and pounces.xml keeps its format
 * (libpurple writes it; the editor sets the same action attributes).
 */
#include "pidgin-internal.h"
#include "pidgin.h"

#include <string.h>

#include "account.h"
#include "connection.h"
#include "conversation.h"
#include "debug.h"
#include "notify.h"
#include "pounce.h"
#include "prefs.h"
#include "request.h"
#include "sound.h"
#include "util.h"

#include "gtkpounce.h"
#include "gtkutils.h"
#include "pidgincomposeentry.h"
#include "pidginselftest.h"

#define PREFS_DIALOG "/pidgin4/pounces"

/**************************************************************************
 * The events, in Pidgin 2's order and with its labels
 **************************************************************************/

static const struct {
	PurplePounceEvent event;
	const char *label;
} pounce_events[] = {
	{ PURPLE_POUNCE_MESSAGE_RECEIVED, N_("Sends a _message") },
	{ PURPLE_POUNCE_SIGNON,           N_("Si_gns on") },
	{ PURPLE_POUNCE_SIGNOFF,          N_("Signs o_ff") },
	{ PURPLE_POUNCE_AWAY,             N_("Goes a_way") },
	{ PURPLE_POUNCE_AWAY_RETURN,      N_("Ret_urns from away") },
	{ PURPLE_POUNCE_IDLE,             N_("Becomes _idle") },
	{ PURPLE_POUNCE_IDLE_RETURN,      N_("Is no longer i_dle") },
	{ PURPLE_POUNCE_TYPING,           N_("Starts _typing") },
	{ PURPLE_POUNCE_TYPED,            N_("P_auses while typing") },
	{ PURPLE_POUNCE_TYPING_STOPPED,   N_("Stops t_yping") },
};

#define N_EVENTS G_N_ELEMENTS(pounce_events)

/* A label without its mnemonic underscore. */
static char *
strip_mnemonic(const char *label)
{
	GString *str = g_string_new(NULL);
	const char *p;

	for (p = label; *p; p++) {
		if (*p == '_' && p[1] != '_')
			continue;
		g_string_append_c(str, *p);
	}
	return g_string_free(str, FALSE);
}

static char *
events_summary(PurplePounceEvent events)
{
	GString *str = g_string_new(NULL);
	gsize i;

	for (i = 0; i < N_EVENTS; i++) {
		char *label;

		if (!(events & pounce_events[i].event))
			continue;
		label = strip_mnemonic(_(pounce_events[i].label));
		if (str->len > 0)
			g_string_append(str, ", ");
		g_string_append(str, label);
		g_free(label);
	}
	return g_string_free(str, FALSE);
}

static gboolean
pounce_exists(PurplePounce *pounce)
{
	return pounce != NULL && g_list_find(purple_pounces_get_all(), pounce) != NULL;
}

/**************************************************************************
 * PidginPounceRow: one pounce in the manager's list
 **************************************************************************/

#define PIDGIN_TYPE_POUNCE_ROW (pidgin_pounce_row_get_type())
G_DECLARE_FINAL_TYPE(PidginPounceRow, pidgin_pounce_row, PIDGIN, POUNCE_ROW, GObject)

struct _PidginPounceRow {
	GObject parent;
	PurplePounce *pounce;
};

G_DEFINE_FINAL_TYPE(PidginPounceRow, pidgin_pounce_row, G_TYPE_OBJECT)

static void
pidgin_pounce_row_class_init(PidginPounceRowClass *klass)
{
}

static void
pidgin_pounce_row_init(PidginPounceRow *row)
{
}

static PidginPounceRow *
pidgin_pounce_row_new(PurplePounce *pounce)
{
	PidginPounceRow *row = g_object_new(PIDGIN_TYPE_POUNCE_ROW, NULL);

	row->pounce = pounce;
	return row;
}

/**************************************************************************
 * The editor
 **************************************************************************/

typedef struct
{
	/* Pounce data */
	PurplePounce  *pounce;
	PurpleAccount *account;

	/* The window */
	GtkWidget *window;

	/* Pounce on Whom */
	GtkWidget *account_menu;
	GtkWidget *buddy_entry;

	/* Pounce options */
	GtkWidget *on_away;

	/* Pounce When Buddy... (in pounce_events[] order) */
	GtkWidget *events[N_EVENTS];

	/* Action */
	GtkWidget *open_win;
	GtkWidget *popup;
	GtkWidget *popup_entry;
	GtkWidget *send_msg;
	GtkWidget *send_msg_box;
	GtkWidget *send_msg_entry;
	GtkWidget *exec_cmd;
	GtkWidget *exec_cmd_entry;
	GtkWidget *exec_cmd_browse;
	GtkWidget *play_sound;
	GtkWidget *play_sound_entry;
	GtkWidget *play_sound_browse;
	GtkWidget *play_sound_test;
	GtkWidget *play_sound_reset;

	GtkWidget *save_pounce;

	/* Buttons */
	GtkWidget *save_button;

	GCancellable *cancellable;
	guint close_idle;
} PidginPounceDialog;

typedef struct
{
	GtkWidget *window;
	GListStore *store;
	GtkSingleSelection *selection;
	GtkWidget *columnview;
	GtkWidget *add_button;
	GtkWidget *modify_button;
	GtkWidget *delete_button;
	GCancellable *cancellable;
} PouncesManager;

static PouncesManager *pounces_manager = NULL;
static GList *pounce_editors = NULL;

static void update_pounces(void);

static GtkWidget *
event_check(PidginPounceDialog *dialog, PurplePounceEvent event)
{
	gsize i;

	for (i = 0; i < N_EVENTS; i++)
		if (pounce_events[i].event == event)
			return dialog->events[i];
	g_return_val_if_reached(NULL);
}

static gboolean
check_active(GtkWidget *check)
{
	return gtk_check_button_get_active(GTK_CHECK_BUTTON(check));
}

static void
set_check_active(GtkWidget *check, gboolean active)
{
	gtk_check_button_set_active(GTK_CHECK_BUTTON(check), active);
}

static void
editor_destroy_cb(GtkWidget *window, PidginPounceDialog *dialog)
{
	pounce_editors = g_list_remove(pounce_editors, dialog);
	g_cancellable_cancel(dialog->cancellable);
	g_clear_object(&dialog->cancellable);
	g_clear_handle_id(&dialog->close_idle, g_source_remove);
	g_free(dialog);
}

static gboolean
editor_close_idle_cb(gpointer data)
{
	PidginPounceDialog *dialog = data;

	dialog->close_idle = 0;
	/* The destroy handler frees the dialog. */
	gtk_window_destroy(GTK_WINDOW(dialog->window));
	return G_SOURCE_REMOVE;
}

/* Closes the editor from an idle callback: saving can be triggered from
 * inside the compose entry's "message-send" emission. */
static void
editor_close_later(PidginPounceDialog *dialog)
{
	gtk_widget_set_visible(dialog->window, FALSE);
	if (dialog->close_idle == 0)
		dialog->close_idle = g_idle_add(editor_close_idle_cb, dialog);
}

static void
cancel_cb(GtkWidget *w, PidginPounceDialog *dialog)
{
	editor_close_later(dialog);
}

static const char *
entry_text(GtkWidget *entry)
{
	return gtk_editable_get_text(GTK_EDITABLE(entry));
}

static void
save_pounce_cb(GtkWidget *w, PidginPounceDialog *dialog)
{
	const char *name;
	const char *command, *sound, *reason;
	char *message;
	PurplePounceEvent events   = PURPLE_POUNCE_NONE;
	PurplePounceOption options = PURPLE_POUNCE_OPTION_NONE;
	gsize i;

	if (dialog->close_idle != 0)
		return;

	name = entry_text(dialog->buddy_entry);

	if (*name == '\0')
	{
		purple_notify_error(NULL, NULL,
						  _("Please enter a buddy to pounce."), NULL);
		return;
	}

	if (dialog->account == NULL)
		return;

	/* Options */
	if (check_active(dialog->on_away))
		options |= PURPLE_POUNCE_OPTION_AWAY;

	/* Events */
	for (i = 0; i < N_EVENTS; i++)
		if (check_active(dialog->events[i]))
			events |= pounce_events[i].event;

	/* Data fields */
	message = pidgin_compose_entry_get_markup(PIDGIN_COMPOSE_ENTRY(dialog->send_msg_entry));
	command = entry_text(dialog->exec_cmd_entry);
	sound   = entry_text(dialog->play_sound_entry);
	reason  = entry_text(dialog->popup_entry);

	if (*reason == '\0') reason = NULL;
	if (message != NULL && *message == '\0') {
		g_free(message);
		message = NULL;
	}
	if (*command == '\0') command = NULL;
	if (*sound   == '\0' || purple_strequal(sound, _("(default)"))) sound   = NULL;

	/* If the pounce has already been triggered, let's pretend it is a new one */
	if (dialog->pounce != NULL && !pounce_exists(dialog->pounce)) {
		purple_debug_info("gtkpounce", "Saving pounce that no longer exists; creating new pounce.\n");
		dialog->pounce = NULL;
	}

	if (dialog->pounce == NULL)
	{
		dialog->pounce = purple_pounce_new(PIDGIN_UI, dialog->account,
										 name, events, options);
	}
	else {
		purple_pounce_set_events(dialog->pounce, events);
		purple_pounce_set_options(dialog->pounce, options);
		purple_pounce_set_pouncer(dialog->pounce, dialog->account);
		purple_pounce_set_pouncee(dialog->pounce, name);
	}

	/* Actions */
	purple_pounce_action_set_enabled(dialog->pounce, "open-window",
		check_active(dialog->open_win));
	purple_pounce_action_set_enabled(dialog->pounce, "popup-notify",
		check_active(dialog->popup));
	purple_pounce_action_set_enabled(dialog->pounce, "send-message",
		check_active(dialog->send_msg));
	purple_pounce_action_set_enabled(dialog->pounce, "execute-command",
		check_active(dialog->exec_cmd));
	purple_pounce_action_set_enabled(dialog->pounce, "play-sound",
		check_active(dialog->play_sound));

	purple_pounce_action_set_attribute(dialog->pounce, "send-message",
									 "message", message);
	purple_pounce_action_set_attribute(dialog->pounce, "execute-command",
									 "command", command);
	purple_pounce_action_set_attribute(dialog->pounce, "play-sound",
									 "filename", sound);
	purple_pounce_action_set_attribute(dialog->pounce, "popup-notify",
									 "reason", reason);

	/* Set the defaults for next time. */
	purple_prefs_set_bool(PIDGIN_PREFS_ROOT "/pounces/default_actions/open-window",
		check_active(dialog->open_win));
	purple_prefs_set_bool(PIDGIN_PREFS_ROOT "/pounces/default_actions/popup-notify",
		check_active(dialog->popup));
	purple_prefs_set_bool(PIDGIN_PREFS_ROOT "/pounces/default_actions/send-message",
		check_active(dialog->send_msg));
	purple_prefs_set_bool(PIDGIN_PREFS_ROOT "/pounces/default_actions/execute-command",
		check_active(dialog->exec_cmd));
	purple_prefs_set_bool(PIDGIN_PREFS_ROOT "/pounces/default_actions/play-sound",
		check_active(dialog->play_sound));

	purple_pounce_set_save(dialog->pounce, check_active(dialog->save_pounce));

	update_pounces();
	g_free(message);

	editor_close_later(dialog);
}

static gboolean
message_send_cb(PidginComposeEntry *entry, const char *markup, PidginPounceDialog *dialog)
{
	/* Enter in the message saves, like Pidgin 2's "message_send". The
	 * entry keeps its text (the window closes anyway). */
	save_pounce_cb(NULL, dialog);
	return FALSE;
}

static void
account_changed_cb(GObject *dropdown, GParamSpec *pspec, PidginPounceDialog *dialog)
{
	PurpleAccount *account = pidgin_account_dropdown_get_selected(GTK_WIDGET(dropdown));
	PurpleConnection *gc;

	dialog->account = account;

	/* Pidgin 2's reset_send_msg_entry() */
	gc = account ? purple_account_get_connection(account) : NULL;
	pidgin_compose_entry_setup(PIDGIN_COMPOSE_ENTRY(dialog->send_msg_entry),
		gc ? gc->flags : PURPLE_CONNECTION_HTML);
}

static void
buddy_changed_cb(GtkEditable *entry, PidginPounceDialog *dialog)
{
	if (dialog->save_button == NULL)
		return;

	gtk_widget_set_sensitive(dialog->save_button,
		*gtk_editable_get_text(entry) != '\0');
}

static void
message_recv_toggle(GtkCheckButton *message_recv, PidginPounceDialog *dialog)
{
	gboolean active = gtk_check_button_get_active(message_recv);

	gtk_widget_set_sensitive(dialog->send_msg, !active);
	if (active)
		set_check_active(dialog->send_msg, FALSE);
}

static void
update_action_sensitivity(PidginPounceDialog *dialog)
{
	gboolean exec = check_active(dialog->exec_cmd);
	gboolean sound = check_active(dialog->play_sound);

	gtk_widget_set_sensitive(dialog->send_msg_box, check_active(dialog->send_msg));
	gtk_widget_set_sensitive(dialog->popup_entry, check_active(dialog->popup));
	gtk_widget_set_sensitive(dialog->exec_cmd_entry, exec);
	gtk_widget_set_sensitive(dialog->exec_cmd_browse, exec);
	gtk_widget_set_sensitive(dialog->play_sound_entry, sound);
	gtk_widget_set_sensitive(dialog->play_sound_browse, sound);
	gtk_widget_set_sensitive(dialog->play_sound_test, sound);
	gtk_widget_set_sensitive(dialog->play_sound_reset, sound);
}

static void
action_toggled_cb(GtkCheckButton *check, PidginPounceDialog *dialog)
{
	update_action_sensitivity(dialog);
}

/* Browse: a GtkFileDialog; the chosen path goes into the entry. */
static void
file_chosen_cb(GObject *source, GAsyncResult *result, gpointer data)
{
	GtkWidget *entry = data;
	GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, NULL);

	if (file != NULL) {
		char *path = g_file_get_path(file);

		if (path != NULL)
			gtk_editable_set_text(GTK_EDITABLE(entry), path);
		g_free(path);
		g_object_unref(file);
	}
	g_object_unref(entry);
}

static void
filesel(PidginPounceDialog *dialog, GtkWidget *entry)
{
	GtkFileDialog *fd = gtk_file_dialog_new();
	const char *name = entry_text(entry);

	gtk_file_dialog_set_title(fd, _("Select a file"));
	if (*name != '\0' && g_path_is_absolute(name)) {
		GFile *file = g_file_new_for_path(name);
		gtk_file_dialog_set_initial_file(fd, file);
		g_object_unref(file);
	}
	gtk_file_dialog_open(fd, GTK_WINDOW(dialog->window), dialog->cancellable,
	                     file_chosen_cb, g_object_ref(entry));
	g_object_unref(fd);
}

static void
exec_browse_cb(GtkWidget *button, PidginPounceDialog *dialog)
{
	filesel(dialog, dialog->exec_cmd_entry);
}

static void
sound_browse_cb(GtkWidget *button, PidginPounceDialog *dialog)
{
	filesel(dialog, dialog->play_sound_entry);
}

static void
pounce_test_sound(GtkWidget *w, PidginPounceDialog *dialog)
{
	const char *filename = entry_text(dialog->play_sound_entry);

	if (filename != NULL && *filename != '\0' && !purple_strequal(filename, _("(default)")))
		purple_sound_play_file(filename, NULL);
	else
		purple_sound_play_event(PURPLE_SOUND_POUNCE_DEFAULT, NULL);
}

static void
pounce_reset_sound(GtkWidget *w, PidginPounceDialog *dialog)
{
	gtk_editable_set_text(GTK_EDITABLE(dialog->play_sound_entry), _("(default)"));
}

static GtkWidget *
check_new(const char *label)
{
	return gtk_check_button_new_with_mnemonic(label);
}

static GtkWidget *
labelled_row(GtkWidget *parent, const char *text, GtkSizeGroup *sg, GtkWidget *widget)
{
	GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PIDGIN_HIG_BOX_SPACE);
	GtkWidget *label = gtk_label_new_with_mnemonic(text);

	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_size_group_add_widget(sg, label);
	gtk_label_set_mnemonic_widget(GTK_LABEL(label), widget);
	gtk_box_append(GTK_BOX(hbox), label);
	gtk_widget_set_hexpand(widget, TRUE);
	gtk_box_append(GTK_BOX(hbox), widget);
	gtk_box_append(GTK_BOX(parent), hbox);
	return label;
}

void
pidgin_pounce_editor_show(PurpleAccount *account, const char *name,
                          PurplePounce *cur_pounce)
{
	PidginPounceDialog *dialog;
	GtkWidget *window;
	GtkWidget *vbox;
	GtkWidget *frame;
	GtkWidget *grid;
	GtkWidget *button;
	GtkSizeGroup *sg;
	gsize i;

	g_return_if_fail((cur_pounce != NULL) ||
	                 (account != NULL) ||
	                 (purple_accounts_get_all() != NULL));

	dialog = g_new0(PidginPounceDialog, 1);
	dialog->cancellable = g_cancellable_new();

	if (cur_pounce != NULL)
	{
		dialog->pounce  = cur_pounce;
		dialog->account = purple_pounce_get_pouncer(cur_pounce);
	}
	else if (account != NULL)
	{
		dialog->pounce  = NULL;
		dialog->account = account;
	}
	else
	{
		GList *connections = purple_connections_get_all();

		if (connections != NULL)
			dialog->account = purple_connection_get_account(connections->data);
		else
			dialog->account = purple_accounts_get_all()->data;

		dialog->pounce  = NULL;
	}

	/* Create the window. */
	dialog->window = window = pidgin_dialog_new(
		(cur_pounce == NULL ? _("Add Buddy Pounce") : _("Modify Buddy Pounce")),
		pidgin_get_active_window(), "buddy_pounce", TRUE);
	g_signal_connect(window, "destroy", G_CALLBACK(editor_destroy_cb), dialog);
	pounce_editors = g_list_prepend(pounce_editors, dialog);

	vbox = pidgin_dialog_get_content_area(window);

	/* Create the "Pounce on Whom" frame. */
	frame = pidgin_make_frame(vbox, _("Pounce on Whom"));
	sg = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);

	dialog->account_menu = pidgin_account_dropdown_new(dialog->account, TRUE, NULL, NULL);
	dialog->account = pidgin_account_dropdown_get_selected(dialog->account_menu);
	labelled_row(frame, _("_Account:"), sg, dialog->account_menu);

	dialog->buddy_entry = gtk_entry_new();
	gtk_entry_set_activates_default(GTK_ENTRY(dialog->buddy_entry), TRUE);
	labelled_row(frame, _("_Buddy name:"), sg, dialog->buddy_entry);
	g_object_unref(sg);

	if (cur_pounce != NULL)
		gtk_editable_set_text(GTK_EDITABLE(dialog->buddy_entry),
		                      purple_pounce_get_pouncee(cur_pounce));
	else if (name != NULL)
		gtk_editable_set_text(GTK_EDITABLE(dialog->buddy_entry), name);
	g_signal_connect(dialog->buddy_entry, "changed", G_CALLBACK(buddy_changed_cb), dialog);

	/* Create the "Pounce When Buddy..." frame. */
	frame = pidgin_make_frame(vbox, _("Pounce When Buddy..."));
	grid = gtk_grid_new();
	gtk_grid_set_column_spacing(GTK_GRID(grid), PIDGIN_HIG_BORDER);
	gtk_box_append(GTK_BOX(frame), grid);
	for (i = 0; i < N_EVENTS; i++) {
		dialog->events[i] = check_new(_(pounce_events[i].label));
		/* Pidgin 2's layout: three rows, filled column by column. */
		gtk_grid_attach(GTK_GRID(grid), dialog->events[i], i / 3, i % 3, 1, 1);
	}

	/* Create the "Action" frame. */
	frame = pidgin_make_frame(vbox, _("Action"));
	grid = gtk_grid_new();
	gtk_grid_set_column_spacing(GTK_GRID(grid), PIDGIN_HIG_BORDER);
	gtk_grid_set_row_spacing(GTK_GRID(grid), PIDGIN_HIG_BOX_SPACE / 2);
	gtk_box_append(GTK_BOX(frame), grid);

	dialog->open_win   = check_new(_("Ope_n an IM window"));
	dialog->popup      = check_new(_("_Pop up a notification"));
	dialog->send_msg   = check_new(_("Send a _message"));
	dialog->exec_cmd   = check_new(_("E_xecute a command"));
	dialog->play_sound = check_new(_("P_lay a sound"));

	dialog->send_msg_box = pidgin_create_compose_entry(PURPLE_CONNECTION_HTML, TRUE,
		&dialog->send_msg_entry, NULL);
	gtk_widget_set_size_request(dialog->send_msg_box, -1, 80);
	pidgin_compose_entry_set_return_inserts_newline(
		PIDGIN_COMPOSE_ENTRY(dialog->send_msg_entry), FALSE);
	g_signal_connect(dialog->send_msg_entry, "message-send",
	                 G_CALLBACK(message_send_cb), dialog);

	dialog->popup_entry       = gtk_entry_new();
	dialog->exec_cmd_entry    = gtk_entry_new();
	dialog->exec_cmd_browse   = gtk_button_new_with_mnemonic(_("Brows_e..."));
	dialog->play_sound_entry  = gtk_entry_new();
	gtk_editable_set_text(GTK_EDITABLE(dialog->play_sound_entry), _("(default)"));
	gtk_editable_set_editable(GTK_EDITABLE(dialog->play_sound_entry), FALSE);
	dialog->play_sound_browse = gtk_button_new_with_mnemonic(_("Br_owse..."));
	dialog->play_sound_test   = gtk_button_new_with_mnemonic(_("Pre_view"));
	dialog->play_sound_reset  = gtk_button_new_with_mnemonic(_("Reset"));

	gtk_widget_set_hexpand(dialog->popup_entry, TRUE);
	gtk_widget_set_hexpand(dialog->exec_cmd_entry, TRUE);
	gtk_widget_set_hexpand(dialog->play_sound_entry, TRUE);

	gtk_grid_attach(GTK_GRID(grid), dialog->open_win,          0, 0, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), dialog->popup,             0, 1, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), dialog->popup_entry,       1, 1, 4, 1);
	gtk_grid_attach(GTK_GRID(grid), dialog->send_msg,          0, 2, 5, 1);
	gtk_grid_attach(GTK_GRID(grid), dialog->send_msg_box,      0, 3, 5, 1);
	gtk_grid_attach(GTK_GRID(grid), dialog->exec_cmd,          0, 4, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), dialog->exec_cmd_entry,    1, 4, 3, 1);
	gtk_grid_attach(GTK_GRID(grid), dialog->exec_cmd_browse,   4, 4, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), dialog->play_sound,        0, 5, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), dialog->play_sound_entry,  1, 5, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), dialog->play_sound_browse, 2, 5, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), dialog->play_sound_test,   3, 5, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), dialog->play_sound_reset,  4, 5, 1, 1);

	g_signal_connect(event_check(dialog, PURPLE_POUNCE_MESSAGE_RECEIVED), "toggled",
	                 G_CALLBACK(message_recv_toggle), dialog);
	g_signal_connect(dialog->send_msg, "toggled", G_CALLBACK(action_toggled_cb), dialog);
	g_signal_connect(dialog->popup, "toggled", G_CALLBACK(action_toggled_cb), dialog);
	g_signal_connect(dialog->exec_cmd, "toggled", G_CALLBACK(action_toggled_cb), dialog);
	g_signal_connect(dialog->play_sound, "toggled", G_CALLBACK(action_toggled_cb), dialog);
	g_signal_connect(dialog->exec_cmd_browse, "clicked", G_CALLBACK(exec_browse_cb), dialog);
	g_signal_connect(dialog->play_sound_browse, "clicked", G_CALLBACK(sound_browse_cb), dialog);
	g_signal_connect(dialog->play_sound_test, "clicked", G_CALLBACK(pounce_test_sound), dialog);
	g_signal_connect(dialog->play_sound_reset, "clicked", G_CALLBACK(pounce_reset_sound), dialog);
	g_signal_connect(dialog->popup_entry, "activate", G_CALLBACK(save_pounce_cb), dialog);
	g_signal_connect(dialog->exec_cmd_entry, "activate", G_CALLBACK(save_pounce_cb), dialog);

	/* Create the "Options" frame. */
	frame = pidgin_make_frame(vbox, _("Options"));
	dialog->on_away =
		check_new(_("P_ounce only when my status is not Available"));
	gtk_box_append(GTK_BOX(frame), dialog->on_away);
	dialog->save_pounce = check_new(_("_Recurring"));
	gtk_box_append(GTK_BOX(frame), dialog->save_pounce);

	/* Buttons */
	pidgin_dialog_add_button(window, _("_Cancel"), G_CALLBACK(cancel_cb), dialog);
	dialog->save_button = button = pidgin_dialog_add_button(window,
		(cur_pounce == NULL ? _("_Add") : _("_Save")),
		G_CALLBACK(save_pounce_cb), dialog);
	gtk_window_set_default_widget(GTK_WINDOW(window), button);

	/* The drop-down's selection drives the account and the entry setup. */
	g_signal_connect(dialog->account_menu, "notify::selected",
	                 G_CALLBACK(account_changed_cb), dialog);
	account_changed_cb(G_OBJECT(dialog->account_menu), NULL, dialog);

	/* Set the values of stuff. */
	if (cur_pounce != NULL)
	{
		PurplePounceEvent events   = purple_pounce_get_events(cur_pounce);
		PurplePounceOption options = purple_pounce_get_options(cur_pounce);
		const char *value;

		/* Options */
		set_check_active(dialog->on_away, (options & PURPLE_POUNCE_OPTION_AWAY));

		/* Events */
		for (i = 0; i < N_EVENTS; i++)
			set_check_active(dialog->events[i], (events & pounce_events[i].event) != 0);

		/* Actions */
		set_check_active(dialog->open_win,
			purple_pounce_action_is_enabled(cur_pounce, "open-window"));
		set_check_active(dialog->popup,
			purple_pounce_action_is_enabled(cur_pounce, "popup-notify"));
		set_check_active(dialog->send_msg,
			purple_pounce_action_is_enabled(cur_pounce, "send-message"));
		set_check_active(dialog->exec_cmd,
			purple_pounce_action_is_enabled(cur_pounce, "execute-command"));
		set_check_active(dialog->play_sound,
			purple_pounce_action_is_enabled(cur_pounce, "play-sound"));

		set_check_active(dialog->save_pounce, purple_pounce_get_save(cur_pounce));

		if ((value = purple_pounce_action_get_attribute(cur_pounce,
		                                                "send-message", "message")) != NULL)
			pidgin_compose_entry_set_markup(PIDGIN_COMPOSE_ENTRY(dialog->send_msg_entry), value);

		if ((value = purple_pounce_action_get_attribute(cur_pounce,
		                                                "popup-notify", "reason")) != NULL)
			gtk_editable_set_text(GTK_EDITABLE(dialog->popup_entry), value);

		if ((value = purple_pounce_action_get_attribute(cur_pounce,
		                                                "execute-command", "command")) != NULL)
			gtk_editable_set_text(GTK_EDITABLE(dialog->exec_cmd_entry), value);

		if ((value = purple_pounce_action_get_attribute(cur_pounce,
		                                                "play-sound", "filename")) != NULL)
			gtk_editable_set_text(GTK_EDITABLE(dialog->play_sound_entry),
				(*value != '\0') ? value : _("(default)"));
	}
	else
	{
		PurpleBuddy *buddy = NULL;

		if (name != NULL && account != NULL)
			buddy = purple_find_buddy(account, name);

		/* Set some defaults */
		if (buddy == NULL || !PURPLE_BUDDY_IS_ONLINE(buddy))
		{
			set_check_active(event_check(dialog, PURPLE_POUNCE_SIGNON), TRUE);
		}
		else
		{
			gboolean default_set = FALSE;
			PurplePresence *presence = purple_buddy_get_presence(buddy);

			if (purple_presence_is_idle(presence))
			{
				set_check_active(event_check(dialog, PURPLE_POUNCE_IDLE_RETURN), TRUE);
				default_set = TRUE;
			}

			if (!purple_presence_is_available(presence))
			{
				set_check_active(event_check(dialog, PURPLE_POUNCE_AWAY_RETURN), TRUE);
				default_set = TRUE;
			}

			if (!default_set)
				set_check_active(event_check(dialog, PURPLE_POUNCE_SIGNON), TRUE);
		}

		set_check_active(dialog->open_win,
			purple_prefs_get_bool(PIDGIN_PREFS_ROOT "/pounces/default_actions/open-window"));
		set_check_active(dialog->popup,
			purple_prefs_get_bool(PIDGIN_PREFS_ROOT "/pounces/default_actions/popup-notify"));
		set_check_active(dialog->send_msg,
			purple_prefs_get_bool(PIDGIN_PREFS_ROOT "/pounces/default_actions/send-message"));
		set_check_active(dialog->exec_cmd,
			purple_prefs_get_bool(PIDGIN_PREFS_ROOT "/pounces/default_actions/execute-command"));
		set_check_active(dialog->play_sound,
			purple_prefs_get_bool(PIDGIN_PREFS_ROOT "/pounces/default_actions/play-sound"));
	}

	update_action_sensitivity(dialog);
	buddy_changed_cb(GTK_EDITABLE(dialog->buddy_entry), dialog);

	gtk_window_present(GTK_WINDOW(window));
}

/**************************************************************************
 * The manager
 **************************************************************************/

static gint
pounce_compare_func(gconstpointer a, gconstpointer b)
{
	char *ka = g_utf8_casefold(purple_pounce_get_pouncee((PurplePounce *)a), -1);
	char *kb = g_utf8_casefold(purple_pounce_get_pouncee((PurplePounce *)b), -1);
	gint ret = g_utf8_collate(ka, kb);

	g_free(ka);
	g_free(kb);
	return ret;
}

static void
update_buttons(void)
{
	gboolean selected;

	if (pounces_manager == NULL)
		return;

	selected = gtk_single_selection_get_selected_item(pounces_manager->selection) != NULL;
	gtk_widget_set_sensitive(pounces_manager->modify_button, selected);
	gtk_widget_set_sensitive(pounces_manager->delete_button, selected);
	gtk_widget_set_sensitive(pounces_manager->add_button, purple_accounts_get_all() != NULL);
}

static void
populate_pounces_list(PouncesManager *dialog)
{
	GList *pounces, *l;
	PurplePounce *selected = NULL;
	PidginPounceRow *row;
	GPtrArray *rows = g_ptr_array_new_with_free_func(g_object_unref);
	guint i, sel_pos = GTK_INVALID_LIST_POSITION;

	row = gtk_single_selection_get_selected_item(dialog->selection);
	if (row != NULL)
		selected = row->pounce;

	pounces = g_list_sort(purple_pounces_get_all_for_ui(PIDGIN_UI), pounce_compare_func);
	for (l = pounces, i = 0; l != NULL; l = l->next, i++) {
		g_ptr_array_add(rows, pidgin_pounce_row_new(l->data));
		if (l->data == selected)
			sel_pos = i;
	}
	g_list_free(pounces);

	g_list_store_splice(dialog->store, 0, g_list_model_get_n_items(G_LIST_MODEL(dialog->store)),
	                    rows->pdata, rows->len);
	g_ptr_array_unref(rows);

	gtk_single_selection_set_selected(dialog->selection, sel_pos);
	update_buttons();
}

static void
update_pounces(void)
{
	/* Rebuild the pounces list if the pounces manager is open */
	if (pounces_manager != NULL)
		populate_pounces_list(pounces_manager);
}

static void
signed_on_off_cb(PurpleConnection *gc, gpointer user_data)
{
	update_pounces();
}

static PurplePounce *
selected_pounce(void)
{
	PidginPounceRow *row;

	if (pounces_manager == NULL)
		return NULL;
	row = gtk_single_selection_get_selected_item(pounces_manager->selection);
	return (row != NULL && pounce_exists(row->pounce)) ? row->pounce : NULL;
}

static void
pounces_manager_add_cb(GtkButton *button, gpointer user_data)
{
	if (purple_accounts_get_all() != NULL)
		pidgin_pounce_editor_show(NULL, NULL, NULL);
}

static void
pounces_manager_modify_cb(GtkButton *button, gpointer user_data)
{
	PurplePounce *pounce = selected_pounce();

	if (pounce != NULL)
		pidgin_pounce_editor_show(NULL, NULL, pounce);
}

static void
pounce_activate_cb(GtkColumnView *view, guint position, gpointer data)
{
	PidginPounceRow *row = g_list_model_get_item(G_LIST_MODEL(pounces_manager->store), position);

	if (row != NULL && pounce_exists(row->pounce))
		pidgin_pounce_editor_show(NULL, NULL, row->pounce);
	g_clear_object(&row);
}

static void
delete_response_cb(GObject *source, GAsyncResult *result, gpointer data)
{
	PurplePounce *pounce = data;
	int button = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, NULL);

	if (button != 1 || !pounce_exists(pounce))
		return;

	purple_request_close_with_handle(pounce);
	/* free_pounce() refreshes the list. */
	purple_pounce_destroy(pounce);
}

static void
pounces_manager_delete_cb(GtkButton *button, gpointer user_data)
{
	PurplePounce *pounce = selected_pounce();
	PurpleAccount *account;
	GtkAlertDialog *alert;
	const char *buttons[] = { _("_Cancel"), _("_Delete"), NULL };

	if (pounce == NULL)
		return;

	account = purple_pounce_get_pouncer(pounce);
	alert = gtk_alert_dialog_new(_("Are you sure you want to delete the pounce on %s for %s?"),
	                             purple_pounce_get_pouncee(pounce),
	                             purple_account_get_username(account));
	gtk_alert_dialog_set_buttons(alert, buttons);
	gtk_alert_dialog_set_cancel_button(alert, 0);
	gtk_alert_dialog_set_default_button(alert, 0);
	gtk_alert_dialog_choose(alert, GTK_WINDOW(pounces_manager->window),
	                        pounces_manager->cancellable, delete_response_cb, pounce);
	g_object_unref(alert);
}

static void
pounces_manager_close_cb(GtkButton *button, gpointer user_data)
{
	pidgin_pounces_manager_hide();
}

static void
selection_changed_cb(GtkSelectionModel *model, guint position, guint n_items,
                     gpointer data)
{
	update_buttons();
}

/* Cells */

static void
account_setup_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	GtkWidget *box, *image, *label;

	box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PIDGIN_HIG_BOX_SPACE);
	image = gtk_image_new();
	gtk_image_set_pixel_size(GTK_IMAGE(image),
		pidgin_prpl_icon_size_to_pixels(PIDGIN_PRPL_ICON_SMALL));
	label = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
	gtk_box_append(GTK_BOX(box), image);
	gtk_box_append(GTK_BOX(box), label);
	gtk_list_item_set_child(li, box);
}

static void
account_bind_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	PidginPounceRow *row = gtk_list_item_get_item(li);
	GtkWidget *image = gtk_widget_get_first_child(gtk_list_item_get_child(li));
	GtkWidget *label = gtk_widget_get_next_sibling(image);
	PurpleAccount *account;
	GIcon *icon;

	if (!pounce_exists(row->pounce))
		return;
	account = purple_pounce_get_pouncer(row->pounce);
	icon = pidgin_create_prpl_gicon(account, NULL);
	gtk_image_set_from_gicon(GTK_IMAGE(image), icon);
	g_object_unref(icon);
	gtk_label_set_text(GTK_LABEL(label), purple_account_get_username(account));
}

static void
label_setup_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	GtkWidget *label = gtk_label_new(NULL);

	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
	gtk_list_item_set_child(li, label);
}

static void
target_bind_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	PidginPounceRow *row = gtk_list_item_get_item(li);

	if (pounce_exists(row->pounce))
		gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(li)),
		                   purple_pounce_get_pouncee(row->pounce));
}

static void
events_bind_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	PidginPounceRow *row = gtk_list_item_get_item(li);
	GtkWidget *label = gtk_list_item_get_child(li);
	char *text;

	if (!pounce_exists(row->pounce))
		return;
	text = events_summary(purple_pounce_get_events(row->pounce));
	gtk_label_set_text(GTK_LABEL(label), text);
	gtk_widget_set_tooltip_text(label, text);
	g_free(text);
}

static void
recurring_toggled_cb(GtkCheckButton *check, gpointer data)
{
	PurplePounce *pounce = g_object_get_data(G_OBJECT(check), "pidgin-pounce");

	if (pounce_exists(pounce))
		purple_pounce_set_save(pounce, gtk_check_button_get_active(check));
}

static void
recurring_setup_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	GtkWidget *check = gtk_check_button_new();

	gtk_widget_set_halign(check, GTK_ALIGN_CENTER);
	gtk_accessible_update_property(GTK_ACCESSIBLE(check),
		GTK_ACCESSIBLE_PROPERTY_LABEL, _("Recurring"), -1);
	g_signal_connect(check, "toggled", G_CALLBACK(recurring_toggled_cb), NULL);
	gtk_list_item_set_child(li, check);
}

static void
recurring_bind_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	PidginPounceRow *row = gtk_list_item_get_item(li);
	GtkWidget *check = gtk_list_item_get_child(li);

	g_signal_handlers_block_by_func(check, recurring_toggled_cb, NULL);
	g_object_set_data(G_OBJECT(check), "pidgin-pounce", row->pounce);
	if (pounce_exists(row->pounce))
		gtk_check_button_set_active(GTK_CHECK_BUTTON(check),
		                            purple_pounce_get_save(row->pounce));
	g_signal_handlers_unblock_by_func(check, recurring_toggled_cb, NULL);
}

static void
add_column(GtkWidget *columnview, const char *title, GCallback setup,
           GCallback bind, gboolean expand)
{
	GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
	GtkColumnViewColumn *column;

	g_signal_connect(factory, "setup", setup, NULL);
	g_signal_connect(factory, "bind", bind, NULL);
	column = gtk_column_view_column_new(title, factory);
	gtk_column_view_column_set_expand(column, expand);
	gtk_column_view_column_set_resizable(column, TRUE);
	gtk_column_view_append_column(GTK_COLUMN_VIEW(columnview), column);
	g_object_unref(column);
}

static gboolean
manager_close_request_cb(GtkWindow *window, gpointer data)
{
	int width, height;

	gtk_window_get_default_size(window, &width, &height);
	if (width > 0 && height > 0) {
		purple_prefs_set_int(PREFS_DIALOG "/width", width);
		purple_prefs_set_int(PREFS_DIALOG "/height", height);
	}
	return FALSE;
}

static void
manager_destroy_cb(GtkWidget *window, gpointer data)
{
	PouncesManager *dialog = pounces_manager;

	if (dialog == NULL)
		return;

	pounces_manager = NULL;
	purple_signals_disconnect_by_handle(dialog);
	g_cancellable_cancel(dialog->cancellable);
	g_clear_object(&dialog->cancellable);
	g_clear_object(&dialog->store);
	g_clear_object(&dialog->selection);
	g_free(dialog);
}

void
pidgin_pounces_manager_show(void)
{
	PouncesManager *dialog;
	GtkWidget *win, *vbox, *sw;

	if (pounces_manager != NULL) {
		gtk_window_present(GTK_WINDOW(pounces_manager->window));
		return;
	}

	pounces_manager = dialog = g_new0(PouncesManager, 1);
	dialog->cancellable = g_cancellable_new();

	dialog->window = win = pidgin_dialog_new(_("Buddy Pounces"), NULL, "pounces", TRUE);
	gtk_window_set_default_size(GTK_WINDOW(win),
		purple_prefs_get_int(PREFS_DIALOG "/width"),
		purple_prefs_get_int(PREFS_DIALOG "/height"));
	g_signal_connect(win, "close-request", G_CALLBACK(manager_close_request_cb), NULL);
	g_signal_connect(win, "destroy", G_CALLBACK(manager_destroy_cb), NULL);

	vbox = pidgin_dialog_get_content_area(win);

	/* List of saved buddy pounces */
	dialog->store = g_list_store_new(PIDGIN_TYPE_POUNCE_ROW);
	dialog->selection = gtk_single_selection_new(G_LIST_MODEL(g_object_ref(dialog->store)));
	gtk_single_selection_set_autoselect(dialog->selection, FALSE);
	gtk_single_selection_set_can_unselect(dialog->selection, TRUE);
	g_signal_connect(dialog->selection, "selection-changed",
	                 G_CALLBACK(selection_changed_cb), NULL);

	dialog->columnview = gtk_column_view_new(
		GTK_SELECTION_MODEL(g_object_ref(dialog->selection)));
	g_signal_connect(dialog->columnview, "activate", G_CALLBACK(pounce_activate_cb), NULL);

	add_column(dialog->columnview, _("Account"), G_CALLBACK(account_setup_cb),
	           G_CALLBACK(account_bind_cb), TRUE);
	add_column(dialog->columnview, _("Pounce Target"), G_CALLBACK(label_setup_cb),
	           G_CALLBACK(target_bind_cb), TRUE);
	add_column(dialog->columnview, _("Events"), G_CALLBACK(label_setup_cb),
	           G_CALLBACK(events_bind_cb), TRUE);
	add_column(dialog->columnview, _("Recurring"), G_CALLBACK(recurring_setup_cb),
	           G_CALLBACK(recurring_bind_cb), FALSE);

	sw = pidgin_make_scrollable(dialog->columnview, GTK_POLICY_AUTOMATIC,
	                            GTK_POLICY_AUTOMATIC, -1, -1);
	gtk_widget_set_vexpand(sw, TRUE);
	gtk_box_append(GTK_BOX(vbox), sw);

	dialog->add_button = pidgin_dialog_add_button(win, _("_Add"),
		G_CALLBACK(pounces_manager_add_cb), NULL);
	dialog->modify_button = pidgin_dialog_add_button(win, _("_Modify"),
		G_CALLBACK(pounces_manager_modify_cb), NULL);
	dialog->delete_button = pidgin_dialog_add_button(win, _("_Delete"),
		G_CALLBACK(pounces_manager_delete_cb), NULL);
	pidgin_dialog_add_button(win, _("_Close"),
		G_CALLBACK(pounces_manager_close_cb), NULL);

	purple_signal_connect(purple_connections_get_handle(), "signed-on",
	                      dialog, PURPLE_CALLBACK(signed_on_off_cb), NULL);
	purple_signal_connect(purple_connections_get_handle(), "signed-off",
	                      dialog, PURPLE_CALLBACK(signed_on_off_cb), NULL);

	populate_pounces_list(dialog);

	gtk_window_present(GTK_WINDOW(win));
}

void
pidgin_pounces_manager_hide(void)
{
	if (pounces_manager == NULL)
		return;

	manager_close_request_cb(GTK_WINDOW(pounces_manager->window), NULL);
	/* The destroy handler frees pounces_manager. */
	gtk_window_destroy(GTK_WINDOW(pounces_manager->window));
}

/**************************************************************************
 * The pounce handler
 **************************************************************************/

static void
pounce_cb(PurplePounce *pounce, PurplePounceEvent events, void *data)
{
	PurpleConversation *conv;
	PurpleAccount *account;
	PurpleBuddy *buddy;
	const char *pouncee;
	const char *alias;

	pouncee = purple_pounce_get_pouncee(pounce);
	account = purple_pounce_get_pouncer(pounce);

	buddy = purple_find_buddy(account, pouncee);
	if (buddy != NULL)
	{
		alias = purple_buddy_get_alias(buddy);
		if (alias == NULL)
			alias = pouncee;
	}
	else
		alias = pouncee;

	if (purple_pounce_action_is_enabled(pounce, "open-window"))
	{
		conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM, pouncee, account);
		if (conv == NULL)
			conv = purple_conversation_new(PURPLE_CONV_TYPE_IM, account, pouncee);
		if (conv != NULL)
			purple_conversation_present(conv);
	}

	if (purple_pounce_action_is_enabled(pounce, "popup-notify"))
	{
		char *tmp = NULL;
		const char *name_shown;
		const char *reason;
		static const struct {
			PurplePounceEvent event;
			const char *format;
		} messages[] = {
			{PURPLE_POUNCE_TYPING, N_("%s has started typing to you (%s)")},
			{PURPLE_POUNCE_TYPED, N_("%s has paused while typing to you (%s)")},
			{PURPLE_POUNCE_SIGNON, N_("%s has signed on (%s)")},
			{PURPLE_POUNCE_IDLE_RETURN, N_("%s has returned from being idle (%s)")},
			{PURPLE_POUNCE_AWAY_RETURN, N_("%s has returned from being away (%s)")},
			{PURPLE_POUNCE_TYPING_STOPPED, N_("%s has stopped typing to you (%s)")},
			{PURPLE_POUNCE_SIGNOFF, N_("%s has signed off (%s)")},
			{PURPLE_POUNCE_IDLE, N_("%s has become idle (%s)")},
			{PURPLE_POUNCE_AWAY, N_("%s has gone away. (%s)")},
			{PURPLE_POUNCE_MESSAGE_RECEIVED, N_("%s has sent you a message. (%s)")},
			{0, NULL}
		};
		int i;

		reason = purple_pounce_action_get_attribute(pounce, "popup-notify", "reason");

		/*
		 * Here we place the protocol name in the pounce dialog to lessen
		 * confusion about what protocol a pounce is for.
		 */
		for (i = 0; messages[i].format != NULL; i++) {
			if (messages[i].event & events) {
				tmp = g_strdup_printf(_(messages[i].format), alias,
						purple_account_get_protocol_name(account));
				break;
			}
		}
		if (tmp == NULL)
			tmp = g_strdup(_("Unknown pounce event. Please report this!"));

		/* The title is the account's alias, or its name. */
		if ((name_shown = purple_account_get_alias(account)) == NULL)
			name_shown = purple_account_get_username(account);

		if (reason == NULL)
		{
			purple_notify_info(NULL, name_shown, tmp, purple_date_format_full(NULL));
		}
		else
		{
			char *tmp2 = g_strdup_printf("%s\n\n%s", reason, purple_date_format_full(NULL));
			purple_notify_info(NULL, name_shown, tmp, tmp2);
			g_free(tmp2);
		}
		g_free(tmp);
	}

	if (purple_pounce_action_is_enabled(pounce, "send-message"))
	{
		const char *message;

		message = purple_pounce_action_get_attribute(pounce, "send-message", "message");

		if (message != NULL)
		{
			conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM, pouncee, account);

			if (conv == NULL)
				conv = purple_conversation_new(PURPLE_CONV_TYPE_IM, account, pouncee);

			/* Sends it and writes it to the conversation (and its log)
			 * as sent, like Pidgin 2's write + serv_send_im(). */
			if (conv != NULL)
				purple_conv_im_send(PURPLE_CONV_IM(conv), message);
		}
	}

	if (purple_pounce_action_is_enabled(pounce, "execute-command"))
	{
		const char *command;

		command = purple_pounce_action_get_attribute(pounce,
				"execute-command", "command");

		if (command != NULL)
		{
			/* sh -c, in the locale's encoding, like Pidgin 2 */
			char *localecmd = g_locale_from_utf8(command, -1, NULL, NULL, NULL);

			if (localecmd != NULL)
			{
				char *argv[] = { "sh", "-c", localecmd, NULL };
				GError *error = NULL;

				if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
				                   NULL, NULL, NULL, &error)) {
					purple_debug_error("gtkpounce", "Could not run '%s': %s\n",
					                   command, error->message);
					g_error_free(error);
				}
				g_free(localecmd);
			}
		}
	}

	if (purple_pounce_action_is_enabled(pounce, "play-sound"))
	{
		const char *sound;

		sound = purple_pounce_action_get_attribute(pounce, "play-sound", "filename");

		if (sound != NULL)
			purple_sound_play_file(sound, account);
		else
			purple_sound_play_event(PURPLE_SOUND_POUNCE_DEFAULT, account);
	}
}

static void
free_pounce(PurplePounce *pounce)
{
	GList *l;

	/* An editor of this pounce saves as a new one (see save_pounce_cb). */
	for (l = pounce_editors; l != NULL; l = l->next) {
		PidginPounceDialog *dialog = l->data;
		if (dialog->pounce == pounce)
			dialog->pounce = NULL;
	}
	update_pounces();
}

/*
 * The UI must register its pounce actions on every new pounce, as
 * pidgin/gtkpounce.c:new_pounce() does: while pounces.xml is parsed,
 * libpurple only keeps an action's <param>s if that action is registered
 * already, so without this the "command" of an execute-command action
 * would be dropped and pounces.xml rewritten without it.
 */
static void
new_pounce(PurplePounce *pounce)
{
	purple_pounce_action_register(pounce, "open-window");
	purple_pounce_action_register(pounce, "popup-notify");
	purple_pounce_action_register(pounce, "send-message");
	purple_pounce_action_register(pounce, "execute-command");
	purple_pounce_action_register(pounce, "play-sound");

	update_pounces();
}

void *
pidgin_pounces_get_handle(void)
{
	static int handle;

	return &handle;
}

void
pidgin_pounces_init(void)
{
	purple_pounces_register_handler(PIDGIN_UI, pounce_cb, new_pounce,
	                                free_pounce);

	/* Shared with Pidgin 2 (same meaning, type and defaults). */
	purple_prefs_add_none(PIDGIN_PREFS_ROOT "/pounces");
	purple_prefs_add_none(PIDGIN_PREFS_ROOT "/pounces/default_actions");
	purple_prefs_add_bool(PIDGIN_PREFS_ROOT "/pounces/default_actions/open-window",
	                      FALSE);
	purple_prefs_add_bool(PIDGIN_PREFS_ROOT "/pounces/default_actions/popup-notify",
	                      TRUE);
	purple_prefs_add_bool(PIDGIN_PREFS_ROOT "/pounces/default_actions/send-message",
	                      FALSE);
	purple_prefs_add_bool(PIDGIN_PREFS_ROOT "/pounces/default_actions/execute-command",
	                      FALSE);
	purple_prefs_add_bool(PIDGIN_PREFS_ROOT "/pounces/default_actions/play-sound",
	                      FALSE);

	/* The manager's size (Pidgin 2 kept it in /pidgin/pounces/dialog). */
	purple_prefs_add_none("/pidgin4");
	purple_prefs_add_none(PREFS_DIALOG);
	purple_prefs_add_int(PREFS_DIALOG "/width",  520);
	purple_prefs_add_int(PREFS_DIALOG "/height", 321);

	purple_signal_connect(purple_connections_get_handle(), "signed-on",
	                      pidgin_pounces_get_handle(),
	                      PURPLE_CALLBACK(signed_on_off_cb), NULL);
	purple_signal_connect(purple_connections_get_handle(), "signed-off",
	                      pidgin_pounces_get_handle(),
	                      PURPLE_CALLBACK(signed_on_off_cb), NULL);
}

void
pidgin_pounces_uninit(void)
{
	while (pounce_editors != NULL) {
		PidginPounceDialog *dialog = pounce_editors->data;
		/* The destroy handler removes it from the list. */
		gtk_window_destroy(GTK_WINDOW(dialog->window));
	}
	pidgin_pounces_manager_hide();
	purple_signals_disconnect_by_handle(pidgin_pounces_get_handle());
}

/**************************************************************************
 * Selftest
 **************************************************************************/

static void
selftest_close_editors(void)
{
	GList *l, *copy = g_list_copy(pounce_editors);

	for (l = copy; l != NULL; l = l->next) {
		PidginPounceDialog *dialog = l->data;
		/* The Cancel button's handler. */
		cancel_cb(NULL, dialog);
	}
	g_list_free(copy);
	pidgin_selftest_iterate(100);
}

void
pidgin_pounces_selftest(void)
{
	GList *pounces, *l;
	guint n_pounces, n_rows;
	int n = 0;

	pidgin_pounces_manager_show();
	pidgin_selftest_iterate(300);
	if (pounces_manager == NULL) {
		pidgin_selftest_fail("pounces", "the manager did not open");
		return;
	}

	pounces = purple_pounces_get_all_for_ui(PIDGIN_UI);
	n_pounces = g_list_length(pounces);
	n_rows = g_list_model_get_n_items(G_LIST_MODEL(pounces_manager->store));
	pidgin_selftest_log("pounces", "manager: %u rows for %u pounces", n_rows, n_pounces);
	if (n_rows != n_pounces)
		pidgin_selftest_fail("pounces", "the manager shows %u rows for %u pounces",
		                     n_rows, n_pounces);

	/* Select the first row: Modify and Delete become sensitive. */
	if (n_rows > 0) {
		gtk_single_selection_set_selected(pounces_manager->selection, 0);
		pidgin_selftest_iterate(50);
		if (!gtk_widget_get_sensitive(pounces_manager->modify_button))
			pidgin_selftest_fail("pounces", "Modify is not sensitive with a selection");
	}

	for (l = pounces; l != NULL; l = l->next) {
		PurplePounce *pounce = l->data;
		char *summary = events_summary(purple_pounce_get_events(pounce));

		pidgin_pounce_editor_show(NULL, NULL, pounce);
		pidgin_selftest_iterate(150);
		pidgin_selftest_log("pounces", "editor for %s (%s): open",
		                    purple_pounce_get_pouncee(pounce), summary);
		g_free(summary);
		if (pounce_editors == NULL)
			pidgin_selftest_fail("pounces", "no editor for %s",
			                     purple_pounce_get_pouncee(pounce));
		selftest_close_editors();
		if (++n >= 20)
			break;
	}
	g_list_free(pounces);

	if (purple_accounts_get_all() != NULL) {
		pidgin_pounce_editor_show(NULL, NULL, NULL);
		pidgin_selftest_iterate(150);
		if (pounce_editors == NULL)
			pidgin_selftest_fail("pounces", "the new-pounce editor did not open");
		else
			pidgin_selftest_log("pounces", "new-pounce editor: open");
		selftest_close_editors();
	}

	pidgin_pounces_manager_hide();
	pidgin_selftest_iterate(100);
	if (pounces_manager != NULL || pounce_editors != NULL)
		pidgin_selftest_fail("pounces", "windows left open");
}
