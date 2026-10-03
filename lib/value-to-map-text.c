/*
 * libstarter-configurable-service - value-to-map-text.c
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

#include "value-to-map-text.h"

#include <string.h>
#include <math.h>

#define LOGD(msg...) g_log("value-to-map-text", G_LOG_LEVEL_DEBUG, msg)
#define LOGW(msg...) g_log("value-to-map-text", G_LOG_LEVEL_WARNING, msg)

typedef struct {
  gdouble value;
  gchar *text;
} STCSMapValueItem;

typedef struct {
  STCSValueToText parent;
  GArray *items; /* STCSMapValueItem */
  gboolean has_default;
  gchar *default_text;
} STCSValueToMapText;

static void
stcs_map_value_item_clear(STCSMapValueItem *item)
{
  g_free(item->text);
  item->text = NULL;
}

static gboolean
stcs_map_parse_double_attr(xmlNodePtr node,
			   const gchar *attr_name,
			   gdouble *out,
			   GError **error)
{
  xmlChar *attr;
  gchar *end = NULL;
  gdouble v;

  attr = xmlGetProp(node, (const xmlChar *) attr_name);
  if (attr == NULL || *attr == '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "<%s> requires a non-empty %s attribute",
		  (const char *) node->name, attr_name);
      if (attr != NULL)
	xmlFree(attr);
      return FALSE;
    }

  v = g_ascii_strtod((const char *) attr, &end);
  if (end == (const char *) attr || *end != '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "<%s> has invalid %s attribute: %s",
		  (const char *) node->name, attr_name, (const char *) attr);
      xmlFree(attr);
      return FALSE;
    }

  *out = v;
  xmlFree(attr);
  return TRUE;
}

static gboolean
stcs_map_parse_text_attr(xmlNodePtr node, gchar **out, GError **error)
{
  xmlChar *attr;

  attr = xmlGetProp(node, (const xmlChar *) "text");
  if (attr == NULL || *attr == '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "<%s> requires a non-empty text attribute",
		  (const char *) node->name);
      if (attr != NULL)
	xmlFree(attr);
      return FALSE;
    }

  g_free(*out);
  *out = g_strdup((const char *) attr);
  xmlFree(attr);
  return TRUE;
}

static gboolean
stcs_map_values_equal(gdouble a, gdouble b)
{
  return a == b || (isnan(a) && isnan(b));
}

static STCSValueToText *
stcs_value_to_map_text_create(void)
{
  STCSValueToMapText *self = g_new0(STCSValueToMapText, 1);

  self->items = g_array_new(FALSE, FALSE, sizeof(STCSMapValueItem));
  g_array_set_clear_func(self->items, (GDestroyNotify) stcs_map_value_item_clear);
  return (STCSValueToText *) self;
}

static void
stcs_value_to_map_text_destroy(STCSValueToText *transform)
{
  STCSValueToMapText *self = (STCSValueToMapText *) transform;

  g_clear_pointer(&self->items, g_array_unref);
  g_free(self->default_text);
  g_free(self);
}

static gboolean
stcs_value_to_map_text_configure(STCSValueToText *transform,
				 xmlNodePtr node,
				 GError **error)
{
  STCSValueToMapText *self = (STCSValueToMapText *) transform;
  xmlNodePtr child;
  gboolean saw_default = FALSE;

  g_array_set_size(self->items, 0);
  g_clear_pointer(&self->default_text, g_free);
  self->has_default = FALSE;

  for (child = node->children; child != NULL; child = child->next)
    {
      if (child->type != XML_ELEMENT_NODE)
	continue;

      if (xmlStrEqual(child->name, (const xmlChar *) "item"))
	{
	  STCSMapValueItem item;

	  if (saw_default)
	    {
	      g_set_error(error,
			  G_IO_ERROR,
			  G_IO_ERROR_INVALID_DATA,
			  "value-to-text type=map: <item> must appear before <default>");
	      return FALSE;
	    }

	  memset(&item, 0, sizeof(item));
	  if (!stcs_map_parse_double_attr(child, "value", &item.value, error))
	    return FALSE;
	  if (!stcs_map_parse_text_attr(child, &item.text, error))
	    return FALSE;

	  g_array_append_val(self->items, item);
	  LOGD("item value=%g → text=`%s'", item.value, item.text);
	}
      else if (xmlStrEqual(child->name, (const xmlChar *) "default"))
	{
	  if (saw_default)
	    {
	      g_set_error(error,
			  G_IO_ERROR,
			  G_IO_ERROR_INVALID_DATA,
			  "value-to-text type=map allows at most one <default>");
	      return FALSE;
	    }
	  if (!stcs_map_parse_text_attr(child, &self->default_text, error))
	    return FALSE;
	  saw_default = TRUE;
	  self->has_default = TRUE;
	  LOGD("default text=`%s'", self->default_text);
	}
      else
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "unexpected element <%s> inside value-to-text type=map "
		      "(expected <item> or <default>)",
		      (const char *) child->name);
	  return FALSE;
	}
    }

  if (self->items->len == 0 && !self->has_default)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "value-to-text type=map requires at least one <item> or <default>");
      return FALSE;
    }

  return TRUE;
}

static gboolean
stcs_value_to_map_text_to_text(STCSValueToText *transform,
			       gdouble value,
			       gchar **out,
			       GError **error)
{
  STCSValueToMapText *self = (STCSValueToMapText *) transform;
  guint i;

  for (i = 0; i < self->items->len; i++)
    {
      STCSMapValueItem *item = &g_array_index(self->items, STCSMapValueItem, i);

      if (stcs_map_values_equal(item->value, value))
	{
	  *out = g_strdup(item->text);
	  LOGD("matched value=%g → `%s'", value, *out);
	  return TRUE;
	}
    }

  if (self->has_default)
    {
      *out = g_strdup(self->default_text);
      LOGD("default for value=%g → `%s'", value, *out);
      return TRUE;
    }

  g_set_error(error,
	      G_IO_ERROR,
	      G_IO_ERROR_NOT_FOUND,
	      "map value-to-text: no item for value %g",
	      value);
  LOGW("no match for value=%g", value);
  return FALSE;
}

const STCSValueToTextClass stcs_value_to_map_text_class = {
  .type = "map",
  .create = stcs_value_to_map_text_create,
  .destroy = stcs_value_to_map_text_destroy,
  .configure = stcs_value_to_map_text_configure,
  .to_text = stcs_value_to_map_text_to_text,
};
