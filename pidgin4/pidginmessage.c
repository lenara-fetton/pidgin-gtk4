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

#include "util.h"

#include "pidginmessage.h"

struct _PidginMessage
{
	GObject parent;

	PidginMessageKind kind;
	char *sender;
	char *alias;
	PurpleMessageFlags flags;
	time_t when;
	char *html;

	PidginMarkupOptions options;
	char *protocol_sml;
	PidginMarkupResult *markup;

	char *stanza_id;
	char *origin_id;
	char *server_id;
	char *occupant_id;
	char *correction_of;
	char *reply_to;
	char *reply_to_sender;
	char *reply_preview;

	gboolean edited;
	GPtrArray *history;

	PidginReceiptState receipt;
	GHashTable *reactions;          /* emoji -> GList of senders */
	GList *reaction_order;          /* emoji, owned by the table keys */
	gboolean retracted;
	gint64 index_id;

	GPtrArray *css_classes;         /* M7: extra row classes (plugins) */
	PidginAttachment *attachment;   /* shown under the text */
};

enum {
	PROP_0,
	PROP_KIND,
	PROP_SENDER,
	PROP_ALIAS,
	PROP_FLAGS,
	PROP_TIME,
	PROP_HTML,
	PROP_STANZA_ID,
	PROP_ORIGIN_ID,
	PROP_SERVER_ID,
	PROP_OCCUPANT_ID,
	PROP_CORRECTION_OF,
	PROP_REPLY_TO,
	PROP_REPLY_TO_SENDER,
	PROP_REPLY_PREVIEW,
	PROP_EDITED,
	PROP_RECEIPT,
	PROP_RETRACTED,
	PROP_INDEX_ID,
	PROP_CSS_CLASSES,
	PROP_ATTACHMENT,
	N_PROPS
};

enum {
	SIG_REACTIONS_CHANGED,
	N_SIGNALS
};

static GParamSpec *props[N_PROPS];
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(PidginMessage, pidgin_message, G_TYPE_OBJECT)

static void
set_string(PidginMessage *msg, char **field, const char *value, int prop)
{
	if (g_strcmp0(*field, value) == 0)
		return;
	g_free(*field);
	*field = g_strdup(value);
	g_object_notify_by_pspec(G_OBJECT(msg), props[prop]);
}

static void
pidgin_message_get_property(GObject *obj, guint prop_id, GValue *value,
                            GParamSpec *pspec)
{
	PidginMessage *msg = PIDGIN_MESSAGE(obj);

	switch (prop_id) {
		case PROP_KIND: g_value_set_int(value, msg->kind); break;
		case PROP_SENDER: g_value_set_string(value, msg->sender); break;
		case PROP_ALIAS: g_value_set_string(value, pidgin_message_get_alias(msg)); break;
		case PROP_FLAGS: g_value_set_uint(value, msg->flags); break;
		case PROP_TIME: g_value_set_int64(value, msg->when); break;
		case PROP_HTML: g_value_set_string(value, msg->html); break;
		case PROP_ATTACHMENT: g_value_set_object(value, msg->attachment); break;
		case PROP_STANZA_ID: g_value_set_string(value, msg->stanza_id); break;
		case PROP_ORIGIN_ID: g_value_set_string(value, msg->origin_id); break;
		case PROP_SERVER_ID: g_value_set_string(value, msg->server_id); break;
		case PROP_OCCUPANT_ID: g_value_set_string(value, msg->occupant_id); break;
		case PROP_CORRECTION_OF: g_value_set_string(value, msg->correction_of); break;
		case PROP_REPLY_TO: g_value_set_string(value, msg->reply_to); break;
		case PROP_REPLY_TO_SENDER: g_value_set_string(value, msg->reply_to_sender); break;
		case PROP_REPLY_PREVIEW: g_value_set_string(value, msg->reply_preview); break;
		case PROP_EDITED: g_value_set_boolean(value, msg->edited); break;
		case PROP_RECEIPT: g_value_set_int(value, msg->receipt); break;
		case PROP_RETRACTED: g_value_set_boolean(value, msg->retracted); break;
		case PROP_INDEX_ID: g_value_set_int64(value, msg->index_id); break;
		case PROP_CSS_CLASSES:
			g_value_set_boxed(value, msg->css_classes ? msg->css_classes->pdata : NULL);
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
	}
}

static void
pidgin_message_set_property(GObject *obj, guint prop_id, const GValue *value,
                            GParamSpec *pspec)
{
	PidginMessage *msg = PIDGIN_MESSAGE(obj);

	switch (prop_id) {
		case PROP_ALIAS: pidgin_message_set_alias(msg, g_value_get_string(value)); break;
		case PROP_FLAGS: pidgin_message_set_flags(msg, g_value_get_uint(value)); break;
		case PROP_HTML: pidgin_message_set_html(msg, g_value_get_string(value)); break;
		case PROP_ATTACHMENT: pidgin_message_set_attachment(msg, g_value_get_object(value)); break;
		case PROP_STANZA_ID: pidgin_message_set_stanza_id(msg, g_value_get_string(value)); break;
		case PROP_ORIGIN_ID: pidgin_message_set_origin_id(msg, g_value_get_string(value)); break;
		case PROP_SERVER_ID: pidgin_message_set_server_id(msg, g_value_get_string(value)); break;
		case PROP_OCCUPANT_ID: pidgin_message_set_occupant_id(msg, g_value_get_string(value)); break;
		case PROP_CORRECTION_OF: pidgin_message_set_correction_of(msg, g_value_get_string(value)); break;
		case PROP_RECEIPT: pidgin_message_set_receipt(msg, g_value_get_int(value)); break;
		case PROP_RETRACTED: pidgin_message_set_retracted(msg, g_value_get_boolean(value)); break;
		case PROP_INDEX_ID: pidgin_message_set_index_id(msg, g_value_get_int64(value)); break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
	}
}

static void
free_senders(gpointer data)
{
	g_list_free_full(data, g_free);
}

static void
pidgin_message_finalize(GObject *obj)
{
	PidginMessage *msg = PIDGIN_MESSAGE(obj);

	g_free(msg->sender);
	g_free(msg->alias);
	g_free(msg->html);
	g_free(msg->protocol_sml);
	g_clear_pointer(&msg->markup, pidgin_markup_result_unref);
	g_free(msg->stanza_id);
	g_free(msg->origin_id);
	g_free(msg->server_id);
	g_free(msg->occupant_id);
	g_free(msg->correction_of);
	g_free(msg->reply_to);
	g_free(msg->reply_to_sender);
	g_free(msg->reply_preview);
	g_clear_pointer(&msg->history, g_ptr_array_unref);
	g_list_free(msg->reaction_order);
	g_clear_pointer(&msg->reactions, g_hash_table_destroy);
	g_clear_pointer(&msg->css_classes, g_ptr_array_unref);
	g_clear_object(&msg->attachment);

	G_OBJECT_CLASS(pidgin_message_parent_class)->finalize(obj);
}

static void
pidgin_message_class_init(PidginMessageClass *klass)
{
	GObjectClass *obj_class = G_OBJECT_CLASS(klass);
	GParamFlags rw = G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS;
	GParamFlags ro = G_PARAM_READABLE | G_PARAM_STATIC_STRINGS;

	obj_class->get_property = pidgin_message_get_property;
	obj_class->set_property = pidgin_message_set_property;
	obj_class->finalize = pidgin_message_finalize;

	props[PROP_KIND] = g_param_spec_int("kind", NULL, NULL, 0, 1, 0, ro);
	props[PROP_SENDER] = g_param_spec_string("sender", NULL, NULL, NULL, ro);
	props[PROP_ALIAS] = g_param_spec_string("alias", NULL, NULL, NULL, rw);
	props[PROP_FLAGS] = g_param_spec_uint("flags", NULL, NULL, 0, G_MAXUINT, 0, rw);
	props[PROP_TIME] = g_param_spec_int64("time", NULL, NULL, G_MININT64, G_MAXINT64, 0, ro);
	props[PROP_HTML] = g_param_spec_string("html", NULL, NULL, NULL, rw);
	props[PROP_ATTACHMENT] = g_param_spec_object("attachment", NULL, NULL,
		PIDGIN_TYPE_ATTACHMENT, rw);
	props[PROP_STANZA_ID] = g_param_spec_string("stanza-id", NULL, NULL, NULL, rw);
	props[PROP_ORIGIN_ID] = g_param_spec_string("origin-id", NULL, NULL, NULL, rw);
	props[PROP_SERVER_ID] = g_param_spec_string("server-id", NULL, NULL, NULL, rw);
	props[PROP_OCCUPANT_ID] = g_param_spec_string("occupant-id", NULL, NULL, NULL, rw);
	props[PROP_CORRECTION_OF] = g_param_spec_string("correction-of", NULL, NULL, NULL, rw);
	props[PROP_REPLY_TO] = g_param_spec_string("reply-to", NULL, NULL, NULL, ro);
	props[PROP_REPLY_TO_SENDER] = g_param_spec_string("reply-to-sender", NULL, NULL, NULL, ro);
	props[PROP_REPLY_PREVIEW] = g_param_spec_string("reply-preview", NULL, NULL, NULL, ro);
	props[PROP_EDITED] = g_param_spec_boolean("edited", NULL, NULL, FALSE, ro);
	props[PROP_RECEIPT] = g_param_spec_int("receipt", NULL, NULL, 0, PIDGIN_RECEIPT_DISPLAYED,
	                                       0, rw);
	props[PROP_RETRACTED] = g_param_spec_boolean("retracted", NULL, NULL, FALSE, rw);
	props[PROP_INDEX_ID] = g_param_spec_int64("index-id", NULL, NULL, 0, G_MAXINT64, 0, rw);
	props[PROP_CSS_CLASSES] = g_param_spec_boxed("css-classes", NULL, NULL, G_TYPE_STRV, ro);
	g_object_class_install_properties(obj_class, N_PROPS, props);

	/**
	 * PidginMessage::reactions-changed:
	 *
	 * A reaction was added or removed.
	 */
	signals[SIG_REACTIONS_CHANGED] = g_signal_new("reactions-changed",
		G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 0);
}

static void
pidgin_message_init(PidginMessage *msg)
{
	msg->reactions = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                       free_senders);
}

PidginMessage *
pidgin_message_new(const char *sender, const char *alias, const char *html,
                   PurpleMessageFlags flags, time_t when)
{
	PidginMessage *msg = g_object_new(PIDGIN_TYPE_MESSAGE, NULL);

	msg->sender = g_strdup(sender);
	msg->alias = g_strdup(alias);
	msg->html = g_strdup(html ? html : "");
	msg->flags = flags;
	msg->when = when;
	return msg;
}

PidginMessage *
pidgin_message_new_marker(void)
{
	PidginMessage *msg = pidgin_message_new(NULL, NULL, "", 0, time(NULL));

	msg->kind = PIDGIN_MESSAGE_KIND_MARKER;
	return msg;
}

void
pidgin_message_apply_meta(PidginMessage *msg, GHashTable *meta)
{
	const char *v;

	g_return_if_fail(PIDGIN_IS_MESSAGE(msg));
	if (meta == NULL)
		return;

	g_object_freeze_notify(G_OBJECT(msg));
	if ((v = g_hash_table_lookup(meta, "stanza-id")) != NULL)
		pidgin_message_set_stanza_id(msg, v);
	if ((v = g_hash_table_lookup(meta, "origin-id")) != NULL)
		pidgin_message_set_origin_id(msg, v);
	if ((v = g_hash_table_lookup(meta, "server-id")) != NULL)
		pidgin_message_set_server_id(msg, v);
	if ((v = g_hash_table_lookup(meta, "occupant-id")) != NULL)
		pidgin_message_set_occupant_id(msg, v);
	if ((v = g_hash_table_lookup(meta, "correction-of")) != NULL)
		pidgin_message_set_correction_of(msg, v);
	if ((v = g_hash_table_lookup(meta, "reply-to")) != NULL)
		pidgin_message_set_reply(msg, v, g_hash_table_lookup(meta, "reply-to-sender"),
		                         msg->reply_preview);
	/* A described file share (XEP-0447/0385): its card, at once, in place
	 * of what the body URL would have given. The text stays. */
	{
		PidginAttachment *share = pidgin_attachment_new_for_share(meta);

		if (share != NULL) {
			pidgin_message_set_attachment(msg, share);
			g_object_unref(share);
		}
	}
	g_object_thaw_notify(G_OBJECT(msg));
}

PidginMessageKind
pidgin_message_get_kind(PidginMessage *msg)
{
	g_return_val_if_fail(PIDGIN_IS_MESSAGE(msg), 0);
	return msg->kind;
}

const char *
pidgin_message_get_sender(PidginMessage *msg)
{
	g_return_val_if_fail(PIDGIN_IS_MESSAGE(msg), NULL);
	return msg->sender;
}

const char *
pidgin_message_get_alias(PidginMessage *msg)
{
	g_return_val_if_fail(PIDGIN_IS_MESSAGE(msg), NULL);
	return (msg->alias && *msg->alias) ? msg->alias : msg->sender;
}

void
pidgin_message_set_alias(PidginMessage *msg, const char *alias)
{
	g_return_if_fail(PIDGIN_IS_MESSAGE(msg));
	set_string(msg, &msg->alias, alias, PROP_ALIAS);
}

PurpleMessageFlags
pidgin_message_get_flags(PidginMessage *msg)
{
	g_return_val_if_fail(PIDGIN_IS_MESSAGE(msg), 0);
	return msg->flags;
}

void
pidgin_message_set_flags(PidginMessage *msg, PurpleMessageFlags flags)
{
	g_return_if_fail(PIDGIN_IS_MESSAGE(msg));
	if (msg->flags == flags)
		return;
	msg->flags = flags;
	g_object_notify_by_pspec(G_OBJECT(msg), props[PROP_FLAGS]);
}

time_t
pidgin_message_get_time(PidginMessage *msg)
{
	g_return_val_if_fail(PIDGIN_IS_MESSAGE(msg), 0);
	return msg->when;
}

const char *
pidgin_message_get_html(PidginMessage *msg)
{
	g_return_val_if_fail(PIDGIN_IS_MESSAGE(msg), NULL);
	return msg->html;
}

void
pidgin_message_set_html(PidginMessage *msg, const char *html)
{
	g_return_if_fail(PIDGIN_IS_MESSAGE(msg));
	if (g_strcmp0(msg->html, html) == 0)
		return;
	g_clear_pointer(&msg->markup, pidgin_markup_result_unref);
	set_string(msg, &msg->html, html ? html : "", PROP_HTML);
}

PidginAttachment *
pidgin_message_get_attachment(PidginMessage *msg)
{
	g_return_val_if_fail(PIDGIN_IS_MESSAGE(msg), NULL);
	return msg->attachment;
}

void
pidgin_message_set_attachment(PidginMessage *msg, PidginAttachment *attachment)
{
	g_return_if_fail(PIDGIN_IS_MESSAGE(msg));
	if (g_set_object(&msg->attachment, attachment))
		g_object_notify_by_pspec(G_OBJECT(msg), props[PROP_ATTACHMENT]);
}

void
pidgin_message_set_parse_options(PidginMessage *msg, const PidginMarkupOptions *options)
{
	g_return_if_fail(PIDGIN_IS_MESSAGE(msg));

	g_free(msg->protocol_sml);
	if (options != NULL) {
		msg->options = *options;
		msg->protocol_sml = g_strdup(options->protocol_sml);
	} else {
		memset(&msg->options, 0, sizeof(msg->options));
		msg->protocol_sml = NULL;
	}
	msg->options.protocol_sml = msg->protocol_sml;
	if (msg->markup != NULL) {
		g_clear_pointer(&msg->markup, pidgin_markup_result_unref);
		g_object_notify_by_pspec(G_OBJECT(msg), props[PROP_HTML]);
	}
}

PidginMarkupResult *
pidgin_message_get_markup(PidginMessage *msg)
{
	g_return_val_if_fail(PIDGIN_IS_MESSAGE(msg), NULL);

	if (msg->markup == NULL) {
		PidginMarkupOptions opts = msg->options;

		/* Newlines in bodies are line breaks as Pidgin 2 wrote them
		 * (gtkconv converts them), except for XHTML-ish bodies. */
		if (msg->flags & PURPLE_MESSAGE_NO_LINKIFY)
			opts.flags |= PIDGIN_MARKUP_NO_LINKIFY;
		if (msg->flags & PURPLE_MESSAGE_RAW)
			opts.flags |= PIDGIN_MARKUP_NO_SMILEYS;
		msg->markup = pidgin_markup_parse_html(msg->html, &opts);
	}
	return msg->markup;
}

const char *
pidgin_message_get_plain_text(PidginMessage *msg)
{
	return pidgin_message_get_markup(msg)->text;
}

#define STRING_ACCESSORS(field, prop) \
const char * \
pidgin_message_get_##field(PidginMessage *msg) \
{ \
	g_return_val_if_fail(PIDGIN_IS_MESSAGE(msg), NULL); \
	return msg->field; \
} \
void \
pidgin_message_set_##field(PidginMessage *msg, const char *value) \
{ \
	g_return_if_fail(PIDGIN_IS_MESSAGE(msg)); \
	set_string(msg, &msg->field, value, prop); \
}

STRING_ACCESSORS(stanza_id, PROP_STANZA_ID)
STRING_ACCESSORS(origin_id, PROP_ORIGIN_ID)
STRING_ACCESSORS(server_id, PROP_SERVER_ID)
STRING_ACCESSORS(occupant_id, PROP_OCCUPANT_ID)
STRING_ACCESSORS(correction_of, PROP_CORRECTION_OF)

gboolean
pidgin_message_has_id(PidginMessage *msg, const char *id)
{
	g_return_val_if_fail(PIDGIN_IS_MESSAGE(msg), FALSE);

	if (id == NULL || *id == '\0')
		return FALSE;
	return purple_strequal(msg->stanza_id, id) ||
	       purple_strequal(msg->origin_id, id) ||
	       purple_strequal(msg->server_id, id);
}

const char *
pidgin_message_get_reply_to(PidginMessage *msg)
{
	g_return_val_if_fail(PIDGIN_IS_MESSAGE(msg), NULL);
	return msg->reply_to;
}

const char *
pidgin_message_get_reply_to_sender(PidginMessage *msg)
{
	g_return_val_if_fail(PIDGIN_IS_MESSAGE(msg), NULL);
	return msg->reply_to_sender;
}

const char *
pidgin_message_get_reply_preview(PidginMessage *msg)
{
	g_return_val_if_fail(PIDGIN_IS_MESSAGE(msg), NULL);
	return msg->reply_preview;
}

void
pidgin_message_set_reply(PidginMessage *msg, const char *id, const char *sender,
                         const char *preview)
{
	char *p;

	g_return_if_fail(PIDGIN_IS_MESSAGE(msg));

	p = g_strdup(preview);        /* may alias msg->reply_preview */
	g_object_freeze_notify(G_OBJECT(msg));
	set_string(msg, &msg->reply_to, id, PROP_REPLY_TO);
	set_string(msg, &msg->reply_to_sender, sender, PROP_REPLY_TO_SENDER);
	set_string(msg, &msg->reply_preview, p, PROP_REPLY_PREVIEW);
	g_object_thaw_notify(G_OBJECT(msg));
	g_free(p);
}

void
pidgin_message_apply_correction(PidginMessage *msg, const char *new_html,
                                const char *new_id)
{
	g_return_if_fail(PIDGIN_IS_MESSAGE(msg));

	if (msg->history == NULL)
		msg->history = g_ptr_array_new_with_free_func(g_free);
	g_ptr_array_add(msg->history, g_strdup(msg->html));

	g_object_freeze_notify(G_OBJECT(msg));
	pidgin_message_set_html(msg, new_html);
	if (!msg->edited) {
		msg->edited = TRUE;
		g_object_notify_by_pspec(G_OBJECT(msg), props[PROP_EDITED]);
	}
	/* 1:1 XEP-0308 corrections refer to the first message's id, and
	 * Discord keeps its id; remember the newest id too (as origin-id if
	 * none) so a further correction referring to it still matches. */
	if (new_id != NULL && !pidgin_message_has_id(msg, new_id) && msg->origin_id == NULL)
		pidgin_message_set_origin_id(msg, new_id);
	g_object_thaw_notify(G_OBJECT(msg));
}

gboolean
pidgin_message_get_edited(PidginMessage *msg)
{
	g_return_val_if_fail(PIDGIN_IS_MESSAGE(msg), FALSE);
	return msg->edited;
}

GPtrArray *
pidgin_message_get_history(PidginMessage *msg)
{
	g_return_val_if_fail(PIDGIN_IS_MESSAGE(msg), NULL);
	return msg->history;
}

PidginReceiptState
pidgin_message_get_receipt(PidginMessage *msg)
{
	g_return_val_if_fail(PIDGIN_IS_MESSAGE(msg), PIDGIN_RECEIPT_NONE);
	return msg->receipt;
}

void
pidgin_message_set_receipt(PidginMessage *msg, PidginReceiptState state)
{
	g_return_if_fail(PIDGIN_IS_MESSAGE(msg));
	if (state <= msg->receipt)
		return;
	msg->receipt = state;
	g_object_notify_by_pspec(G_OBJECT(msg), props[PROP_RECEIPT]);
}

PidginReceiptState
pidgin_receipt_state_from_string(const char *state)
{
	if (purple_strequal(state, "displayed") || purple_strequal(state, "read"))
		return PIDGIN_RECEIPT_DISPLAYED;
	if (purple_strequal(state, "delivered") || purple_strequal(state, "received"))
		return PIDGIN_RECEIPT_DELIVERED;
	if (purple_strequal(state, "sent"))
		return PIDGIN_RECEIPT_SENT;
	return PIDGIN_RECEIPT_NONE;
}

GHashTable *
pidgin_message_get_reactions(PidginMessage *msg)
{
	g_return_val_if_fail(PIDGIN_IS_MESSAGE(msg), NULL);
	return msg->reactions;
}

gboolean
pidgin_message_has_reaction(PidginMessage *msg, const char *emoji, const char *sender)
{
	GList *senders;

	g_return_val_if_fail(PIDGIN_IS_MESSAGE(msg), FALSE);
	g_return_val_if_fail(emoji != NULL, FALSE);

	senders = g_hash_table_lookup(msg->reactions, emoji);
	if (sender == NULL)
		return senders != NULL;
	return g_list_find_custom(senders, sender, (GCompareFunc)g_strcmp0) != NULL;
}

gboolean
pidgin_message_add_reaction(PidginMessage *msg, const char *emoji, const char *sender)
{
	GList *senders;
	char *key = NULL;

	g_return_val_if_fail(PIDGIN_IS_MESSAGE(msg), FALSE);
	g_return_val_if_fail(emoji != NULL && sender != NULL, FALSE);

	if (pidgin_message_has_reaction(msg, emoji, sender))
		return FALSE;

	if (!g_hash_table_lookup_extended(msg->reactions, emoji, (gpointer *)&key,
	                                  (gpointer *)&senders)) {
		key = g_strdup(emoji);
		senders = NULL;
		msg->reaction_order = g_list_append(msg->reaction_order, key);
	}
	g_hash_table_steal(msg->reactions, key);
	senders = g_list_append(senders, g_strdup(sender));
	g_hash_table_insert(msg->reactions, key, senders);

	g_signal_emit(msg, signals[SIG_REACTIONS_CHANGED], 0);
	return TRUE;
}

gboolean
pidgin_message_remove_reaction(PidginMessage *msg, const char *emoji, const char *sender)
{
	GList *senders, *link;
	char *key;

	g_return_val_if_fail(PIDGIN_IS_MESSAGE(msg), FALSE);
	g_return_val_if_fail(emoji != NULL && sender != NULL, FALSE);

	if (!g_hash_table_lookup_extended(msg->reactions, emoji, (gpointer *)&key,
	                                  (gpointer *)&senders))
		return FALSE;
	link = g_list_find_custom(senders, sender, (GCompareFunc)g_strcmp0);
	if (link == NULL)
		return FALSE;

	g_hash_table_steal(msg->reactions, key);
	g_free(link->data);
	senders = g_list_delete_link(senders, link);
	if (senders == NULL) {
		msg->reaction_order = g_list_remove(msg->reaction_order, key);
		g_free(key);
	} else {
		g_hash_table_insert(msg->reactions, key, senders);
	}

	g_signal_emit(msg, signals[SIG_REACTIONS_CHANGED], 0);
	return TRUE;
}

GList *
pidgin_message_get_reaction_emojis(PidginMessage *msg)
{
	g_return_val_if_fail(PIDGIN_IS_MESSAGE(msg), NULL);
	return g_list_copy(msg->reaction_order);
}

gboolean
pidgin_message_get_retracted(PidginMessage *msg)
{
	g_return_val_if_fail(PIDGIN_IS_MESSAGE(msg), FALSE);
	return msg->retracted;
}

void
pidgin_message_set_retracted(PidginMessage *msg, gboolean retracted)
{
	g_return_if_fail(PIDGIN_IS_MESSAGE(msg));
	if (msg->retracted == retracted)
		return;
	msg->retracted = retracted;
	g_object_notify_by_pspec(G_OBJECT(msg), props[PROP_RETRACTED]);
}

gint64
pidgin_message_get_index_id(PidginMessage *msg)
{
	g_return_val_if_fail(PIDGIN_IS_MESSAGE(msg), 0);
	return msg->index_id;
}

void
pidgin_message_set_index_id(PidginMessage *msg, gint64 id)
{
	g_return_if_fail(PIDGIN_IS_MESSAGE(msg));
	if (msg->index_id == id)
		return;
	msg->index_id = id;
	g_object_notify_by_pspec(G_OBJECT(msg), props[PROP_INDEX_ID]);
}

/* ---- M7: extra CSS classes for the row (plugins) ---- */

void
pidgin_message_add_css_class(PidginMessage *msg, const char *css_class)
{
	g_return_if_fail(PIDGIN_IS_MESSAGE(msg));
	g_return_if_fail(css_class != NULL && *css_class != '\0');

	if (pidgin_message_has_css_class(msg, css_class))
		return;
	if (msg->css_classes == NULL) {
		msg->css_classes = g_ptr_array_new_with_free_func(g_free);
		g_ptr_array_add(msg->css_classes, NULL);
	}
	/* keep it NULL-terminated */
	msg->css_classes->pdata[msg->css_classes->len - 1] = g_strdup(css_class);
	g_ptr_array_add(msg->css_classes, NULL);
	g_object_notify_by_pspec(G_OBJECT(msg), props[PROP_CSS_CLASSES]);
}

void
pidgin_message_remove_css_class(PidginMessage *msg, const char *css_class)
{
	guint i;

	g_return_if_fail(PIDGIN_IS_MESSAGE(msg));

	if (msg->css_classes == NULL || css_class == NULL)
		return;
	for (i = 0; i + 1 < msg->css_classes->len; i++) {
		if (g_str_equal(msg->css_classes->pdata[i], css_class)) {
			g_ptr_array_remove_index(msg->css_classes, i);
			g_object_notify_by_pspec(G_OBJECT(msg), props[PROP_CSS_CLASSES]);
			return;
		}
	}
}

gboolean
pidgin_message_has_css_class(PidginMessage *msg, const char *css_class)
{
	guint i;

	g_return_val_if_fail(PIDGIN_IS_MESSAGE(msg), FALSE);

	if (msg->css_classes == NULL || css_class == NULL)
		return FALSE;
	for (i = 0; i + 1 < msg->css_classes->len; i++)
		if (g_str_equal(msg->css_classes->pdata[i], css_class))
			return TRUE;
	return FALSE;
}

const char * const *
pidgin_message_get_css_classes(PidginMessage *msg)
{
	g_return_val_if_fail(PIDGIN_IS_MESSAGE(msg), NULL);

	return msg->css_classes ? (const char * const *)msg->css_classes->pdata : NULL;
}
