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
 * Buddy name completion (Pidgin 2's pidgin_setup_screenname_autocomplete
 * without the deprecated GtkEntryCompletion): a non-modal popover under
 * the entry lists up to MAX_MATCHES buddies whose name, alias or a word
 * of the alias starts with the typed text (case-insensitive). The focus
 * stays in the entry: a key controller on it handles Up/Down, Enter/Tab
 * (pick) and Escape; a click on a row picks it too. Picking sets the
 * entry to the buddy's name and selects the buddy's account in the
 * account drop-down or request field, if any.
 *
 * Candidates are read from the buddy list each time the popover opens,
 * and hold names and account pointers only; an account is checked to
 * still exist before it is used.
 */
#include "pidgin-internal.h"
#include "pidgin.h"

#include "account.h"
#include "blist.h"
#include "request.h"

#include "gtkutils.h"
#include "pidgincompletion.h"

#define MAX_MATCHES 10

typedef struct {
	char *name;
	char *alias;            /* or NULL */
	char *name_key;         /* casefolded */
	char *alias_key;        /* casefolded, or NULL */
	PurpleAccount *account; /* not owned; checked before use */
} Candidate;

typedef struct {
	GtkWidget *entry;
	GtkWidget *popover;
	GtkWidget *listview;
	GtkWidget *scrolled;
	GListStore *store;             /* PidginItems: id = name, data = account */
	GtkSingleSelection *selection;
	GtkWidget *account_dropdown;    /* weak */
	PurpleRequestField *account_field;
	gboolean all_accounts;
	GPtrArray *candidates;          /* NULL until the popover first opens */
	gboolean setting_text;
} Completion;

static void
candidate_free(gpointer data)
{
	Candidate *c = data;

	g_free(c->name);
	g_free(c->alias);
	g_free(c->name_key);
	g_free(c->alias_key);
	g_free(c);
}

static gboolean
account_exists(PurpleAccount *account)
{
	return account != NULL && g_list_find(purple_accounts_get_all(), account) != NULL;
}

static void
load_candidates(Completion *comp)
{
	PurpleBlistNode *node;

	if (comp->candidates != NULL)
		g_ptr_array_free(comp->candidates, TRUE);
	comp->candidates = g_ptr_array_new_with_free_func(candidate_free);

	if (purple_get_blist() == NULL)
		return;

	for (node = purple_blist_get_root(); node != NULL;
	     node = purple_blist_node_next(node, TRUE)) {
		PurpleBuddy *buddy;
		PurpleAccount *account;
		const char *alias;
		Candidate *c;

		if (!PURPLE_BLIST_NODE_IS_BUDDY(node))
			continue;
		buddy = (PurpleBuddy *)node;
		account = purple_buddy_get_account(buddy);
		/* Pidgin 2's default filter. */
		if (!comp->all_accounts && !purple_account_is_connected(account))
			continue;

		c = g_new0(Candidate, 1);
		c->name = g_strdup(purple_buddy_get_name(buddy));
		alias = purple_buddy_get_alias(buddy);
		if (alias != NULL && *alias != '\0' && !purple_strequal(alias, c->name))
			c->alias = g_strdup(alias);
		c->name_key = g_utf8_casefold(c->name, -1);
		c->alias_key = c->alias ? g_utf8_casefold(c->alias, -1) : NULL;
		c->account = account;
		g_ptr_array_add(comp->candidates, c);
	}
}

static gboolean
candidate_matches(Candidate *c, const char *key)
{
	const char *p;

	if (g_str_has_prefix(c->name_key, key))
		return TRUE;
	if (c->alias_key == NULL)
		return FALSE;
	if (g_str_has_prefix(c->alias_key, key))
		return TRUE;
	/* Any word of the alias, like Pidgin 2. */
	for (p = strchr(c->alias_key, ' '); p != NULL; p = strchr(p + 1, ' '))
		if (g_str_has_prefix(p + 1, key))
			return TRUE;
	return FALSE;
}

static gboolean
entry_has_focus(Completion *comp)
{
	GtkRoot *root = gtk_widget_get_root(comp->entry);
	GtkWidget *focus = root ? gtk_root_get_focus(root) : NULL;

	return focus != NULL && gtk_widget_get_mapped(comp->entry) &&
	       (focus == comp->entry || gtk_widget_is_ancestor(focus, comp->entry));
}

static void
popdown(Completion *comp)
{
	if (gtk_widget_get_visible(comp->popover))
		gtk_popover_popdown(GTK_POPOVER(comp->popover));
}

static void
update_matches(Completion *comp)
{
	const char *text = gtk_editable_get_text(GTK_EDITABLE(comp->entry));
	char *key;
	guint i, n = 0;

	g_list_store_remove_all(comp->store);
	/* Only while the user types (not for text set by the dialog). */
	if (text == NULL || *text == '\0' || !entry_has_focus(comp)) {
		popdown(comp);
		return;
	}

	/* Fresh candidates each time the popover opens. */
	if (comp->candidates == NULL || !gtk_widget_get_visible(comp->popover))
		load_candidates(comp);

	key = g_utf8_casefold(text, -1);
	for (i = 0; i < comp->candidates->len && n < MAX_MATCHES; i++) {
		Candidate *c = g_ptr_array_index(comp->candidates, i);
		PidginItem *item;
		GIcon *icon;
		char *label;

		if (!candidate_matches(c, key) || !account_exists(c->account))
			continue;
		label = c->alias ? g_strdup_printf("%s (%s)", c->alias, c->name) :
		                   g_strdup(c->name);
		item = pidgin_item_new(label, c->name, c->account);
		icon = pidgin_create_prpl_gicon(c->account, NULL);
		pidgin_item_set_icon(item, icon);
		g_object_unref(icon);
		g_list_store_append(comp->store, item);
		g_object_unref(item);
		g_free(label);
		n++;
	}
	g_free(key);

	/* Nothing to complete when the only match is the text itself. */
	if (n == 1) {
		PidginItem *only = g_list_model_get_item(G_LIST_MODEL(comp->store), 0);
		gboolean same = purple_strequal(pidgin_item_get_id(only), text);

		g_object_unref(only);
		if (same)
			n = 0;
	}
	if (n == 0) {
		popdown(comp);
		return;
	}

	gtk_single_selection_set_selected(comp->selection, GTK_INVALID_LIST_POSITION);
	if (!gtk_widget_get_visible(comp->popover)) {
		/* At least as wide as the entry. */
		gtk_widget_set_size_request(comp->scrolled,
			MAX(gtk_widget_get_width(comp->entry), 200), -1);
		gtk_popover_popup(GTK_POPOVER(comp->popover));
	}
}

static void
pick(Completion *comp, guint position)
{
	PidginItem *item = g_list_model_get_item(G_LIST_MODEL(comp->store), position);
	PurpleAccount *account;

	if (item == NULL)
		return;
	account = pidgin_item_get_data(item);

	comp->setting_text = TRUE;
	gtk_editable_set_text(GTK_EDITABLE(comp->entry), pidgin_item_get_id(item));
	gtk_editable_set_position(GTK_EDITABLE(comp->entry), -1);
	comp->setting_text = FALSE;
	popdown(comp);

	if (account_exists(account)) {
		if (comp->account_dropdown != NULL) {
			pidgin_account_dropdown_set_selected(comp->account_dropdown, account);
		} else if (comp->account_field != NULL) {
			/* gtkrequest.c keeps each field's widget as its ui_data:
			 * show the account in the drop-down (if the field is
			 * visible), and set the field itself either way. */
			GtkWidget *dropdown = purple_request_field_get_ui_data(comp->account_field);

			if (dropdown != NULL && GTK_IS_DROP_DOWN(dropdown))
				pidgin_account_dropdown_set_selected(dropdown, account);
			purple_request_field_account_set_value(comp->account_field, account);
		}
	}
	g_object_unref(item);
}

static void
move_selection(Completion *comp, int delta)
{
	guint n = g_list_model_get_n_items(G_LIST_MODEL(comp->store));
	guint sel = gtk_single_selection_get_selected(comp->selection);
	int next;

	if (n == 0)
		return;
	if (sel == GTK_INVALID_LIST_POSITION)
		next = delta > 0 ? 0 : (int)n - 1;
	else
		next = ((int)sel + delta + (int)n) % (int)n;
	gtk_single_selection_set_selected(comp->selection, next);
	gtk_list_view_scroll_to(GTK_LIST_VIEW(comp->listview), next,
	                        GTK_LIST_SCROLL_NONE, NULL);
}

static gboolean
key_pressed_cb(GtkEventControllerKey *controller, guint keyval, guint keycode,
               GdkModifierType state, gpointer data)
{
	Completion *comp = data;
	guint sel;

	if (!gtk_widget_get_visible(comp->popover))
		return FALSE;

	switch (keyval) {
	case GDK_KEY_Down:
	case GDK_KEY_KP_Down:
		move_selection(comp, 1);
		return TRUE;
	case GDK_KEY_Up:
	case GDK_KEY_KP_Up:
		move_selection(comp, -1);
		return TRUE;
	case GDK_KEY_Return:
	case GDK_KEY_KP_Enter:
	case GDK_KEY_ISO_Enter:
	case GDK_KEY_Tab:
		sel = gtk_single_selection_get_selected(comp->selection);
		if (sel == GTK_INVALID_LIST_POSITION && keyval == GDK_KEY_Tab &&
		    g_list_model_get_n_items(G_LIST_MODEL(comp->store)) > 0)
			sel = 0;
		if (sel == GTK_INVALID_LIST_POSITION) {
			/* Enter without a selection: close, and let the entry
			 * activate the dialog's default button. */
			popdown(comp);
			return FALSE;
		}
		pick(comp, sel);
		return TRUE;
	case GDK_KEY_Escape:
		popdown(comp);
		return TRUE;
	default:
		return FALSE;
	}
}

static void
changed_cb(GtkEditable *editable, gpointer data)
{
	Completion *comp = data;

	if (!comp->setting_text)
		update_matches(comp);
}

static void
focus_leave_cb(GtkEventControllerFocus *controller, gpointer data)
{
	popdown(data);
}

static void
activate_cb(GtkListView *view, guint position, gpointer data)
{
	pick(data, position);
}

static void
row_setup_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PIDGIN_HIG_BOX_SPACE);
	GtkWidget *image = gtk_image_new();
	GtkWidget *label = gtk_label_new(NULL);

	gtk_image_set_pixel_size(GTK_IMAGE(image),
		pidgin_prpl_icon_size_to_pixels(PIDGIN_PRPL_ICON_SMALL));
	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
	gtk_label_set_max_width_chars(GTK_LABEL(label), 40);
	gtk_box_append(GTK_BOX(box), image);
	gtk_box_append(GTK_BOX(box), label);
	gtk_list_item_set_focusable(li, FALSE);
	gtk_list_item_set_child(li, box);
}

static void
row_bind_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	PidginItem *item = gtk_list_item_get_item(li);
	GtkWidget *box = gtk_list_item_get_child(li);
	GtkWidget *image = gtk_widget_get_first_child(box);
	GtkWidget *label = gtk_widget_get_next_sibling(image);

	gtk_image_set_from_gicon(GTK_IMAGE(image), pidgin_item_get_icon(item));
	gtk_label_set_text(GTK_LABEL(label), pidgin_item_get_label(item));
}

static void
entry_destroy_cb(GtkWidget *entry, gpointer data)
{
	Completion *comp = data;

	if (comp->popover != NULL) {
		gtk_widget_unparent(comp->popover);
		comp->popover = NULL;
	}
}

static void
completion_free(gpointer data)
{
	Completion *comp = data;

	g_clear_weak_pointer(&comp->account_dropdown);
	if (comp->candidates != NULL)
		g_ptr_array_free(comp->candidates, TRUE);
	g_clear_object(&comp->selection);
	g_clear_object(&comp->store);
	g_free(comp);
}

static Completion *
completion_new(GtkWidget *entry, gboolean all_accounts)
{
	Completion *comp = g_new0(Completion, 1);
	GtkListItemFactory *factory;
	GtkEventController *controller;
	GtkWidget *sw;

	comp->entry = entry;
	comp->all_accounts = all_accounts;

	comp->store = g_list_store_new(PIDGIN_TYPE_ITEM);
	comp->selection = gtk_single_selection_new(G_LIST_MODEL(g_object_ref(comp->store)));
	gtk_single_selection_set_autoselect(comp->selection, FALSE);
	gtk_single_selection_set_can_unselect(comp->selection, TRUE);

	factory = gtk_signal_list_item_factory_new();
	g_signal_connect(factory, "setup", G_CALLBACK(row_setup_cb), NULL);
	g_signal_connect(factory, "bind", G_CALLBACK(row_bind_cb), NULL);
	comp->listview = gtk_list_view_new(
		GTK_SELECTION_MODEL(g_object_ref(comp->selection)), factory);
	gtk_list_view_set_single_click_activate(GTK_LIST_VIEW(comp->listview), TRUE);
	gtk_widget_set_can_focus(comp->listview, FALSE);
	gtk_widget_set_focusable(comp->listview, FALSE);
	g_signal_connect(comp->listview, "activate", G_CALLBACK(activate_cb), comp);

	sw = gtk_scrolled_window_new();
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), comp->listview);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER,
	                               GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(sw), TRUE);
	gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(sw), TRUE);
	comp->scrolled = sw;
	gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(sw), 320);

	comp->popover = gtk_popover_new();
	gtk_popover_set_child(GTK_POPOVER(comp->popover), sw);
	gtk_popover_set_autohide(GTK_POPOVER(comp->popover), FALSE);
	gtk_popover_set_has_arrow(GTK_POPOVER(comp->popover), FALSE);
	gtk_popover_set_position(GTK_POPOVER(comp->popover), GTK_POS_BOTTOM);
	gtk_widget_set_can_focus(comp->popover, FALSE);
	gtk_widget_set_halign(comp->popover, GTK_ALIGN_START);
	gtk_widget_add_css_class(comp->popover, "menu");
	gtk_widget_set_parent(comp->popover, entry);

	/* Before the entry's own handling (GtkEntry's GtkText child). */
	controller = gtk_event_controller_key_new();
	gtk_event_controller_set_propagation_phase(controller, GTK_PHASE_CAPTURE);
	gtk_event_controller_set_name(controller, "pidgin-buddy-completion");
	g_signal_connect(controller, "key-pressed", G_CALLBACK(key_pressed_cb), comp);
	gtk_widget_add_controller(entry, controller);

	controller = gtk_event_controller_focus_new();
	g_signal_connect(controller, "leave", G_CALLBACK(focus_leave_cb), comp);
	gtk_widget_add_controller(entry, controller);

	g_signal_connect(entry, "changed", G_CALLBACK(changed_cb), comp);
	g_signal_connect(entry, "destroy", G_CALLBACK(entry_destroy_cb), comp);
	g_object_set_data_full(G_OBJECT(entry), "pidgin-buddy-completion", comp,
	                       completion_free);
	return comp;
}

void
pidgin_buddy_completion_attach(GtkWidget *entry, GtkWidget *account_dropdown,
                               gboolean all_accounts)
{
	Completion *comp;

	g_return_if_fail(GTK_IS_EDITABLE(entry));
	g_return_if_fail(account_dropdown == NULL || GTK_IS_DROP_DOWN(account_dropdown));

	if (g_object_get_data(G_OBJECT(entry), "pidgin-buddy-completion") != NULL)
		return;
	comp = completion_new(entry, all_accounts);
	g_set_weak_pointer(&comp->account_dropdown, account_dropdown);
}

void
pidgin_buddy_completion_attach_to_field(GtkWidget *entry, gpointer account_field,
                                        gboolean all_accounts)
{
	Completion *comp;

	g_return_if_fail(GTK_IS_EDITABLE(entry));

	if (g_object_get_data(G_OBJECT(entry), "pidgin-buddy-completion") != NULL)
		return;
	comp = completion_new(entry, all_accounts);
	comp->account_field = account_field;
}
