/*
 * libstarter-configurable-service - transport.h
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

#ifndef STCS_TRANSPORT_H_INCLUDED
#define STCS_TRANSPORT_H_INCLUDED

#include <glib-object.h>
#include <gio/gio.h>

#include <libxml/tree.h>

G_BEGIN_DECLS

#define STCS_TYPE_TRANSPORT (stcs_transport_get_type())
G_DECLARE_DERIVABLE_TYPE(STCSTransport, stcs_transport, STCS, TRANSPORT, GObject)

struct _STCSTransportClass {
    GObjectClass parent_class;

    gboolean (*configure) (STCSTransport *self, xmlNodePtr node, GError **error);

    /*
     * Start one write (called by the base class with at most one in flight).
     * Must eventually call stcs_transport_finish_write() or
     * stcs_transport_finish_write_error() with the same @op_id.
     * @data is valid until finish_* is called (base holds a reference).
     */
    void (*start_write) (STCSTransport *self, guint64 op_id, GBytes *data);

    gpointer padding[8];
};

gboolean stcs_transport_configure(STCSTransport *self, xmlNodePtr node, GError **error);

/*
 * Enqueue a write (queue depth 1: a newer write replaces a pending one).
 * Returns op id (>0) on accept, or 0 and sets @error.
 */
guint64 stcs_transport_write(STCSTransport *self, GBytes *data, GError **error);

gboolean stcs_transport_is_syncing(STCSTransport *self);
gboolean stcs_transport_get_write_failed(STCSTransport *self);

/* Called by transport plugins when an in-flight write completes. */
void stcs_transport_finish_write(STCSTransport *self, guint64 op_id, gsize nbytes);
void stcs_transport_finish_write_error(STCSTransport *self, guint64 op_id, GError *error);

/* Emitters for subclasses / plugins (transfer none for data/error during handlers). */
void stcs_transport_emit_data_ready(STCSTransport *self, GBytes *data);
void stcs_transport_emit_data_error(STCSTransport *self, GError *error);

G_END_DECLS

#endif /* STCS_TRANSPORT_H_INCLUDED */
