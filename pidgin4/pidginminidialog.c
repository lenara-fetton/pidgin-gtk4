/*
 * pidgin
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
#include "pidgin-internal.h"
#include "pidgin.h"

#include "pidginminidialog.h"

struct _PidginMiniDialog
{
	GtkBox parent;

	GtkWidget *icon;
	GtkWidget *title;
	GtkWidget *description;
	GtkWidget *contents;
	GtkWidget *buttons;
};

G_DEFINE_FINAL_TYPE(PidginMiniDialog, pidgin_mini_dialog, GTK_TYPE_BOX)

typedef struct {
	PidginMiniDialog *mini_dialog;
	PidginMiniDialogCallback callback;
	gpointer user_data;
	gboolean close_dialog;
} ButtonInfo;

static void
pidgin_mini_dialog_class_init(PidginMiniDialogClass *klass)
{
	gtk_widget_class_set_css_name(GTK_WIDGET_CLASS(klass), "minidialog");
}

static void
pidgin_mini_dialog_init(PidginMiniDialog *self)
{
	GtkWidget *hbox, *vbox;

	gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
	gtk_box_set_spacing(GTK_BOX(self), PIDGIN_HIG_BOX_SPACE);
	gtk_widget_add_css_class(GTK_WIDGET(self), "pidgin-mini-dialog");

	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PIDGIN_HIG_BOX_SPACE);
	gtk_box_append(GTK_BOX(self), hbox);

	self->icon = gtk_image_new();
	gtk_image_set_pixel_size(GTK_IMAGE(self->icon), 24);
	gtk_widget_set_valign(self->icon, GTK_ALIGN_START);
	gtk_widget_set_visible(self->icon, FALSE);
	gtk_box_append(GTK_BOX(hbox), self->icon);

	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	gtk_widget_set_hexpand(vbox, TRUE);
	gtk_box_append(GTK_BOX(hbox), vbox);

	self->title = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(self->title), 0.0);
	gtk_label_set_wrap(GTK_LABEL(self->title), TRUE);
	gtk_label_set_wrap_mode(GTK_LABEL(self->title), PANGO_WRAP_WORD_CHAR);
	gtk_widget_add_css_class(self->title, "pidgin-mini-dialog-title");
	gtk_box_append(GTK_BOX(vbox), self->title);

	self->description = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(self->description), 0.0);
	gtk_label_set_wrap(GTK_LABEL(self->description), TRUE);
	gtk_label_set_wrap_mode(GTK_LABEL(self->description), PANGO_WRAP_WORD_CHAR);
	gtk_label_set_selectable(GTK_LABEL(self->description), FALSE);
	gtk_widget_add_css_class(self->description, "pidgin-mini-dialog-description");
	gtk_widget_set_visible(self->description, FALSE);
	gtk_box_append(GTK_BOX(vbox), self->description);

	self->contents = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	gtk_box_append(GTK_BOX(vbox), self->contents);

	self->buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PIDGIN_HIG_BOX_SPACE);
	gtk_widget_set_halign(self->buttons, GTK_ALIGN_END);
	gtk_widget_set_visible(self->buttons, FALSE);
	gtk_box_append(GTK_BOX(self), self->buttons);
}

PidginMiniDialog *
pidgin_mini_dialog_new(const char *title, const char *description,
                       const char *icon_name)
{
	PidginMiniDialog *self = g_object_new(PIDGIN_TYPE_MINI_DIALOG, NULL);

	pidgin_mini_dialog_set_title(self, title);
	pidgin_mini_dialog_set_description(self, description);
	pidgin_mini_dialog_set_icon_name(self, icon_name);
	return self;
}

void
pidgin_mini_dialog_set_title(PidginMiniDialog *self, const char *title)
{
	g_return_if_fail(PIDGIN_IS_MINI_DIALOG(self));
	gtk_label_set_text(GTK_LABEL(self->title), title ? title : "");
}

void
pidgin_mini_dialog_set_description(PidginMiniDialog *self, const char *description)
{
	g_return_if_fail(PIDGIN_IS_MINI_DIALOG(self));
	gtk_label_set_text(GTK_LABEL(self->description), description ? description : "");
	gtk_widget_set_visible(self->description, description != NULL && *description != '\0');
}

void
pidgin_mini_dialog_set_description_markup(PidginMiniDialog *self, const char *markup)
{
	g_return_if_fail(PIDGIN_IS_MINI_DIALOG(self));
	gtk_label_set_markup(GTK_LABEL(self->description), markup ? markup : "");
	gtk_widget_set_visible(self->description, markup != NULL && *markup != '\0');
}

GtkLabel *
pidgin_mini_dialog_get_description_label(PidginMiniDialog *self)
{
	g_return_val_if_fail(PIDGIN_IS_MINI_DIALOG(self), NULL);
	return GTK_LABEL(self->description);
}

void
pidgin_mini_dialog_set_icon_name(PidginMiniDialog *self, const char *icon_name)
{
	g_return_if_fail(PIDGIN_IS_MINI_DIALOG(self));
	gtk_image_set_from_icon_name(GTK_IMAGE(self->icon), icon_name);
	gtk_widget_set_visible(self->icon, icon_name != NULL);
}

void
pidgin_mini_dialog_set_gicon(PidginMiniDialog *self, GIcon *icon)
{
	g_return_if_fail(PIDGIN_IS_MINI_DIALOG(self));
	gtk_image_set_from_gicon(GTK_IMAGE(self->icon), icon);
	gtk_widget_set_visible(self->icon, icon != NULL);
}

GtkBox *
pidgin_mini_dialog_get_contents(PidginMiniDialog *self)
{
	g_return_val_if_fail(PIDGIN_IS_MINI_DIALOG(self), NULL);
	return GTK_BOX(self->contents);
}

guint
pidgin_mini_dialog_get_num_children(PidginMiniDialog *self)
{
	GtkWidget *child;
	guint n = 0;

	g_return_val_if_fail(PIDGIN_IS_MINI_DIALOG(self), 0);

	for (child = gtk_widget_get_first_child(self->contents); child != NULL;
	     child = gtk_widget_get_next_sibling(child))
		n++;
	return n;
}

void
pidgin_mini_dialog_close(PidginMiniDialog *self)
{
	GtkWidget *parent;

	g_return_if_fail(PIDGIN_IS_MINI_DIALOG(self));

	parent = gtk_widget_get_parent(GTK_WIDGET(self));
	if (parent == NULL)
		return;
	if (GTK_IS_BOX(parent))
		gtk_box_remove(GTK_BOX(parent), GTK_WIDGET(self));
	else
		gtk_widget_unparent(GTK_WIDGET(self));
}

static void
button_clicked_cb(GtkButton *button, ButtonInfo *info)
{
	PidginMiniDialog *self = info->mini_dialog;

	/* The callback may close us itself. */
	g_object_ref(self);
	if (info->callback != NULL)
		info->callback(self, button, info->user_data);
	if (info->close_dialog)
		pidgin_mini_dialog_close(self);
	g_object_unref(self);
}

static GtkWidget *
add_button(PidginMiniDialog *self, const char *text,
           PidginMiniDialogCallback callback, gpointer user_data,
           gboolean close_dialog)
{
	GtkWidget *button;
	ButtonInfo *info;

	g_return_val_if_fail(PIDGIN_IS_MINI_DIALOG(self), NULL);

	info = g_new0(ButtonInfo, 1);
	info->mini_dialog = self;
	info->callback = callback;
	info->user_data = user_data;
	info->close_dialog = close_dialog;

	button = gtk_button_new_with_mnemonic(text);
	gtk_widget_add_css_class(button, "pidgin-mini-dialog-button");
	g_object_set_data_full(G_OBJECT(button), "pidgin-mini-dialog-info", info, g_free);
	g_signal_connect(button, "clicked", G_CALLBACK(button_clicked_cb), info);
	gtk_box_append(GTK_BOX(self->buttons), button);
	gtk_widget_set_visible(self->buttons, TRUE);

	return button;
}

GtkWidget *
pidgin_mini_dialog_add_button(PidginMiniDialog *self, const char *text,
                              PidginMiniDialogCallback callback, gpointer user_data)
{
	return add_button(self, text, callback, user_data, TRUE);
}

GtkWidget *
pidgin_mini_dialog_add_non_closing_button(PidginMiniDialog *self, const char *text,
                                          PidginMiniDialogCallback callback,
                                          gpointer user_data)
{
	return add_button(self, text, callback, user_data, FALSE);
}
