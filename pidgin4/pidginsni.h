/*
 * pidgin4: a StatusNotifierItem (tray icon) on the session bus.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef _PIDGINSNI_H_
#define _PIDGINSNI_H_

#include <gio/gio.h>

G_BEGIN_DECLS

/*
 * PidginSni implements org.kde.StatusNotifierItem, the tray protocol of
 * Waybar, KDE and the GNOME AppIndicator extension, directly on GDBus.
 *
 * pidgin_sni_start() exports the item at /StatusNotifierItem (and its menu,
 * a PidginDBusMenu, at /MenuBar), owns the name
 * org.kde.StatusNotifierItem-<pid>-<n> and watches
 * org.kde.StatusNotifierWatcher. Whenever a watcher appears (also later,
 * e.g. after Waybar restarts) the item registers with
 * RegisterStatusNotifierItem; "registered" is emitted when that succeeds,
 * "unregistered" when the watcher goes away. Without a watcher there is no
 * tray: pidgin_sni_is_registered() stays FALSE.
 *
 * Icons: an icon has a name (IconName, looked up by the host in its icon
 * theme and in IconThemePath) and pixmaps (IconPixmap, ARGB32 in network
 * byte order, one per size) loaded from GResource PNGs, for hosts that
 * cannot find the name.
 *
 * Signals (from the host's method calls):
 *   "activate" (gint x, gint y)            left click
 *   "secondary-activate" (gint x, gint y)  middle click
 *   "context-menu" (gint x, gint y)        right click, only when the host
 *                                          does not show the Menu itself
 *   "scroll" (gint delta, const char *orientation)
 *   "registered", "unregistered"
 */

typedef enum
{
	PIDGIN_SNI_STATUS_PASSIVE,
	PIDGIN_SNI_STATUS_ACTIVE,
	PIDGIN_SNI_STATUS_NEEDS_ATTENTION
} PidginSniStatus;

#define PIDGIN_SNI_ITEM_INTERFACE "org.kde.StatusNotifierItem"
#define PIDGIN_SNI_WATCHER_NAME "org.kde.StatusNotifierWatcher"
#define PIDGIN_SNI_WATCHER_PATH "/StatusNotifierWatcher"
#define PIDGIN_SNI_ITEM_PATH "/StatusNotifierItem"
#define PIDGIN_SNI_MENU_PATH "/MenuBar"

#define PIDGIN_TYPE_SNI (pidgin_sni_get_type())
G_DECLARE_FINAL_TYPE(PidginSni, pidgin_sni, PIDGIN, SNI, GObject)

/** A new item with the Id @id and Category @category ("Communications"). */
PidginSni *pidgin_sni_new(const char *id, const char *category);

/**
 * Exports the item on @connection and starts watching for a watcher.
 * FALSE (with @error) if the objects cannot be exported.
 */
gboolean pidgin_sni_start(PidginSni *sni, GDBusConnection *connection,
                          GError **error);

/** Unexports everything and releases the name ("unregistered" if it was). */
void pidgin_sni_stop(PidginSni *sni);

/** TRUE while a watcher has accepted the item. */
gboolean pidgin_sni_is_registered(PidginSni *sni);

/** The well-known name, once started. */
const char *pidgin_sni_get_bus_name(PidginSni *sni);

void pidgin_sni_set_title(PidginSni *sni, const char *title);
void pidgin_sni_set_status(PidginSni *sni, PidginSniStatus status);
PidginSniStatus pidgin_sni_get_status(PidginSni *sni);

/**
 * Sets the icon: @icon_name for IconName, and IconPixmap from the PNGs at
 * the GResource paths in @pixmap_resources (NULL-terminated; may be NULL).
 */
void pidgin_sni_set_icon(PidginSni *sni, const char *icon_name,
                         const char * const *pixmap_resources);
void pidgin_sni_set_attention_icon(PidginSni *sni, const char *icon_name,
                                   const char * const *pixmap_resources);
void pidgin_sni_set_overlay_icon_name(PidginSni *sni, const char *icon_name);

/** Extra directory in which hosts look up the icon names (IconThemePath). */
void pidgin_sni_set_icon_theme_path(PidginSni *sni, const char *path);

/** ToolTip: a title and a body (the body may use simple markup). */
void pidgin_sni_set_tooltip(PidginSni *sni, const char *title,
                            const char *body);

/**
 * The menu the host shows on right click: @model with the actions of
 * @actions under "@prefix.". Exported as com.canonical.dbusmenu.
 */
void pidgin_sni_set_menu(PidginSni *sni, GMenuModel *model,
                         GActionGroup *actions, const char *prefix);

/** The menu exporter (a PidginDBusMenu), for its "about-to-show" signal. */
GObject *pidgin_sni_get_menu_exporter(PidginSni *sni);

/**
 * The xdg-activation token a host passed with ProvideXdgActivationToken
 * before the last Activate, or NULL. Transfer full; clears it.
 */
char *pidgin_sni_take_activation_token(PidginSni *sni);

/** The org.kde.StatusNotifierItem introspection XML. */
const char *pidgin_sni_get_introspection_xml(void);

/**
 * Converts a PNG in a GResource to one IconPixmap entry, (iiay) with
 * ARGB32 in network byte order. NULL if it cannot be loaded.
 */
GVariant *pidgin_sni_pixmap_from_resource(const char *path);

G_END_DECLS

#endif /* _PIDGINSNI_H_ */
