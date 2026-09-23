/*
 * pidgin4: tests for the StatusNotifierItem (pidginsni.c) over a private
 * bus (GTestDBus), with a fake org.kde.StatusNotifierWatcher.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include <gtk/gtk.h>

#include "pidgindbusmenu.h"
#include "pidginsni.h"
#include "fake-sni-watcher.h"

#define RES "/com/minowick/Pidgin4/icons/"

typedef struct
{
	GTestDBus *bus;
	char *address;
	GDBusConnection *service;
	GDBusConnection *client;
	GDBusConnection *watcher_conn;
	FakeWatcher *watcher;
	PidginSni *sni;
	int registered;
	int unregistered;
	int activated;
	int secondary;
	int context;
	int scrolled;
	gint last_x, last_y;
	GPtrArray *signals;
	char *last_status;
	guint signal_id;
} Fixture;

static void
spin_until(gboolean (*cond)(Fixture *), Fixture *f)
{
	gint64 deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;

	while (!cond(f)) {
		g_main_context_iteration(NULL, FALSE);
		g_usleep(1000);
		g_assert_cmpint(g_get_monotonic_time(), <, deadline);
	}
}

static void
spin_briefly(void)
{
	gint64 end = g_get_monotonic_time() + 150 * 1000;

	while (g_get_monotonic_time() < end) {
		g_main_context_iteration(NULL, FALSE);
		g_usleep(1000);
	}
}

static GDBusConnection *
connect_bus(Fixture *f)
{
	GError *error = NULL;
	GDBusConnection *c = g_dbus_connection_new_for_address_sync(f->address,
		G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
		G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION, NULL, NULL, &error);

	g_assert_no_error(error);
	return c;
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

static GVariant *
call_on(Fixture *f, const char *dest, const char *path, const char *iface,
        const char *method, GVariant *params)
{
	CallData data = { FALSE, NULL, NULL };
	gint64 deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;

	g_dbus_connection_call(f->client, dest, path, iface, method, params, NULL,
		G_DBUS_CALL_FLAGS_NONE, 5000, NULL, call_done_cb, &data);
	while (!data.done) {
		g_main_context_iteration(NULL, TRUE);
		g_assert_cmpint(g_get_monotonic_time(), <, deadline);
	}
	g_assert_no_error(data.error);
	return data.result;
}

static GVariant *
get_prop(Fixture *f, const char *name)
{
	GVariant *reply, *value;

	reply = call_on(f, pidgin_sni_get_bus_name(f->sni), PIDGIN_SNI_ITEM_PATH,
		"org.freedesktop.DBus.Properties", "Get",
		g_variant_new("(ss)", PIDGIN_SNI_ITEM_INTERFACE, name));
	g_variant_get(reply, "(v)", &value);
	g_variant_unref(reply);
	return value;
}

static void
item_call(Fixture *f, const char *method, GVariant *params)
{
	g_variant_unref(call_on(f, pidgin_sni_get_bus_name(f->sni),
		PIDGIN_SNI_ITEM_PATH, PIDGIN_SNI_ITEM_INTERFACE, method, params));
}

static void
registered_cb(PidginSni *sni, gpointer data)
{
	((Fixture *)data)->registered++;
}

static void
unregistered_cb(PidginSni *sni, gpointer data)
{
	((Fixture *)data)->unregistered++;
}

static void
activate_cb(PidginSni *sni, gint x, gint y, gpointer data)
{
	Fixture *f = data;

	f->activated++;
	f->last_x = x;
	f->last_y = y;
}

static void
secondary_cb(PidginSni *sni, gint x, gint y, gpointer data)
{
	((Fixture *)data)->secondary++;
}

static void
context_cb(PidginSni *sni, gint x, gint y, gpointer data)
{
	((Fixture *)data)->context++;
}

static void
scroll_cb(PidginSni *sni, gint delta, const char *orientation, gpointer data)
{
	((Fixture *)data)->scrolled += delta;
}

static void
signal_cb(GDBusConnection *connection, const char *sender, const char *path,
          const char *iface, const char *name, GVariant *params, gpointer data)
{
	Fixture *f = data;

	g_ptr_array_add(f->signals, g_strdup(name));
	if (g_str_equal(name, "NewStatus")) {
		g_free(f->last_status);
		g_variant_get(params, "(s)", &f->last_status);
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

static gboolean
is_registered(Fixture *f)
{
	return pidgin_sni_is_registered(f->sni) &&
	       f->watcher != NULL && fake_watcher_get_n_items(f->watcher) == 1;
}

static gboolean
is_unregistered(Fixture *f)
{
	return !pidgin_sni_is_registered(f->sni);
}

static void
fixture_setup(Fixture *f, gconstpointer data)
{
	const char *icons[] = {
		RES "16x16/apps/com.minowick.Pidgin4-available.png",
		RES "22x22/apps/com.minowick.Pidgin4-available.png",
		RES "32x32/apps/com.minowick.Pidgin4-available.png",
		NULL
	};
	const char *attention[] = {
		RES "22x22/apps/com.minowick.Pidgin4-pending.png",
		NULL
	};
	GError *error = NULL;

	f->bus = g_test_dbus_new(G_TEST_DBUS_NONE);
	g_test_dbus_up(f->bus);
	f->address = g_strdup(g_test_dbus_get_bus_address(f->bus));
	f->service = connect_bus(f);
	f->client = connect_bus(f);
	f->signals = g_ptr_array_new_with_free_func(g_free);

	f->sni = pidgin_sni_new("com.minowick.Pidgin4", "Communications");
	pidgin_sni_set_title(f->sni, "Pidgin");
	pidgin_sni_set_icon(f->sni, "com.minowick.Pidgin4-available", icons);
	pidgin_sni_set_attention_icon(f->sni, "com.minowick.Pidgin4-pending", attention);
	pidgin_sni_set_icon_theme_path(f->sni, "/opt/icons");
	pidgin_sni_set_tooltip(f->sni, "Pidgin", "Available");
	g_signal_connect(f->sni, "registered", G_CALLBACK(registered_cb), f);
	g_signal_connect(f->sni, "unregistered", G_CALLBACK(unregistered_cb), f);
	g_signal_connect(f->sni, "activate", G_CALLBACK(activate_cb), f);
	g_signal_connect(f->sni, "secondary-activate", G_CALLBACK(secondary_cb), f);
	g_signal_connect(f->sni, "context-menu", G_CALLBACK(context_cb), f);
	g_signal_connect(f->sni, "scroll", G_CALLBACK(scroll_cb), f);

	g_assert_true(pidgin_sni_start(f->sni, f->service, &error));
	g_assert_no_error(error);

	f->signal_id = g_dbus_connection_signal_subscribe(f->client, NULL,
		PIDGIN_SNI_ITEM_INTERFACE, NULL, PIDGIN_SNI_ITEM_PATH, NULL,
		G_DBUS_SIGNAL_FLAGS_NONE, signal_cb, f, NULL);
}

static void
start_watcher(Fixture *f)
{
	f->watcher_conn = connect_bus(f);
	f->watcher = fake_watcher_new(f->watcher_conn, FALSE);
}

static void
stop_watcher(Fixture *f)
{
	fake_watcher_free(f->watcher);
	f->watcher = NULL;
	g_dbus_connection_close_sync(f->watcher_conn, NULL, NULL);
	g_clear_object(&f->watcher_conn);
}

static void
fixture_teardown(Fixture *f, gconstpointer data)
{
	g_dbus_connection_signal_unsubscribe(f->client, f->signal_id);
	g_clear_object(&f->sni);
	if (f->watcher != NULL)
		stop_watcher(f);
	g_ptr_array_free(f->signals, TRUE);
	g_free(f->last_status);
	g_dbus_connection_close_sync(f->service, NULL, NULL);
	g_dbus_connection_close_sync(f->client, NULL, NULL);
	g_clear_object(&f->service);
	g_clear_object(&f->client);
	g_test_dbus_down(f->bus);
	g_clear_object(&f->bus);
	g_free(f->address);
}

/* No watcher, no tray; one appearing later gets the item registered;
 * losing it unregisters; a new one registers again. */
static void
test_registration(Fixture *f, gconstpointer data)
{
	char *expected;

	spin_briefly();
	g_assert_false(pidgin_sni_is_registered(f->sni));
	g_assert_cmpint(f->registered, ==, 0);

	start_watcher(f);
	spin_until(is_registered, f);
	g_assert_cmpint(f->registered, ==, 1);
	expected = g_strconcat(pidgin_sni_get_bus_name(f->sni),
	                       "/StatusNotifierItem", NULL);
	g_assert_cmpstr(fake_watcher_get_item(f->watcher, 0), ==, expected);
	g_free(expected);
	g_assert_true(g_str_has_prefix(pidgin_sni_get_bus_name(f->sni),
	                               "org.kde.StatusNotifierItem-"));

	stop_watcher(f);
	spin_until(is_unregistered, f);
	g_assert_cmpint(f->unregistered, ==, 1);

	start_watcher(f);
	spin_until(is_registered, f);
	g_assert_cmpint(f->registered, ==, 2);

	/* Stopping the item takes it off the watcher's list. */
	pidgin_sni_stop(f->sni);
	g_assert_cmpint(f->unregistered, ==, 2);
	spin_briefly();
	g_assert_cmpuint(fake_watcher_get_n_items(f->watcher), ==, 0);
}

static void
test_properties(Fixture *f, gconstpointer data)
{
	GVariant *value, *pixmap, *bytes;
	gint w, h;
	const char *s;
	gsize i, n;

	start_watcher(f);
	spin_until(is_registered, f);

	value = get_prop(f, "Id");
	g_assert_cmpstr(g_variant_get_string(value, NULL), ==, "com.minowick.Pidgin4");
	g_variant_unref(value);
	value = get_prop(f, "Category");
	g_assert_cmpstr(g_variant_get_string(value, NULL), ==, "Communications");
	g_variant_unref(value);
	value = get_prop(f, "Title");
	g_assert_cmpstr(g_variant_get_string(value, NULL), ==, "Pidgin");
	g_variant_unref(value);
	value = get_prop(f, "Status");
	g_assert_cmpstr(g_variant_get_string(value, NULL), ==, "Active");
	g_variant_unref(value);
	value = get_prop(f, "IconName");
	g_assert_cmpstr(g_variant_get_string(value, NULL), ==, "com.minowick.Pidgin4-available");
	g_variant_unref(value);
	value = get_prop(f, "AttentionIconName");
	g_assert_cmpstr(g_variant_get_string(value, NULL), ==, "com.minowick.Pidgin4-pending");
	g_variant_unref(value);
	value = get_prop(f, "IconThemePath");
	g_assert_cmpstr(g_variant_get_string(value, NULL), ==, "/opt/icons");
	g_variant_unref(value);
	value = get_prop(f, "ItemIsMenu");
	g_assert_false(g_variant_get_boolean(value));
	g_variant_unref(value);
	value = get_prop(f, "Menu");
	g_assert_cmpstr(g_variant_get_string(value, NULL), ==, PIDGIN_SNI_MENU_PATH);
	g_variant_unref(value);

	/* Three pixmaps, 16, 22, 32 px, 4 bytes a pixel. */
	value = get_prop(f, "IconPixmap");
	n = g_variant_n_children(value);
	g_assert_cmpuint(n, ==, 3);
	for (i = 0; i < n; i++) {
		static const int sizes[] = { 16, 22, 32 };

		pixmap = g_variant_get_child_value(value, i);
		g_variant_get(pixmap, "(ii@ay)", &w, &h, &bytes);
		g_assert_cmpint(w, ==, sizes[i]);
		g_assert_cmpint(h, ==, sizes[i]);
		g_assert_cmpuint(g_variant_get_size(bytes), ==, (gsize)w * h * 4);
		g_variant_unref(bytes);
		g_variant_unref(pixmap);
	}
	g_variant_unref(value);

	value = get_prop(f, "AttentionIconPixmap");
	g_assert_cmpuint(g_variant_n_children(value), ==, 1);
	g_variant_unref(value);

	value = get_prop(f, "ToolTip");
	g_variant_get(value, "(&s@a(iiay)&s&s)", &s, NULL, NULL, NULL);
	g_assert_cmpstr(s, ==, "com.minowick.Pidgin4-available");
	{
		const char *title, *body;

		g_variant_get(value, "(&s@a(iiay)&s&s)", NULL, NULL, &title, &body);
		g_assert_cmpstr(title, ==, "Pidgin");
		g_assert_cmpstr(body, ==, "Available");
	}
	g_variant_unref(value);

	/* The menu is exported next to the item. */
	{
		const char *none[] = { NULL };
		GVariant *reply = call_on(f, pidgin_sni_get_bus_name(f->sni),
			PIDGIN_SNI_MENU_PATH, PIDGIN_DBUS_MENU_INTERFACE, "GetLayout",
			g_variant_new("(ii^as)", 0, -1, none));

		g_variant_unref(reply);
	}
}

/* The byte order of IconPixmap: A, R, G, B. */
static void
test_pixmap_byte_order(void)
{
	const char *path = RES "22x22/apps/com.minowick.Pidgin4-pending.png";
	GVariant *pixmap, *bytes;
	GdkTexture *texture;
	GdkTextureDownloader *downloader;
	GBytes *rgba;
	const guint8 *argb, *ref;
	gsize stride, i;
	gint w, h;
	gboolean checked = FALSE;

	pixmap = pidgin_sni_pixmap_from_resource(path);
	g_assert_nonnull(pixmap);
	g_variant_ref_sink(pixmap);
	g_variant_get(pixmap, "(ii@ay)", &w, &h, &bytes);
	argb = g_variant_get_data(bytes);

	texture = gdk_texture_new_from_resource(path);
	downloader = gdk_texture_downloader_new(texture);
	gdk_texture_downloader_set_format(downloader, GDK_MEMORY_R8G8B8A8);
	rgba = gdk_texture_downloader_download_bytes(downloader, &stride);
	ref = g_bytes_get_data(rgba, NULL);

	for (i = 0; i < (gsize)w * h; i++) {
		const guint8 *p = argb + i * 4;
		const guint8 *q = ref + (i / w) * stride + (i % w) * 4;

		g_assert_cmpuint(p[0], ==, q[3]);   /* A */
		if (q[3] == 255) {
			g_assert_cmpuint(p[1], ==, q[0]); /* R */
			g_assert_cmpuint(p[2], ==, q[1]); /* G */
			g_assert_cmpuint(p[3], ==, q[2]); /* B */
			checked = TRUE;
		}
	}
	g_assert_true(checked);

	g_bytes_unref(rgba);
	gdk_texture_downloader_free(downloader);
	g_object_unref(texture);
	g_variant_unref(bytes);
	g_variant_unref(pixmap);

	g_assert_null(pidgin_sni_pixmap_from_resource("/no/such/icon.png"));
}

static gboolean
got_new_status(Fixture *f)
{
	return f->last_status != NULL;
}

static void
test_methods_and_signals(Fixture *f, gconstpointer data)
{
	GVariant *value;
	char *token;

	start_watcher(f);
	spin_until(is_registered, f);

	item_call(f, "Activate", g_variant_new("(ii)", 10, 20));
	g_assert_cmpint(f->activated, ==, 1);
	g_assert_cmpint(f->last_x, ==, 10);
	g_assert_cmpint(f->last_y, ==, 20);
	item_call(f, "SecondaryActivate", g_variant_new("(ii)", 0, 0));
	g_assert_cmpint(f->secondary, ==, 1);
	item_call(f, "ContextMenu", g_variant_new("(ii)", 0, 0));
	g_assert_cmpint(f->context, ==, 1);
	item_call(f, "Scroll", g_variant_new("(is)", 3, "vertical"));
	g_assert_cmpint(f->scrolled, ==, 3);

	item_call(f, "ProvideXdgActivationToken", g_variant_new("(s)", "tok-1"));
	token = pidgin_sni_take_activation_token(f->sni);
	g_assert_cmpstr(token, ==, "tok-1");
	g_free(token);
	g_assert_null(pidgin_sni_take_activation_token(f->sni));

	/* Setters emit the New* signals; unchanged values emit nothing. */
	pidgin_sni_set_status(f->sni, PIDGIN_SNI_STATUS_NEEDS_ATTENTION);
	spin_until(got_new_status, f);
	g_assert_cmpstr(f->last_status, ==, "NeedsAttention");
	value = get_prop(f, "Status");
	g_assert_cmpstr(g_variant_get_string(value, NULL), ==, "NeedsAttention");
	g_variant_unref(value);

	g_ptr_array_set_size(f->signals, 0);
	pidgin_sni_set_title(f->sni, "Pidgin");
	pidgin_sni_set_tooltip(f->sni, "Pidgin", "Available");
	spin_briefly();
	g_assert_false(seen_signal(f, "NewTitle"));
	g_assert_false(seen_signal(f, "NewToolTip"));

	pidgin_sni_set_title(f->sni, "Pidgin (2 unread)");
	pidgin_sni_set_tooltip(f->sni, "Pidgin", "2 unread messages from bob");
	pidgin_sni_set_icon(f->sni, "com.minowick.Pidgin4-away", NULL);
	pidgin_sni_set_attention_icon(f->sni, "x", NULL);
	pidgin_sni_set_overlay_icon_name(f->sni, "y");
	spin_briefly();
	g_assert_true(seen_signal(f, "NewTitle"));
	g_assert_true(seen_signal(f, "NewToolTip"));
	g_assert_true(seen_signal(f, "NewIcon"));
	g_assert_true(seen_signal(f, "NewAttentionIcon"));
	g_assert_true(seen_signal(f, "NewOverlayIcon"));

	value = get_prop(f, "Title");
	g_assert_cmpstr(g_variant_get_string(value, NULL), ==, "Pidgin (2 unread)");
	g_variant_unref(value);
	value = get_prop(f, "IconPixmap");
	g_assert_cmpuint(g_variant_n_children(value), ==, 0);
	g_variant_unref(value);
}

/* The menu set on the item is what GetLayout returns, and clicks work. */
static void
quit_cb(GSimpleAction *action, GVariant *param, gpointer data)
{
	(*(int *)data)++;
}

static void
test_menu(Fixture *f, gconstpointer data)
{
	GSimpleActionGroup *group = g_simple_action_group_new();
	GSimpleAction *quit = g_simple_action_new("quit", NULL);
	GMenu *menu = g_menu_new();
	const char *none[] = { NULL };
	GVariant *reply, *layout, *children, *item;
	int quits = 0;
	gint id;

	g_signal_connect(quit, "activate", G_CALLBACK(quit_cb), &quits);
	g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(quit));
	g_menu_append(menu, "_Quit", "tray.quit");
	pidgin_sni_set_menu(f->sni, G_MENU_MODEL(menu), G_ACTION_GROUP(group), "tray");

	start_watcher(f);
	spin_until(is_registered, f);

	reply = call_on(f, pidgin_sni_get_bus_name(f->sni), PIDGIN_SNI_MENU_PATH,
		PIDGIN_DBUS_MENU_INTERFACE, "GetLayout",
		g_variant_new("(ii^as)", 0, -1, none));
	g_variant_get(reply, "(u@(ia{sv}av))", NULL, &layout);
	children = g_variant_get_child_value(layout, 2);
	g_assert_cmpuint(g_variant_n_children(children), ==, 1);
	g_variant_get_child(children, 0, "v", &item);
	g_variant_get_child(item, 0, "i", &id);

	g_variant_unref(call_on(f, pidgin_sni_get_bus_name(f->sni),
		PIDGIN_SNI_MENU_PATH, PIDGIN_DBUS_MENU_INTERFACE, "Event",
		g_variant_new("(isvu)", id, "clicked", g_variant_new_int32(0), 0)));
	g_assert_cmpint(quits, ==, 1);

	g_variant_unref(item);
	g_variant_unref(children);
	g_variant_unref(layout);
	g_variant_unref(reply);
	g_object_unref(menu);
	g_object_unref(quit);
	g_object_unref(group);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/sni/pixmap-byte-order", test_pixmap_byte_order);
	g_test_add("/sni/registration", Fixture, NULL, fixture_setup,
	           test_registration, fixture_teardown);
	g_test_add("/sni/properties", Fixture, NULL, fixture_setup,
	           test_properties, fixture_teardown);
	g_test_add("/sni/methods-and-signals", Fixture, NULL, fixture_setup,
	           test_methods_and_signals, fixture_teardown);
	g_test_add("/sni/menu", Fixture, NULL, fixture_setup,
	           test_menu, fixture_teardown);

	return g_test_run();
}
