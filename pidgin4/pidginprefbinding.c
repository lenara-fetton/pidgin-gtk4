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
 * Widget <-> pref bindings. Each binding is attached to its widget as
 * object data and uses itself as the purple_prefs callback handle, so
 * destroying the widget disconnects the pref callback. The "updating"
 * flag breaks the loop widget -> pref -> callback -> widget.
 */
#include "pidgin-internal.h"
#include "pidgin.h"

#include "debug.h"
#include "prefs.h"

#include "gtkutils.h"
#include "pidginprefbinding.h"

typedef enum {
	BIND_BOOL,
	BIND_INT,
	BIND_STRING,
	BIND_PATH,
	BIND_DROPDOWN_STRING,
	BIND_DROPDOWN_INT,
	BIND_SENSITIVE,
	BIND_SENSITIVE_STRING,
} BindType;

typedef struct {
	BindType type;
	char *pref;
	GtkWidget *widget;      /* not owned: the binding dies with it */
	gboolean updating;
	gboolean invert;        /* BIND_SENSITIVE */
	char *value;            /* BIND_SENSITIVE_STRING */
} PrefBinding;

static void
binding_free(gpointer data)
{
	PrefBinding *b = data;

	purple_prefs_disconnect_by_handle(b);
	g_free(b->pref);
	g_free(b->value);
	g_free(b);
}

/* A widget may have several bindings (e.g. a value and a sensitivity);
 * each gets its own key. */
static PrefBinding *
binding_new(GtkWidget *widget, BindType type, const char *pref)
{
	PrefBinding *b = g_new0(PrefBinding, 1);
	char *key;

	b->type = type;
	b->pref = g_strdup(pref);
	b->widget = widget;
	key = g_strdup_printf("pidgin-pref-binding-%d-%s", type, pref);
	g_object_set_data_full(G_OBJECT(widget), key, b, binding_free);
	g_free(key);

	if (!purple_prefs_exists(pref))
		purple_debug_warning("prefbinding", "binding to missing pref %s\n", pref);

	return b;
}

/**************************************************************************
 * Pref -> widget
 **************************************************************************/

static PidginItem *
find_item_by_id(GtkWidget *dropdown, const char *id, guint *pos)
{
	GListModel *model = gtk_drop_down_get_model(GTK_DROP_DOWN(dropdown));
	guint i, n = model ? g_list_model_get_n_items(model) : 0;

	for (i = 0; i < n; i++) {
		PidginItem *item = g_list_model_get_item(model, i);

		if (purple_strequal(pidgin_item_get_id(item), id)) {
			*pos = i;
			return item;
		}
		g_object_unref(item);
	}
	return NULL;
}

static void
dropdown_select_id(GtkWidget *dropdown, const char *id)
{
	PidginItem *item;
	guint pos;

	if ((item = find_item_by_id(dropdown, id, &pos)) != NULL) {
		if (gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown)) != pos)
			gtk_drop_down_set_selected(GTK_DROP_DOWN(dropdown), pos);
		g_object_unref(item);
	}
}

static void
binding_update_widget(PrefBinding *b)
{
	GtkWidget *w = b->widget;
	char *str;

	if (!purple_prefs_exists(b->pref))
		return;

	b->updating = TRUE;
	switch (b->type) {
	case BIND_BOOL:
		if (GTK_IS_SWITCH(w))
			gtk_switch_set_active(GTK_SWITCH(w), purple_prefs_get_bool(b->pref));
		else
			gtk_check_button_set_active(GTK_CHECK_BUTTON(w),
			                            purple_prefs_get_bool(b->pref));
		break;
	case BIND_INT:
		if ((int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(w)) !=
		    purple_prefs_get_int(b->pref))
			gtk_spin_button_set_value(GTK_SPIN_BUTTON(w),
			                          purple_prefs_get_int(b->pref));
		break;
	case BIND_STRING:
	case BIND_PATH: {
		const char *value = b->type == BIND_PATH ?
			purple_prefs_get_path(b->pref) : purple_prefs_get_string(b->pref);

		if (!purple_strequal(gtk_editable_get_text(GTK_EDITABLE(w)),
		                     value ? value : ""))
			gtk_editable_set_text(GTK_EDITABLE(w), value ? value : "");
		break;
	}
	case BIND_DROPDOWN_STRING:
		dropdown_select_id(w, purple_prefs_get_string(b->pref));
		break;
	case BIND_DROPDOWN_INT:
		str = g_strdup_printf("%d", purple_prefs_get_int(b->pref));
		dropdown_select_id(w, str);
		g_free(str);
		break;
	case BIND_SENSITIVE:
		gtk_widget_set_sensitive(w,
			purple_prefs_get_bool(b->pref) != b->invert);
		break;
	case BIND_SENSITIVE_STRING:
		gtk_widget_set_sensitive(w,
			purple_strequal(purple_prefs_get_string(b->pref), b->value));
		break;
	}
	b->updating = FALSE;
}

static void
pref_changed_cb(const char *name, PurplePrefType type, gconstpointer value,
                gpointer data)
{
	PrefBinding *b = data;

	if (!b->updating)
		binding_update_widget(b);
}

/**************************************************************************
 * Widget -> pref
 **************************************************************************/

static void
bool_changed_cb(GtkWidget *w, gpointer unused, PrefBinding *b)
{
	gboolean active;

	if (b->updating)
		return;
	active = GTK_IS_SWITCH(w) ? gtk_switch_get_active(GTK_SWITCH(w)) :
	         gtk_check_button_get_active(GTK_CHECK_BUTTON(w));
	b->updating = TRUE;
	purple_prefs_set_bool(b->pref, active);
	b->updating = FALSE;
}

static void
check_toggled_cb(GtkCheckButton *button, PrefBinding *b)
{
	bool_changed_cb(GTK_WIDGET(button), NULL, b);
}

static void
int_changed_cb(GtkSpinButton *spin, PrefBinding *b)
{
	if (b->updating)
		return;
	b->updating = TRUE;
	purple_prefs_set_int(b->pref, gtk_spin_button_get_value_as_int(spin));
	b->updating = FALSE;
}

static void
string_changed_cb(GtkEditable *editable, PrefBinding *b)
{
	if (b->updating)
		return;
	b->updating = TRUE;
	if (b->type == BIND_PATH)
		purple_prefs_set_path(b->pref, gtk_editable_get_text(editable));
	else
		purple_prefs_set_string(b->pref, gtk_editable_get_text(editable));
	b->updating = FALSE;
}

static void
dropdown_changed_cb(GtkDropDown *dropdown, GParamSpec *pspec, PrefBinding *b)
{
	PidginItem *item;
	const char *id;

	if (b->updating)
		return;
	item = gtk_drop_down_get_selected_item(dropdown);
	if (item == NULL || !PIDGIN_IS_ITEM(item) ||
	    (id = pidgin_item_get_id(item)) == NULL)
		return;

	b->updating = TRUE;
	if (b->type == BIND_DROPDOWN_INT)
		purple_prefs_set_int(b->pref, (int)g_ascii_strtoll(id, NULL, 10));
	else
		purple_prefs_set_string(b->pref, id);
	b->updating = FALSE;
}

/**************************************************************************
 * API
 **************************************************************************/

static void
binding_connect_pref(PrefBinding *b)
{
	purple_prefs_connect_callback(b, b->pref, pref_changed_cb, b);
	binding_update_widget(b);
}

void
pidgin_pref_bind_bool(GtkWidget *widget, const char *pref)
{
	PrefBinding *b;

	g_return_if_fail(GTK_IS_CHECK_BUTTON(widget) || GTK_IS_SWITCH(widget));
	g_return_if_fail(pref != NULL);

	b = binding_new(widget, BIND_BOOL, pref);
	binding_connect_pref(b);
	if (GTK_IS_SWITCH(widget))
		g_signal_connect(widget, "notify::active", G_CALLBACK(bool_changed_cb), b);
	else
		g_signal_connect(widget, "toggled", G_CALLBACK(check_toggled_cb), b);
}

void
pidgin_pref_bind_int(GtkWidget *spin, const char *pref)
{
	PrefBinding *b;

	g_return_if_fail(GTK_IS_SPIN_BUTTON(spin));
	g_return_if_fail(pref != NULL);

	b = binding_new(spin, BIND_INT, pref);
	binding_connect_pref(b);
	g_signal_connect(spin, "value-changed", G_CALLBACK(int_changed_cb), b);
}

static void
bind_editable(GtkWidget *editable, const char *pref, BindType type)
{
	PrefBinding *b;

	g_return_if_fail(GTK_IS_EDITABLE(editable));
	g_return_if_fail(pref != NULL);

	b = binding_new(editable, type, pref);
	binding_connect_pref(b);
	g_signal_connect(editable, "changed", G_CALLBACK(string_changed_cb), b);
}

void
pidgin_pref_bind_string(GtkWidget *editable, const char *pref)
{
	bind_editable(editable, pref, BIND_STRING);
}

void
pidgin_pref_bind_path(GtkWidget *editable, const char *pref)
{
	bind_editable(editable, pref, BIND_PATH);
}

static void
bind_dropdown(GtkWidget *dropdown, const char *pref, BindType type)
{
	PrefBinding *b;

	g_return_if_fail(GTK_IS_DROP_DOWN(dropdown));
	g_return_if_fail(pref != NULL);

	b = binding_new(dropdown, type, pref);
	binding_connect_pref(b);
	g_signal_connect(dropdown, "notify::selected", G_CALLBACK(dropdown_changed_cb), b);
}

void
pidgin_pref_bind_dropdown_string(GtkWidget *dropdown, const char *pref)
{
	bind_dropdown(dropdown, pref, BIND_DROPDOWN_STRING);
}

void
pidgin_pref_bind_dropdown_int(GtkWidget *dropdown, const char *pref)
{
	bind_dropdown(dropdown, pref, BIND_DROPDOWN_INT);
}

void
pidgin_pref_bind_sensitive(GtkWidget *widget, const char *pref, gboolean invert)
{
	PrefBinding *b;

	g_return_if_fail(GTK_IS_WIDGET(widget));
	g_return_if_fail(pref != NULL);

	b = binding_new(widget, BIND_SENSITIVE, pref);
	b->invert = invert;
	binding_connect_pref(b);
}

void
pidgin_pref_bind_sensitive_string(GtkWidget *widget, const char *pref,
                                  const char *value)
{
	PrefBinding *b;

	g_return_if_fail(GTK_IS_WIDGET(widget));
	g_return_if_fail(pref != NULL);

	b = binding_new(widget, BIND_SENSITIVE_STRING, pref);
	b->value = g_strdup(value);
	binding_connect_pref(b);
}

GtkWidget *
pidgin_pref_checkbox_new(const char *label, const char *pref)
{
	GtkWidget *check = gtk_check_button_new_with_mnemonic(label);

	pidgin_pref_bind_bool(check, pref);
	return check;
}

GtkWidget *
pidgin_pref_spin_new(const char *pref, int min, int max)
{
	GtkWidget *spin = gtk_spin_button_new_with_range(min, max, 1);

	gtk_spin_button_set_digits(GTK_SPIN_BUTTON(spin), 0);
	pidgin_pref_bind_int(spin, pref);
	return spin;
}

GtkWidget *
pidgin_pref_entry_new(const char *pref)
{
	GtkWidget *entry = gtk_entry_new();

	pidgin_pref_bind_string(entry, pref);
	return entry;
}

GtkWidget *
pidgin_pref_dropdown_string_new(const char *pref, const char *const *labels,
                                const char *const *values, int n)
{
	GListStore *store = g_list_store_new(PIDGIN_TYPE_ITEM);
	GtkWidget *dropdown;
	int i;

	for (i = 0; i < n; i++) {
		PidginItem *item = pidgin_item_new(labels[i], values[i], NULL);

		g_list_store_append(store, item);
		g_object_unref(item);
	}
	dropdown = pidgin_item_dropdown_new(G_LIST_MODEL(store));
	pidgin_pref_bind_dropdown_string(dropdown, pref);
	return dropdown;
}

GtkWidget *
pidgin_pref_dropdown_int_new(const char *pref, const char *const *labels,
                             const int *values, int n)
{
	GListStore *store = g_list_store_new(PIDGIN_TYPE_ITEM);
	GtkWidget *dropdown;
	int i;

	for (i = 0; i < n; i++) {
		char *id = g_strdup_printf("%d", values[i]);
		PidginItem *item = pidgin_item_new(labels[i], id, NULL);

		g_list_store_append(store, item);
		g_object_unref(item);
		g_free(id);
	}
	dropdown = pidgin_item_dropdown_new(G_LIST_MODEL(store));
	pidgin_pref_bind_dropdown_int(dropdown, pref);
	return dropdown;
}
