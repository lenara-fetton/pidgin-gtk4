/**
 * @file dnssrv.c
 */

/* purple
 *
 * Copyright (C) 2005 Thomas Butter <butter@uni-mannheim.de>
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
#define _PURPLE_DNSSRV_C_

#include "glibcompat.h"
#include "internal.h"
#include "util.h"

#include "debug.h"
#include "dnssrv.h"
#include "eventloop.h"
#include "network.h"

#include <gio/gio.h>

/*
 * SRV and TXT records are looked up with GResolver
 * (g_resolver_lookup_records_async() on g_resolver_get_default()), which
 * runs the blocking res_query() in a GLib worker thread and delivers the
 * result on the main context.  This replaces the fork()ed resolver process
 * (Unix) and the DnsQuery thread (Windows) used before; the public API, the
 * PurpleSrvResponse/PurpleTxtResponse structs and the RFC 2782 ordering are
 * unchanged.
 */

static PurpleSrvTxtQueryUiOps *srv_txt_query_ui_ops = NULL;

/*
 * Links a PurpleSrvTxtQueryData to its in-flight GResolver lookup.  The
 * GResolver callback always runs, even after cancellation, so it owns this
 * struct.  purple_srv_txt_query_destroy() only detaches the query from it
 * (query_data = NULL) and cancels the lookup.
 */
typedef struct {
	PurpleSrvTxtQueryData *query_data;
	GCancellable *cancellable;
} PurpleSrvTxtLookup;

struct _PurpleSrvTxtQueryData {
	union {
		PurpleSrvCallback srv;
		PurpleTxtCallback txt;
	} cb;

	gpointer extradata;
	int type;
	char *query;
	PurpleSrvTxtLookup *lookup;
};

typedef struct _PurpleSrvResponseContainer {
	PurpleSrvResponse *response;
	int sum;
} PurpleSrvResponseContainer;

static gboolean purple_srv_txt_query_ui_resolve(PurpleSrvTxtQueryData *query_data);

/**
 * Sort by priority, then by weight.  Strictly numerically--no
 * randomness.  Technically we only need to sort by pref and then
 * make sure any records with weight 0 are at the beginning of
 * their group, but it's just as easy to sort by weight.
 */
static gint
responsecompare(gconstpointer ar, gconstpointer br)
{
	PurpleSrvResponse *a = (PurpleSrvResponse*)ar;
	PurpleSrvResponse *b = (PurpleSrvResponse*)br;

	if(a->pref == b->pref) {
		if(a->weight == b->weight)
			return 0;
		if(a->weight < b->weight)
			return -1;
		return 1;
	}
	if(a->pref < b->pref)
		return -1;
	return 1;
}

/**
 * Iterate over a list of PurpleSrvResponseContainer making the sum
 * the running total of the sums.  Select a random integer in the range
 * (1, sum+1), then find the first element greater than or equal to the
 * number selected.  From RFC 2782.
 *
 * @param list The list of PurpleSrvResponseContainer.  This function
 *        removes a node from this list and returns the new list.
 * @param container_ptr The PurpleSrvResponseContainer that was chosen
 *        will be returned here.
 */
static GList *
select_random_response(GList *list, PurpleSrvResponseContainer **container_ptr)
{
	GList *cur;
	size_t runningtotal;
	int r;

	runningtotal = 0;
	cur = list;

	while (cur) {
		PurpleSrvResponseContainer *container = cur->data;
		runningtotal += container->response->weight;
		container->sum = runningtotal;
		cur = cur->next;
	}

	/*
	 * If the running total is greater than 0, pick a number between
	 * 1 and the runningtotal inclusive. (This is not precisely what
	 * the RFC algorithm describes, but we wish to deal with integers
	 * and avoid floats.  This is functionally equivalent.)
	 * If running total is 0, then choose r = 0.
	 */
	r = runningtotal ? g_random_int_range(1, runningtotal + 1) : 0;
	cur = list;
	while (r > ((PurpleSrvResponseContainer *)cur->data)->sum) {
		if(cur->next == NULL) {
			break;
		}

		cur = cur->next;
	}

	/* Set the return parameter and remove cur from the list */
	*container_ptr = cur->data;
	return g_list_delete_link(list, cur);
}

/**
 * Reorder a GList of PurpleSrvResponses that have the same priority
 * (aka "pref").
 */
static void
srv_reorder(GList *list, int num)
{
	int i;
	GList *cur, *container_list = NULL;
	PurpleSrvResponseContainer *container;

	if (num < 2)
		/* Nothing to sort */
		return;

	/* First build a list of container structs */
	for (i = 0, cur = list; i < num; i++, cur = cur->next) {
		container = g_new(PurpleSrvResponseContainer, 1);
		container->response = cur->data;
		container_list = g_list_prepend(container_list, container);
	}
	container_list = g_list_reverse(container_list);

	/*
	 * Re-order the list that was passed in as a parameter.  We leave
	 * the list nodes in place, but replace their data pointers.
	 */
	cur = list;
	while (container_list) {
		container_list = select_random_response(container_list, &container);

		if(container == NULL) {
			break;
		}

		cur->data = container->response;
		g_free(container);
		cur = cur->next;

		if(cur == NULL) {
			break;
		}
	}
}

/**
 * Sorts a GList of PurpleSrvResponses according to the
 * algorithm described in RFC 2782.
 *
 * @param response GList of PurpleSrvResponse's
 * @param The original list, resorted
 */
static GList *
purple_srv_sort(GList *list)
{
	int count;
	GList *cur, *start;

	if (!list || !list->next) {
		/* Nothing to sort */
		return list;
	}

	list = g_list_sort(list, responsecompare);

	start = cur = list;
	count = 1;
	while (cur) {
		PurpleSrvResponse *next_response;
		PurpleSrvResponse *resp = (PurpleSrvResponse *)cur->data;
		next_response = cur->next ? cur->next->data : NULL;

		if(resp == NULL) {
			continue;
		}

		if (!next_response || next_response->pref != resp->pref) {
			/*
			 * The 'count' records starting at 'start' all have the same
			 * priority.  Sort them by weight.
			 */
			srv_reorder(start, count);
			start = cur->next;
			count = 0;
		}
		count++;
		cur = cur->next;
	}

	return list;
}

static PurpleSrvTxtQueryData *
query_data_new(int type, gchar *query, gpointer extradata)
{
	PurpleSrvTxtQueryData *query_data = g_new0(PurpleSrvTxtQueryData, 1);
	query_data->type = type;
	query_data->extradata = extradata;
	query_data->query = query;
	return query_data;
}

void
purple_srv_txt_query_destroy(PurpleSrvTxtQueryData *query_data)
{
	PurpleSrvTxtQueryUiOps *ops = purple_srv_txt_query_get_ui_ops();

	if (ops && ops->destroy)
		ops->destroy(query_data);

	if (query_data->lookup != NULL) {
		/*
		 * Cancelling an in-progress lookup.  The GResolver callback
		 * still runs later; it finds the lookup detached and just
		 * frees it.
		 */
		query_data->lookup->query_data = NULL;
		g_cancellable_cancel(query_data->lookup->cancellable);
		query_data->lookup = NULL;
	}

	g_free(query_data->query);
	g_free(query_data);
}

static gboolean
dns_str_is_ascii(const char *name)
{
	const guchar *c;
	for (c = (const guchar *)name; c && *c; ++c) {
		if (*c > 0x7f)
			return FALSE;
	}

	return TRUE;
}

/*
 * Convert a non-ASCII domain to punycode with libpurple's own IDNA code, so
 * that all of libpurple agrees on the mapping.  Returns NULL on failure.
 */
static gchar *
domain_to_ascii(const char *domain)
{
	gchar *hostname;

	if (dns_str_is_ascii(domain))
		return g_strdup(domain);

	if (purple_network_convert_idn_to_ascii(domain, &hostname) != 0) {
		purple_debug_error("dnssrv", "IDNA ToASCII failed\n");
		return NULL;
	}

	return hostname;
}

static void
purple_srv_txt_lookup_free(PurpleSrvTxtLookup *lookup)
{
	g_object_unref(lookup->cancellable);
	g_free(lookup);
}

/* Build PurpleSrvResponses from GResolver's "(qqqs)" SRV records. */
static GList *
srv_records_to_responses(GList *records)
{
	GList *ret = NULL, *l;

	for (l = records; l != NULL; l = l->next) {
		PurpleSrvResponse *srvres;
		guint16 pref, weight, port;
		const gchar *target;

		if (!g_variant_is_of_type(l->data, G_VARIANT_TYPE("(qqqs)")))
			continue;

		g_variant_get(l->data, "(qqq&s)", &pref, &weight, &port, &target);

		srvres = g_new0(PurpleSrvResponse, 1);
		if (strlen(target) > sizeof(srvres->hostname) - 1) {
			purple_debug_error("dnssrv", "hostname is longer than available buffer ('%s', %"
			                   G_GSIZE_FORMAT " bytes)!\n",
			                   target, strlen(target));
		}
		g_strlcpy(srvres->hostname, target, sizeof(srvres->hostname));
		srvres->pref = pref;
		srvres->port = port;
		srvres->weight = weight;

		ret = g_list_prepend(ret, srvres);
	}

	return g_list_reverse(ret);
}

/*
 * Build PurpleTxtResponses from GResolver's "(as)" TXT records.  A TXT
 * record made of several character-strings is concatenated, as RFC 7208
 * and the old Windows code did.
 */
static GList *
txt_records_to_responses(GList *records)
{
	GList *ret = NULL, *l;

	for (l = records; l != NULL; l = l->next) {
		PurpleTxtResponse *txtres;
		GVariantIter *iter;
		const gchar *str;
		GString *content;

		if (!g_variant_is_of_type(l->data, G_VARIANT_TYPE("(as)")))
			continue;

		content = g_string_new(NULL);
		g_variant_get(l->data, "(as)", &iter);
		while (g_variant_iter_loop(iter, "&s", &str))
			g_string_append(content, str);
		g_variant_iter_free(iter);

		txtres = g_new0(PurpleTxtResponse, 1);
		txtres->content = g_string_free(content, FALSE);
		ret = g_list_prepend(ret, txtres);
	}

	return g_list_reverse(ret);
}

static void
records_resolved(GObject *source, GAsyncResult *result, gpointer user_data)
{
	PurpleSrvTxtLookup *lookup = user_data;
	PurpleSrvTxtQueryData *query_data = lookup->query_data;
	GList *records;
	GError *error = NULL;

	records = g_resolver_lookup_records_finish(G_RESOLVER(source), result,
			&error);

	if (query_data == NULL) {
		/* purple_srv_txt_query_destroy() was called while resolving. */
		g_list_free_full(records, (GDestroyNotify)g_variant_unref);
		g_clear_error(&error);
		purple_srv_txt_lookup_free(lookup);
		return;
	}

	query_data->lookup = NULL;
	purple_srv_txt_lookup_free(lookup);

	if (error != NULL) {
		if (g_error_matches(error, G_RESOLVER_ERROR, G_RESOLVER_ERROR_NOT_FOUND))
			purple_debug_info("dnssrv", "Found 0 entries for %s: %s\n",
					query_data->query, error->message);
		else
			purple_debug_warning("dnssrv", "Lookup of %s failed: %s\n",
					query_data->query, error->message);
		g_error_free(error);
	}

	if (query_data->type == PurpleDnsTypeSrv) {
		GList *responses, *l;
		PurpleSrvResponse *res = NULL;
		int size, i;

		responses = purple_srv_sort(srv_records_to_responses(records));
		size = g_list_length(responses);
		purple_debug_info("dnssrv", "found %d SRV entries\n", size);

		if (size > 0) {
			res = g_new0(PurpleSrvResponse, size);
			for (l = responses, i = 0; l != NULL; l = l->next, i++)
				res[i] = *(PurpleSrvResponse *)l->data;
		}
		g_list_free_full(responses, g_free);

		/* The callback owns (and g_free()s) the array. */
		query_data->cb.srv(res, size, query_data->extradata);
	} else if (query_data->type == PurpleDnsTypeTxt) {
		GList *responses = txt_records_to_responses(records);

		purple_debug_info("dnssrv", "found %d TXT entries\n",
				g_list_length(responses));

		/* The callback owns the list and its entries. */
		query_data->cb.txt(responses, query_data->extradata);
	} else {
		purple_debug_error("dnssrv", "unknown query type %d\n", query_data->type);
	}

	g_list_free_full(records, (GDestroyNotify)g_variant_unref);

	purple_srv_txt_query_destroy(query_data);
}

static void
lookup_records(PurpleSrvTxtQueryData *query_data)
{
	GResolver *resolver;
	PurpleSrvTxtLookup *lookup;

	lookup = g_new0(PurpleSrvTxtLookup, 1);
	lookup->query_data = query_data;
	lookup->cancellable = g_cancellable_new();
	query_data->lookup = lookup;

	resolver = g_resolver_get_default();
	g_resolver_lookup_records_async(resolver, query_data->query,
			query_data->type == PurpleDnsTypeSrv ?
				G_RESOLVER_RECORD_SRV : G_RESOLVER_RECORD_TXT,
			lookup->cancellable, records_resolved, lookup);
	g_object_unref(resolver);
}

PurpleSrvTxtQueryData *
purple_srv_resolve(const char *protocol, const char *transport,
	const char *domain, PurpleSrvCallback cb, gpointer extradata)
{
	return purple_srv_resolve_account(NULL, protocol, transport, domain,
			cb, extradata);
}

PurpleSrvTxtQueryData *
purple_srv_resolve_account(PurpleAccount *account, const char *protocol,
	const char *transport, const char *domain, PurpleSrvCallback cb,
	gpointer extradata)
{
	char *query;
	char *hostname;
	PurpleSrvTxtQueryData *query_data;
	PurpleProxyType proxy_type;

	if (!protocol || !*protocol || !transport || !*transport || !domain || !*domain) {
		purple_debug_error("dnssrv", "Wrong arguments\n");
		cb(NULL, 0, extradata);
		g_return_val_if_reached(NULL);
	}

	proxy_type = purple_proxy_info_get_type(
		purple_proxy_get_setup(account));
	if (proxy_type == PURPLE_PROXY_TOR) {
		purple_debug_info("dnssrv", "Aborting SRV lookup in Tor Proxy mode.\n");
		cb(NULL, 0, extradata);
		return NULL;
	}

	hostname = domain_to_ascii(domain);
	if (hostname == NULL) {
		cb(NULL, 0, extradata);
		return NULL;
	}

	query = g_strdup_printf("_%s._%s.%s", protocol, transport, hostname);
	purple_debug_info("dnssrv","querying SRV record for %s: %s\n", domain,
			query);
	g_free(hostname);

	query_data = query_data_new(PurpleDnsTypeSrv, query, extradata);
	query_data->cb.srv = cb;

	if (purple_srv_txt_query_ui_resolve(query_data))
	{
		return query_data;
	}

	lookup_records(query_data);

	return query_data;
}

PurpleSrvTxtQueryData *purple_txt_resolve(const char *owner,
	const char *domain, PurpleTxtCallback cb, gpointer extradata)
{
	return purple_txt_resolve_account(NULL, owner, domain, cb, extradata);
}

PurpleSrvTxtQueryData *purple_txt_resolve_account(PurpleAccount *account,
	const char *owner, const char *domain, PurpleTxtCallback cb,
	gpointer extradata)
{
	char *query;
	char *hostname;
	PurpleSrvTxtQueryData *query_data;
	PurpleProxyType proxy_type;

	proxy_type = purple_proxy_info_get_type(
		purple_proxy_get_setup(account));
	if (proxy_type == PURPLE_PROXY_TOR) {
		purple_debug_info("dnssrv", "Aborting TXT lookup in Tor Proxy mode.\n");
		cb(NULL, extradata);
		return NULL;
	}

	hostname = domain_to_ascii(domain);
	if (hostname == NULL) {
		cb(NULL, extradata);
		return NULL;
	}

	query = g_strdup_printf("%s.%s", owner, hostname);
	purple_debug_info("dnssrv","querying TXT record for %s: %s\n", domain,
			query);
	g_free(hostname);

	query_data = query_data_new(PurpleDnsTypeTxt, query, extradata);
	query_data->cb.txt = cb;

	if (purple_srv_txt_query_ui_resolve(query_data)) {
		/* query intentionally not freed
		 */
		return query_data;
	}

	lookup_records(query_data);

	return query_data;
}

void
purple_txt_cancel(PurpleSrvTxtQueryData *query_data)
{
	purple_srv_txt_query_destroy(query_data);
}

void
purple_srv_cancel(PurpleSrvTxtQueryData *query_data)
{
	purple_srv_txt_query_destroy(query_data);
}

const gchar *
purple_txt_response_get_content(PurpleTxtResponse *resp)
{
	g_return_val_if_fail(resp != NULL, NULL);

	return resp->content;
}

void purple_txt_response_destroy(PurpleTxtResponse *resp)
{
	g_return_if_fail(resp != NULL);

	g_free(resp->content);
	g_free(resp);
}

/*
 * Only used as the callback for the ui ops.
 */
static void
purple_srv_query_resolved(PurpleSrvTxtQueryData *query_data, GList *records)
{
	GList *l;
	PurpleSrvResponse *records_array;
	int i = 0, length;

	g_return_if_fail(records != NULL);

	if (query_data->cb.srv == NULL) {
		purple_srv_txt_query_destroy(query_data);

		while (records) {
			g_free(records->data);
			records = g_list_delete_link(records, records);
		}
		return;
	}

	records = purple_srv_sort(records);
	length = g_list_length(records);

	purple_debug_info("dnssrv", "SRV records resolved for %s, count: %d\n",
	                            query_data->query, length);

	records_array = g_new(PurpleSrvResponse, length);
	for (l = records; l; l = l->next, i++) {
		records_array[i] = *(PurpleSrvResponse *)l->data;
	}

	query_data->cb.srv(records_array, length, query_data->extradata);

	purple_srv_txt_query_destroy(query_data);

	while (records) {
		g_free(records->data);
		records = g_list_delete_link(records, records);
	}
}

/*
 * Only used as the callback for the ui ops.
 */
static void
purple_txt_query_resolved(PurpleSrvTxtQueryData *query_data, GList *entries)
{
	g_return_if_fail(entries != NULL);

	purple_debug_info("dnssrv", "TXT entries resolved for %s, count: %d\n", query_data->query, g_list_length(entries));

	/* the callback should g_free the entries.
	 */
	if (query_data->cb.txt != NULL)
		query_data->cb.txt(entries, query_data->extradata);
	else {
		while (entries) {
			g_free(entries->data);
			entries = g_list_delete_link(entries, entries);
		}
	}

	purple_srv_txt_query_destroy(query_data);
}

/*
 * Only used as the callback for the ui ops.
 */
static void
purple_srv_query_failed(PurpleSrvTxtQueryData *query_data, const gchar *error_message)
{
	purple_debug_error("dnssrv", "%s\n", error_message);

	if (query_data->type == PurpleDnsTypeTxt) {
		if (query_data->cb.txt != NULL)
			query_data->cb.txt(NULL, query_data->extradata);
	} else if (query_data->cb.srv != NULL)
		query_data->cb.srv(NULL, 0, query_data->extradata);

	purple_srv_txt_query_destroy(query_data);
}

static gboolean
purple_srv_txt_query_ui_resolve(PurpleSrvTxtQueryData *query_data)
{
	PurpleSrvTxtQueryUiOps *ops = purple_srv_txt_query_get_ui_ops();

	if (ops && ops->resolve)
		return ops->resolve(query_data, (query_data->type == PurpleDnsTypeSrv ? purple_srv_query_resolved : purple_txt_query_resolved), purple_srv_query_failed);

	return FALSE;
}

void
purple_srv_txt_query_set_ui_ops(PurpleSrvTxtQueryUiOps *ops)
{
	srv_txt_query_ui_ops = ops;
}

PurpleSrvTxtQueryUiOps *
purple_srv_txt_query_get_ui_ops(void)
{
	/* It is perfectly acceptable for srv_txt_query_ui_ops to be NULL; this just
	 * means that the default GResolver implementation will be used.
	 */
	return srv_txt_query_ui_ops;
}

char *
purple_srv_txt_query_get_query(PurpleSrvTxtQueryData *query_data)
{
	g_return_val_if_fail(query_data != NULL, NULL);

	return query_data->query;
}


int
purple_srv_txt_query_get_type(PurpleSrvTxtQueryData *query_data)
{
	g_return_val_if_fail(query_data != NULL, 0);

	return query_data->type;
}
