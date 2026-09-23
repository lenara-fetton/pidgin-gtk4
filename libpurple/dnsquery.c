/**
 * @file dnsquery.c DNS query API
 * @ingroup core
 */

/* purple
 *
 * Purple is the legal property of its developers, whose names are too numerous
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
 *
 */
#define _PURPLE_DNSQUERY_C_

#include "internal.h"
#include "debug.h"
#include "dnsquery.h"
#include "glibcompat.h"
#include "network.h"
#include "notify.h"
#include "prefs.h"
#include "util.h"

#include <gio/gio.h>

/**************************************************************************
 * DNS query API
 *
 * Host names are resolved with GResolver (g_resolver_get_default()), which
 * runs getaddrinfo() in a GLib worker thread and delivers the result on the
 * main context.  This replaces the fork()ed resolver children used before;
 * the public API and the callback contract are unchanged.
 **************************************************************************/

static PurpleDnsQueryUiOps *dns_query_ui_ops = NULL;

/*
 * Links a PurpleDnsQueryData to its in-flight GResolver lookup.  The
 * GResolver callback always runs, even after cancellation, so the callback
 * owns this struct.  purple_dnsquery_destroy() only detaches the query from
 * it (query_data = NULL) and cancels the lookup.
 */
typedef struct {
	PurpleDnsQueryData *query_data;
	GCancellable *cancellable;
} PurpleDnsQueryLookup;

struct _PurpleDnsQueryData {
	char *hostname;
	int port;
	PurpleDnsQueryConnectFunction callback;
	gpointer data;
	guint timeout;
	PurpleAccount *account;

	PurpleDnsQueryLookup *lookup;
};

static void
purple_dnsquery_free_hosts(GSList *hosts)
{
	/* The list is pairs of an address length and a struct sockaddr *. */
	while (hosts != NULL)
	{
		hosts = g_slist_delete_link(hosts, hosts);
		if (hosts == NULL)
			break;
		g_free(hosts->data);
		hosts = g_slist_delete_link(hosts, hosts);
	}
}

static void
purple_dnsquery_resolved(PurpleDnsQueryData *query_data, GSList *hosts)
{
	purple_debug_info("dnsquery", "IP resolved for %s\n", query_data->hostname);
	if (query_data->callback != NULL)
		query_data->callback(hosts, query_data->data, NULL);
	else
		/*
		 * Callback is a required parameter, but be safe and don't
		 * leak the hosts if a UI resolver hands us a result anyway.
		 */
		purple_dnsquery_free_hosts(hosts);

	purple_dnsquery_destroy(query_data);
}

static void
purple_dnsquery_failed(PurpleDnsQueryData *query_data, const gchar *error_message)
{
	purple_debug_error("dnsquery", "%s\n", error_message);
	if (query_data->callback != NULL)
		query_data->callback(NULL, query_data->data, error_message);
	purple_dnsquery_destroy(query_data);
}

static gboolean
purple_dnsquery_ui_resolve(PurpleDnsQueryData *query_data)
{
	PurpleDnsQueryUiOps *ops = purple_dnsquery_get_ui_ops();

	if (ops && ops->resolve_host)
		return ops->resolve_host(query_data, purple_dnsquery_resolved, purple_dnsquery_failed);

	return FALSE;
}

/*
 * If the "hostname" is a literal IPv4 or IPv6 address (including an IPv6
 * scope id), answer without a lookup.
 */
static gboolean
resolve_ip(PurpleDnsQueryData *query_data)
{
	struct addrinfo hints, *res;
	char servname[20];

	g_snprintf(servname, sizeof(servname), "%d", query_data->port);
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_flags |= AI_NUMERICHOST;

	if (0 == getaddrinfo(query_data->hostname, servname, &hints, &res))
	{
		GSList *hosts = NULL;
		hosts = g_slist_append(hosts, GINT_TO_POINTER(res->ai_addrlen));
		hosts = g_slist_append(hosts, g_memdup2(res->ai_addr, res->ai_addrlen));
		freeaddrinfo(res);

		purple_dnsquery_resolved(query_data, hosts);

		return TRUE;
	}

	return FALSE;
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
 * Convert GResolver's list of GInetAddresses into the historical "hosts"
 * list: alternating (size_t addrlen, struct sockaddr *addr) entries, in
 * resolver order, with the port filled in.
 */
static GSList *
addresses_to_hosts(GList *addresses, int port)
{
	GSList *hosts = NULL;
	GList *l;

	for (l = addresses; l != NULL; l = l->next) {
		GSocketAddress *sockaddr;
		gssize addrlen;
		gpointer addr;
		GError *error = NULL;

		sockaddr = g_inet_socket_address_new(G_INET_ADDRESS(l->data), port);
		addrlen = g_socket_address_get_native_size(sockaddr);
		if (addrlen <= 0) {
			g_object_unref(sockaddr);
			continue;
		}

		addr = g_malloc0(addrlen);
		if (!g_socket_address_to_native(sockaddr, addr, addrlen, &error)) {
			purple_debug_warning("dnsquery",
					"Unable to convert address: %s\n", error->message);
			g_error_free(error);
			g_free(addr);
			g_object_unref(sockaddr);
			continue;
		}
		g_object_unref(sockaddr);

		hosts = g_slist_prepend(hosts, GSIZE_TO_POINTER((gsize)addrlen));
		hosts = g_slist_prepend(hosts, addr);
	}

	return g_slist_reverse(hosts);
}

static void
purple_dnsquery_lookup_free(PurpleDnsQueryLookup *lookup)
{
	g_object_unref(lookup->cancellable);
	g_free(lookup);
}

static void
host_resolved(GObject *source, GAsyncResult *result, gpointer user_data)
{
	PurpleDnsQueryLookup *lookup = user_data;
	PurpleDnsQueryData *query_data = lookup->query_data;
	GList *addresses;
	GSList *hosts;
	GError *error = NULL;

	addresses = g_resolver_lookup_by_name_finish(G_RESOLVER(source), result,
			&error);

	if (query_data == NULL) {
		/* purple_dnsquery_destroy() was called while we were resolving. */
		g_resolver_free_addresses(addresses);
		g_clear_error(&error);
		purple_dnsquery_lookup_free(lookup);
		return;
	}

	query_data->lookup = NULL;
	purple_dnsquery_lookup_free(lookup);

	purple_debug_info("dns", "Got response for '%s'\n", query_data->hostname);

	hosts = addresses_to_hosts(addresses, query_data->port);
	g_resolver_free_addresses(addresses);

	if (hosts == NULL) {
		/* GResolver's message already names the host. */
		gchar *message = error ? g_strdup(error->message) :
				g_strdup_printf(_("Error resolving %s:\n%s"),
						query_data->hostname, _("Unknown reason"));
		g_clear_error(&error);
		purple_dnsquery_failed(query_data, message);
		g_free(message);
		return;
	}

	g_clear_error(&error);
	purple_dnsquery_resolved(query_data, hosts);
}

static void
resolve_host(PurpleDnsQueryData *query_data)
{
	GResolver *resolver;
	PurpleDnsQueryLookup *lookup;
	gchar *hostname;

	/*
	 * Keep using libpurple's own IDNA conversion so all of libpurple
	 * agrees on how a non-ASCII name maps to punycode.
	 */
	if (!dns_str_is_ascii(query_data->hostname)) {
		int ret = purple_network_convert_idn_to_ascii(query_data->hostname,
				&hostname);
		if (ret != 0) {
			gchar *message = g_strdup_printf(_("Error resolving %s: %d"),
					query_data->hostname, ret);
			purple_dnsquery_failed(query_data, message);
			g_free(message);
			return;
		}
	} else
		hostname = g_strdup(query_data->hostname);

	lookup = g_new0(PurpleDnsQueryLookup, 1);
	lookup->query_data = query_data;
	lookup->cancellable = g_cancellable_new();
	query_data->lookup = lookup;

	resolver = g_resolver_get_default();
	g_resolver_lookup_by_name_async(resolver, hostname, lookup->cancellable,
			host_resolved, lookup);
	g_object_unref(resolver);

	g_free(hostname);
}

static gboolean
initiate_resolving(gpointer data)
{
	PurpleDnsQueryData *query_data;
	PurpleProxyType proxy_type;

	query_data = data;
	query_data->timeout = 0;

	if (resolve_ip(query_data))
		/* resolve_ip calls purple_dnsquery_resolved */
		return FALSE;

	proxy_type = purple_proxy_info_get_type(
		purple_proxy_get_setup(query_data->account));
	if (proxy_type == PURPLE_PROXY_TOR) {
		purple_dnsquery_failed(query_data,
			_("Aborting DNS lookup in Tor Proxy mode."));
		return FALSE;
	}

	if (purple_dnsquery_ui_resolve(query_data))
		/* The UI is handling the resolve; we're done */
		return FALSE;

	resolve_host(query_data);

	return FALSE;
}

PurpleDnsQueryData *
purple_dnsquery_a_account(PurpleAccount *account, const char *hostname, int port,
				PurpleDnsQueryConnectFunction callback, gpointer data)
{
	PurpleDnsQueryData *query_data;

	g_return_val_if_fail(hostname != NULL, NULL);
	g_return_val_if_fail(port != 0, NULL);
	g_return_val_if_fail(callback != NULL, NULL);

	purple_debug_info("dnsquery", "Performing DNS lookup for %s\n", hostname);

	query_data = g_new0(PurpleDnsQueryData, 1);
	query_data->hostname = g_strdup(hostname);
	g_strstrip(query_data->hostname);
	query_data->port = port;
	query_data->callback = callback;
	query_data->data = data;
	query_data->account = account;

	if (*query_data->hostname == '\0')
	{
		purple_dnsquery_destroy(query_data);
		g_return_val_if_reached(NULL);
	}

	/* Always answer from the event loop, never from within this call. */
	query_data->timeout = purple_timeout_add(0, initiate_resolving, query_data);

	return query_data;
}

PurpleDnsQueryData *
purple_dnsquery_a(const char *hostname, int port,
				PurpleDnsQueryConnectFunction callback, gpointer data)
{
	return purple_dnsquery_a_account(NULL, hostname, port, callback, data);
}

void
purple_dnsquery_destroy(PurpleDnsQueryData *query_data)
{
	PurpleDnsQueryUiOps *ops = purple_dnsquery_get_ui_ops();

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

	if (query_data->timeout > 0)
		purple_timeout_remove(query_data->timeout);

	g_free(query_data->hostname);
	g_free(query_data);
}

char *
purple_dnsquery_get_host(PurpleDnsQueryData *query_data)
{
	g_return_val_if_fail(query_data != NULL, NULL);

	return query_data->hostname;
}

unsigned short
purple_dnsquery_get_port(PurpleDnsQueryData *query_data)
{
	g_return_val_if_fail(query_data != NULL, 0);

	return query_data->port;
}

void
purple_dnsquery_set_ui_ops(PurpleDnsQueryUiOps *ops)
{
	dns_query_ui_ops = ops;
}

PurpleDnsQueryUiOps *
purple_dnsquery_get_ui_ops(void)
{
	/* It is perfectly acceptable for dns_query_ui_ops to be NULL; this just
	 * means that the default GResolver implementation will be used.
	 */
	return dns_query_ui_ops;
}

void
purple_dnsquery_init(void)
{
}

void
purple_dnsquery_uninit(void)
{
	/*
	 * Nothing to tear down: in-flight lookups belong to their
	 * PurpleDnsQueryData and are cancelled by purple_dnsquery_destroy().
	 */
}
