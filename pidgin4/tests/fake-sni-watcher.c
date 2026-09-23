/*
 * pidgin4: a minimal org.kde.StatusNotifierWatcher for tests.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Linked into test-sni, and built as the standalone fake-sni-watcher for
 * headless runs of pidgin4 (dbus-run-session: start it, then pidgin4
 * registers its tray item with it). It keeps the list of registered items,
 * drops an item when its bus name vanishes, and emits the watcher signals.
 * With FAKE_WATCHER_MAIN it has a main() that owns the name on the session
 * bus and prints every registration on stdout.
 */
#include <gio/gio.h>

#include "fake-sni-watcher.h"

static const char watcher_xml[] =
	"<node>"
	"  <interface name='org.kde.StatusNotifierWatcher'>"
	"    <method name='RegisterStatusNotifierItem'>"
	"      <arg type='s' name='service' direction='in'/>"
	"    </method>"
	"    <method name='RegisterStatusNotifierHost'>"
	"      <arg type='s' name='service' direction='in'/>"
	"    </method>"
	"    <property name='RegisteredStatusNotifierItems' type='as' access='read'/>"
	"    <property name='IsStatusNotifierHostRegistered' type='b' access='read'/>"
	"    <property name='ProtocolVersion' type='i' access='read'/>"
	"    <signal name='StatusNotifierItemRegistered'>"
	"      <arg type='s' name='service'/>"
	"    </signal>"
	"    <signal name='StatusNotifierItemUnregistered'>"
	"      <arg type='s' name='service'/>"
	"    </signal>"
	"    <signal name='StatusNotifierHostRegistered'/>"
	"  </interface>"
	"</node>";

typedef struct
{
	char *service;   /* "<bus name><object path>" as KDE's watcher has it */
	char *bus_name;
	guint watch_id;
} WatchedItem;

struct _FakeWatcher
{
	GDBusConnection *connection;
	GDBusNodeInfo *info;
	guint registration_id;
	guint own_id;
	GList *items;        /* WatchedItem */
	gboolean verbose;
};

static void
watched_item_free(WatchedItem *item)
{
	if (item->watch_id != 0)
		g_bus_unwatch_name(item->watch_id);
	g_free(item->service);
	g_free(item->bus_name);
	g_free(item);
}

static void
emit(FakeWatcher *watcher, const char *signal, const char *service)
{
	g_dbus_connection_emit_signal(watcher->connection, NULL,
		"/StatusNotifierWatcher", "org.kde.StatusNotifierWatcher", signal,
		service != NULL ? g_variant_new("(s)", service) : NULL, NULL);
}

static void
item_vanished_cb(GDBusConnection *connection, const char *name, gpointer data)
{
	FakeWatcher *watcher = data;
	GList *l;

	for (l = watcher->items; l != NULL; l = l->next) {
		WatchedItem *item = l->data;

		if (g_str_equal(item->bus_name, name)) {
			if (watcher->verbose)
				g_print("fake-sni-watcher: unregistered %s\n", item->service);
			emit(watcher, "StatusNotifierItemUnregistered", item->service);
			watcher->items = g_list_delete_link(watcher->items, l);
			/* Unwatching from inside the callback is allowed. */
			watched_item_free(item);
			return;
		}
	}
}

static void
method_call_cb(GDBusConnection *connection, const char *sender,
               const char *object_path, const char *interface_name,
               const char *method_name, GVariant *parameters,
               GDBusMethodInvocation *invocation, gpointer data)
{
	FakeWatcher *watcher = data;
	const char *service;

	g_variant_get(parameters, "(&s)", &service);

	if (g_str_equal(method_name, "RegisterStatusNotifierItem")) {
		WatchedItem *item = g_new0(WatchedItem, 1);
		GList *l;

		/* A path means "the sender's unique name at this path". */
		if (service[0] == '/') {
			item->bus_name = g_strdup(sender);
			item->service = g_strconcat(sender, service, NULL);
		} else {
			item->bus_name = g_strdup(service);
			item->service = g_strconcat(service, "/StatusNotifierItem", NULL);
		}

		for (l = watcher->items; l != NULL; l = l->next) {
			if (g_str_equal(((WatchedItem *)l->data)->service, item->service)) {
				watched_item_free(item);
				g_dbus_method_invocation_return_value(invocation, NULL);
				return;
			}
		}

		item->watch_id = g_bus_watch_name_on_connection(connection,
			item->bus_name, G_BUS_NAME_WATCHER_FLAGS_NONE, NULL,
			item_vanished_cb, watcher, NULL);
		watcher->items = g_list_append(watcher->items, item);
		if (watcher->verbose)
			g_print("fake-sni-watcher: registered %s\n", item->service);
		g_dbus_method_invocation_return_value(invocation, NULL);
		emit(watcher, "StatusNotifierItemRegistered", item->service);
	} else if (g_str_equal(method_name, "RegisterStatusNotifierHost")) {
		g_dbus_method_invocation_return_value(invocation, NULL);
		emit(watcher, "StatusNotifierHostRegistered", NULL);
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
	FakeWatcher *watcher = data;

	if (g_str_equal(property_name, "RegisteredStatusNotifierItems")) {
		GVariantBuilder builder;
		GList *l;

		g_variant_builder_init(&builder, G_VARIANT_TYPE_STRING_ARRAY);
		for (l = watcher->items; l != NULL; l = l->next)
			g_variant_builder_add(&builder, "s", ((WatchedItem *)l->data)->service);
		return g_variant_builder_end(&builder);
	}
	if (g_str_equal(property_name, "IsStatusNotifierHostRegistered"))
		return g_variant_new_boolean(TRUE);
	if (g_str_equal(property_name, "ProtocolVersion"))
		return g_variant_new_int32(0);

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

FakeWatcher *
fake_watcher_new(GDBusConnection *connection, gboolean verbose)
{
	FakeWatcher *watcher = g_new0(FakeWatcher, 1);
	GError *error = NULL;

	watcher->connection = g_object_ref(connection);
	watcher->verbose = verbose;
	watcher->info = g_dbus_node_info_new_for_xml(watcher_xml, &error);
	g_assert_no_error(error);
	watcher->registration_id = g_dbus_connection_register_object(connection,
		"/StatusNotifierWatcher", watcher->info->interfaces[0], &vtable,
		watcher, NULL, &error);
	g_assert_no_error(error);
	watcher->own_id = g_bus_own_name_on_connection(connection,
		"org.kde.StatusNotifierWatcher", G_BUS_NAME_OWNER_FLAGS_NONE,
		NULL, NULL, NULL, NULL);

	return watcher;
}

guint
fake_watcher_get_n_items(FakeWatcher *watcher)
{
	return g_list_length(watcher->items);
}

const char *
fake_watcher_get_item(FakeWatcher *watcher, guint index)
{
	WatchedItem *item = g_list_nth_data(watcher->items, index);

	return item != NULL ? item->service : NULL;
}

void
fake_watcher_free(FakeWatcher *watcher)
{
	if (watcher == NULL)
		return;

	g_bus_unown_name(watcher->own_id);
	g_dbus_connection_unregister_object(watcher->connection,
	                                    watcher->registration_id);
	g_list_free_full(watcher->items, (GDestroyNotify)watched_item_free);
	g_dbus_node_info_unref(watcher->info);
	g_object_unref(watcher->connection);
	g_free(watcher);
}

#ifdef FAKE_WATCHER_MAIN
int
main(int argc, char *argv[])
{
	GDBusConnection *connection;
	GMainLoop *loop;
	GError *error = NULL;
	FakeWatcher *watcher;

	connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
	if (connection == NULL) {
		g_printerr("fake-sni-watcher: %s\n", error->message);
		return 1;
	}

	watcher = fake_watcher_new(connection, TRUE);
	g_print("fake-sni-watcher: running on %s\n",
	        g_dbus_connection_get_unique_name(connection));
	loop = g_main_loop_new(NULL, FALSE);
	g_main_loop_run(loop);

	fake_watcher_free(watcher);
	g_main_loop_unref(loop);
	g_object_unref(connection);
	return 0;
}
#endif
