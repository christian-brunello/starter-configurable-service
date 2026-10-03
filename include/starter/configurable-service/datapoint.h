/*
 * libstarter-configurable-service - datapoint.h
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

#ifndef STCS_DATAPOINT_H_INCLUDED
#define STCS_DATAPOINT_H_INCLUDED

#include <glib-object.h>

#include "transform.h"

G_BEGIN_DECLS

#define STCS_TYPE_DATAPOINT (stcs_datapoint_get_type())
G_DECLARE_FINAL_TYPE(STCSDatapoint, stcs_datapoint, STCS, DATAPOINT, GObject)

/* Resolves a datapoint id during reader/writer configure(). */
typedef STCSDatapoint *(*STCSDatapointLookupFunc)(const gchar *id, gpointer user_data);

STCSDatapoint *stcs_datapoint_new(const gchar *id);

const gchar *stcs_datapoint_get_id(STCSDatapoint *self);

/* Returns FALSE if no value has been pushed yet. */
gboolean stcs_datapoint_get_value(STCSDatapoint *self, gdouble *out);

/* Appends a transform to the pipeline (takes a reference). */
void stcs_datapoint_add_transform(STCSDatapoint *self, STCSTransform *transform);

/*
 * Pushes a raw value through the transform pipeline.
 * Emits "value-changed" only when the resulting value differs from the previous one.
 */
gboolean stcs_datapoint_push(STCSDatapoint *self, gdouble raw, GError **error);

G_END_DECLS

#endif /* STCS_DATAPOINT_H_INCLUDED */
