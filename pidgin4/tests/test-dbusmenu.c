/*
 * pidgin4: tests for the com.canonical.dbusmenu exporter (pidgindbusmenu.c)
 * over a private session bus (GTestDBus); no display, no real session bus.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include <gio/gio.h>

#include "pidgindbusmenu.h"

#define MENU_PATH "/MenuBar"

typedef struct
{
	GTestDBus *bus;
	GDBusConnection *service;  /* exports the menu */
	GDBusConnection *client;   /* talks to it, like a tray host */
	GMenu *menu;
	GMenu *submenu;
	GSimpleActionGroup *actions;
	PidginDBusMenu *exporter;
	int quit_count;
	int about_to_show_count;
	GPtrArray *signals;        /* "Name" of every dbusmenu signal seen */
	GVariant *last_props;      /* the last ItemsPropertiesUpdated */
	guint signal_id;
} Fixture;

/* Iterates the default main context until @done is set. */
static void
spin_until(gboolean *done)
{
	gint64 deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;

	while (!*done) {
		g_main_context_iteration(NULL, TRUE);
		g_assert_cmpint(g_get_monotonic_time(), <, deadline);
	}
}

static void
spin_briefly(void)
{
	gint64 end = g_get_monotonic_time() + 100 * 1000;

	while (g_get_monotonic_time() < end)
		g_main_context_iteration(NULL, FALSE);
}

typedef struct
{
	gboolean done;
	GVariant *result;
	GError *error;
} CallData;

static void
call_done_cb(GObject *source, GAsyncResult *res, gpointer data)
{
	CallData *call = data;

	call->result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source),
	                                             res, &call->error);
	call->done = TRUE;
}

/* A method call from the client, answered by the service in this thread. */
static GVariant *
call(Fixture *f, const char *iface, const char *method, GVariant *params,
     GError **error)
{
	CallData data = { FALSE, NULL, NULL };

	g_dbus_connection_call(f->client,
		g_dbus_connection_get_unique_name(f->service), MENU_PATH, iface,
		method, params, NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL,
		call_done_cb, &data);
	spin_until(&data.done);

	if (data.error != NULL) {
		if (error != NULL)
			*error = data.error;
		else
			g_error("%s failed: %s", method, data.error->message);
	}
	return data.result;
}

static GVariant *
menu_call(Fixture *f, const char *method, GVariant *params)
{
	return call(f, PIDGIN_DBUS_MENU_INTERFACE, method, params, NULL);
}

static void
signal_cb(GDBusConnection *connection, const char *sender, const char *path,
          const char *iface, const char *name, GVariant *params, gpointer data)
{
	Fixture *f = data;

	g_ptr_array_add(f->signals, g_strdup(name));
	if (g_str_equal(name, "ItemsPropertiesUpdated")) {
		g_clear_pointer(&f->last_props, g_variant_unref);
		f->last_props = g_variant_ref(params);
	}
}

static gboolean
seen_signal(Fixture *f, const char *name)
{
	guint i;

	for (i = 0; i < f->signals->len; i++)
		if (g_str_equal(g_ptr_array_index(f->signals, i), name))
			return TRUE;
	return FALSE;
}

static void
quit_cb(GSimpleAction *action, GVariant *param, gpointer data)
{
	Fixture *f = data;

	f->quit_count++;
}

static gboolean
about_to_show_cb(PidginDBusMenu *menu, gint id, gpointer data)
{
	Fixture *f = data;

	f->about_to_show_count++;
	/* Like the docklet: refresh a dynamic part of the menu on open. */
	if (id == 0 && f->about_to_show_count == 1) {
		g_menu_append(f->submenu, "Fresh", "tray.quit");
		return TRUE;
	}
	return FALSE;
}

static const GActionEntry entries[] = {
	{ .name = "quit", .activate = quit_cb },
	{ .name = "mute", .state = "false" },
	{ .name = "status", .parameter_type = "s", .state = "'away'" },
	{ .name = "disabled", .activate = quit_cb },
};

static void
fixture_setup(Fixture *f, gconstpointer data)
{
	GMenu *section;
	GMenuItem *item;
	GIcon *icon;
	GBytes *png;
	GError *error = NULL;
	char *address;

	f->bus = g_test_dbus_new(G_TEST_DBUS_NONE);
	g_test_dbus_up(f->bus);
	address = g_strdup(g_test_dbus_get_bus_address(f->bus));

	f->service = g_dbus_connection_new_for_address_sync(address,
		G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
		G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION, NULL, NULL, &error);
	g_assert_no_error(error);
	f->client = g_dbus_connection_new_for_address_sync(address,
		G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
		G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION, NULL, NULL, &error);
	g_assert_no_error(error);
	g_free(address);

	f->actions = g_simple_action_group_new();
	g_action_map_add_action_entries(G_ACTION_MAP(f->actions), entries,
	                                G_N_ELEMENTS(entries), f);
	g_simple_action_set_enabled(G_SIMPLE_ACTION(g_action_map_lookup_action(
		G_ACTION_MAP(f->actions), "disabled")), FALSE);

	/*
	 * Show Buddy _List      (checkmark, tray.mute)
	 * ---
	 * Change _Status  >     Available (radio) / Away (radio, on)
	 * Dis_abled             (disabled action)
	 * No action             (no action: disabled)
	 * a__b                  (literal underscore; icon-name)
	 * Bytes icon            (icon-data)
	 * Hidden                (hidden-when action-missing)
	 * ---
	 * _Quit
	 */
	f->menu = g_menu_new();
	section = g_menu_new();
	g_menu_append(section, "Show Buddy _List", "tray.mute");
	g_menu_append_section(f->menu, NULL, G_MENU_MODEL(section));
	g_object_unref(section);

	section = g_menu_new();
	f->submenu = g_menu_new();
	item = g_menu_item_new("Available", NULL);
	g_menu_item_set_action_and_target(item, "tray.status", "s", "available");
	g_menu_append_item(f->submenu, item);
	g_object_unref(item);
	item = g_menu_item_new("Away", NULL);
	g_menu_item_set_action_and_target(item, "tray.status", "s", "away");
	g_menu_append_item(f->submenu, item);
	g_object_unref(item);
	g_menu_append_submenu(section, "Change _Status", G_MENU_MODEL(f->submenu));
	g_menu_append(section, "Dis_abled", "tray.disabled");
	g_menu_append(section, "No action", NULL);
	item = g_menu_item_new("a__b", "tray.quit");
	icon = g_themed_icon_new("pidgin-status-away");
	g_menu_item_set_icon(item, icon);
	g_object_unref(icon);
	g_menu_append_item(section, item);
	g_object_unref(item);
	item = g_menu_item_new("Bytes icon", "tray.quit");
	png = g_bytes_new_static("\x89PNG\r\n\x1a\n", 8);
	icon = g_bytes_icon_new(png);
	g_bytes_unref(png);
	g_menu_item_set_icon(item, icon);
	g_object_unref(icon);
	g_menu_append_item(section, item);
	g_object_unref(item);
	item = g_menu_item_new("Hidden", "tray.nonexistent");
	g_menu_item_set_attribute(item, "hidden-when", "s", "action-missing");
	g_menu_append_item(section, item);
	g_object_unref(item);
	g_menu_append_section(f->menu, NULL, G_MENU_MODEL(section));
	g_object_unref(section);

	section = g_menu_new();
	g_menu_append(section, "_Quit", "tray.quit");
	g_menu_append_section(f->menu, NULL, G_MENU_MODEL(section));
	g_object_unref(section);

	f->exporter = pidgin_dbus_menu_new(G_MENU_MODEL(f->menu),
	                                   G_ACTION_GROUP(f->actions), "tray");
	g_signal_connect(f->exporter, "about-to-show", G_CALLBACK(about_to_show_cb), f);
	pidgin_dbus_menu_export(f->exporter, f->service, MENU_PATH, &error);
	g_assert_no_error(error);

	f->signals = g_ptr_array_new_with_free_func(g_free);
	f->signal_id = g_dbus_connection_signal_subscribe(f->client,
		NULL, PIDGIN_DBUS_MENU_INTERFACE, NULL, MENU_PATH, NULL,
		G_DBUS_SIGNAL_FLAGS_NONE, signal_cb, f, NULL);
	/* Make sure the match rule is in place before anything is emitted. */
	g_variant_unref(call(f, "org.freedesktop.DBus.Peer", "Ping", NULL, NULL));
}

static void
fixture_teardown(Fixture *f, gconstpointer data)
{
	g_dbus_connection_signal_unsubscribe(f->client, f->signal_id);
	g_clear_object(&f->exporter);
	g_clear_object(&f->menu);
	g_clear_object(&f->submenu);
	g_clear_object(&f->actions);
	g_ptr_array_free(f->signals, TRUE);
	g_clear_pointer(&f->last_props, g_variant_unref);
	g_dbus_connection_close_sync(f->service, NULL, NULL);
	g_dbus_connection_close_sync(f->client, NULL, NULL);
	g_clear_object(&f->service);
	g_clear_object(&f->client);
	g_test_dbus_down(f->bus);
	g_clear_object(&f->bus);
}

/* The layout of GetLayout(0, -1, []) as (revision, root). */
static GVariant *
get_layout(Fixture *f, guint *revision)
{
	const char *none[] = { NULL };
	GVariant *reply, *layout;

	reply = menu_call(f, "GetLayout", g_variant_new("(ii^as)", 0, -1, none));
	g_variant_get(reply, "(u@(ia{sv}av))", revision, &layout);
	g_variant_unref(reply);
	return layout;
}

static GVariant *
child(GVariant *node, gsize index)
{
	GVariant *children = g_variant_get_child_value(node, 2);
	GVariant *boxed = g_variant_get_child_value(children, index);
	GVariant *result = g_variant_get_variant(boxed);

	g_variant_unref(boxed);
	g_variant_unref(children);
	return result;
}

static gsize
n_children(GVariant *node)
{
	GVariant *children = g_variant_get_child_value(node, 2);
	gsize n = g_variant_n_children(children);

	g_variant_unref(children);
	return n;
}

static gint
node_id(GVariant *node)
{
	gint id;

	g_variant_get_child(node, 0, "i", &id);
	return id;
}

static char *
prop_string(GVariant *node, const char *name)
{
	GVariant *props = g_variant_get_child_value(node, 1);
	char *value = NULL;

	g_variant_lookup(props, name, "s", &value);
	g_variant_unref(props);
	return value;
}

static gboolean
prop_lookup(GVariant *node, const char *name, const char *format, gpointer out)
{
	GVariant *props = g_variant_get_child_value(node, 1);
	gboolean found = g_variant_lookup(props, name, format, out);

	g_variant_unref(props);
	return found;
}

static void
assert_label(GVariant *node, const char *label)
{
	char *value = prop_string(node, "label");

	g_assert_cmpstr(value, ==, label);
	g_free(value);
}

static void
test_labels(void)
{
	struct { const char *in, *out; } cases[] = {
		{ "_File", "File" },
		{ "Show Buddy _List", "Show Buddy List" },
		{ "a__b", "a__b" },          /* literal _ stays escaped */
		{ "plain", "plain" },
		{ "_", "" },
		{ "x___y", "x__y" },         /* literal _ then a mnemonic */
		{ "", "" },
	};
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		char *out = pidgin_dbus_menu_label_from_mnemonic(cases[i].in);

		g_assert_cmpstr(out, ==, cases[i].out);
		g_free(out);
	}
}

static void
test_layout(Fixture *f, gconstpointer data)
{
	GVariant *root, *node, *sub;
	guint revision;
	gint32 state;
	gboolean enabled, visible;
	char *type, *display, *icon_name;
	GVariant *icon_data;

	root = get_layout(f, &revision);
	g_assert_cmpuint(revision, ==, pidgin_dbus_menu_get_revision(f->exporter));
	g_assert_cmpint(node_id(root), ==, 0);
	display = prop_string(root, "children-display");
	g_assert_cmpstr(display, ==, "submenu");
	g_free(display);

	/* 3 sections -> items plus 2 separators:
	 * 0 Show Buddy List, 1 sep, 2 Change Status, 3 Disabled,
	 * 4 No action, 5 a__b, 6 Bytes icon, 7 Hidden, 8 sep, 9 Quit */
	g_assert_cmpuint(n_children(root), ==, 10);

	node = child(root, 0);
	assert_label(node, "Show Buddy List");
	type = prop_string(node, "toggle-type");
	g_assert_cmpstr(type, ==, "checkmark");
	g_free(type);
	g_assert_true(prop_lookup(node, "toggle-state", "i", &state));
	g_assert_cmpint(state, ==, 0);
	g_assert_false(prop_lookup(node, "enabled", "b", &enabled));
	g_variant_unref(node);

	node = child(root, 1);
	type = prop_string(node, "type");
	g_assert_cmpstr(type, ==, "separator");
	g_free(type);
	g_variant_unref(node);

	node = child(root, 2);
	assert_label(node, "Change Status");
	display = prop_string(node, "children-display");
	g_assert_cmpstr(display, ==, "submenu");
	g_free(display);
	g_assert_cmpuint(n_children(node), ==, 2);
	sub = child(node, 0);
	assert_label(sub, "Available");
	type = prop_string(sub, "toggle-type");
	g_assert_cmpstr(type, ==, "radio");
	g_free(type);
	g_assert_true(prop_lookup(sub, "toggle-state", "i", &state));
	g_assert_cmpint(state, ==, 0);
	g_variant_unref(sub);
	sub = child(node, 1);
	assert_label(sub, "Away");
	g_assert_true(prop_lookup(sub, "toggle-state", "i", &state));
	g_assert_cmpint(state, ==, 1);
	g_variant_unref(sub);
	g_variant_unref(node);

	node = child(root, 3);
	assert_label(node, "Disabled");
	g_assert_true(prop_lookup(node, "enabled", "b", &enabled));
	g_assert_false(enabled);
	g_variant_unref(node);

	node = child(root, 4);
	g_assert_true(prop_lookup(node, "enabled", "b", &enabled));
	g_assert_false(enabled);
	g_variant_unref(node);

	node = child(root, 5);
	assert_label(node, "a__b");
	icon_name = prop_string(node, "icon-name");
	g_assert_cmpstr(icon_name, ==, "pidgin-status-away");
	g_free(icon_name);
	g_variant_unref(node);

	node = child(root, 6);
	g_assert_true(prop_lookup(node, "icon-data", "@ay", &icon_data));
	g_assert_cmpuint(g_variant_get_size(icon_data), ==, 8);
	g_variant_unref(icon_data);
	g_variant_unref(node);

	node = child(root, 7);
	g_assert_true(prop_lookup(node, "visible", "b", &visible));
	g_assert_false(visible);
	g_variant_unref(node);

	node = child(root, 9);
	assert_label(node, "Quit");
	g_variant_unref(node);

	g_variant_unref(root);
}

static void
test_depth_and_filter(Fixture *f, gconstpointer data)
{
	const char *names[] = { "label", NULL };
	GVariant *reply, *root, *node, *props;
	guint revision;

	/* Depth 1: the root's children, without grandchildren. */
	reply = menu_call(f, "GetLayout", g_variant_new("(ii^as)", 0, 1, names));
	g_variant_get(reply, "(u@(ia{sv}av))", &revision, &root);
	node = child(root, 2);
	g_assert_cmpuint(n_children(node), ==, 0);
	props = g_variant_get_child_value(node, 1);
	/* Only "label" was asked for. */
	g_assert_cmpuint(g_variant_n_children(props), ==, 1);
	g_variant_unref(props);
	g_variant_unref(node);
	g_variant_unref(root);
	g_variant_unref(reply);
}

static void
test_properties(Fixture *f, gconstpointer data)
{
	GVariant *root, *node, *reply, *value, *array;
	const char *none[] = { NULL };
	guint revision;
	gint quit_id, ids[2];
	GError *error = NULL;
	char *label;

	root = get_layout(f, &revision);
	node = child(root, 9);
	quit_id = node_id(node);
	g_variant_unref(node);
	g_variant_unref(root);

	reply = menu_call(f, "GetProperty", g_variant_new("(is)", quit_id, "label"));
	g_variant_get(reply, "(v)", &value);
	g_assert_cmpstr(g_variant_get_string(value, NULL), ==, "Quit");
	g_variant_unref(value);
	g_variant_unref(reply);

	ids[0] = 0;
	ids[1] = quit_id;
	array = g_variant_new_fixed_array(G_VARIANT_TYPE_INT32, ids, 2, sizeof(gint));
	reply = menu_call(f, "GetGroupProperties",
	                  g_variant_new("(@ai^as)", array, none));
	g_variant_get_child(reply, 0, "@a(ia{sv})", &value);
	g_assert_cmpuint(g_variant_n_children(value), ==, 2);
	g_variant_get_child(value, 1, "(i@a{sv})", &ids[0], &node);
	g_assert_cmpint(ids[0], ==, quit_id);
	g_assert_true(g_variant_lookup(node, "label", "s", &label));
	g_assert_cmpstr(label, ==, "Quit");
	g_free(label);
	g_variant_unref(node);
	g_variant_unref(value);
	g_variant_unref(reply);

	/* Properties */
	reply = call(f, "org.freedesktop.DBus.Properties", "Get",
		g_variant_new("(ss)", PIDGIN_DBUS_MENU_INTERFACE, "Version"), NULL);
	g_variant_get(reply, "(v)", &value);
	g_assert_cmpuint(g_variant_get_uint32(value), ==, 3);
	g_variant_unref(value);
	g_variant_unref(reply);
	reply = call(f, "org.freedesktop.DBus.Properties", "GetAll",
		g_variant_new("(s)", PIDGIN_DBUS_MENU_INTERFACE), NULL);
	g_variant_get(reply, "(@a{sv})", &value);
	g_assert_true(g_variant_lookup(value, "Status", "&s", &label));
	g_assert_cmpstr(label, ==, "normal");
	g_assert_true(g_variant_lookup(value, "TextDirection", "&s", &label));
	g_assert_cmpstr(label, ==, "ltr");
	g_assert_true(g_variant_lookup(value, "IconThemePath", "@as", &node));
	g_variant_unref(node);
	g_variant_unref(value);
	g_variant_unref(reply);

	/* Unknown ids are errors. */
	reply = call(f, PIDGIN_DBUS_MENU_INTERFACE, "GetProperty",
	             g_variant_new("(is)", 9999, "label"), &error);
	g_assert_null(reply);
	g_assert_error(error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS);
	g_clear_error(&error);
}

static void
test_events(Fixture *f, gconstpointer data)
{
	GVariant *root, *node, *sub, *reply, *state, *updated, *props;
	GVariantBuilder events;
	guint revision;
	gint quit_id, mute_id, available_id, disabled_id, id;
	gint32 toggle;

	root = get_layout(f, &revision);
	node = child(root, 9);
	quit_id = node_id(node);
	g_variant_unref(node);
	node = child(root, 0);
	mute_id = node_id(node);
	g_variant_unref(node);
	node = child(root, 3);
	disabled_id = node_id(node);
	g_variant_unref(node);
	node = child(root, 2);
	sub = child(node, 0);
	available_id = node_id(sub);
	g_variant_unref(sub);
	g_variant_unref(node);
	g_variant_unref(root);

	/* A click runs the action. */
	reply = menu_call(f, "Event", g_variant_new("(isvu)", quit_id, "clicked",
	                  g_variant_new_int32(0), 0));
	g_variant_unref(reply);
	g_assert_cmpint(f->quit_count, ==, 1);

	/* Disabled items do nothing. */
	reply = menu_call(f, "Event", g_variant_new("(isvu)", disabled_id,
	                  "clicked", g_variant_new_int32(0), 0));
	g_variant_unref(reply);
	g_assert_cmpint(f->quit_count, ==, 1);

	/* A checkmark toggles its boolean state, and the host hears it. */
	reply = menu_call(f, "Event", g_variant_new("(isvu)", mute_id, "clicked",
	                  g_variant_new_int32(0), 0));
	g_variant_unref(reply);
	state = g_action_group_get_action_state(G_ACTION_GROUP(f->actions), "mute");
	g_assert_true(g_variant_get_boolean(state));
	g_variant_unref(state);
	spin_briefly();
	g_assert_true(seen_signal(f, "ItemsPropertiesUpdated"));
	g_assert_nonnull(f->last_props);
	updated = g_variant_get_child_value(f->last_props, 0);
	g_assert_cmpuint(g_variant_n_children(updated), ==, 1);
	g_variant_get_child(updated, 0, "(i@a{sv})", &id, &props);
	g_assert_cmpint(id, ==, mute_id);
	g_assert_true(g_variant_lookup(props, "toggle-state", "i", &toggle));
	g_assert_cmpint(toggle, ==, 1);
	g_variant_unref(props);
	g_variant_unref(updated);

	/* A radio item sets the state to its target (EventGroup this time). */
	g_variant_builder_init(&events, G_VARIANT_TYPE("a(isvu)"));
	g_variant_builder_add(&events, "(isvu)", available_id, "clicked",
	                      g_variant_new_int32(0), 0);
	g_variant_builder_add(&events, "(isvu)", 4242, "clicked",
	                      g_variant_new_int32(0), 0);
	reply = menu_call(f, "EventGroup", g_variant_new("(a(isvu))", &events));
	g_assert_cmpstr(g_variant_get_type_string(reply), ==, "(ai)");
	{
		GVariant *errors = g_variant_get_child_value(reply, 0);

		g_assert_cmpuint(g_variant_n_children(errors), ==, 1);
		g_variant_unref(errors);
	}
	g_variant_unref(reply);
	state = g_action_group_get_action_state(G_ACTION_GROUP(f->actions), "status");
	g_assert_cmpstr(g_variant_get_string(state, NULL), ==, "available");
	g_variant_unref(state);

	/* Disabling an action is reported too. */
	g_ptr_array_set_size(f->signals, 0);
	g_simple_action_set_enabled(G_SIMPLE_ACTION(g_action_map_lookup_action(
		G_ACTION_MAP(f->actions), "quit")), FALSE);
	spin_briefly();
	g_assert_true(seen_signal(f, "ItemsPropertiesUpdated"));
}

static void
test_layout_updated(Fixture *f, gconstpointer data)
{
	GVariant *root, *node;
	guint revision, before;

	root = get_layout(f, &before);
	g_variant_unref(root);

	/* A change to a submenu model: LayoutUpdated, new revision. */
	g_menu_append(f->submenu, "Invisible", "tray.status::invisible");
	spin_briefly();
	g_assert_true(seen_signal(f, "LayoutUpdated"));

	root = get_layout(f, &revision);
	g_assert_cmpuint(revision, >, before);
	node = child(root, 2);
	g_assert_cmpuint(n_children(node), ==, 3);
	g_variant_unref(node);
	g_variant_unref(root);

	/* Removing the whole last section drops its separator as well. */
	g_ptr_array_set_size(f->signals, 0);
	g_menu_remove(f->menu, 2);
	spin_briefly();
	g_assert_true(seen_signal(f, "LayoutUpdated"));
	root = get_layout(f, &revision);
	g_assert_cmpuint(n_children(root), ==, 8);
	g_variant_unref(root);
}

static void
test_about_to_show(Fixture *f, gconstpointer data)
{
	GVariant *reply, *root, *node, *updates;
	gboolean need_update;
	guint revision, before;
	gint ids[] = { 0, 777 };

	root = get_layout(f, &before);
	g_variant_unref(root);

	/* The handler adds an item and says so: needUpdate, and the layout
	 * fetched right after has it (no idle in between). */
	reply = menu_call(f, "AboutToShow", g_variant_new("(i)", 0));
	g_variant_get(reply, "(b)", &need_update);
	g_variant_unref(reply);
	g_assert_true(need_update);
	g_assert_cmpint(f->about_to_show_count, ==, 1);
	root = get_layout(f, &revision);
	g_assert_cmpuint(revision, >, before);
	node = child(root, 2);
	g_assert_cmpuint(n_children(node), ==, 3);
	g_variant_unref(node);
	g_variant_unref(root);

	/* Nothing changes the second time. */
	reply = menu_call(f, "AboutToShowGroup", g_variant_new("(@ai)",
		g_variant_new_fixed_array(G_VARIANT_TYPE_INT32, ids, 2, sizeof(gint))));
	g_variant_get(reply, "(@aiai)", &updates, NULL);
	g_assert_cmpuint(g_variant_n_children(updates), ==, 0);
	g_variant_unref(updates);
	g_variant_unref(reply);
	g_assert_cmpint(f->about_to_show_count, ==, 2);
}

static void
test_set_model(Fixture *f, gconstpointer data)
{
	GMenu *other = g_menu_new();
	GVariant *root, *node;
	guint revision;

	g_menu_append(other, "Only", "tray.quit");
	pidgin_dbus_menu_set_model(f->exporter, G_MENU_MODEL(other));
	spin_briefly();
	g_assert_true(seen_signal(f, "LayoutUpdated"));

	root = get_layout(f, &revision);
	g_assert_cmpuint(n_children(root), ==, 1);
	node = child(root, 0);
	assert_label(node, "Only");
	g_variant_unref(node);
	g_variant_unref(root);

	/* The old model is no longer watched. */
	g_ptr_array_set_size(f->signals, 0);
	g_menu_append(f->menu, "ignored", NULL);
	spin_briefly();
	g_assert_false(seen_signal(f, "LayoutUpdated"));

	g_object_unref(other);

	/* Unexported: calls fail. */
	pidgin_dbus_menu_unexport(f->exporter);
	{
		GError *error = NULL;
		const char *none[] = { NULL };
		GVariant *reply = call(f, PIDGIN_DBUS_MENU_INTERFACE, "GetLayout",
			g_variant_new("(ii^as)", 0, -1, none), &error);

		g_assert_null(reply);
		g_assert_nonnull(error);
		g_clear_error(&error);
	}
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/dbusmenu/labels", test_labels);
	g_test_add("/dbusmenu/layout", Fixture, NULL, fixture_setup,
	           test_layout, fixture_teardown);
	g_test_add("/dbusmenu/depth-and-filter", Fixture, NULL, fixture_setup,
	           test_depth_and_filter, fixture_teardown);
	g_test_add("/dbusmenu/properties", Fixture, NULL, fixture_setup,
	           test_properties, fixture_teardown);
	g_test_add("/dbusmenu/events", Fixture, NULL, fixture_setup,
	           test_events, fixture_teardown);
	g_test_add("/dbusmenu/layout-updated", Fixture, NULL, fixture_setup,
	           test_layout_updated, fixture_teardown);
	g_test_add("/dbusmenu/about-to-show", Fixture, NULL, fixture_setup,
	           test_about_to_show, fixture_teardown);
	g_test_add("/dbusmenu/set-model", Fixture, NULL, fixture_setup,
	           test_set_model, fixture_teardown);

	return g_test_run();
}
