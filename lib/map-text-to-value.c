/*
 * libstarter-configurable-service - map-text-to-value.c
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

#include "map-text-to-value.h"

#include <string.h>
#include <math.h>

#define LOGD(msg...) g_log("map-text-to-value", G_LOG_LEVEL_DEBUG, msg)
#define LOGW(msg...) g_log("map-text-to-value", G_LOG_LEVEL_WARNING, msg)

typedef struct {
  gchar *text; /* stored lowercased for case-insensitive match */
  gdouble value;
} STCSMapTextItem;

typedef struct {
  STCSTextToValue parent;
  GArray *items; /* STCSMapTextItem */
  gboolean has_default;
  gdouble default_value;
} STCSMapTextToValue;

static void
stcs_map_text_item_clear(STCSMapTextItem *item)
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
  gchar *normalized;

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

  normalized = g_ascii_strdown((const char *) attr, -1);
  g_strstrip(normalized);
  if (*normalized == '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "<%s> text attribute is empty after trim",
		  (const char *) node->name);
      g_free(normalized);
      xmlFree(attr);
      return FALSE;
    }

  g_free(*out);
  *out = normalized;
  xmlFree(attr);
  return TRUE;
}

static STCSTextToValue *
stcs_map_text_to_value_create(void)
{
  STCSMapTextToValue *self = g_new0(STCSMapTextToValue, 1);

  self->items = g_array_new(FALSE, FALSE, sizeof(STCSMapTextItem));
  g_array_set_clear_func(self->items, (GDestroyNotify) stcs_map_text_item_clear);
  return (STCSTextToValue *) self;
}

static void
stcs_map_text_to_value_destroy(STCSTextToValue *transform)
{
  STCSMapTextToValue *self = (STCSMapTextToValue *) transform;

  g_clear_pointer(&self->items, g_array_unref);
  g_free(self);
}

static gboolean
stcs_map_text_to_value_configure(STCSTextToValue *transform,
				 xmlNodePtr node,
				 GError **error)
{
  STCSMapTextToValue *self = (STCSMapTextToValue *) transform;
  xmlNodePtr child;
  gboolean saw_default = FALSE;

  g_array_set_size(self->items, 0);
  self->has_default = FALSE;

  for (child = node->children; child != NULL; child = child->next)
    {
      if (child->type != XML_ELEMENT_NODE)
	continue;

      if (xmlStrEqual(child->name, (const xmlChar *) "item"))
	{
	  STCSMapTextItem item;

	  if (saw_default)
	    {
	      g_set_error(error,
			  G_IO_ERROR,
			  G_IO_ERROR_INVALID_DATA,
			  "text-to-value type=map: <item> must appear before <default>");
	      return FALSE;
	    }

	  memset(&item, 0, sizeof(item));
	  if (!stcs_map_parse_text_attr(child, &item.text, error))
	    return FALSE;
	  if (!stcs_map_parse_double_attr(child, "value", &item.value, error))
	    {
	      g_free(item.text);
	      return FALSE;
	    }

	  g_array_append_val(self->items, item);
	  LOGD("item text=`%s' → value=%g", item.text, item.value);
	}
      else if (xmlStrEqual(child->name, (const xmlChar *) "default"))
	{
	  if (saw_default)
	    {
	      g_set_error(error,
			  G_IO_ERROR,
			  G_IO_ERROR_INVALID_DATA,
			  "text-to-value type=map allows at most one <default>");
	      return FALSE;
	    }
	  if (!stcs_map_parse_double_attr(child, "value", &self->default_value, error))
	    return FALSE;
	  saw_default = TRUE;
	  self->has_default = TRUE;
	  LOGD("default value=%g", self->default_value);
	}
      else
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "unexpected element <%s> inside text-to-value type=map "
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
		  "text-to-value type=map requires at least one <item> or <default>");
      return FALSE;
    }

  return TRUE;
}

static gboolean
stcs_map_text_to_value_to_value(STCSTextToValue *transform,
				const gchar *text,
				gdouble *out,
				GError **error)
{
  STCSMapTextToValue *self = (STCSMapTextToValue *) transform;
  gchar *key;
  guint i;

  if (text == NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "map text-to-value received NULL text");
      return FALSE;
    }

  key = g_ascii_strdown(text, -1);
  g_strstrip(key);

  for (i = 0; i < self->items->len; i++)
    {
      STCSMapTextItem *item = &g_array_index(self->items, STCSMapTextItem, i);

      if (g_strcmp0(item->text, key) == 0)
	{
	  *out = item->value;
	  LOGD("matched text=`%s' → %g", key, *out);
	  g_free(key);
	  return TRUE;
	}
    }

  if (self->has_default)
    {
      *out = self->default_value;
      LOGD("default for text=`%s' → %g", key, *out);
      g_free(key);
      return TRUE;
    }

  g_set_error(error,
	      G_IO_ERROR,
	      G_IO_ERROR_NOT_FOUND,
	      "map text-to-value: no item for text `%s'",
	      text);
  LOGW("no match for text=`%s'", key);
  g_free(key);
  return FALSE;
}

const STCSTextToValueClass stcs_map_text_to_value_class = {
  .type = "map",
  .create = stcs_map_text_to_value_create,
  .destroy = stcs_map_text_to_value_destroy,
  .configure = stcs_map_text_to_value_configure,
  .to_value = stcs_map_text_to_value_to_value,
};
