/*
 * pidgin4: a StatusNotifierItem (tray icon) on the session bus.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * The protocol: https://www.freedesktop.org/wiki/Specifications/StatusNotifierItem/
 * plus the KDE additions hosts use today (ProvideXdgActivationToken). See
 * pidginsni.h.
 */
#include <unistd.h>

#include <gtk/gtk.h>

#include "pidgindbusmenu.h"
#include "pidginsni.h"

static const char introspection_xml[] =
	"<node>"
	"  <interface name='org.kde.StatusNotifierItem'>"
	"    <property name='Category' type='s' access='read'/>"
	"    <property name='Id' type='s' access='read'/>"
	"    <property name='Title' type='s' access='read'/>"
	"    <property name='Status' type='s' access='read'/>"
	"    <property name='WindowId' type='i' access='read'/>"
	"    <property name='IconThemePath' type='s' access='read'/>"
	"    <property name='Menu' type='o' access='read'/>"
	"    <property name='ItemIsMenu' type='b' access='read'/>"
	"    <property name='IconName' type='s' access='read'/>"
	"    <property name='IconPixmap' type='a(iiay)' access='read'/>"
	"    <property name='OverlayIconName' type='s' access='read'/>"
	"    <property name='OverlayIconPixmap' type='a(iiay)' access='read'/>"
	"    <property name='AttentionIconName' type='s' access='read'/>"
	"    <property name='AttentionIconPixmap' type='a(iiay)' access='read'/>"
	"    <property name='AttentionMovieName' type='s' access='read'/>"
	"    <property name='ToolTip' type='(sa(iiay)ss)' access='read'/>"
	"    <method name='ContextMenu'>"
	"      <arg type='i' name='x' direction='in'/>"
	"      <arg type='i' name='y' direction='in'/>"
	"    </method>"
	"    <method name='Activate'>"
	"      <arg type='i' name='x' direction='in'/>"
	"      <arg type='i' name='y' direction='in'/>"
	"    </method>"
	"    <method name='SecondaryActivate'>"
	"      <arg type='i' name='x' direction='in'/>"
	"      <arg type='i' name='y' direction='in'/>"
	"    </method>"
	"    <method name='Scroll'>"
	"      <arg type='i' name='delta' direction='in'/>"
	"      <arg type='s' name='orientation' direction='in'/>"
	"    </method>"
	"    <method name='ProvideXdgActivationToken'>"
	"      <arg type='s' name='token' direction='in'/>"
	"    </method>"
	"    <signal name='NewTitle'/>"
	"    <signal name='NewIcon'/>"
	"    <signal name='NewAttentionIcon'/>"
	"    <signal name='NewOverlayIcon'/>"
	"    <signal name='NewMenu'/>"
	"    <signal name='NewToolTip'/>"
	"    <signal name='NewStatus'>"
	"      <arg type='s' name='status'/>"
	"    </signal>"
	"  </interface>"
	"</node>";

typedef struct
{
	char *name;
	GVariant *pixmaps; /* a(iiay), floating ref sunk */
} SniIcon;

struct _PidginSni
{
	GObject parent;

	char *id;
	char *category;
	char *title;
	PidginSniStatus status;
	SniIcon icon;
	SniIcon attention;
	char *overlay_name;
	char *icon_theme_path;
	char *tooltip_title;
	char *tooltip_body;
	char *activation_token;

	PidginDBusMenu *menu;

	GDBusConnection *connection;
	char *bus_name;
	guint registration_id;
	guint own_id;
	guint watch_id;
	gboolean name_owned;
	gboolean watcher_present;
	gboolean registered;
	GCancellable *register_cancellable;
};

enum {
	SIGNAL_ACTIVATE,
	SIGNAL_SECONDARY_ACTIVATE,
	SIGNAL_CONTEXT_MENU,
	SIGNAL_SCROLL,
	SIGNAL_REGISTERED,
	SIGNAL_UNREGISTERED,
	N_SIGNALS
};
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(PidginSni, pidgin_sni, G_TYPE_OBJECT)

static GDBusNodeInfo *node_info = NULL;

const char *
pidgin_sni_get_introspection_xml(void)
{
	return introspection_xml;
}

/**************************************************************************
 * Pixmaps
 **************************************************************************/

GVariant *
pidgin_sni_pixmap_from_resource(const char *path)
{
	GdkTexture *texture;
	GdkTextureDownloader *downloader;
	GBytes *bytes;
	gsize stride;
	int width, height;
	GVariant *result;

	if (!g_resources_get_info(path, G_RESOURCE_LOOKUP_FLAGS_NONE, NULL, NULL,
	                          NULL))
		return NULL;

	texture = gdk_texture_new_from_resource(path);
	if (texture == NULL)
		return NULL;

	width = gdk_texture_get_width(texture);
	height = gdk_texture_get_height(texture);

	/* GDK_MEMORY_A8R8G8B8 is the bytes A, R, G, B in that order,
	 * unpremultiplied: exactly the SNI's ARGB32 in network byte order. */
	downloader = gdk_texture_downloader_new(texture);
	gdk_texture_downloader_set_format(downloader, GDK_MEMORY_A8R8G8B8);
	bytes = gdk_texture_downloader_download_bytes(downloader, &stride);
	gdk_texture_downloader_free(downloader);
	g_object_unref(texture);

	if (stride != (gsize)width * 4) {
		/* Repack without row padding. */
		GByteArray *packed = g_byte_array_sized_new(width * 4 * height);
		const guint8 *data = g_bytes_get_data(bytes, NULL);
		int row;

		for (row = 0; row < height; row++)
			g_byte_array_append(packed, data + row * stride, width * 4);
		g_bytes_unref(bytes);
		bytes = g_byte_array_free_to_bytes(packed);
	}

	result = g_variant_new("(ii@ay)", width, height,
		g_variant_new_from_bytes(G_VARIANT_TYPE_BYTESTRING, bytes, TRUE));
	g_bytes_unref(bytes);
	return result;
}

static void
sni_icon_set(SniIcon *icon, const char *name, const char * const *resources)
{
	GVariantBuilder builder;

	g_free(icon->name);
	icon->name = g_strdup(name != NULL ? name : "");
	g_clear_pointer(&icon->pixmaps, g_variant_unref);

	g_variant_builder_init(&builder, G_VARIANT_TYPE("a(iiay)"));
	for (; resources != NULL && *resources != NULL; resources++) {
		GVariant *pixmap = pidgin_sni_pixmap_from_resource(*resources);

		if (pixmap != NULL)
			g_variant_builder_add_value(&builder, pixmap);
		else
			g_warning("SNI: cannot load the icon resource %s", *resources);
	}
	icon->pixmaps = g_variant_ref_sink(g_variant_builder_end(&builder));
}

static void
sni_icon_clear(SniIcon *icon)
{
	g_clear_pointer(&icon->name, g_free);
	g_clear_pointer(&icon->pixmaps, g_variant_unref);
}

static GVariant *
empty_pixmaps(void)
{
	return g_variant_new_array(G_VARIANT_TYPE("(iiay)"), NULL, 0);
}

static GVariant *
icon_pixmaps(SniIcon *icon)
{
	return icon->pixmaps != NULL ? g_variant_ref(icon->pixmaps) : empty_pixmaps();
}

/**************************************************************************
 * D-Bus
 **************************************************************************/

static const char *
status_string(PidginSniStatus status)
{
	switch (status) {
	case PIDGIN_SNI_STATUS_PASSIVE:
		return "Passive";
	case PIDGIN_SNI_STATUS_NEEDS_ATTENTION:
		return "NeedsAttention";
	case PIDGIN_SNI_STATUS_ACTIVE:
	default:
		return "Active";
	}
}

static void
emit_item_signal(PidginSni *sni, const char *name, GVariant *params)
{
	if (sni->connection == NULL || sni->registration_id == 0) {
		if (params != NULL)
			g_variant_unref(g_variant_ref_sink(params));
		return;
	}

	g_dbus_connection_emit_signal(sni->connection, NULL, PIDGIN_SNI_ITEM_PATH,
	                              PIDGIN_SNI_ITEM_INTERFACE, name, params, NULL);
}

static void
method_call_cb(GDBusConnection *connection, const char *sender,
               const char *object_path, const char *interface_name,
               const char *method_name, GVariant *parameters,
               GDBusMethodInvocation *invocation, gpointer data)
{
	PidginSni *sni = data;
	gint x = 0, y = 0;

	/* Reply first: a handler may run a nested main loop or take long,
	 * and the host must not wait for it. */
	g_object_ref(sni);
	if (g_str_equal(method_name, "Activate")) {
		g_variant_get(parameters, "(ii)", &x, &y);
		g_dbus_method_invocation_return_value(invocation, NULL);
		g_signal_emit(sni, signals[SIGNAL_ACTIVATE], 0, x, y);
	} else if (g_str_equal(method_name, "SecondaryActivate")) {
		g_variant_get(parameters, "(ii)", &x, &y);
		g_dbus_method_invocation_return_value(invocation, NULL);
		g_signal_emit(sni, signals[SIGNAL_SECONDARY_ACTIVATE], 0, x, y);
	} else if (g_str_equal(method_name, "ContextMenu")) {
		g_variant_get(parameters, "(ii)", &x, &y);
		g_dbus_method_invocation_return_value(invocation, NULL);
		g_signal_emit(sni, signals[SIGNAL_CONTEXT_MENU], 0, x, y);
	} else if (g_str_equal(method_name, "Scroll")) {
		const char *orientation;

		g_variant_get(parameters, "(i&s)", &x, &orientation);
		g_signal_emit(sni, signals[SIGNAL_SCROLL], 0, x, orientation);
		g_dbus_method_invocation_return_value(invocation, NULL);
	} else if (g_str_equal(method_name, "ProvideXdgActivationToken")) {
		g_free(sni->activation_token);
		g_variant_get(parameters, "(s)", &sni->activation_token);
		g_dbus_method_invocation_return_value(invocation, NULL);
	} else {
		g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
			G_DBUS_ERROR_UNKNOWN_METHOD, "Unknown method %s", method_name);
	}
	g_object_unref(sni);
}

static GVariant *
get_property_cb(GDBusConnection *connection, const char *sender,
                const char *object_path, const char *interface_name,
                const char *property_name, GError **error, gpointer data)
{
	PidginSni *sni = data;

	if (g_str_equal(property_name, "Category"))
		return g_variant_new_string(sni->category);
	if (g_str_equal(property_name, "Id"))
		return g_variant_new_string(sni->id);
	if (g_str_equal(property_name, "Title"))
		return g_variant_new_string(sni->title ? sni->title : "");
	if (g_str_equal(property_name, "Status"))
		return g_variant_new_string(status_string(sni->status));
	if (g_str_equal(property_name, "WindowId"))
		return g_variant_new_int32(0);
	if (g_str_equal(property_name, "IconThemePath"))
		return g_variant_new_string(sni->icon_theme_path ? sni->icon_theme_path : "");
	if (g_str_equal(property_name, "Menu"))
		return g_variant_new_object_path(PIDGIN_SNI_MENU_PATH);
	if (g_str_equal(property_name, "ItemIsMenu"))
		return g_variant_new_boolean(FALSE);
	if (g_str_equal(property_name, "IconName"))
		return g_variant_new_string(sni->icon.name ? sni->icon.name : "");
	if (g_str_equal(property_name, "IconPixmap"))
		return icon_pixmaps(&sni->icon);
	if (g_str_equal(property_name, "OverlayIconName"))
		return g_variant_new_string(sni->overlay_name ? sni->overlay_name : "");
	if (g_str_equal(property_name, "OverlayIconPixmap"))
		return empty_pixmaps();
	if (g_str_equal(property_name, "AttentionIconName"))
		return g_variant_new_string(sni->attention.name ? sni->attention.name : "");
	if (g_str_equal(property_name, "AttentionIconPixmap"))
		return icon_pixmaps(&sni->attention);
	if (g_str_equal(property_name, "AttentionMovieName"))
		return g_variant_new_string("");
	if (g_str_equal(property_name, "ToolTip"))
		/* The tooltip icon: the item's own, as hosts expect. */
		return g_variant_new("(s@a(iiay)ss)",
			sni->icon.name ? sni->icon.name : "", icon_pixmaps(&sni->icon),
			sni->tooltip_title ? sni->tooltip_title : "",
			sni->tooltip_body ? sni->tooltip_body : "");

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
 * Registration with the watcher
 **************************************************************************/

static void
set_registered(PidginSni *sni, gboolean registered)
{
	if (sni->registered == registered)
		return;

	sni->registered = registered;
	g_signal_emit(sni, signals[registered ? SIGNAL_REGISTERED : SIGNAL_UNREGISTERED], 0);
}

static void
register_done_cb(GObject *source, GAsyncResult *result, gpointer data)
{
	PidginSni *sni;
	GVariant *reply;
	GError *error = NULL;

	reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
	if (reply == NULL) {
		if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
			sni = data;
			g_message("SNI: RegisterStatusNotifierItem failed: %s",
			          error->message);
			g_clear_object(&sni->register_cancellable);
		}
		g_error_free(error);
		return;
	}
	g_variant_unref(reply);

	sni = data;
	g_clear_object(&sni->register_cancellable);
	if (sni->watcher_present)
		set_registered(sni, TRUE);
}

static void
try_register(PidginSni *sni)
{
	if (!sni->name_owned || !sni->watcher_present || sni->connection == NULL)
		return;

	if (sni->register_cancellable != NULL) {
		g_cancellable_cancel(sni->register_cancellable);
		g_clear_object(&sni->register_cancellable);
	}
	sni->register_cancellable = g_cancellable_new();

	g_dbus_connection_call(sni->connection, PIDGIN_SNI_WATCHER_NAME,
		PIDGIN_SNI_WATCHER_PATH, PIDGIN_SNI_WATCHER_NAME,
		"RegisterStatusNotifierItem", g_variant_new("(s)", sni->bus_name),
		NULL, G_DBUS_CALL_FLAGS_NONE, 10000, sni->register_cancellable,
		register_done_cb, sni);
}

static void
watcher_appeared_cb(GDBusConnection *connection, const char *name,
                    const char *owner, gpointer data)
{
	PidginSni *sni = data;

	g_debug("SNI: %s appeared (%s)", name, owner);
	sni->watcher_present = TRUE;
	set_registered(sni, FALSE);
	try_register(sni);
}

static void
watcher_vanished_cb(GDBusConnection *connection, const char *name,
                    gpointer data)
{
	PidginSni *sni = data;

	g_debug("SNI: %s vanished", name);
	sni->watcher_present = FALSE;
	if (sni->register_cancellable != NULL) {
		g_cancellable_cancel(sni->register_cancellable);
		g_clear_object(&sni->register_cancellable);
	}
	set_registered(sni, FALSE);
}

static void
name_acquired_cb(GDBusConnection *connection, const char *name, gpointer data)
{
	PidginSni *sni = data;

	sni->name_owned = TRUE;
	try_register(sni);
}

static void
name_lost_cb(GDBusConnection *connection, const char *name, gpointer data)
{
	PidginSni *sni = data;

	if (connection != NULL)
		g_message("SNI: could not own %s", name);
	sni->name_owned = FALSE;
	set_registered(sni, FALSE);
}

/**************************************************************************
 * Public API
 **************************************************************************/

gboolean
pidgin_sni_start(PidginSni *sni, GDBusConnection *connection, GError **error)
{
	static int instance = 0;

	g_return_val_if_fail(PIDGIN_IS_SNI(sni), FALSE);
	g_return_val_if_fail(G_IS_DBUS_CONNECTION(connection), FALSE);

	pidgin_sni_stop(sni);

	sni->registration_id = g_dbus_connection_register_object(connection,
		PIDGIN_SNI_ITEM_PATH, node_info->interfaces[0], &vtable, sni, NULL,
		error);
	if (sni->registration_id == 0)
		return FALSE;

	if (!pidgin_dbus_menu_export(sni->menu, connection, PIDGIN_SNI_MENU_PATH,
	                             error)) {
		g_dbus_connection_unregister_object(connection, sni->registration_id);
		sni->registration_id = 0;
		return FALSE;
	}

	sni->connection = g_object_ref(connection);
	g_free(sni->bus_name);
	sni->bus_name = g_strdup_printf("org.kde.StatusNotifierItem-%d-%d",
	                                (int)getpid(), ++instance);

	sni->own_id = g_bus_own_name_on_connection(connection, sni->bus_name,
		G_BUS_NAME_OWNER_FLAGS_NONE, name_acquired_cb, name_lost_cb, sni, NULL);
	sni->watch_id = g_bus_watch_name_on_connection(connection,
		PIDGIN_SNI_WATCHER_NAME, G_BUS_NAME_WATCHER_FLAGS_NONE,
		watcher_appeared_cb, watcher_vanished_cb, sni, NULL);

	return TRUE;
}

void
pidgin_sni_stop(PidginSni *sni)
{
	g_return_if_fail(PIDGIN_IS_SNI(sni));

	if (sni->register_cancellable != NULL) {
		g_cancellable_cancel(sni->register_cancellable);
		g_clear_object(&sni->register_cancellable);
	}
	if (sni->watch_id != 0) {
		g_bus_unwatch_name(sni->watch_id);
		sni->watch_id = 0;
	}
	if (sni->own_id != 0) {
		g_bus_unown_name(sni->own_id);
		sni->own_id = 0;
	}
	if (sni->menu != NULL)
		pidgin_dbus_menu_unexport(sni->menu);
	if (sni->registration_id != 0) {
		g_dbus_connection_unregister_object(sni->connection, sni->registration_id);
		sni->registration_id = 0;
	}
	g_clear_object(&sni->connection);
	sni->name_owned = FALSE;
	sni->watcher_present = FALSE;
	set_registered(sni, FALSE);
}

gboolean
pidgin_sni_is_registered(PidginSni *sni)
{
	g_return_val_if_fail(PIDGIN_IS_SNI(sni), FALSE);
	return sni->registered;
}

const char *
pidgin_sni_get_bus_name(PidginSni *sni)
{
	g_return_val_if_fail(PIDGIN_IS_SNI(sni), NULL);
	return sni->bus_name;
}

void
pidgin_sni_set_title(PidginSni *sni, const char *title)
{
	g_return_if_fail(PIDGIN_IS_SNI(sni));

	if (g_strcmp0(sni->title, title) == 0)
		return;
	g_free(sni->title);
	sni->title = g_strdup(title);
	emit_item_signal(sni, "NewTitle", NULL);
}

void
pidgin_sni_set_status(PidginSni *sni, PidginSniStatus status)
{
	g_return_if_fail(PIDGIN_IS_SNI(sni));

	if (sni->status == status)
		return;
	sni->status = status;
	emit_item_signal(sni, "NewStatus",
	                 g_variant_new("(s)", status_string(status)));
}

PidginSniStatus
pidgin_sni_get_status(PidginSni *sni)
{
	g_return_val_if_fail(PIDGIN_IS_SNI(sni), PIDGIN_SNI_STATUS_PASSIVE);
	return sni->status;
}

void
pidgin_sni_set_icon(PidginSni *sni, const char *icon_name,
                    const char * const *pixmap_resources)
{
	g_return_if_fail(PIDGIN_IS_SNI(sni));

	if (sni->icon.name != NULL && g_strcmp0(sni->icon.name, icon_name) == 0)
		return;
	sni_icon_set(&sni->icon, icon_name, pixmap_resources);
	emit_item_signal(sni, "NewIcon", NULL);
	/* The tooltip carries the icon as well. */
	emit_item_signal(sni, "NewToolTip", NULL);
}

void
pidgin_sni_set_attention_icon(PidginSni *sni, const char *icon_name,
                              const char * const *pixmap_resources)
{
	g_return_if_fail(PIDGIN_IS_SNI(sni));

	if (sni->attention.name != NULL &&
	    g_strcmp0(sni->attention.name, icon_name) == 0)
		return;
	sni_icon_set(&sni->attention, icon_name, pixmap_resources);
	emit_item_signal(sni, "NewAttentionIcon", NULL);
}

void
pidgin_sni_set_overlay_icon_name(PidginSni *sni, const char *icon_name)
{
	g_return_if_fail(PIDGIN_IS_SNI(sni));

	if (g_strcmp0(sni->overlay_name, icon_name) == 0)
		return;
	g_free(sni->overlay_name);
	sni->overlay_name = g_strdup(icon_name);
	emit_item_signal(sni, "NewOverlayIcon", NULL);
}

void
pidgin_sni_set_icon_theme_path(PidginSni *sni, const char *path)
{
	g_return_if_fail(PIDGIN_IS_SNI(sni));

	g_free(sni->icon_theme_path);
	sni->icon_theme_path = g_strdup(path);
	/* No signal for this one; hosts re-read it with the icon. */
	emit_item_signal(sni, "NewIcon", NULL);
}

void
pidgin_sni_set_tooltip(PidginSni *sni, const char *title, const char *body)
{
	g_return_if_fail(PIDGIN_IS_SNI(sni));

	if (g_strcmp0(sni->tooltip_title, title) == 0 &&
	    g_strcmp0(sni->tooltip_body, body) == 0)
		return;
	g_free(sni->tooltip_title);
	g_free(sni->tooltip_body);
	sni->tooltip_title = g_strdup(title);
	sni->tooltip_body = g_strdup(body);
	emit_item_signal(sni, "NewToolTip", NULL);
}

void
pidgin_sni_set_menu(PidginSni *sni, GMenuModel *model, GActionGroup *actions,
                    const char *prefix)
{
	g_return_if_fail(PIDGIN_IS_SNI(sni));

	pidgin_dbus_menu_set_actions(sni->menu, actions, prefix);
	pidgin_dbus_menu_set_model(sni->menu, model);
}

GObject *
pidgin_sni_get_menu_exporter(PidginSni *sni)
{
	g_return_val_if_fail(PIDGIN_IS_SNI(sni), NULL);
	return G_OBJECT(sni->menu);
}

char *
pidgin_sni_take_activation_token(PidginSni *sni)
{
	g_return_val_if_fail(PIDGIN_IS_SNI(sni), NULL);
	return g_steal_pointer(&sni->activation_token);
}

PidginSni *
pidgin_sni_new(const char *id, const char *category)
{
	PidginSni *sni = g_object_new(PIDGIN_TYPE_SNI, NULL);

	sni->id = g_strdup(id);
	sni->category = g_strdup(category != NULL ? category : "ApplicationStatus");
	return sni;
}

/**************************************************************************
 * GObject
 **************************************************************************/

static void
pidgin_sni_dispose(GObject *object)
{
	PidginSni *sni = PIDGIN_SNI(object);

	if (sni->connection != NULL || sni->own_id != 0 || sni->watch_id != 0)
		pidgin_sni_stop(sni);
	g_clear_object(&sni->menu);

	G_OBJECT_CLASS(pidgin_sni_parent_class)->dispose(object);
}

static void
pidgin_sni_finalize(GObject *object)
{
	PidginSni *sni = PIDGIN_SNI(object);

	g_free(sni->id);
	g_free(sni->category);
	g_free(sni->title);
	sni_icon_clear(&sni->icon);
	sni_icon_clear(&sni->attention);
	g_free(sni->overlay_name);
	g_free(sni->icon_theme_path);
	g_free(sni->tooltip_title);
	g_free(sni->tooltip_body);
	g_free(sni->activation_token);
	g_free(sni->bus_name);

	G_OBJECT_CLASS(pidgin_sni_parent_class)->finalize(object);
}

static void
pidgin_sni_init(PidginSni *sni)
{
	sni->status = PIDGIN_SNI_STATUS_ACTIVE;
	sni->menu = pidgin_dbus_menu_new(NULL, NULL, NULL);
}

static void
pidgin_sni_class_init(PidginSniClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GError *error = NULL;

	object_class->dispose = pidgin_sni_dispose;
	object_class->finalize = pidgin_sni_finalize;

	signals[SIGNAL_ACTIVATE] = g_signal_new("activate",
		G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 2, G_TYPE_INT, G_TYPE_INT);
	signals[SIGNAL_SECONDARY_ACTIVATE] = g_signal_new("secondary-activate",
		G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 2, G_TYPE_INT, G_TYPE_INT);
	signals[SIGNAL_CONTEXT_MENU] = g_signal_new("context-menu",
		G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 2, G_TYPE_INT, G_TYPE_INT);
	signals[SIGNAL_SCROLL] = g_signal_new("scroll",
		G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 2, G_TYPE_INT, G_TYPE_STRING);
	signals[SIGNAL_REGISTERED] = g_signal_new("registered",
		G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 0);
	signals[SIGNAL_UNREGISTERED] = g_signal_new("unregistered",
		G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 0);

	node_info = g_dbus_node_info_new_for_xml(introspection_xml, &error);
	g_assert_no_error(error);
}
