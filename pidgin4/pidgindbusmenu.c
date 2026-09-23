/*
 * pidgin4: a com.canonical.dbusmenu exporter for a GMenuModel.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * The protocol: https://github.com/AyatanaIndicators/libdbusmenu
 * (libdbusmenu-glib/dbus-menu.xml), version 3. See pidgindbusmenu.h for
 * how a GMenuModel maps onto it.
 */
#include <string.h>

#include "pidgindbusmenu.h"

#define DBUSMENU_VERSION 3

static const char introspection_xml[] =
	"<node>"
	"  <interface name='com.canonical.dbusmenu'>"
	"    <property name='Version' type='u' access='read'/>"
	"    <property name='TextDirection' type='s' access='read'/>"
	"    <property name='Status' type='s' access='read'/>"
	"    <property name='IconThemePath' type='as' access='read'/>"
	"    <method name='GetLayout'>"
	"      <arg type='i' name='parentId' direction='in'/>"
	"      <arg type='i' name='recursionDepth' direction='in'/>"
	"      <arg type='as' name='propertyNames' direction='in'/>"
	"      <arg type='u' name='revision' direction='out'/>"
	"      <arg type='(ia{sv}av)' name='layout' direction='out'/>"
	"    </method>"
	"    <method name='GetGroupProperties'>"
	"      <arg type='ai' name='ids' direction='in'/>"
	"      <arg type='as' name='propertyNames' direction='in'/>"
	"      <arg type='a(ia{sv})' name='properties' direction='out'/>"
	"    </method>"
	"    <method name='GetProperty'>"
	"      <arg type='i' name='id' direction='in'/>"
	"      <arg type='s' name='name' direction='in'/>"
	"      <arg type='v' name='value' direction='out'/>"
	"    </method>"
	"    <method name='Event'>"
	"      <arg type='i' name='id' direction='in'/>"
	"      <arg type='s' name='eventId' direction='in'/>"
	"      <arg type='v' name='data' direction='in'/>"
	"      <arg type='u' name='timestamp' direction='in'/>"
	"    </method>"
	"    <method name='EventGroup'>"
	"      <arg type='a(isvu)' name='events' direction='in'/>"
	"      <arg type='ai' name='idErrors' direction='out'/>"
	"    </method>"
	"    <method name='AboutToShow'>"
	"      <arg type='i' name='id' direction='in'/>"
	"      <arg type='b' name='needUpdate' direction='out'/>"
	"    </method>"
	"    <method name='AboutToShowGroup'>"
	"      <arg type='ai' name='ids' direction='in'/>"
	"      <arg type='ai' name='updatesNeeded' direction='out'/>"
	"      <arg type='ai' name='idErrors' direction='out'/>"
	"    </method>"
	"    <signal name='ItemsPropertiesUpdated'>"
	"      <arg type='a(ia{sv})' name='updatedProps'/>"
	"      <arg type='a(ias)' name='removedProps'/>"
	"    </signal>"
	"    <signal name='LayoutUpdated'>"
	"      <arg type='u' name='revision'/>"
	"      <arg type='i' name='parent'/>"
	"    </signal>"
	"    <signal name='ItemActivationRequested'>"
	"      <arg type='i' name='id'/>"
	"      <arg type='u' name='timestamp'/>"
	"    </signal>"
	"  </interface>"
	"</node>";

typedef struct
{
	gint id;
	gint parent;
	gboolean separator;
	gboolean submenu;
	gboolean visible;
	gboolean pending_separator; /* build state: separate the next child */
	char *label;
	char *action;      /* name in the action group (prefix stripped) */
	GVariant *target;
	char *hidden_when;
	char *icon_name;
	GBytes *icon_data;
	GArray *children;  /* gint ids */
} MenuItem;

struct _PidginDBusMenu
{
	GObject parent;

	GMenuModel *model;
	GActionGroup *actions;
	char *prefix;
	gulong action_handlers[4];

	GDBusConnection *connection;
	char *object_path;
	guint registration_id;

	GHashTable *items;     /* id -> MenuItem */
	GPtrArray *watched;    /* GMenuModels with an items-changed handler */
	gint next_id;
	guint revision;
	guint rebuild_idle;
	gboolean in_about_to_show;
};

enum {
	SIGNAL_ABOUT_TO_SHOW,
	SIGNAL_ACTIVATED,
	N_SIGNALS
};
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(PidginDBusMenu, pidgin_dbus_menu, G_TYPE_OBJECT)

static GDBusNodeInfo *node_info = NULL;

static void rebuild(PidginDBusMenu *self, gboolean emit);

/**************************************************************************
 * Labels
 **************************************************************************/

char *
pidgin_dbus_menu_label_from_mnemonic(const char *label)
{
	GString *out;
	const char *p;

	if (label == NULL)
		return g_strdup("");

	out = g_string_sized_new(strlen(label) + 4);
	for (p = label; *p != '\0'; p++) {
		if (*p == '_') {
			if (p[1] == '_') {
				/* A literal underscore: escaped again for dbusmenu. */
				g_string_append(out, "__");
				p++;
			}
			/* else: a mnemonic marker, dropped */
		} else {
			g_string_append_c(out, *p);
		}
	}
	return g_string_free(out, FALSE);
}

const char *
pidgin_dbus_menu_get_introspection_xml(void)
{
	return introspection_xml;
}

/**************************************************************************
 * The item tree
 **************************************************************************/

static void
menu_item_free(MenuItem *item)
{
	g_free(item->label);
	g_free(item->action);
	g_clear_pointer(&item->target, g_variant_unref);
	g_free(item->hidden_when);
	g_free(item->icon_name);
	g_clear_pointer(&item->icon_data, g_bytes_unref);
	g_array_free(item->children, TRUE);
	g_free(item);
}

static MenuItem *
menu_item_new(PidginDBusMenu *self, MenuItem *parent)
{
	MenuItem *item = g_new0(MenuItem, 1);

	item->id = parent == NULL ? 0 : self->next_id++;
	item->parent = parent == NULL ? -1 : parent->id;
	item->visible = TRUE;
	item->children = g_array_new(FALSE, FALSE, sizeof(gint));
	g_hash_table_insert(self->items, GINT_TO_POINTER(item->id), item);

	return item;
}

/* Adds @item to @parent, with a separator first if one is pending. */
static void
append_child(PidginDBusMenu *self, MenuItem *parent, MenuItem *item)
{
	if (parent->pending_separator) {
		parent->pending_separator = FALSE;
		if (parent->children->len > 0) {
			MenuItem *sep = menu_item_new(self, parent);

			sep->separator = TRUE;
			g_array_append_val(parent->children, sep->id);
		}
	}

	item->parent = parent->id;
	g_array_append_val(parent->children, item->id);
}

static char *
strip_prefix(PidginDBusMenu *self, const char *action)
{
	gsize len;

	if (action == NULL || self->prefix == NULL)
		return NULL;

	len = strlen(self->prefix);
	if (strncmp(action, self->prefix, len) == 0 && action[len] == '.')
		return g_strdup(action + len + 1);

	return NULL;
}

static void
load_icon(MenuItem *item, GVariant *serialized)
{
	GIcon *icon = g_icon_deserialize(serialized);

	if (icon == NULL)
		return;

	if (G_IS_THEMED_ICON(icon)) {
		const char * const *names = g_themed_icon_get_names(G_THEMED_ICON(icon));

		if (names != NULL && names[0] != NULL)
			item->icon_name = g_strdup(names[0]);
	} else if (G_IS_BYTES_ICON(icon)) {
		item->icon_data = g_bytes_ref(g_bytes_icon_get_bytes(G_BYTES_ICON(icon)));
	} else if (G_IS_FILE_ICON(icon)) {
		char *contents = NULL;
		gsize length = 0;

		/* Small PNGs, usually from our own GResource. */
		if (g_file_load_contents(g_file_icon_get_file(G_FILE_ICON(icon)),
		                         NULL, &contents, &length, NULL, NULL))
			item->icon_data = g_bytes_new_take(contents, length);
	}

	g_object_unref(icon);
}

static void
model_changed_cb(GMenuModel *model, gint position, gint removed, gint added,
                 gpointer data);

static void
watch_model(PidginDBusMenu *self, GMenuModel *model)
{
	g_ptr_array_add(self->watched, g_object_ref(model));
	g_signal_connect(model, "items-changed", G_CALLBACK(model_changed_cb), self);
}

static void
build_children(PidginDBusMenu *self, MenuItem *parent, GMenuModel *model)
{
	gint i, n;

	watch_model(self, model);

	n = g_menu_model_get_n_items(model);
	for (i = 0; i < n; i++) {
		GMenuModel *link;
		GVariant *value;
		MenuItem *item;
		char *action = NULL;

		link = g_menu_model_get_item_link(model, i, G_MENU_LINK_SECTION);
		if (link != NULL) {
			/* Sections: flattened, separated from what is before and
			 * after them (only if both sides have items). */
			if (parent->children->len > 0)
				parent->pending_separator = TRUE;
			build_children(self, parent, link);
			parent->pending_separator = parent->children->len > 0;
			g_object_unref(link);
			continue;
		}

		item = menu_item_new(self, parent);

		g_menu_model_get_item_attribute(model, i, G_MENU_ATTRIBUTE_LABEL, "s",
		                                &item->label);
		g_menu_model_get_item_attribute(model, i, G_MENU_ATTRIBUTE_ACTION, "s",
		                                &action);
		item->action = strip_prefix(self, action);
		g_free(action);
		item->target = g_menu_model_get_item_attribute_value(model, i,
			G_MENU_ATTRIBUTE_TARGET, NULL);
		g_menu_model_get_item_attribute(model, i, "hidden-when", "s",
		                                &item->hidden_when);
		value = g_menu_model_get_item_attribute_value(model, i,
			G_MENU_ATTRIBUTE_ICON, NULL);
		if (value != NULL) {
			load_icon(item, value);
			g_variant_unref(value);
		}

		append_child(self, parent, item);

		link = g_menu_model_get_item_link(model, i, G_MENU_LINK_SUBMENU);
		if (link != NULL) {
			item->submenu = TRUE;
			build_children(self, item, link);
			g_object_unref(link);
		}
	}
}

static void
unwatch_models(PidginDBusMenu *self)
{
	guint i;

	for (i = 0; i < self->watched->len; i++) {
		GMenuModel *model = g_ptr_array_index(self->watched, i);

		g_signal_handlers_disconnect_by_func(model, model_changed_cb, self);
	}
	g_ptr_array_set_size(self->watched, 0);
}

/**************************************************************************
 * Item properties
 **************************************************************************/

static gboolean
item_action_exists(PidginDBusMenu *self, MenuItem *item)
{
	return item->action != NULL && self->actions != NULL &&
	       g_action_group_has_action(self->actions, item->action);
}

static gboolean
item_enabled(PidginDBusMenu *self, MenuItem *item)
{
	if (item->submenu || item->id == 0)
		return TRUE;
	if (!item_action_exists(self, item))
		return FALSE;
	return g_action_group_get_action_enabled(self->actions, item->action);
}

static gboolean
item_visible(PidginDBusMenu *self, MenuItem *item)
{
	if (item->hidden_when == NULL)
		return TRUE;
	if (g_str_equal(item->hidden_when, "action-missing"))
		return item_action_exists(self, item);
	if (g_str_equal(item->hidden_when, "action-disabled"))
		return item_action_exists(self, item) &&
		       g_action_group_get_action_enabled(self->actions, item->action);
	return TRUE;
}

/* toggle-type ("checkmark", "radio" or NULL) and toggle-state. */
static const char *
item_toggle(PidginDBusMenu *self, MenuItem *item, gint *state)
{
	GVariant *value;
	const char *type = NULL;

	*state = -1;
	if (!item_action_exists(self, item))
		return NULL;

	value = g_action_group_get_action_state(self->actions, item->action);
	if (value == NULL)
		return NULL;

	if (item->target == NULL &&
	    g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN)) {
		type = "checkmark";
		*state = g_variant_get_boolean(value) ? 1 : 0;
	} else if (item->target != NULL &&
	           g_variant_is_of_type(value, g_variant_get_type(item->target))) {
		type = "radio";
		*state = g_variant_equal(value, item->target) ? 1 : 0;
	}

	g_variant_unref(value);
	return type;
}

static gboolean
wanted(const char * const *names, const char *name)
{
	return names == NULL || names[0] == NULL || g_strv_contains(names, name);
}

/* The a{sv} of @item, only the non-default values, filtered by @names. */
static GVariant *
item_properties(PidginDBusMenu *self, MenuItem *item, const char * const *names)
{
	GVariantBuilder builder;

	g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);

	if (item->separator) {
		if (wanted(names, "type"))
			g_variant_builder_add(&builder, "{sv}", "type",
			                      g_variant_new_string("separator"));
		return g_variant_builder_end(&builder);
	}

	if (item->id == 0) {
		if (wanted(names, "children-display"))
			g_variant_builder_add(&builder, "{sv}", "children-display",
			                      g_variant_new_string("submenu"));
		return g_variant_builder_end(&builder);
	}

	if (wanted(names, "label")) {
		char *label = pidgin_dbus_menu_label_from_mnemonic(item->label);

		g_variant_builder_add(&builder, "{sv}", "label",
		                      g_variant_new_take_string(label));
	}
	if (!item_enabled(self, item) && wanted(names, "enabled"))
		g_variant_builder_add(&builder, "{sv}", "enabled",
		                      g_variant_new_boolean(FALSE));
	if (!item_visible(self, item) && wanted(names, "visible"))
		g_variant_builder_add(&builder, "{sv}", "visible",
		                      g_variant_new_boolean(FALSE));
	if (item->icon_name != NULL && wanted(names, "icon-name"))
		g_variant_builder_add(&builder, "{sv}", "icon-name",
		                      g_variant_new_string(item->icon_name));
	if (item->icon_data != NULL && wanted(names, "icon-data"))
		g_variant_builder_add(&builder, "{sv}", "icon-data",
			g_variant_new_from_bytes(G_VARIANT_TYPE_BYTESTRING,
			                         item->icon_data, TRUE));
	if (item->submenu && wanted(names, "children-display"))
		g_variant_builder_add(&builder, "{sv}", "children-display",
		                      g_variant_new_string("submenu"));

	{
		gint state;
		const char *toggle = item_toggle(self, item, &state);

		if (toggle != NULL) {
			if (wanted(names, "toggle-type"))
				g_variant_builder_add(&builder, "{sv}", "toggle-type",
				                      g_variant_new_string(toggle));
			if (wanted(names, "toggle-state"))
				g_variant_builder_add(&builder, "{sv}", "toggle-state",
				                      g_variant_new_int32(state));
		}
	}

	return g_variant_builder_end(&builder);
}

static GVariant *
item_layout(PidginDBusMenu *self, MenuItem *item, gint depth,
            const char * const *names)
{
	GVariantBuilder children;
	guint i;

	g_variant_builder_init(&children, G_VARIANT_TYPE("av"));
	if (depth != 0) {
		for (i = 0; i < item->children->len; i++) {
			gint id = g_array_index(item->children, gint, i);
			MenuItem *child = g_hash_table_lookup(self->items, GINT_TO_POINTER(id));

			if (child != NULL)
				g_variant_builder_add(&children, "v",
					item_layout(self, child, depth < 0 ? -1 : depth - 1, names));
		}
	}

	return g_variant_new("(i@a{sv}av)", item->id,
	                     item_properties(self, item, names), &children);
}

/**************************************************************************
 * Change tracking
 **************************************************************************/

static void
rebuild(PidginDBusMenu *self, gboolean emit)
{
	MenuItem *root;

	if (self->rebuild_idle != 0) {
		g_source_remove(self->rebuild_idle);
		self->rebuild_idle = 0;
	}

	unwatch_models(self);
	g_hash_table_remove_all(self->items);
	self->next_id = 1;

	root = menu_item_new(self, NULL);
	if (self->model != NULL)
		build_children(self, root, self->model);

	self->revision++;

	if (emit && self->connection != NULL)
		g_dbus_connection_emit_signal(self->connection, NULL,
			self->object_path, PIDGIN_DBUS_MENU_INTERFACE, "LayoutUpdated",
			g_variant_new("(ui)", self->revision, 0), NULL);
}

static gboolean
rebuild_idle_cb(gpointer data)
{
	PidginDBusMenu *self = data;

	self->rebuild_idle = 0;
	rebuild(self, TRUE);
	return G_SOURCE_REMOVE;
}

static void
model_changed_cb(GMenuModel *model, gint position, gint removed, gint added,
                 gpointer data)
{
	PidginDBusMenu *self = data;

	if (self->rebuild_idle == 0)
		self->rebuild_idle = g_idle_add(rebuild_idle_cb, self);
}

void
pidgin_dbus_menu_flush(PidginDBusMenu *menu)
{
	g_return_if_fail(PIDGIN_IS_DBUS_MENU(menu));

	if (menu->rebuild_idle != 0)
		rebuild(menu, TRUE);
}

/* An action changed: tell the host about the items that use it. */
static void
action_changed(PidginDBusMenu *self, const char *action_name)
{
	static const char *const names[] = {
		"enabled", "visible", "toggle-type", "toggle-state", NULL
	};
	GVariantBuilder updated, removed;
	GHashTableIter iter;
	gpointer value;
	gboolean any = FALSE;

	if (self->connection == NULL)
		return;

	g_variant_builder_init(&updated, G_VARIANT_TYPE("a(ia{sv})"));
	g_variant_builder_init(&removed, G_VARIANT_TYPE("a(ias)"));

	g_hash_table_iter_init(&iter, self->items);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		MenuItem *item = value;
		GVariantBuilder props, gone;
		gint state;
		const char *toggle;

		if (item->action == NULL || !g_str_equal(item->action, action_name))
			continue;

		any = TRUE;
		toggle = item_toggle(self, item, &state);

		/* Report the values explicitly, defaults included, so a change
		 * back to the default is seen as well. */
		g_variant_builder_init(&props, G_VARIANT_TYPE_VARDICT);
		g_variant_builder_add(&props, "{sv}", "enabled",
		                      g_variant_new_boolean(item_enabled(self, item)));
		g_variant_builder_add(&props, "{sv}", "visible",
		                      g_variant_new_boolean(item_visible(self, item)));
		if (toggle != NULL) {
			g_variant_builder_add(&props, "{sv}", "toggle-type",
			                      g_variant_new_string(toggle));
			g_variant_builder_add(&props, "{sv}", "toggle-state",
			                      g_variant_new_int32(state));
		}
		g_variant_builder_add(&updated, "(ia{sv})", item->id, &props);

		if (toggle == NULL) {
			g_variant_builder_init(&gone, G_VARIANT_TYPE_STRING_ARRAY);
			g_variant_builder_add(&gone, "s", names[2]);
			g_variant_builder_add(&gone, "s", names[3]);
			g_variant_builder_add(&removed, "(ias)", item->id, &gone);
		}
	}

	if (!any) {
		g_variant_builder_clear(&updated);
		g_variant_builder_clear(&removed);
		return;
	}

	g_dbus_connection_emit_signal(self->connection, NULL, self->object_path,
		PIDGIN_DBUS_MENU_INTERFACE, "ItemsPropertiesUpdated",
		g_variant_new("(a(ia{sv})a(ias))", &updated, &removed), NULL);
}

static void
action_state_changed_cb(GActionGroup *group, const char *name, GVariant *state,
                        gpointer data)
{
	action_changed(data, name);
}

static void
action_enabled_changed_cb(GActionGroup *group, const char *name,
                          gboolean enabled, gpointer data)
{
	action_changed(data, name);
}

static void
action_added_removed_cb(GActionGroup *group, const char *name, gpointer data)
{
	action_changed(data, name);
}

static void
disconnect_actions(PidginDBusMenu *self)
{
	guint i;

	if (self->actions == NULL)
		return;

	for (i = 0; i < G_N_ELEMENTS(self->action_handlers); i++) {
		if (self->action_handlers[i] != 0)
			g_signal_handler_disconnect(self->actions, self->action_handlers[i]);
		self->action_handlers[i] = 0;
	}
	g_clear_object(&self->actions);
}

/**************************************************************************
 * D-Bus methods
 **************************************************************************/

static MenuItem *
lookup_item(PidginDBusMenu *self, gint id)
{
	return g_hash_table_lookup(self->items, GINT_TO_POINTER(id));
}

static gboolean
about_to_show(PidginDBusMenu *self, gint id)
{
	gboolean changed = FALSE;
	guint revision = self->revision;

	if (self->in_about_to_show)
		return FALSE;

	self->in_about_to_show = TRUE;
	g_signal_emit(self, signals[SIGNAL_ABOUT_TO_SHOW], 0, id, &changed);
	self->in_about_to_show = FALSE;

	/* A handler that changed the model: rebuild now, so the host's
	 * GetLayout sees the new items. */
	if (self->rebuild_idle != 0)
		rebuild(self, TRUE);

	return changed || self->revision != revision;
}

/* Handles one event; FALSE if @id is unknown. */
static gboolean
handle_event(PidginDBusMenu *self, gint id, const char *event_id)
{
	MenuItem *item = lookup_item(self, id);

	if (item == NULL)
		return FALSE;

	if (g_str_equal(event_id, "clicked")) {
		if (item_action_exists(self, item) && item_enabled(self, item)) {
			char *action = g_strdup(item->action);
			GVariant *target = item->target ? g_variant_ref(item->target) : NULL;

			/* May change the model; @item is not used afterwards. */
			g_action_group_activate_action(self->actions, action, target);
			g_signal_emit(self, signals[SIGNAL_ACTIVATED], 0, action, target);
			g_free(action);
			if (target != NULL)
				g_variant_unref(target);
		}
	} else if (g_str_equal(event_id, "opened")) {
		if (item->submenu || item->id == 0)
			about_to_show(self, id);
	}
	/* "closed", "hovered": nothing to do. */

	return TRUE;
}

static void
method_call_cb(GDBusConnection *connection, const char *sender,
               const char *object_path, const char *interface_name,
               const char *method_name, GVariant *parameters,
               GDBusMethodInvocation *invocation, gpointer data)
{
	PidginDBusMenu *self = data;

	/* A model change that has not reached the idle yet: the host must
	 * see the current state. */
	if (self->rebuild_idle != 0 && !g_str_equal(method_name, "Event") &&
	    !g_str_equal(method_name, "EventGroup"))
		rebuild(self, TRUE);

	if (g_str_equal(method_name, "GetLayout")) {
		gint parent_id, depth;
		const char **names = NULL;
		MenuItem *item;

		g_variant_get(parameters, "(ii^a&s)", &parent_id, &depth, &names);
		item = lookup_item(self, parent_id);
		if (item == NULL) {
			g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
				G_DBUS_ERROR_INVALID_ARGS, "Unknown menu item %d", parent_id);
		} else {
			g_dbus_method_invocation_return_value(invocation,
				g_variant_new("(u@(ia{sv}av))", self->revision,
				              item_layout(self, item, depth, names)));
		}
		g_free(names);

	} else if (g_str_equal(method_name, "GetGroupProperties")) {
		GVariantIter *ids;
		const char **names = NULL;
		GVariantBuilder builder;
		gint id;

		g_variant_get(parameters, "(ai^a&s)", &ids, &names);
		g_variant_builder_init(&builder, G_VARIANT_TYPE("a(ia{sv})"));
		while (g_variant_iter_next(ids, "i", &id)) {
			MenuItem *item = lookup_item(self, id);

			if (item != NULL)
				g_variant_builder_add(&builder, "(i@a{sv})", id,
				                      item_properties(self, item, names));
		}
		g_variant_iter_free(ids);
		g_free(names);
		g_dbus_method_invocation_return_value(invocation,
			g_variant_new("(a(ia{sv}))", &builder));

	} else if (g_str_equal(method_name, "GetProperty")) {
		gint id;
		const char *name;
		MenuItem *item;
		GVariant *props, *value = NULL;

		g_variant_get(parameters, "(i&s)", &id, &name);
		item = lookup_item(self, id);
		if (item != NULL) {
			const char *names[] = { name, NULL };

			props = item_properties(self, item, names);
			value = g_variant_lookup_value(props, name, NULL);
			g_variant_unref(g_variant_ref_sink(props));
		}
		if (value == NULL) {
			g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
				G_DBUS_ERROR_INVALID_ARGS, "No property %s on item %d",
				name, id);
		} else {
			g_dbus_method_invocation_return_value(invocation,
				g_variant_new("(v)", value));
			g_variant_unref(value);
		}

	} else if (g_str_equal(method_name, "Event")) {
		gint id;
		const char *event_id;

		g_variant_get(parameters, "(i&svu)", &id, &event_id, NULL, NULL);
		if (self->rebuild_idle != 0 && !g_str_equal(event_id, "clicked"))
			rebuild(self, TRUE);
		/* A click on an id from an old revision is still resolved in the
		 * current tree: the host acts on what it showed, and the ids of a
		 * rebuild of an unchanged model are the same. */
		if (handle_event(self, id, event_id))
			g_dbus_method_invocation_return_value(invocation, NULL);
		else
			g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
				G_DBUS_ERROR_INVALID_ARGS, "Unknown menu item %d", id);

	} else if (g_str_equal(method_name, "EventGroup")) {
		GVariantIter *events;
		GVariantBuilder errors;
		gint id;
		const char *event_id;
		gboolean any_ok = FALSE;
		gsize count;

		g_variant_get(parameters, "(a(isvu))", &events);
		count = g_variant_iter_n_children(events);
		g_variant_builder_init(&errors, G_VARIANT_TYPE("ai"));
		while (g_variant_iter_next(events, "(i&svu)", &id, &event_id, NULL, NULL)) {
			if (handle_event(self, id, event_id))
				any_ok = TRUE;
			else
				g_variant_builder_add(&errors, "i", id);
		}
		g_variant_iter_free(events);
		if (!any_ok && count > 0) {
			g_variant_builder_clear(&errors);
			g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
				G_DBUS_ERROR_INVALID_ARGS, "No valid menu item in the group");
		} else {
			g_dbus_method_invocation_return_value(invocation,
				g_variant_new("(ai)", &errors));
		}

	} else if (g_str_equal(method_name, "AboutToShow")) {
		gint id;

		g_variant_get(parameters, "(i)", &id);
		if (lookup_item(self, id) == NULL) {
			g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
				G_DBUS_ERROR_INVALID_ARGS, "Unknown menu item %d", id);
		} else {
			g_dbus_method_invocation_return_value(invocation,
				g_variant_new("(b)", about_to_show(self, id)));
		}

	} else if (g_str_equal(method_name, "AboutToShowGroup")) {
		GVariantIter *ids;
		GVariantBuilder updates, errors;
		gint id;

		g_variant_get(parameters, "(ai)", &ids);
		g_variant_builder_init(&updates, G_VARIANT_TYPE("ai"));
		g_variant_builder_init(&errors, G_VARIANT_TYPE("ai"));
		while (g_variant_iter_next(ids, "i", &id)) {
			if (lookup_item(self, id) == NULL)
				g_variant_builder_add(&errors, "i", id);
			else if (about_to_show(self, id))
				g_variant_builder_add(&updates, "i", id);
		}
		g_variant_iter_free(ids);
		g_dbus_method_invocation_return_value(invocation,
			g_variant_new("(aiai)", &updates, &errors));

	} else {
		g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
			G_DBUS_ERROR_UNKNOWN_METHOD, "Unknown method %s", method_name);
	}
}

static GVariant *
get_property_cb(GDBusConnection *connection, const char *sender,
                const char *object_path, const char *interface_name,
                const char *property_name, GError **error, gpointer data)
{
	if (g_str_equal(property_name, "Version"))
		return g_variant_new_uint32(DBUSMENU_VERSION);
	if (g_str_equal(property_name, "TextDirection"))
		return g_variant_new_string("ltr");
	if (g_str_equal(property_name, "Status"))
		return g_variant_new_string("normal");
	if (g_str_equal(property_name, "IconThemePath"))
		return g_variant_new_strv(NULL, 0);

	g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY,
	            "Unknown property %s", property_name);
	return NULL;
}

static const GDBusInterfaceVTable vtable = {
	method_call_cb,
	get_property_cb,
	NULL,
	{ 0 }
};

/**************************************************************************
 * Public API
 **************************************************************************/

gboolean
pidgin_dbus_menu_export(PidginDBusMenu *menu, GDBusConnection *connection,
                        const char *object_path, GError **error)
{
	g_return_val_if_fail(PIDGIN_IS_DBUS_MENU(menu), FALSE);
	g_return_val_if_fail(G_IS_DBUS_CONNECTION(connection), FALSE);
	g_return_val_if_fail(object_path != NULL, FALSE);

	pidgin_dbus_menu_unexport(menu);

	menu->registration_id = g_dbus_connection_register_object(connection,
		object_path, node_info->interfaces[0], &vtable, menu, NULL, error);
	if (menu->registration_id == 0)
		return FALSE;

	menu->connection = g_object_ref(connection);
	menu->object_path = g_strdup(object_path);
	return TRUE;
}

void
pidgin_dbus_menu_unexport(PidginDBusMenu *menu)
{
	g_return_if_fail(PIDGIN_IS_DBUS_MENU(menu));

	if (menu->registration_id != 0)
		g_dbus_connection_unregister_object(menu->connection,
		                                    menu->registration_id);
	menu->registration_id = 0;
	g_clear_object(&menu->connection);
	g_clear_pointer(&menu->object_path, g_free);
}

const char *
pidgin_dbus_menu_get_object_path(PidginDBusMenu *menu)
{
	g_return_val_if_fail(PIDGIN_IS_DBUS_MENU(menu), NULL);
	return menu->object_path;
}

void
pidgin_dbus_menu_set_model(PidginDBusMenu *menu, GMenuModel *model)
{
	g_return_if_fail(PIDGIN_IS_DBUS_MENU(menu));
	g_return_if_fail(model == NULL || G_IS_MENU_MODEL(model));

	if (model != NULL)
		g_object_ref(model);
	unwatch_models(menu);
	g_clear_object(&menu->model);
	menu->model = model;
	rebuild(menu, TRUE);
}

void
pidgin_dbus_menu_set_actions(PidginDBusMenu *menu, GActionGroup *actions,
                             const char *action_prefix)
{
	g_return_if_fail(PIDGIN_IS_DBUS_MENU(menu));
	g_return_if_fail(actions == NULL || G_IS_ACTION_GROUP(actions));

	if (actions != NULL)
		g_object_ref(actions);
	disconnect_actions(menu);
	menu->actions = actions;
	g_free(menu->prefix);
	menu->prefix = g_strdup(action_prefix);

	if (actions != NULL) {
		menu->action_handlers[0] = g_signal_connect(actions,
			"action-state-changed", G_CALLBACK(action_state_changed_cb), menu);
		menu->action_handlers[1] = g_signal_connect(actions,
			"action-enabled-changed", G_CALLBACK(action_enabled_changed_cb), menu);
		menu->action_handlers[2] = g_signal_connect(actions,
			"action-added", G_CALLBACK(action_added_removed_cb), menu);
		menu->action_handlers[3] = g_signal_connect(actions,
			"action-removed", G_CALLBACK(action_added_removed_cb), menu);
	}

	rebuild(menu, TRUE);
}

guint
pidgin_dbus_menu_get_revision(PidginDBusMenu *menu)
{
	g_return_val_if_fail(PIDGIN_IS_DBUS_MENU(menu), 0);
	return menu->revision;
}

PidginDBusMenu *
pidgin_dbus_menu_new(GMenuModel *model, GActionGroup *actions,
                     const char *action_prefix)
{
	PidginDBusMenu *menu = g_object_new(PIDGIN_TYPE_DBUS_MENU, NULL);

	if (model != NULL)
		menu->model = g_object_ref(model);
	/* Builds the first layout. */
	pidgin_dbus_menu_set_actions(menu, actions, action_prefix);

	return menu;
}

/**************************************************************************
 * GObject
 **************************************************************************/

static void
pidgin_dbus_menu_dispose(GObject *object)
{
	PidginDBusMenu *self = PIDGIN_DBUS_MENU(object);

	pidgin_dbus_menu_unexport(self);
	if (self->rebuild_idle != 0) {
		g_source_remove(self->rebuild_idle);
		self->rebuild_idle = 0;
	}
	unwatch_models(self);
	disconnect_actions(self);
	g_clear_object(&self->model);

	G_OBJECT_CLASS(pidgin_dbus_menu_parent_class)->dispose(object);
}

static void
pidgin_dbus_menu_finalize(GObject *object)
{
	PidginDBusMenu *self = PIDGIN_DBUS_MENU(object);

	g_hash_table_destroy(self->items);
	g_ptr_array_free(self->watched, TRUE);
	g_free(self->prefix);

	G_OBJECT_CLASS(pidgin_dbus_menu_parent_class)->finalize(object);
}

static void
pidgin_dbus_menu_init(PidginDBusMenu *self)
{
	self->items = g_hash_table_new_full(NULL, NULL, NULL,
	                                    (GDestroyNotify)menu_item_free);
	self->watched = g_ptr_array_new_with_free_func(g_object_unref);
	self->next_id = 1;
}

static void
pidgin_dbus_menu_class_init(PidginDBusMenuClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GError *error = NULL;

	object_class->dispose = pidgin_dbus_menu_dispose;
	object_class->finalize = pidgin_dbus_menu_finalize;

	signals[SIGNAL_ABOUT_TO_SHOW] = g_signal_new("about-to-show",
		G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
		g_signal_accumulator_true_handled, NULL, NULL,
		G_TYPE_BOOLEAN, 1, G_TYPE_INT);
	signals[SIGNAL_ACTIVATED] = g_signal_new("activated",
		G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_VARIANT);

	node_info = g_dbus_node_info_new_for_xml(introspection_xml, &error);
	g_assert_no_error(error);
}
