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
 * The certificate manager (the port of pidgin/gtkcertmgr.c): the X.509
 * "tls_peers" pool, i.e. the server certificates the user accepted
 * (<profile>/certificates/x509/tls_peers; libpurple owns that format).
 * The dialog for unknown certificates is libpurple's request, shown by
 * gtkrequest.c.
 */
#include "pidgin-internal.h"
#include "pidgin.h"

#include "certificate.h"
#include "cipher.h"
#include "debug.h"
#include "notify.h"
#include "request.h"
#include "util.h"

#include "gtkcertmgr.h"
#include "gtkutils.h"
#include "pidginrichlabel.h"
#include "pidginselftest.h"

typedef struct {
	GtkWidget *window;
	GtkWidget *listview;
	GtkStringList *ids;
	GtkSingleSelection *selection;
	GtkWidget *export_button;
	GtkWidget *info_button;
	GtkWidget *delete_button;
	PurpleCertificatePool *tls_peers;
	GList *views;           /* open certificate windows */
} CertMgr;

static CertMgr *certmgr = NULL;

static PurpleCertificatePool *
tls_peers_pool(void)
{
	return purple_certificate_find_pool("x509", "tls_peers");
}

/**************************************************************************
 * Certificate details
 **************************************************************************/

static char *
format_time(time_t t)
{
	GDateTime *dt = g_date_time_new_from_unix_local(t);
	char *str;

	if (dt == NULL)
		return g_strdup("?");
	str = g_date_time_format(dt, "%c");
	g_date_time_unref(dt);
	return str;
}

/* SHA-256 over the DER data, when the scheme cannot give it: export the
 * certificate as PEM to a private temporary file and decode it. */
static char *
sha256_from_pem(PurpleCertificate *crt)
{
	char *tmpname = NULL, *pem = NULL, *result = NULL;
	const char *begin, *end;
	int fd;

	fd = g_file_open_tmp("pidgin4-cert-XXXXXX.pem", &tmpname, NULL);
	if (fd < 0)
		return NULL;
	close(fd);
	if (purple_certificate_export(tmpname, crt) &&
	    g_file_get_contents(tmpname, &pem, NULL, NULL) &&
	    (begin = strstr(pem, "-----BEGIN CERTIFICATE-----")) != NULL &&
	    (end = strstr(begin, "-----END CERTIFICATE-----")) != NULL) {
		char *b64;
		guchar *der;
		gsize der_len;

		begin += strlen("-----BEGIN CERTIFICATE-----");
		b64 = g_strndup(begin, end - begin);
		der = g_base64_decode(g_strstrip(b64), &der_len);
		if (der != NULL && der_len > 0) {
			GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA256);
			guint8 digest[32];
			gsize digest_len = sizeof(digest);

			g_checksum_update(sum, der, der_len);
			g_checksum_get_digest(sum, digest, &digest_len);
			g_checksum_free(sum);
			result = purple_base16_encode_chunked(digest, digest_len);
		}
		g_free(der);
		g_free(b64);
	}
	g_unlink(tmpname);
	g_free(tmpname);
	g_free(pem);
	return result;
}

static void
get_fingerprints(PurpleCertificate *crt, char **sha1, char **sha256)
{
	GByteArray *bin;

	*sha1 = NULL;
	*sha256 = NULL;

	bin = purple_certificate_get_fingerprint_sha1(crt);
	if (bin != NULL) {
		*sha1 = purple_base16_encode_chunked(bin->data, bin->len);
		g_byte_array_free(bin, TRUE);
	}
	bin = purple_certificate_get_fingerprint_sha256(crt, FALSE);
	if (bin != NULL) {
		*sha256 = purple_base16_encode_chunked(bin->data, bin->len);
		g_byte_array_free(bin, TRUE);
	} else {
		*sha256 = sha256_from_pem(crt);
	}
}

static void
append_row(GString *str, const char *label, const char *value, gboolean mono)
{
	char *v = g_markup_escape_text(value ? value : "", -1);

	g_string_append_printf(str, "<b>%s</b><br>%s%s%s<br><br>", label,
		mono ? "<font face=\"monospace\">" : "", v, mono ? "</font>" : "");
	g_free(v);
}

static char *
certificate_html(PurpleCertificate *crt, const char *id, char **sha1_out,
                 char **sha256_out)
{
	GString *str = g_string_new(NULL);
	char *cn, *issuer, *sha1, *sha256, *tmp;
	time_t activation = 0, expiration = 0;
	gboolean self_signed;

	cn = purple_certificate_get_subject_name(crt);
	issuer = purple_certificate_get_issuer_unique_id(crt);
	self_signed = purple_certificate_signed_by(crt, crt);
	get_fingerprints(crt, &sha1, &sha256);

	if (id != NULL)
		append_row(str, _("Hostname"), id, FALSE);
	append_row(str, _("Common name:"), cn ? cn : "(null)", FALSE);
	append_row(str, _("Issued By:"),
		self_signed ? _("(self-signed)") : (issuer ? issuer : "(null)"), FALSE);

	if (purple_certificate_get_times(crt, &activation, &expiration)) {
		time_t now = time(NULL);

		tmp = format_time(activation);
		append_row(str, _("Activation date:"), tmp, FALSE);
		g_free(tmp);
		tmp = format_time(expiration);
		if (expiration < now) {
			char *v = g_markup_escape_text(tmp, -1);
			g_string_append_printf(str, "<b>%s</b><br><font color=\"#c01c28\">%s (%s)"
				"</font><br><br>", _("Expiration date:"), v, _("expired"));
			g_free(v);
		} else {
			append_row(str, _("Expiration date:"), tmp, FALSE);
		}
		g_free(tmp);
	}

	append_row(str, _("Fingerprint (SHA1):"), sha1 ? sha1 : _("unavailable"), TRUE);
	append_row(str, _("Fingerprint (SHA256):"), sha256 ? sha256 : _("unavailable"), TRUE);

	g_free(cn);
	g_free(issuer);
	if (sha1_out != NULL)
		*sha1_out = sha1;
	else
		g_free(sha1);
	if (sha256_out != NULL)
		*sha256_out = sha256;
	else
		g_free(sha256);
	return g_string_free(str, FALSE);
}

static void view_certificate(GtkWindow *parent, PurpleCertificate *crt, const char *id);

static void
view_close_cb(GtkWidget *button, gpointer window)
{
	gtk_window_destroy(GTK_WINDOW(window));
}

static void
view_destroy_cb(GtkWidget *window, gpointer data)
{
	if (certmgr != NULL)
		certmgr->views = g_list_remove(certmgr->views, window);
}

static void
view_issuer_cb(GtkWidget *button, gpointer data)
{
	const char *issuer_id = data;
	PurpleCertificatePool *ca = purple_certificate_find_pool("x509", "ca");
	PurpleCertificate *issuer = NULL;

	if (ca != NULL && issuer_id != NULL)
		issuer = purple_certificate_pool_retrieve(ca, issuer_id);
	if (issuer == NULL) {
		purple_notify_info(NULL, _("Certificate Information"), "",
		                   _("Unable to find Issuer Certificate"));
		return;
	}
	view_certificate(GTK_WINDOW(gtk_widget_get_root(button)), issuer, NULL);
	purple_certificate_destroy(issuer);
}

/* Shows @crt (not taken) in a new window; returns nothing, the window is
 * tracked in certmgr->views while the manager is open. */
static void
view_certificate(GtkWindow *parent, PurpleCertificate *crt, const char *id)
{
	GtkWidget *window, *label, *sw, *button;
	char *html, *sha1 = NULL, *sha256 = NULL, *issuer_id;

	window = pidgin_dialog_new(_("Certificate Information"), parent,
	                           "certificate", TRUE);
	gtk_window_set_default_size(GTK_WINDOW(window), 520, 440);

	html = certificate_html(crt, id, &sha1, &sha256);
	label = pidgin_rich_label_new();
	pidgin_rich_label_set_force_text_view(PIDGIN_RICH_LABEL(label), TRUE);
	pidgin_rich_label_set_html(PIDGIN_RICH_LABEL(label), html, NULL);
	g_free(html);
	sw = pidgin_make_scrollable(label, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC, -1, 300);
	gtk_widget_set_vexpand(sw, TRUE);
	gtk_box_append(GTK_BOX(pidgin_dialog_get_content_area(window)), sw);

	/* For the selftest. */
	g_object_set_data_full(G_OBJECT(window), "pidgin-sha1", sha1, g_free);
	g_object_set_data_full(G_OBJECT(window), "pidgin-sha256", sha256, g_free);

	issuer_id = purple_certificate_get_issuer_unique_id(crt);
	if (issuer_id != NULL && !purple_certificate_signed_by(crt, crt)) {
		button = pidgin_dialog_add_button(window, _("View Issuer Certificate"),
			G_CALLBACK(view_issuer_cb), issuer_id);
		g_object_set_data_full(G_OBJECT(button), "pidgin-issuer", issuer_id, g_free);
	} else {
		g_free(issuer_id);
	}
	pidgin_dialog_add_button(window, _("_Close"), G_CALLBACK(view_close_cb), window);

	if (certmgr != NULL) {
		certmgr->views = g_list_prepend(certmgr->views, window);
		g_signal_connect(window, "destroy", G_CALLBACK(view_destroy_cb), NULL);
	}
	gtk_window_present(GTK_WINDOW(window));
}

/**************************************************************************
 * The manager
 **************************************************************************/

static const char *
selected_id(void)
{
	GtkStringObject *obj;

	if (certmgr == NULL)
		return NULL;
	obj = gtk_single_selection_get_selected_item(certmgr->selection);
	return obj != NULL ? gtk_string_object_get_string(obj) : NULL;
}

static void
update_buttons(void)
{
	gboolean sel = selected_id() != NULL;

	gtk_widget_set_sensitive(certmgr->export_button, sel);
	gtk_widget_set_sensitive(certmgr->info_button, sel);
	gtk_widget_set_sensitive(certmgr->delete_button, sel);
}

static int
id_compare(gconstpointer a, gconstpointer b)
{
	return g_utf8_collate(a, b);
}

static void
repopulate_list(void)
{
	GList *idlist, *l;
	GPtrArray *ids;
	char *keep;
	guint i, n;

	if (certmgr == NULL || certmgr->tls_peers == NULL)
		return;

	keep = g_strdup(selected_id());
	idlist = purple_certificate_pool_get_idlist(certmgr->tls_peers);
	idlist = g_list_sort(idlist, id_compare);
	ids = g_ptr_array_new();
	for (l = idlist; l != NULL; l = l->next)
		g_ptr_array_add(ids, l->data);
	g_ptr_array_add(ids, NULL);

	n = g_list_model_get_n_items(G_LIST_MODEL(certmgr->ids));
	gtk_string_list_splice(certmgr->ids, 0, n, (const char *const *)ids->pdata);
	g_ptr_array_free(ids, TRUE);
	purple_certificate_pool_destroy_idlist(idlist);

	gtk_single_selection_set_selected(certmgr->selection, GTK_INVALID_LIST_POSITION);
	n = g_list_model_get_n_items(G_LIST_MODEL(certmgr->ids));
	for (i = 0; keep != NULL && i < n; i++) {
		if (purple_strequal(gtk_string_list_get_string(certmgr->ids, i), keep)) {
			gtk_single_selection_set_selected(certmgr->selection, i);
			break;
		}
	}
	g_free(keep);
	update_buttons();
}

static void
pool_changed_cb(PurpleCertificatePool *pool, const char *id, gpointer data)
{
	repopulate_list();
}

/* Import */

static void
import_ok2_cb(gpointer data, const char *result)
{
	PurpleCertificate *crt = data;
	PurpleCertificatePool *pool = tls_peers_pool();

	if (result != NULL && *result != '\0' && pool != NULL)
		purple_certificate_pool_store(pool, result, crt);
	purple_certificate_destroy(crt);
}

static void
import_cancel2_cb(gpointer data, const char *result)
{
	purple_certificate_destroy(data);
}

static void
import_file(const char *filename)
{
	PurpleCertificatePool *pool = tls_peers_pool();
	PurpleCertificateScheme *x509;
	PurpleCertificate *crt;
	char *default_hostname;

	if (pool == NULL || certmgr == NULL)
		return;
	x509 = purple_certificate_pool_get_scheme(pool);
	crt = x509 != NULL ? purple_certificate_import(x509, filename) : NULL;
	if (crt == NULL) {
		char *secondary = g_strdup_printf(_("File %s could not be imported.\n"
			"Make sure that the file is readable and in PEM format.\n"), filename);
		purple_notify_error(NULL, _("Certificate Import Error"),
		                    _("X.509 certificate import failed"), secondary);
		g_free(secondary);
		return;
	}

	default_hostname = purple_certificate_get_subject_name(crt);
	purple_request_input(certmgr, _("Certificate Import"), _("Specify a hostname"),
		_("Type the host name for this certificate."),
		default_hostname, FALSE, FALSE, NULL,
		_("OK"), G_CALLBACK(import_ok2_cb),
		_("Cancel"), G_CALLBACK(import_cancel2_cb),
		NULL, NULL, NULL, crt);
	g_free(default_hostname);
}

static GListModel *
pem_filters(void)
{
	GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
	GtkFileFilter *filter = gtk_file_filter_new();

	gtk_file_filter_set_name(filter, _("PEM certificates"));
	gtk_file_filter_add_pattern(filter, "*.pem");
	gtk_file_filter_add_pattern(filter, "*.crt");
	gtk_file_filter_add_pattern(filter, "*.cer");
	gtk_file_filter_add_mime_type(filter, "application/x-x509-ca-cert");
	g_list_store_append(filters, filter);
	g_object_unref(filter);
	filter = gtk_file_filter_new();
	gtk_file_filter_set_name(filter, _("All files"));
	gtk_file_filter_add_pattern(filter, "*");
	g_list_store_append(filters, filter);
	g_object_unref(filter);
	return G_LIST_MODEL(filters);
}

static void
import_chosen_cb(GObject *source, GAsyncResult *result, gpointer data)
{
	GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, NULL);
	char *path;

	if (file == NULL)
		return;
	path = g_file_get_path(file);
	if (path != NULL)
		import_file(path);
	g_free(path);
	g_object_unref(file);
}

static void
import_cb(GtkWidget *button, gpointer data)
{
	GtkFileDialog *fd = gtk_file_dialog_new();
	GListModel *filters = pem_filters();

	gtk_file_dialog_set_title(fd, _("Select a PEM certificate"));
	gtk_file_dialog_set_filters(fd, filters);
	g_object_unref(filters);
	gtk_file_dialog_open(fd, GTK_WINDOW(certmgr->window), NULL, import_chosen_cb, NULL);
	g_object_unref(fd);
}

/* Export */

static void
export_chosen_cb(GObject *source, GAsyncResult *result, gpointer data)
{
	char *id = data;
	GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, NULL);
	PurpleCertificatePool *pool = tls_peers_pool();
	PurpleCertificate *crt;
	char *path;

	if (file == NULL || pool == NULL) {
		g_clear_object(&file);
		g_free(id);
		return;
	}
	path = g_file_get_path(file);
	crt = purple_certificate_pool_retrieve(pool, id);
	if (crt == NULL) {
		purple_debug_error("gtkcertmgr", "Certificate %s disappeared\n", id);
	} else if (path == NULL || !purple_certificate_export(path, crt)) {
		char *secondary = g_strdup_printf(_("Export to file %s failed.\n"
			"Check that you have write permission to the target path\n"),
			path ? path : "?");
		purple_notify_error(NULL, _("Certificate Export Error"),
		                    _("X.509 certificate export failed"), secondary);
		g_free(secondary);
	}
	if (crt != NULL)
		purple_certificate_destroy(crt);
	g_free(path);
	g_object_unref(file);
	g_free(id);
}

static void
export_cb(GtkWidget *button, gpointer data)
{
	const char *id = selected_id();
	GtkFileDialog *fd;
	GListModel *filters;
	char *name;

	if (id == NULL)
		return;
	fd = gtk_file_dialog_new();
	filters = pem_filters();
	gtk_file_dialog_set_title(fd, _("PEM X.509 Certificate Export"));
	gtk_file_dialog_set_filters(fd, filters);
	g_object_unref(filters);
	name = g_strdup_printf("%s.pem", id);
	gtk_file_dialog_set_initial_name(fd, name);
	g_free(name);
	gtk_file_dialog_save(fd, GTK_WINDOW(certmgr->window), NULL, export_chosen_cb,
	                     g_strdup(id));
	g_object_unref(fd);
}

/* View */

static void
view_id(const char *id)
{
	PurpleCertificate *crt;

	if (id == NULL || certmgr == NULL || certmgr->tls_peers == NULL)
		return;
	crt = purple_certificate_pool_retrieve(certmgr->tls_peers, id);
	if (crt == NULL) {
		purple_debug_warning("gtkcertmgr", "Could not retrieve %s\n", id);
		return;
	}
	view_certificate(GTK_WINDOW(certmgr->window), crt, id);
	purple_certificate_destroy(crt);
}

static void
info_cb(GtkWidget *button, gpointer data)
{
	view_id(selected_id());
}

static void
activate_cb(GtkListView *view, guint position, gpointer data)
{
	view_id(gtk_string_list_get_string(certmgr->ids, position));
}

/* Delete */

static void
delete_response_cb(GObject *source, GAsyncResult *result, gpointer data)
{
	char *id = data;
	PurpleCertificatePool *pool = tls_peers_pool();
	int button = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, NULL);

	if (button == 1 && pool != NULL && !purple_certificate_pool_delete(pool, id))
		purple_debug_warning("gtkcertmgr", "Deletion failed on id %s\n", id);
	g_free(id);
}

static void
delete_cb(GtkWidget *button, gpointer data)
{
	const char *id = selected_id();
	const char *buttons[] = { _("_Cancel"), _("_Delete"), NULL };
	GtkAlertDialog *alert;

	if (id == NULL)
		return;
	alert = gtk_alert_dialog_new(_("Really delete certificate for %s?"), id);
	gtk_alert_dialog_set_buttons(alert, buttons);
	gtk_alert_dialog_set_cancel_button(alert, 0);
	gtk_alert_dialog_set_default_button(alert, 0);
	gtk_alert_dialog_choose(alert, GTK_WINDOW(certmgr->window), NULL,
	                        delete_response_cb, g_strdup(id));
	g_object_unref(alert);
}

static void
selection_changed_cb(GtkSelectionModel *model, guint position, guint n, gpointer data)
{
	update_buttons();
}

static void
close_cb(GtkWidget *button, gpointer data)
{
	pidgin_certmgr_hide();
}

static void
certmgr_destroy_cb(GtkWidget *window, gpointer data)
{
	CertMgr *mgr = certmgr;
	GList *views;

	if (mgr == NULL)
		return;
	certmgr = NULL;
	purple_signals_disconnect_by_handle(mgr);
	purple_request_close_with_handle(mgr);
	views = mgr->views;
	mgr->views = NULL;
	g_list_free_full(views, (GDestroyNotify)gtk_window_destroy);
	g_clear_object(&mgr->selection);
	g_clear_object(&mgr->ids);
	g_free(mgr);
}

static void
setup_row_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	GtkWidget *label = gtk_label_new(NULL);

	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
	gtk_list_item_set_child(li, label);
}

static void
bind_row_cb(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
	GtkStringObject *obj = gtk_list_item_get_item(li);

	gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(li)),
	                   gtk_string_object_get_string(obj));
}

void
pidgin_certmgr_show(void)
{
	CertMgr *mgr;
	GtkWidget *win, *content, *hbox, *vbox, *sw, *label, *button;
	GtkListItemFactory *factory;

	if (certmgr != NULL) {
		gtk_window_present(GTK_WINDOW(certmgr->window));
		return;
	}

	certmgr = mgr = g_new0(CertMgr, 1);
	mgr->tls_peers = tls_peers_pool();

	mgr->window = win = pidgin_dialog_new(_("Certificate Manager"),
		pidgin_get_active_window(), "certmgr", TRUE);
	gtk_window_set_default_size(GTK_WINDOW(win), 520, 440);
	g_signal_connect(win, "destroy", G_CALLBACK(certmgr_destroy_cb), NULL);
	content = pidgin_dialog_get_content_area(win);

	label = gtk_label_new(NULL);
	{
		char *markup = g_markup_printf_escaped("<b>%s</b>", _("SSL Servers"));
		gtk_label_set_markup(GTK_LABEL(label), markup);
		g_free(markup);
	}
	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_box_append(GTK_BOX(content), label);

	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PIDGIN_HIG_BOX_SPACE);
	gtk_widget_set_vexpand(hbox, TRUE);
	gtk_box_append(GTK_BOX(content), hbox);

	mgr->ids = gtk_string_list_new(NULL);
	mgr->selection = gtk_single_selection_new(G_LIST_MODEL(g_object_ref(mgr->ids)));
	gtk_single_selection_set_autoselect(mgr->selection, FALSE);
	gtk_single_selection_set_can_unselect(mgr->selection, TRUE);
	g_signal_connect(mgr->selection, "selection-changed",
	                 G_CALLBACK(selection_changed_cb), NULL);

	factory = gtk_signal_list_item_factory_new();
	g_signal_connect(factory, "setup", G_CALLBACK(setup_row_cb), NULL);
	g_signal_connect(factory, "bind", G_CALLBACK(bind_row_cb), NULL);
	mgr->listview = gtk_list_view_new(
		GTK_SELECTION_MODEL(g_object_ref(mgr->selection)), factory);
	gtk_accessible_update_property(GTK_ACCESSIBLE(mgr->listview),
		GTK_ACCESSIBLE_PROPERTY_LABEL, _("Hostname"), -1);
	g_signal_connect(mgr->listview, "activate", G_CALLBACK(activate_cb), NULL);
	sw = pidgin_make_scrollable(mgr->listview, GTK_POLICY_NEVER,
	                            GTK_POLICY_AUTOMATIC, 300, 300);
	gtk_widget_add_css_class(sw, "frame");
	gtk_widget_set_hexpand(sw, TRUE);
	gtk_box_append(GTK_BOX(hbox), sw);

	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, PIDGIN_HIG_BOX_SPACE);
	gtk_box_append(GTK_BOX(hbox), vbox);

	button = gtk_button_new_with_mnemonic(_("_Import..."));
	g_signal_connect(button, "clicked", G_CALLBACK(import_cb), NULL);
	gtk_box_append(GTK_BOX(vbox), button);
	gtk_widget_set_sensitive(button, mgr->tls_peers != NULL);

	mgr->export_button = button = gtk_button_new_with_mnemonic(_("_Export..."));
	g_signal_connect(button, "clicked", G_CALLBACK(export_cb), NULL);
	gtk_box_append(GTK_BOX(vbox), button);

	mgr->info_button = button = gtk_button_new_with_mnemonic(_("_Get Info"));
	g_signal_connect(button, "clicked", G_CALLBACK(info_cb), NULL);
	gtk_box_append(GTK_BOX(vbox), button);

	mgr->delete_button = button = gtk_button_new_with_mnemonic(_("_Delete"));
	g_signal_connect(button, "clicked", G_CALLBACK(delete_cb), NULL);
	gtk_box_append(GTK_BOX(vbox), button);

	pidgin_dialog_add_button(win, _("_Close"), G_CALLBACK(close_cb), NULL);

	if (mgr->tls_peers != NULL) {
		purple_signal_connect(mgr->tls_peers, "certificate-stored", mgr,
		                      PURPLE_CALLBACK(pool_changed_cb), NULL);
		purple_signal_connect(mgr->tls_peers, "certificate-deleted", mgr,
		                      PURPLE_CALLBACK(pool_changed_cb), NULL);
	} else {
		purple_debug_warning("gtkcertmgr", "No x509/tls_peers pool\n");
	}
	repopulate_list();

	gtk_window_present(GTK_WINDOW(win));
}

void
pidgin_certmgr_hide(void)
{
	if (certmgr != NULL)
		gtk_window_destroy(GTK_WINDOW(certmgr->window));
}

/**************************************************************************
 * Selftest
 **************************************************************************/

#define MODULE "certmgr"

void
pidgin_certmgr_selftest(void)
{
	guint i, n, viewed = 0;

	pidgin_certmgr_show();
	pidgin_selftest_iterate(100);
	if (certmgr == NULL) {
		pidgin_selftest_fail(MODULE, "the manager did not open");
		return;
	}
	if (certmgr->tls_peers == NULL) {
		pidgin_selftest_fail(MODULE, "no tls_peers pool");
		pidgin_certmgr_hide();
		return;
	}

	n = g_list_model_get_n_items(G_LIST_MODEL(certmgr->ids));
	pidgin_selftest_log(MODULE, "%u certificates in tls_peers", n);
	for (i = 0; i < n; i++) {
		const char *id = gtk_string_list_get_string(certmgr->ids, i);
		GtkWidget *view;
		const char *sha1, *sha256;

		gtk_single_selection_set_selected(certmgr->selection, i);
		info_cb(NULL, NULL);
		if (certmgr->views == NULL) {
			pidgin_selftest_fail(MODULE, "no view for %s", id);
			continue;
		}
		view = certmgr->views->data;
		pidgin_selftest_iterate(5);
		sha1 = g_object_get_data(G_OBJECT(view), "pidgin-sha1");
		sha256 = g_object_get_data(G_OBJECT(view), "pidgin-sha256");
		if (sha1 == NULL || strlen(sha1) != 59)
			pidgin_selftest_fail(MODULE, "%s: bad SHA-1 fingerprint %s", id,
			                     sha1 ? sha1 : "(none)");
		if (sha256 == NULL || strlen(sha256) != 95)
			pidgin_selftest_fail(MODULE, "%s: bad SHA-256 fingerprint %s", id,
			                     sha256 ? sha256 : "(none)");
		gtk_window_destroy(GTK_WINDOW(view));
		viewed++;
	}
	pidgin_selftest_iterate(50);
	if (certmgr->views != NULL)
		pidgin_selftest_fail(MODULE, "views left open");
	pidgin_selftest_log(MODULE, "viewed %u certificates", viewed);

	/* The fallback path: SHA-256 from the exported PEM must match. */
	if (n > 0) {
		PurpleCertificate *crt = purple_certificate_pool_retrieve(certmgr->tls_peers,
			gtk_string_list_get_string(certmgr->ids, 0));
		if (crt != NULL) {
			char *a = NULL, *b = sha256_from_pem(crt);
			GByteArray *bin = purple_certificate_get_fingerprint_sha256(crt, FALSE);

			if (bin != NULL) {
				a = purple_base16_encode_chunked(bin->data, bin->len);
				g_byte_array_free(bin, TRUE);
			}
			if (a != NULL && !purple_strequal(a, b))
				pidgin_selftest_fail(MODULE, "SHA-256 mismatch: %s / %s", a,
				                     b ? b : "(none)");
			g_free(a);
			g_free(b);
			purple_certificate_destroy(crt);
		}
	}

	pidgin_certmgr_hide();
	pidgin_selftest_iterate(50);
	if (certmgr != NULL)
		pidgin_selftest_fail(MODULE, "the manager did not close");
}
