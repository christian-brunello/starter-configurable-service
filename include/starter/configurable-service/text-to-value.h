/*
 * libstarter-configurable-service - text-to-value.h
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

#ifndef STCS_TEXT_TO_VALUE_H
#define STCS_TEXT_TO_VALUE_H

#include <glib.h>
#include <gio/gio.h>
#include <libxml/tree.h>

G_BEGIN_DECLS

typedef struct _STCSTextToValue STCSTextToValue;

typedef struct {
  const gchar *type;
  STCSTextToValue *(*create) (void);
  void (*destroy) (STCSTextToValue *self);
  gboolean (*configure) (STCSTextToValue *self, xmlNodePtr node, GError **error);
  gboolean (*to_value) (STCSTextToValue *self,
			const gchar *text,
			gdouble *out,
			GError **error);
} STCSTextToValueClass;

struct _STCSTextToValue {
  const STCSTextToValueClass *klass;
};

/*
 * Builds a converter from a <text-to-value type="..."> node.
 * Looks up @type in the plugin-local registry and calls configure() on the node.
 */
STCSTextToValue *stcs_text_to_value_new_from_node(xmlNodePtr node, GError **error);

void stcs_text_to_value_free(STCSTextToValue *self);

gboolean stcs_text_to_value_apply(STCSTextToValue *self,
				  const gchar *text,
				  gdouble *out,
				  GError **error);

G_END_DECLS

#endif /* STCS_TEXT_TO_VALUE_H */
