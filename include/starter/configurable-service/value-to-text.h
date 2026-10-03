/*
 * libstarter-configurable-service - value-to-text.h
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

#ifndef STCS_VALUE_TO_TEXT_H
#define STCS_VALUE_TO_TEXT_H

#include <glib.h>
#include <gio/gio.h>
#include <libxml/tree.h>

G_BEGIN_DECLS

typedef struct _STCSValueToText STCSValueToText;

typedef struct {
  const gchar *type;
  STCSValueToText *(*create) (void);
  void (*destroy) (STCSValueToText *self);
  gboolean (*configure) (STCSValueToText *self, xmlNodePtr node, GError **error);
  gboolean (*to_text) (STCSValueToText *self,
		       gdouble value,
		       gchar **out,
		       GError **error);
} STCSValueToTextClass;

struct _STCSValueToText {
  const STCSValueToTextClass *klass;
};

/*
 * Builds a converter from a <value-to-text type="..."> node.
 * Looks up @type in the plugin-local registry and calls configure() on the node.
 */
STCSValueToText *stcs_value_to_text_new_from_node(xmlNodePtr node, GError **error);

void stcs_value_to_text_free(STCSValueToText *self);

gboolean stcs_value_to_text_apply(STCSValueToText *self,
				  gdouble value,
				  gchar **out,
				  GError **error);

G_END_DECLS

#endif /* STCS_VALUE_TO_TEXT_H */
