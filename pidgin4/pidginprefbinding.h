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
#ifndef _PIDGINPREFBINDING_H_
#define _PIDGINPREFBINDING_H_

#include "pidgin.h"
#include "prefs.h"

/*
 * Two-way bindings between widgets and libpurple prefs (M5), for the
 * preferences window and plugin pref frames. A binding writes the pref
 * when the widget changes and updates the widget when the pref changes
 * (purple_prefs_connect_callback), without feedback loops. It is freed
 * (and disconnected) with the widget.
 */

/** GtkCheckButton or GtkSwitch <-> bool pref. */
void pidgin_pref_bind_bool(GtkWidget *widget, const char *pref);

/** GtkSpinButton <-> int pref. */
void pidgin_pref_bind_int(GtkWidget *spin, const char *pref);

/** GtkEditable (GtkEntry, GtkText) <-> string pref, written on every
 * change. */
void pidgin_pref_bind_string(GtkWidget *editable, const char *pref);

/** GtkEditable <-> path pref. */
void pidgin_pref_bind_path(GtkWidget *editable, const char *pref);

/**
 * GtkDropDown over a GListModel of PidginItem (pidgin_item_dropdown_new)
 * <-> string pref: the selected item's id is the value.
 */
void pidgin_pref_bind_dropdown_string(GtkWidget *dropdown, const char *pref);

/**
 * GtkDropDown over PidginItems <-> int pref: the item's id is the decimal
 * value (e.g. "3").
 */
void pidgin_pref_bind_dropdown_int(GtkWidget *dropdown, const char *pref);

/** Makes @widget sensitive only while the bool @pref is TRUE (or FALSE
 * with @invert). */
void pidgin_pref_bind_sensitive(GtkWidget *widget, const char *pref,
                                gboolean invert);

/** Makes @widget sensitive only while the string @pref equals @value. */
void pidgin_pref_bind_sensitive_string(GtkWidget *widget, const char *pref,
                                       const char *value);

/** Makes @widget sensitive only while the string @pref differs from
 * @value. */
void pidgin_pref_bind_insensitive_string(GtkWidget *widget, const char *pref,
                                         const char *value);

/* Convenience constructors: a widget bound to @pref. */
GtkWidget *pidgin_pref_checkbox_new(const char *label, const char *pref);
GtkWidget *pidgin_pref_spin_new(const char *pref, int min, int max);
GtkWidget *pidgin_pref_entry_new(const char *pref);
/** A drop-down of @n (label, value) string pairs. */
GtkWidget *pidgin_pref_dropdown_string_new(const char *pref,
                                           const char *const *labels,
                                           const char *const *values, int n);
/** A drop-down of @n (label, value) int pairs. */
GtkWidget *pidgin_pref_dropdown_int_new(const char *pref,
                                        const char *const *labels,
                                        const int *values, int n);

#endif /* _PIDGINPREFBINDING_H_ */
