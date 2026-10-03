/*
 * libstarter-configurable-service - reader.h
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

#ifndef STCS_READER_H_INCLUDED
#define STCS_READER_H_INCLUDED

#include <glib-object.h>
#include <gio/gio.h>

#include <libxml/tree.h>

#include "transport.h"
#include "datapoint.h"

G_BEGIN_DECLS

#define STCS_TYPE_READER (stcs_reader_get_type())
G_DECLARE_DERIVABLE_TYPE(STCSReader, stcs_reader, STCS, READER, GObject)

struct _STCSReaderClass {
    GObjectClass parent_class;

    /*
     * Parse plugin-specific config (including <map> entries).
     * Use lookup_datapoint to resolve datapoint ids, then stcs_reader_add_mapping().
     */
    gboolean (*configure) (STCSReader *self,
			   xmlNodePtr node,
			   STCSDatapointLookupFunc lookup_datapoint,
			   gpointer lookup_data,
			   GError **error);
    /* Parse payload and update datapoints via stcs_reader_push_value(). */
    gboolean (*parse) (STCSReader *self, GBytes *data, GError **error);

    gpointer padding[8];
};

gboolean stcs_reader_configure(STCSReader *self,
			       xmlNodePtr node,
			       STCSDatapointLookupFunc lookup_datapoint,
			       gpointer lookup_data,
			       GError **error);

/* Binds to a transport and connects data-ready / data-error (takes a reference). */
gboolean stcs_reader_set_transport(STCSReader *self, STCSTransport *transport, GError **error);
STCSTransport *stcs_reader_get_transport(STCSReader *self);

/*
 * Map a plugin-chosen opaque key to a datapoint (takes a reference on datapoint).
 * Multiple datapoints may share the same key (fan-out); the same key must be used
 * later with stcs_reader_push_value().
 */
void stcs_reader_add_mapping(STCSReader *self, const gchar *key, STCSDatapoint *datapoint);

/* Returns the first datapoint mapped to key, or NULL. Prefer foreach / push_value. */
STCSDatapoint *stcs_reader_lookup_mapping(STCSReader *self, const gchar *key);

/* Transfer-none list of datapoints for key, or NULL if missing. */
const GPtrArray *stcs_reader_lookup_mappings(STCSReader *self, const gchar *key);

typedef void (*STCSReaderMappingFunc)(const gchar *key,
				      STCSDatapoint *datapoint,
				      gpointer user_data);

/* Invokes func once per (key, datapoint) pair. */
void stcs_reader_foreach_mapping(STCSReader *self,
				 STCSReaderMappingFunc func,
				 gpointer user_data);

typedef void (*STCSReaderKeyFunc)(const gchar *key,
				  const GPtrArray *datapoints,
				  gpointer user_data);

/* Invokes func once per key with all datapoints for that key. */
void stcs_reader_foreach_key(STCSReader *self,
			     STCSReaderKeyFunc func,
			     gpointer user_data);

/* Push a raw value to every datapoint mapped by key (used from parse()). */
gboolean stcs_reader_push_value(STCSReader *self, const gchar *key, gdouble value, GError **error);

/*
 * Optional hook around parse(): entering=TRUE before parse, FALSE after (also on
 * parse failure). Used to mark feedback outputs as "read-syncing".
 */
typedef void (*STCSReaderSyncHook)(gboolean entering, gpointer user_data);
void stcs_reader_set_sync_hook(STCSReader *self,
			       STCSReaderSyncHook hook,
			       gpointer user_data);

G_END_DECLS

#endif /* STCS_READER_H_INCLUDED */
