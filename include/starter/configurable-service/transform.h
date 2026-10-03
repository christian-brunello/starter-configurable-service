/*
 * libstarter-configurable-service - transform.h
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

#ifndef STCS_TRANSFORM_H_INCLUDED
#define STCS_TRANSFORM_H_INCLUDED

#include <glib-object.h>
#include <gio/gio.h>

#include <libxml/tree.h>

G_BEGIN_DECLS

#define STCS_TYPE_TRANSFORM (stcs_transform_get_type())
G_DECLARE_DERIVABLE_TYPE(STCSTransform, stcs_transform, STCS, TRANSFORM, GObject)

struct _STCSTransformClass {
    GObjectClass parent_class;

    gboolean (*configure) (STCSTransform *self, xmlNodePtr node, GError **error);
    gboolean (*process) (STCSTransform *self, gdouble in, gdouble *out, GError **error);

    gpointer padding[8];
};

gboolean stcs_transform_configure(STCSTransform *self, xmlNodePtr node, GError **error);
gboolean stcs_transform_process(STCSTransform *self, gdouble in, gdouble *out, GError **error);

G_END_DECLS

#endif /* STCS_TRANSFORM_H_INCLUDED */
