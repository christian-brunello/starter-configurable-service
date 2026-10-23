/*
 * libstarter-configurable-service - writer.h
 *
 * Copyright (C) 2026 Christian Brunello <brncrs@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef STCS_WRITER_H_INCLUDED
#define STCS_WRITER_H_INCLUDED

#include <glib-object.h>
#include <gio/gio.h>

#include <libxml/tree.h>

#include "transport.h"
#include "datapoint.h"

G_BEGIN_DECLS

#define STCS_TYPE_WRITER (stcs_writer_get_type())
G_DECLARE_DERIVABLE_TYPE(STCSWriter, stcs_writer, STCS, WRITER, GObject)

struct _STCSWriterClass {
    GObjectClass parent_class;

    /*
     * Parse plugin-specific config (including <map> entries).
     * Use lookup_datapoint to resolve datapoint ids, then stcs_writer_add_mapping().
     */
    gboolean (*configure) (STCSWriter *self,
			   xmlNodePtr node,
			   STCSDatapointLookupFunc lookup_datapoint,
			   gpointer lookup_data,
			   GError **error);
    /*
     * Called when a mapped datapoint emits value-changed.
     * Plugin updates its template/cache and writes via the transport.
     * @key is the opaque key registered via stcs_writer_add_mapping().
     */
    gboolean (*write_value) (STCSWriter *self,
			     const gchar *key,
			     gdouble value,
			     GError **error);

    gpointer padding[8];
};

gboolean stcs_writer_configure(STCSWriter *self,
			       xmlNodePtr node,
			       STCSDatapointLookupFunc lookup_datapoint,
			       gpointer lookup_data,
			       GError **error);

/* Binds to a transport (takes a reference). Connects write-complete/write-error. */
gboolean stcs_writer_set_transport(STCSWriter *self, STCSTransport *transport, GError **error);
STCSTransport *stcs_writer_get_transport(STCSWriter *self);

/*
 * Total write attempts per payload (1 = no retry). Default 1.
 * On write-error the same bytes are re-enqueued until this limit.
 */
void stcs_writer_set_write_attempts(STCSWriter *self, guint attempts);
guint stcs_writer_get_write_attempts(STCSWriter *self);

/*
 * Base delay before a write retry. Default 0 (immediate re-enqueue).
 * Delay before the next attempt is min(60s, backoff_ms * 2^(failed_attempt-1)).
 */
void stcs_writer_set_write_retry_backoff_ms(STCSWriter *self, guint backoff_ms);
guint stcs_writer_get_write_retry_backoff_ms(STCSWriter *self);

/*
 * Map a plugin-chosen opaque key to a datapoint (takes a reference).
 * Multiple datapoints may share the same key; each gets its own value-changed
 * handler that forwards to write_value() with that key.
 */
void stcs_writer_add_mapping(STCSWriter *self, const gchar *key, STCSDatapoint *datapoint);

/* Returns the first datapoint mapped to key, or NULL. */
STCSDatapoint *stcs_writer_lookup_mapping(STCSWriter *self, const gchar *key);

typedef void (*STCSWriterMappingFunc)(const gchar *key,
				      STCSDatapoint *datapoint,
				      gpointer user_data);

/* Invokes func once per (key, datapoint) pair. */
void stcs_writer_foreach_mapping(STCSWriter *self,
				 STCSWriterMappingFunc func,
				 gpointer user_data);

/* Convenience: enqueue a buffer write through the bound transport (async). */
gboolean stcs_writer_write_bytes(STCSWriter *self, GBytes *data, GError **error);

/*
 * Optional ack for command datapoints: called with the datapoint id that
 * triggered the write when the transport write completes or fails for good.
 */
typedef void (*STCSWriterAckHook)(const gchar *datapoint_id,
				  gboolean success,
				  gpointer user_data);
void stcs_writer_set_ack_hook(STCSWriter *self,
			      STCSWriterAckHook hook,
			      gpointer user_data);

G_END_DECLS

#endif /* STCS_WRITER_H_INCLUDED */
