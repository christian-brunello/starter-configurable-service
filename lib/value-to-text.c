/*
 * libstarter-configurable-service - value-to-text.c
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

#include "starter/configurable-service/value-to-text.h"
#include "value-to-date-text.h"
#include "value-to-map-text.h"

#include <string.h>

#define LOGD(msg...) g_log("value-to-text", G_LOG_LEVEL_DEBUG, msg)
#define LOGW(msg...) g_log("value-to-text", G_LOG_LEVEL_WARNING, msg)

/*
 * Registry of value-to-text implementations.
 * NULL entries are ignored.
 */
static const STCSValueToTextClass *const stcs_value_to_text_classes[] = {
  &stcs_value_to_date_text_class,
  &stcs_value_to_map_text_class,
};

static const STCSValueToTextClass *
stcs_value_to_text_lookup_class(const gchar *type)
{
  guint i;

  if (type == NULL || *type == '\0')
    return NULL;

  for (i = 0; i < G_N_ELEMENTS(stcs_value_to_text_classes); i++)
    {
      const STCSValueToTextClass *klass = stcs_value_to_text_classes[i];

      if (klass != NULL && g_strcmp0(klass->type, type) == 0)
	return klass;
    }

  return NULL;
}

STCSValueToText *
stcs_value_to_text_new_from_node(xmlNodePtr node, GError **error)
{
  xmlChar *type_attr = NULL;
  const gchar *type;
  const STCSValueToTextClass *klass;
  STCSValueToText *self = NULL;

  g_return_val_if_fail(node != NULL, NULL);

  if (!xmlStrEqual(node->name, (const xmlChar *) "value-to-text"))
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "expected <value-to-text>, got <%s>",
		  (const char *) node->name);
      return NULL;
    }

  type_attr = xmlGetProp(node, (const xmlChar *) "type");
  if (type_attr == NULL || *type_attr == '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "<value-to-text> requires a non-empty type attribute");
      goto out;
    }

  type = (const gchar *) type_attr;
  LOGD("create type=`%s'", type);

  klass = stcs_value_to_text_lookup_class(type);
  if (klass == NULL)
    {
      LOGW("unknown type `%s'", type);
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_NOT_SUPPORTED,
		  "unknown value-to-text type `%s'", type);
      goto out;
    }

  if (klass->create == NULL || klass->configure == NULL || klass->to_text == NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_FAILED,
		  "value-to-text type `%s' has an incomplete implementation",
		  type);
      goto out;
    }

  self = klass->create();
  if (self == NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_FAILED,
		  "failed to create value-to-text type `%s'", type);
      goto out;
    }

  self->klass = klass;

  if (!klass->configure(self, node, error))
    {
      LOGW("configure failed for type=`%s'", type);
      stcs_value_to_text_free(self);
      self = NULL;
      goto out;
    }

  LOGD("configured type=`%s'", type);

out:
  if (type_attr != NULL)
    xmlFree(type_attr);
  return self;
}

void
stcs_value_to_text_free(STCSValueToText *self)
{
  if (self == NULL)
    return;

  LOGD("free type=`%s'",
       self->klass != NULL && self->klass->type != NULL ? self->klass->type : "?");

  if (self->klass != NULL && self->klass->destroy != NULL)
    self->klass->destroy(self);
  else
    g_free(self);
}

gboolean
stcs_value_to_text_apply(STCSValueToText *self,
			 gdouble value,
			 gchar **out,
			 GError **error)
{
  gboolean r;

  g_return_val_if_fail(self != NULL, FALSE);
  g_return_val_if_fail(self->klass != NULL, FALSE);
  g_return_val_if_fail(self->klass->to_text != NULL, FALSE);
  g_return_val_if_fail(out != NULL, FALSE);

  LOGD("apply type=`%s' value=%g",
       self->klass->type != NULL ? self->klass->type : "?",
       value);

  r = self->klass->to_text(self, value, out, error);
  if (r)
    LOGD("apply type=`%s' -> `%s'",
	 self->klass->type != NULL ? self->klass->type : "?",
	 *out != NULL ? *out : "");
  else
    LOGW("apply type=`%s' failed: %s",
	 self->klass->type != NULL ? self->klass->type : "?",
	 error != NULL && *error != NULL ? (*error)->message : "unknown error");

  return r;
}
