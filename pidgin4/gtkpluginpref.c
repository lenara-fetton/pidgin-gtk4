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
 * PurplePluginPrefFrame -> GTK 4 widgets (the port of pidgin/gtkpluginpref.c),
 * and pidgin_plugin_file_is_foreign_toolkit(), a small ELF reader that
 * tells whether a plugin links another GUI toolkit before it is dlopen'ed.
 * Both live here, in the components library, so the unit test links them.
 */
#include "pidgin-internal.h"
#include "pidgin.h"

#include <elf.h>

#include "debug.h"
#include "pluginpref.h"
#include "prefs.h"

#include "gtkplugin.h"
#include "gtkpluginpref.h"
#include "gtkutils.h"
#include "pidgincomposeentry.h"
#include "pidginformattoolbar.h"
#include "pidginprefbinding.h"

/**************************************************************************
 * Text view / compose entry <-> string pref (pidginprefbinding.c only
 * knows GtkEditables)
 **************************************************************************/

typedef struct {
	char *pref;
	GtkWidget *widget;      /* a GtkTextView or a PidginComposeEntry */
	GtkTextBuffer *buffer;  /* a reference, so handlers can be removed */
	gboolean html;
	gboolean updating;
} TextBinding;

static void
text_binding_free(gpointer data)
{
	TextBinding *b = data;

	purple_prefs_disconnect_by_handle(b);
	/* The buffer may outlive the view (a reference held elsewhere):
	 * never let it call into a freed binding. */
	g_signal_handlers_disconnect_by_data(b->buffer, b);
	g_object_unref(b->buffer);
	g_free(b->pref);
	g_free(b);
}

static char *
text_binding_get(TextBinding *b)
{
	GtkTextIter start, end;

	if (b->html)
		return pidgin_compose_entry_get_markup(PIDGIN_COMPOSE_ENTRY(b->widget));

	gtk_text_buffer_get_bounds(b->buffer, &start, &end);
	return gtk_text_buffer_get_text(b->buffer, &start, &end, FALSE);
}

static void
text_binding_update_widget(TextBinding *b)
{
	const char *value;
	char *current;

	if (!purple_prefs_exists(b->pref))
		return;
	value = purple_prefs_get_string(b->pref);
	if (value == NULL)
		value = "";
	current = text_binding_get(b);
	if (!purple_strequal(current, value)) {
		b->updating = TRUE;
		if (b->html)
			pidgin_compose_entry_set_markup(PIDGIN_COMPOSE_ENTRY(b->widget), value);
		else
			gtk_text_buffer_set_text(b->buffer, value, -1);
		b->updating = FALSE;
	}
	g_free(current);
}

static void
text_binding_pref_cb(const char *name, PurplePrefType type, gconstpointer value,
                     gpointer data)
{
	TextBinding *b = data;

	if (!b->updating)
		text_binding_update_widget(b);
}

static void
text_binding_changed(TextBinding *b)
{
	char *text;

	if (b->updating || !purple_prefs_exists(b->pref))
		return;
	text = text_binding_get(b);
	if (!purple_strequal(text, purple_prefs_get_string(b->pref))) {
		b->updating = TRUE;
		purple_prefs_set_string(b->pref, text);
		b->updating = FALSE;
	}
	g_free(text);
}

static void
buffer_changed_cb(GtkTextBuffer *buffer, gpointer data)
{
	text_binding_changed(data);
}

static void
buffer_tag_cb(GtkTextBuffer *buffer, GtkTextTag *tag, GtkTextIter *start,
              GtkTextIter *end, gpointer data)
{
	text_binding_changed(data);
}

/* Enter in a one-line HTML field: nothing to send. */
static gboolean
compose_send_cb(PidginComposeEntry *entry, const char *markup, gpointer data)
{
	return FALSE;
}

static void
bind_text(GtkWidget *widget, const char *pref, gboolean html)
{
	TextBinding *b = g_new0(TextBinding, 1);

	b->pref = g_strdup(pref);
	b->widget = widget;
	b->buffer = g_object_ref(gtk_text_view_get_buffer(GTK_TEXT_VIEW(widget)));
	b->html = html;

	text_binding_update_widget(b);
	purple_prefs_connect_callback(b, pref, text_binding_pref_cb, b);
	g_signal_connect_after(b->buffer, "changed", G_CALLBACK(buffer_changed_cb), b);
	if (html) {
		g_signal_connect_after(b->buffer, "apply-tag", G_CALLBACK(buffer_tag_cb), b);
		g_signal_connect_after(b->buffer, "remove-tag", G_CALLBACK(buffer_tag_cb), b);
	}
	/* Freed (and disconnected) with the widget. */
	g_object_set_data_full(G_OBJECT(widget), "pidgin-pref-text-binding", b,
	                       text_binding_free);
}

/**************************************************************************
 * Pref widgets
 **************************************************************************/

static GtkWidget *
make_choice_dropdown(PurplePluginPref *pref, PurplePrefType type)
{
	const char *name = purple_plugin_pref_get_name(pref);
	GPtrArray *labels = g_ptr_array_new();
	GPtrArray *svalues = g_ptr_array_new_with_free_func(g_free);
	GArray *ivalues = g_array_new(FALSE, FALSE, sizeof(int));
	GtkWidget *dropdown;
	GList *l;

	/* The choices are (label, value) pairs: a string, or GINT_TO_POINTER
	 * for an int pref. */
	for (l = purple_plugin_pref_get_choices(pref); l != NULL && l->next != NULL;
	     l = l->next->next) {
		g_ptr_array_add(labels, l->data);
		if (type == PURPLE_PREF_INT) {
			int v = GPOINTER_TO_INT(l->next->data);
			g_array_append_val(ivalues, v);
		} else {
			g_ptr_array_add(svalues,
				g_strdup(l->next->data ? (const char *)l->next->data : ""));
		}
	}

	if (type == PURPLE_PREF_INT)
		dropdown = pidgin_pref_dropdown_int_new(name,
			(const char *const *)labels->pdata, (const int *)(void *)ivalues->data,
			labels->len);
	else
		dropdown = pidgin_pref_dropdown_string_new(name,
			(const char *const *)labels->pdata,
			(const char *const *)svalues->pdata, labels->len);

	g_ptr_array_free(labels, TRUE);
	g_ptr_array_free(svalues, TRUE);
	g_array_free(ivalues, TRUE);
	return dropdown;
}

static void
make_string_pref(GtkWidget *parent, PurplePluginPref *pref, GtkSizeGroup *sg)
{
	const char *name = purple_plugin_pref_get_name(pref);
	const char *label = purple_plugin_pref_get_label(pref);
	PurpleStringFormatType format = purple_plugin_pref_get_format_type(pref);
	GtkWidget *widget, *box, *title, *sw;

	if (purple_plugin_pref_get_type(pref) == PURPLE_PLUGIN_PREF_CHOICE) {
		widget = make_choice_dropdown(pref, PURPLE_PREF_STRING);
		gtk_widget_set_name(widget, name);
		pidgin_add_widget_to_vbox(GTK_BOX(parent), label, sg, widget, FALSE, NULL);
		return;
	}

	if (format == PURPLE_STRING_FORMAT_TYPE_NONE) {
		widget = gtk_entry_new();
		gtk_widget_set_name(widget, name);
		gtk_entry_set_max_length(GTK_ENTRY(widget),
			(int)MIN(purple_plugin_pref_get_max_length(pref), G_MAXINT));
		if (purple_plugin_pref_get_masked(pref)) {
			gtk_entry_set_visibility(GTK_ENTRY(widget), FALSE);
			gtk_entry_set_invisible_char(GTK_ENTRY(widget), PIDGIN_INVISIBLE_CHAR);
		}
		pidgin_pref_bind_string(widget, name);
		pidgin_add_widget_to_vbox(GTK_BOX(parent), label, sg, widget, TRUE, NULL);
		return;
	}

	/* Multi-line and/or HTML: the label above, the editor indented below. */
	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, PIDGIN_HIG_BOX_SPACE);
	gtk_box_append(GTK_BOX(parent), box);
	title = gtk_label_new_with_mnemonic(label ? label : "");
	gtk_label_set_xalign(GTK_LABEL(title), 0.0);
	gtk_box_append(GTK_BOX(box), title);

	if (format & PURPLE_STRING_FORMAT_TYPE_HTML) {
		GtkWidget *toolbar, *frame;

		widget = pidgin_compose_entry_new();
		pidgin_compose_entry_set_caps(PIDGIN_COMPOSE_ENTRY(widget),
			PIDGIN_FORMAT_HTML_ALL & ~PIDGIN_FORMAT_IMAGE);
		pidgin_compose_entry_set_return_inserts_newline(PIDGIN_COMPOSE_ENTRY(widget),
			(format & PURPLE_STRING_FORMAT_TYPE_MULTILINE) != 0);
		g_signal_connect(widget, "message-send", G_CALLBACK(compose_send_cb), NULL);

		frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
		toolbar = pidgin_format_toolbar_new(PIDGIN_COMPOSE_ENTRY(widget));
		gtk_box_append(GTK_BOX(frame), toolbar);
		sw = pidgin_make_scrollable(widget, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC,
		                            -1, 80);
		gtk_box_append(GTK_BOX(frame), sw);
		gtk_widget_set_margin_start(frame, PIDGIN_HIG_BORDER);
		gtk_box_append(GTK_BOX(box), frame);
	} else {
		widget = gtk_text_view_new();
		gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(widget), GTK_WRAP_WORD_CHAR);
		sw = pidgin_make_scrollable(widget, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC,
		                            -1, 80);
		gtk_widget_add_css_class(sw, "frame");
		gtk_widget_set_margin_start(sw, PIDGIN_HIG_BORDER);
		gtk_box_append(GTK_BOX(box), sw);
	}

	gtk_widget_set_name(widget, name);
	pidgin_set_accessible_label(widget, title);
	bind_text(widget, name, (format & PURPLE_STRING_FORMAT_TYPE_HTML) != 0);
}

static void
make_int_pref(GtkWidget *parent, PurplePluginPref *pref, GtkSizeGroup *sg)
{
	const char *name = purple_plugin_pref_get_name(pref);
	const char *label = purple_plugin_pref_get_label(pref);
	GtkWidget *widget;
	int min = 0, max = 0;

	if (purple_plugin_pref_get_type(pref) == PURPLE_PLUGIN_PREF_CHOICE) {
		widget = make_choice_dropdown(pref, PURPLE_PREF_INT);
	} else {
		purple_plugin_pref_get_bounds(pref, &min, &max);
		if (max < min)
			max = min;
		widget = pidgin_pref_spin_new(name, min, max);
	}
	gtk_widget_set_name(widget, name);
	pidgin_add_widget_to_vbox(GTK_BOX(parent), label, sg, widget, FALSE, NULL);
}

static void
make_bool_pref(GtkWidget *parent, PurplePluginPref *pref)
{
	const char *name = purple_plugin_pref_get_name(pref);
	const char *label = purple_plugin_pref_get_label(pref);
	GtkWidget *check;

	check = pidgin_pref_checkbox_new(label ? label : "", name);
	gtk_widget_set_name(check, name);
	gtk_box_append(GTK_BOX(parent), check);
}

/* Paths: read-only (Pidgin 2 showed nothing for them). */
static void
make_path_pref(GtkWidget *parent, PurplePluginPref *pref, GtkSizeGroup *sg)
{
	const char *name = purple_plugin_pref_get_name(pref);
	GtkWidget *value;
	char *text;

	if (purple_prefs_get_type(name) == PURPLE_PREF_PATH) {
		text = g_strdup(purple_prefs_get_path(name));
	} else {
		GList *l, *paths = purple_prefs_get_path_list(name);
		GString *str = g_string_new(NULL);

		for (l = paths; l != NULL; l = l->next) {
			if (str->len > 0)
				g_string_append_c(str, '\n');
			g_string_append(str, l->data);
		}
		g_list_free_full(paths, g_free);
		text = g_string_free(str, FALSE);
	}
	value = gtk_label_new(text ? text : "");
	gtk_label_set_xalign(GTK_LABEL(value), 0.0);
	gtk_label_set_selectable(GTK_LABEL(value), TRUE);
	gtk_label_set_wrap(GTK_LABEL(value), TRUE);
	gtk_widget_set_name(value, name);
	pidgin_add_widget_to_vbox(GTK_BOX(parent), purple_plugin_pref_get_label(pref),
	                          sg, value, TRUE, NULL);
	g_free(text);
}

static void
make_info_pref(GtkWidget *parent, PurplePluginPref *pref)
{
	GtkWidget *label = gtk_label_new(purple_plugin_pref_get_label(pref));

	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_label_set_yalign(GTK_LABEL(label), 0.0);
	gtk_label_set_wrap(GTK_LABEL(label), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(label), 60);
	gtk_box_append(GTK_BOX(parent), label);
}

GtkWidget *
pidgin_plugin_pref_frame_to_widget(PurplePluginPrefFrame *frame)
{
	GtkWidget *ret, *parent;
	GtkSizeGroup *sg;
	GList *pp;

	if (frame == NULL)
		return NULL;

	sg = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);

	parent = ret = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
	gtk_widget_set_margin_top(ret, PIDGIN_HIG_BORDER);
	gtk_widget_set_margin_bottom(ret, PIDGIN_HIG_BORDER);
	gtk_widget_set_margin_start(ret, PIDGIN_HIG_BORDER);
	gtk_widget_set_margin_end(ret, PIDGIN_HIG_BORDER);

	for (pp = purple_plugin_pref_frame_get_prefs(frame); pp != NULL; pp = pp->next) {
		PurplePluginPref *pref = pp->data;
		const char *name = purple_plugin_pref_get_name(pref);
		const char *label = purple_plugin_pref_get_label(pref);

		/* Pidgin 2's semantics: an unnamed INFO pref is a wrapped text
		 * label; any other unnamed pref with a label starts a titled
		 * section holding the prefs after it. */
		if (name == NULL) {
			if (label == NULL)
				continue;
			if (purple_plugin_pref_get_type(pref) == PURPLE_PLUGIN_PREF_INFO)
				make_info_pref(parent, pref);
			else
				parent = pidgin_make_frame(ret, label);
			continue;
		}

		if (!purple_prefs_exists(name)) {
			purple_debug_warning("gtkpluginpref",
				"plugin pref %s does not exist\n", name);
			continue;
		}

		switch (purple_prefs_get_type(name)) {
		case PURPLE_PREF_BOOLEAN:
			make_bool_pref(parent, pref);
			break;
		case PURPLE_PREF_INT:
			make_int_pref(parent, pref, sg);
			break;
		case PURPLE_PREF_STRING:
			make_string_pref(parent, pref, sg);
			break;
		case PURPLE_PREF_PATH:
		case PURPLE_PREF_PATH_LIST:
			make_path_pref(parent, pref, sg);
			break;
		default:
			break;
		}
	}

	g_object_unref(sg);

	return ret;
}

/**************************************************************************
 * ELF: does a plugin link another toolkit?
 **************************************************************************/

static const char *const foreign_prefixes[] = {
	"libgtk-x11-2.0",
	"libgdk-x11-2.0",
	"libgtk-3.",
	"libgdk-3.",
	"libgtk-win32-2.0",
	"libgtk-quartz",
	"libgnomeui",
};

/* At most this many DT_NEEDED entries are looked at. */
#define MAX_NEEDED 512

typedef struct {
	const guint8 *data;
	guint64 size;
} ElfImage;

/* TRUE if [off, off + len) lies within the image (no overflow). */
static gboolean
elf_range_ok(const ElfImage *img, guint64 off, guint64 len)
{
	return off <= img->size && len <= img->size - off;
}

/* The NUL-terminated string at @off, which must end before @limit (the
 * end of the string table, itself within the image), or NULL. */
static const char *
elf_string(const ElfImage *img, guint64 off, guint64 limit)
{
	if (limit > img->size)
		limit = img->size;
	if (off >= limit)
		return NULL;
	if (memchr(img->data + off, '\0', limit - off) == NULL)
		return NULL;
	return (const char *)img->data + off;
}

static gboolean
needed_is_foreign(const char *needed, char **lib)
{
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(foreign_prefixes); i++) {
		if (g_str_has_prefix(needed, foreign_prefixes[i])) {
			if (lib != NULL)
				*lib = g_strdup(needed);
			return TRUE;
		}
	}
	return FALSE;
}

/*
 * One scanner per ELF class (native byte order only). Every structure is
 * copied out with memcpy (no alignment assumptions) after its range was
 * checked against the file size. The dynamic section is found through
 * PT_DYNAMIC; DT_STRTAB's address is mapped to a file offset through the
 * PT_LOAD segments.
 */
#define DEFINE_ELF_SCAN(BITS) \
static gboolean \
elf_scan_##BITS(const ElfImage *img, char **lib) \
{ \
	Elf##BITS##_Ehdr eh; \
	Elf##BITS##_Phdr ph, dynph; \
	guint64 strtab_vaddr = 0, strsz = 0, strtab_off = 0, dyn_count, i; \
	guint64 needed[MAX_NEEDED]; \
	guint n_needed = 0, j; \
	gboolean have_dyn = FALSE, have_strtab = FALSE, have_off = FALSE; \
\
	if (!elf_range_ok(img, 0, sizeof(eh))) \
		return FALSE; \
	memcpy(&eh, img->data, sizeof(eh)); \
	if (eh.e_phnum == 0 || eh.e_phentsize < sizeof(ph)) \
		return FALSE; \
	if (!elf_range_ok(img, eh.e_phoff, (guint64)eh.e_phnum * eh.e_phentsize)) \
		return FALSE; \
\
	memset(&dynph, 0, sizeof(dynph)); \
	for (i = 0; i < eh.e_phnum && !have_dyn; i++) { \
		memcpy(&ph, img->data + eh.e_phoff + i * eh.e_phentsize, sizeof(ph)); \
		if (ph.p_type == PT_DYNAMIC) { \
			dynph = ph; \
			have_dyn = TRUE; \
		} \
	} \
	if (!have_dyn || !elf_range_ok(img, dynph.p_offset, dynph.p_filesz)) \
		return FALSE; \
\
	dyn_count = (guint64)dynph.p_filesz / sizeof(Elf##BITS##_Dyn); \
	for (i = 0; i < dyn_count; i++) { \
		Elf##BITS##_Dyn dyn; \
\
		memcpy(&dyn, img->data + dynph.p_offset + i * sizeof(dyn), sizeof(dyn)); \
		if (dyn.d_tag == DT_NULL) \
			break; \
		if (dyn.d_tag == DT_STRTAB) { \
			strtab_vaddr = dyn.d_un.d_ptr; \
			have_strtab = TRUE; \
		} else if (dyn.d_tag == DT_STRSZ) { \
			strsz = dyn.d_un.d_val; \
		} else if (dyn.d_tag == DT_NEEDED && n_needed < MAX_NEEDED) { \
			needed[n_needed++] = dyn.d_un.d_val; \
		} \
	} \
	if (!have_strtab || n_needed == 0) \
		return FALSE; \
\
	for (i = 0; i < eh.e_phnum && !have_off; i++) { \
		memcpy(&ph, img->data + eh.e_phoff + i * eh.e_phentsize, sizeof(ph)); \
		if (ph.p_type != PT_LOAD) \
			continue; \
		if (strtab_vaddr >= ph.p_vaddr && \
		    strtab_vaddr - ph.p_vaddr < ph.p_filesz && \
		    (guint64)ph.p_offset <= img->size) { \
			strtab_off = (guint64)ph.p_offset + (strtab_vaddr - ph.p_vaddr); \
			have_off = TRUE; \
		} \
	} \
	if (!have_off || strtab_off >= img->size) \
		return FALSE; \
	/* Without (or with a bogus) DT_STRSZ, bound by the file. */ \
	if (strsz == 0 || strsz > img->size - strtab_off) \
		strsz = img->size - strtab_off; \
\
	for (j = 0; j < n_needed; j++) { \
		const char *name; \
\
		if (needed[j] >= strsz) \
			continue; \
		name = elf_string(img, strtab_off + needed[j], strtab_off + strsz); \
		if (name != NULL && needed_is_foreign(name, lib)) \
			return TRUE; \
	} \
	return FALSE; \
}

DEFINE_ELF_SCAN(32)
DEFINE_ELF_SCAN(64)

gboolean
pidgin_plugin_file_is_foreign_toolkit(const char *path, char **lib)
{
	GMappedFile *mapped;
	GError *error = NULL;
	ElfImage img;
	gboolean foreign = FALSE;

	if (lib != NULL)
		*lib = NULL;
	g_return_val_if_fail(path != NULL, FALSE);

	if (!g_file_test(path, G_FILE_TEST_IS_REGULAR))
		return FALSE;

	mapped = g_mapped_file_new(path, FALSE, &error);
	if (mapped == NULL) {
		purple_debug_warning("plugins", "Could not read %s: %s\n", path,
		                     error->message);
		g_error_free(error);
		return FALSE;
	}

	img.data = (const guint8 *)g_mapped_file_get_contents(mapped);
	img.size = g_mapped_file_get_length(mapped);

	if (img.data != NULL && img.size >= EI_NIDENT &&
	    memcmp(img.data, ELFMAG, SELFMAG) == 0 &&
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
	    img.data[EI_DATA] == ELFDATA2LSB
#else
	    img.data[EI_DATA] == ELFDATA2MSB
#endif
	    ) {
		if (img.data[EI_CLASS] == ELFCLASS64)
			foreign = elf_scan_64(&img, lib);
		else if (img.data[EI_CLASS] == ELFCLASS32)
			foreign = elf_scan_32(&img, lib);
	}

	g_mapped_file_unref(mapped);
	return foreign;
}
