/*
 * pidgin4: a com.canonical.dbusmenu exporter for a GMenuModel.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef _PIDGINDBUSMENU_H_
#define _PIDGINDBUSMENU_H_

#include <gio/gio.h>

G_BEGIN_DECLS

/*
 * PidginDBusMenu exports a GMenuModel and the GActionGroup behind it as
 * com.canonical.dbusmenu (protocol version 3), the menu protocol that
 * StatusNotifierItem hosts (Waybar, KDE, the GNOME AppIndicator extension)
 * use for a tray icon's context menu. It is written directly on GDBus; the
 * unmaintained libdbusmenu is not used.
 *
 * The mapping:
 *   - every menu item becomes a dbusmenu item with a "label" (the GMenu
 *     mnemonic underscores are stripped, literal underscores are escaped
 *     as "__" for the wire);
 *   - sections are flattened, with a "separator" item between non-empty
 *     sections and the items around them;
 *   - submenu links become items with "children-display" = "submenu";
 *   - "enabled" follows the action (items without an action and without a
 *     submenu are disabled, as in GtkPopoverMenu); "visible" follows the
 *     "hidden-when" attribute;
 *   - stateful actions give "toggle-type": a boolean state without a
 *     target is a "checkmark", a state of the target's type a "radio";
 *     "toggle-state" is 1 or 0;
 *   - the "icon" attribute: a GThemedIcon gives "icon-name"; a GFileIcon
 *     (including resource:// files) or GBytesIcon gives "icon-data" (the
 *     PNG bytes), since hosts cannot see icons in our GResource.
 *
 * Only actions named "<action_prefix>.<name>" are looked up in the action
 * group (as <name>); other items are disabled.
 *
 * "Event" with "clicked" activates the item's action with its target.
 * Changes to any (sub)model rebuild the layout (coalesced in an idle) and
 * emit "LayoutUpdated"; action state and enabled changes emit
 * "ItemsPropertiesUpdated". Item ids are reassigned on every rebuild.
 *
 * Signals:
 *   "about-to-show" (gint id) -> gboolean: emitted for AboutToShow and
 *     AboutToShowGroup, and when a submenu (or the root) gets an "opened"
 *     event. A handler may rebuild the model synchronously (e.g. to refresh
 *     a dynamic menu) and returns TRUE if it did; the reply then tells the
 *     host to fetch the layout again.
 *   "activated" (const char *action, GVariant *target): after an action
 *     was activated through an "Event".
 */

#define PIDGIN_DBUS_MENU_INTERFACE "com.canonical.dbusmenu"

#define PIDGIN_TYPE_DBUS_MENU (pidgin_dbus_menu_get_type())
G_DECLARE_FINAL_TYPE(PidginDBusMenu, pidgin_dbus_menu, PIDGIN, DBUS_MENU, GObject)

/**
 * Creates an exporter for @model (may be NULL: an empty menu) with the
 * actions in @actions under "@action_prefix." (e.g. "tray").
 */
PidginDBusMenu *pidgin_dbus_menu_new(GMenuModel *model, GActionGroup *actions,
                                     const char *action_prefix);

/** Exports the menu at @object_path on @connection. */
gboolean pidgin_dbus_menu_export(PidginDBusMenu *menu,
                                 GDBusConnection *connection,
                                 const char *object_path, GError **error);

/** Unexports the menu (no-op when not exported). */
void pidgin_dbus_menu_unexport(PidginDBusMenu *menu);

/** The object path while exported, else NULL. */
const char *pidgin_dbus_menu_get_object_path(PidginDBusMenu *menu);

/** Replaces the model; the layout is rebuilt at once (LayoutUpdated). */
void pidgin_dbus_menu_set_model(PidginDBusMenu *menu, GMenuModel *model);

/** Replaces the action group and its prefix (rebuilds the layout). */
void pidgin_dbus_menu_set_actions(PidginDBusMenu *menu, GActionGroup *actions,
                                  const char *action_prefix);

/** Rebuilds the layout now if a model change is pending. */
void pidgin_dbus_menu_flush(PidginDBusMenu *menu);

/** The current layout revision (incremented on every rebuild). */
guint pidgin_dbus_menu_get_revision(PidginDBusMenu *menu);

/**
 * GMenu label -> dbusmenu label: drops the mnemonic underscores ("_File"
 * -> "File", "a__b" -> "a_b") and then escapes the remaining literal
 * underscores for dbusmenu ("a_b" -> "a__b").
 */
char *pidgin_dbus_menu_label_from_mnemonic(const char *label);

/** The org.freedesktop.DBus.Introspectable XML of the interface. */
const char *pidgin_dbus_menu_get_introspection_xml(void);

G_END_DECLS

#endif /* _PIDGINDBUSMENU_H_ */
