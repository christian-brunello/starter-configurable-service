/*
 * starter-configurable-service - json-reader.c
 *
 * Copyright (C) 2026 Christian Brunello <brncrs@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <string.h>

#include <glib-object.h>
#include <gmodule.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include "plugin.h"
#include "starter/configurable-service/reader.h"
#include "text-to-value.h"

#define LOGD(msg...) g_log("json-reader", G_LOG_LEVEL_DEBUG, msg)
#define LOGW(msg...) g_log("json-reader", G_LOG_LEVEL_WARNING, msg)

#define STCS_TYPE_JSON_READER (stcs_json_reader_get_type())
G_DECLARE_FINAL_TYPE(STCSJSONReader, stcs_json_reader, STCS, JSON_READER, STCSReader)

struct _STCSJSONReader {
  STCSReader parent_instance;
  GHashTable *text_to_values; /* path → STCSTextToValue* */
  gboolean suppress_missing_path_warnings;
};

G_DEFINE_TYPE(STCSJSONReader, stcs_json_reader, STCS_TYPE_READER)

typedef struct {
  STCSJSONReader *json_reader;
  STCSReader *reader;
  JsonNode *root;
  GError **error;
  gboolean ok;
} STCSJSONReaderParseCtx;

static gboolean
stcs_json_reader_node_to_double(JsonNode *node, gdouble *out, GError **error)
{
  GType value_type;

  if (node == NULL || JSON_NODE_TYPE(node) != JSON_NODE_VALUE)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "JSONPath match is not a scalar value");
      return FALSE;
    }

  value_type = json_node_get_value_type(node);
  if (value_type == G_TYPE_DOUBLE || value_type == G_TYPE_FLOAT)
    {
      *out = json_node_get_double(node);
      return TRUE;
    }

  if (value_type == G_TYPE_INT64 || value_type == G_TYPE_INT)
    {
      *out = (gdouble) json_node_get_int(node);
      return TRUE;
    }

  if (value_type == G_TYPE_BOOLEAN)
    {
      *out = json_node_get_boolean(node) ? 1.0 : 0.0;
      return TRUE;
    }

  if (value_type == G_TYPE_STRING)
    {
      const gchar *s;
      gchar *end = NULL;
      gdouble v;

      s = json_node_get_string(node);
      if (s == NULL || *s == '\0')
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "empty string cannot be converted to double");
	  return FALSE;
	}

      v = g_ascii_strtod(s, &end);
      if (end == s || *end != '\0')
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "string `%s' is not a number", s);
	  return FALSE;
	}

      *out = v;
      return TRUE;
    }

  g_set_error(error,
	      G_IO_ERROR,
	      G_IO_ERROR_INVALID_DATA,
	      "unsupported JSON value type for numeric conversion");
  return FALSE;
}

static gboolean
stcs_json_reader_node_to_double_with_text_to_value(JsonNode *node,
					       STCSTextToValue *converter,
					       gdouble *out,
					       GError **error)
{
  const gchar *text;

  if (converter == NULL)
    return stcs_json_reader_node_to_double(node, out, error);

  if (node == NULL || JSON_NODE_TYPE(node) != JSON_NODE_VALUE)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "JSONPath match is not a scalar value");
      return FALSE;
    }

  if (json_node_get_value_type(node) != G_TYPE_STRING)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "text-to-value requires a JSON string value");
      return FALSE;
    }

  text = json_node_get_string(node);
  if (text == NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "text-to-value received a NULL string");
      return FALSE;
    }

  return stcs_text_to_value_apply(converter, text, out, error);
}

static gboolean
stcs_json_reader_extract_path(JsonNode *root,
			      const gchar *path,
			      STCSTextToValue *converter,
			      gdouble *out,
			      GError **error)
{
  JsonNode *matches = NULL;
  JsonArray *array;
  JsonNode *leaf;
  gboolean r = FALSE;

  matches = json_path_query(path, root, error);
  if (matches == NULL)
    return FALSE;

  if (JSON_NODE_TYPE(matches) != JSON_NODE_ARRAY)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "JSONPath `%s' did not return a match array", path);
      goto out;
    }

  array = json_node_get_array(matches);
  if (json_array_get_length(array) == 0)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_NOT_FOUND,
		  "JSONPath `%s' matched no nodes", path);
      goto out;
    }

  leaf = json_array_get_element(array, 0);
  if (!stcs_json_reader_node_to_double_with_text_to_value(leaf, converter, out, error))
    goto out;

  r = TRUE;

out:
  if (matches != NULL)
    json_node_unref(matches);
  return r;
}

static void
stcs_json_reader_parse_key(const gchar *key,
			   const GPtrArray *datapoints,
			   gpointer user_data)
{
  STCSJSONReaderParseCtx *ctx = user_data;
  STCSTextToValue *converter;
  gdouble value = 0.0;
  GError *local_error = NULL;

  (void) datapoints;

  if (!ctx->ok)
    return;

  converter = g_hash_table_lookup(ctx->json_reader->text_to_values, key);

  if (!stcs_json_reader_extract_path(ctx->root, key, converter, &value, &local_error))
    {
      if (ctx->json_reader->suppress_missing_path_warnings)
	LOGD("path `%s': %s", key, local_error ? local_error->message : "unknown error");
      else
	LOGW("path `%s': %s", key, local_error ? local_error->message : "unknown error");
      g_clear_error(&local_error);
      return;
    }

  if (!stcs_reader_push_value(ctx->reader, key, value, ctx->error))
    {
      ctx->ok = FALSE;
      return;
    }

  LOGD("pushed path=`%s' value=%g → %u datapoint(s)",
       key, value, datapoints != NULL ? datapoints->len : 0);
}

static gboolean
stcs_json_reader_configure_map(STCSJSONReader *self,
			       xmlNodePtr node,
			       STCSDatapointLookupFunc lookup_datapoint,
			       gpointer lookup_data,
			       GError **error)
{
  xmlChar *datapoint_attr = NULL;
  xmlChar *path_attr = NULL;
  const gchar *datapoint_id;
  const gchar *path;
  STCSDatapoint *datapoint;
  STCSTextToValue *converter = NULL;
  xmlNodePtr child;
  gboolean r = FALSE;

  datapoint_attr = xmlGetProp(node, (const xmlChar *) "datapoint");
  path_attr = xmlGetProp(node, (const xmlChar *) "path");

  if (datapoint_attr == NULL || *datapoint_attr == '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "json-reader <map> requires a non-empty datapoint attribute");
      goto out;
    }

  if (path_attr == NULL || *path_attr == '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "json-reader <map> requires a non-empty path attribute");
      goto out;
    }

  datapoint_id = (const gchar *) datapoint_attr;
  path = (const gchar *) path_attr;

  datapoint = lookup_datapoint(datapoint_id, lookup_data);
  if (datapoint == NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_NOT_FOUND,
		  "datapoint `%s' not found (json-reader path=%s)",
		  datapoint_id, path);
      goto out;
    }

  for (child = node->children; child != NULL; child = child->next)
    {
      if (child->type != XML_ELEMENT_NODE)
	continue;

      if (!xmlStrEqual(child->name, (const xmlChar *) "text-to-value"))
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "unexpected element <%s> inside json-reader <map>, expected <text-to-value>",
		      (const char *) child->name);
	  goto out;
	}

      if (converter != NULL)
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "json-reader <map path=\"%s\"> allows at most one <text-to-value>",
		      path);
	  goto out;
	}

      converter = stcs_text_to_value_new_from_node(child, error);
      if (converter == NULL)
	goto out;
    }

  stcs_reader_add_mapping(STCS_READER(self), path, datapoint);

  if (converter != NULL)
    {
      if (g_hash_table_contains(self->text_to_values, path))
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "json-reader path `%s' already has a <text-to-value>; "
		      "shared paths must use one converter (use datapoint transforms for differences)",
		      path);
	  goto out;
	}

      g_hash_table_insert(self->text_to_values, g_strdup(path), converter);
      converter = NULL;
      LOGD("map path=`%s' → datapoint=`%s' (with text-to-value)", path, datapoint_id);
    }
  else
    {
      LOGD("map path=`%s' → datapoint=`%s'", path, datapoint_id);
    }

  r = TRUE;

out:
  if (converter != NULL)
    stcs_text_to_value_free(converter);
  if (datapoint_attr != NULL)
    xmlFree(datapoint_attr);
  if (path_attr != NULL)
    xmlFree(path_attr);
  return r;
}

static gboolean
stcs_json_reader_parse_bool(const gchar *text, gboolean *out, GError **error)
{
  if (g_ascii_strcasecmp(text, "true") == 0 || g_strcmp0(text, "1") == 0)
    {
      *out = TRUE;
      return TRUE;
    }
  if (g_ascii_strcasecmp(text, "false") == 0 || g_strcmp0(text, "0") == 0)
    {
      *out = FALSE;
      return TRUE;
    }

  g_set_error(error,
	      G_IO_ERROR,
	      G_IO_ERROR_INVALID_DATA,
	      "invalid boolean `%s'", text);
  return FALSE;
}

static gboolean
stcs_json_reader_configure(STCSReader *reader,
			   xmlNodePtr node,
			   STCSDatapointLookupFunc lookup_datapoint,
			   gpointer lookup_data,
			   GError **error)
{
  STCSJSONReader *self = STCS_JSON_READER(reader);
  xmlNodePtr child;

  g_return_val_if_fail(lookup_datapoint != NULL, FALSE);

  g_hash_table_remove_all(self->text_to_values);
  self->suppress_missing_path_warnings = FALSE;

  for (child = node->children; child != NULL; child = child->next)
    {
      if (child->type != XML_ELEMENT_NODE)
	continue;

      if (xmlStrEqual(child->name, (const xmlChar *) "suppress-missing-path-warnings"))
	{
	  xmlChar *content = xmlNodeGetContent(child);
	  gboolean v = FALSE;

	  if (content == NULL || *content == '\0'
	      || !stcs_json_reader_parse_bool((const gchar *) content, &v, error))
	    {
	      if (content != NULL && *content != '\0' && error != NULL && *error == NULL)
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
			    "suppress-missing-path-warnings requires true/false");
	      xmlFree(content);
	      return FALSE;
	    }
	  self->suppress_missing_path_warnings = v;
	  xmlFree(content);
	  continue;
	}

      if (!xmlStrEqual(child->name, (const xmlChar *) "map"))
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "unexpected element <%s> inside json-reader, expected <map>",
		      (const char *) child->name);
	  return FALSE;
	}

      if (!stcs_json_reader_configure_map(self, child, lookup_datapoint, lookup_data, error))
	return FALSE;
    }

  return TRUE;
}

static gboolean
stcs_json_reader_parse(STCSReader *reader, GBytes *data, GError **error)
{
  STCSJSONReader *self = STCS_JSON_READER(reader);
  JsonParser *parser = NULL;
  const gchar *buf;
  gsize len = 0;
  STCSJSONReaderParseCtx ctx;
  gboolean r = FALSE;

  g_return_val_if_fail(data != NULL, FALSE);

  buf = g_bytes_get_data(data, &len);
  if (buf == NULL || len == 0)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "empty JSON payload");
      return FALSE;
    }

  parser = json_parser_new();
  if (!json_parser_load_from_data(parser, buf, (gssize) len, error))
    goto out;

  ctx.json_reader = self;
  ctx.reader = reader;
  ctx.root = json_parser_get_root(parser);
  ctx.error = error;
  ctx.ok = TRUE;

  if (ctx.root == NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "JSON document has no root node");
      goto out;
    }

  stcs_reader_foreach_key(reader, stcs_json_reader_parse_key, &ctx);
  if (!ctx.ok)
    goto out;

  r = TRUE;

out:
  if (parser != NULL)
    g_object_unref(parser);
  return r;
}

static void
stcs_json_reader_dispose(GObject *object)
{
  STCSJSONReader *self = STCS_JSON_READER(object);

  g_clear_pointer(&self->text_to_values, g_hash_table_unref);

  G_OBJECT_CLASS(stcs_json_reader_parent_class)->dispose(object);
}

static void
stcs_json_reader_class_init(STCSJSONReaderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  STCSReaderClass *reader_class = STCS_READER_CLASS(klass);

  object_class->dispose = stcs_json_reader_dispose;

  reader_class->configure = stcs_json_reader_configure;
  reader_class->parse = stcs_json_reader_parse;
}

static void
stcs_json_reader_init(STCSJSONReader *self)
{
  self->suppress_missing_path_warnings = FALSE;
  self->text_to_values = g_hash_table_new_full(g_str_hash,
						g_str_equal,
						g_free,
						(GDestroyNotify) stcs_text_to_value_free);
}

G_MODULE_EXPORT STCSPluginInfo *
stcs_plugin_get_info(void)
{
  return stcs_plugin_info_new("json-reader", STCS_PLUGIN_CATEGORY_READER, "1.0.0");
}

G_MODULE_EXPORT GObject *
stcs_plugin_create(void)
{
  return g_object_new(STCS_TYPE_JSON_READER, NULL);
}
