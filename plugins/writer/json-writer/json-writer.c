/*
 * starter-configurable-service - json-writer.c
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
#include "starter/configurable-service/writer.h"
#include "value-to-text.h"

#define LOGD(msg...) g_log("json-writer", G_LOG_LEVEL_DEBUG, msg)
#define LOGI(msg...) g_log("json-writer", G_LOG_LEVEL_INFO, msg)
#define LOGW(msg...) g_log("json-writer", G_LOG_LEVEL_WARNING, msg)

#define STCS_TYPE_JSON_WRITER (stcs_json_writer_get_type())
G_DECLARE_FINAL_TYPE(STCSJSONWriter, stcs_json_writer, STCS, JSON_WRITER, STCSWriter)

struct _STCSJSONWriter {
  STCSWriter parent_instance;
  JsonNode *document; /* working copy of the template */
  GHashTable *value_to_texts; /* path → STCSValueToText* */
};

G_DEFINE_TYPE(STCSJSONWriter, stcs_json_writer, STCS_TYPE_WRITER)

static gboolean
stcs_json_writer_parse_string(xmlNodePtr node, gchar **out, GError **error)
{
  xmlChar *content;

  content = xmlNodeGetContent(node);
  if (content == NULL || *content == '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "element <%s> has empty content",
		  (const char *) node->name);
      xmlFree(content);
      return FALSE;
    }

  g_free(*out);
  *out = g_strdup((const char *) content);
  xmlFree(content);
  return TRUE;
}

/*
 * Resolve a simple path: "$"? ("." name)+ to the parent object and leaf name.
 * Arrays / filters are not supported; the template must already contain the path.
 */
static gboolean
stcs_json_writer_resolve_leaf(JsonNode *root,
			      const gchar *path,
			      JsonObject **parent_out,
			      const gchar **leaf_out,
			      gchar ***parts_out,
			      GError **error)
{
  const gchar *p = path;
  JsonNode *node = root;
  gchar **parts;
  guint i, n;

  *parent_out = NULL;
  *leaf_out = NULL;
  *parts_out = NULL;

  if (path == NULL || *path == '\0')
    {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "empty JSON path");
      return FALSE;
    }

  if (*p == '$')
    p++;
  while (*p == '.')
    p++;

  if (*p == '\0')
    {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		  "JSON path `%s' has no members", path);
      return FALSE;
    }

  parts = g_strsplit(p, ".", 0);
  n = g_strv_length(parts);
  if (n == 0)
    {
      g_strfreev(parts);
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		  "invalid JSON path `%s'", path);
      return FALSE;
    }

  for (i = 0; i + 1 < n; i++)
    {
      JsonObject *object;

      if (parts[i] == NULL || *parts[i] == '\0')
	{
	  g_strfreev(parts);
	  g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		      "empty member in JSON path `%s'", path);
	  return FALSE;
	}

      if (!JSON_NODE_HOLDS_OBJECT(node))
	{
	  g_strfreev(parts);
	  g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		      "JSON path `%s': expected object before `%s'",
		      path, parts[i]);
	  return FALSE;
	}

      object = json_node_get_object(node);
      if (!json_object_has_member(object, parts[i]))
	{
	  g_strfreev(parts);
	  g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
		      "JSON path `%s': missing member `%s'", path, parts[i]);
	  return FALSE;
	}

      node = json_object_get_member(object, parts[i]);
    }

  {
    const gchar *leaf = parts[n - 1];

    if (leaf == NULL || *leaf == '\0')
      {
	g_strfreev(parts);
	g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		    "empty leaf in JSON path `%s'", path);
	return FALSE;
      }

    if (!JSON_NODE_HOLDS_OBJECT(node))
      {
	g_strfreev(parts);
	g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		    "JSON path `%s': parent of `%s' is not an object",
		    path, leaf);
	return FALSE;
      }

    *parent_out = json_node_get_object(node);
    *leaf_out = leaf;
    *parts_out = parts;
  }

  return TRUE;
}

static gboolean
stcs_json_writer_set_path_double(JsonNode *root,
				 const gchar *path,
				 gdouble value,
				 GError **error)
{
  JsonObject *parent = NULL;
  const gchar *leaf = NULL;
  gchar **parts = NULL;
  JsonNode *leaf_node;

  if (!stcs_json_writer_resolve_leaf(root, path, &parent, &leaf, &parts, error))
    return FALSE;

  leaf_node = json_node_new(JSON_NODE_VALUE);
  json_node_set_double(leaf_node, value);
  json_object_set_member(parent, leaf, leaf_node);
  g_strfreev(parts);
  return TRUE;
}

static gboolean
stcs_json_writer_set_path_string(JsonNode *root,
				 const gchar *path,
				 const gchar *value,
				 GError **error)
{
  JsonObject *parent = NULL;
  const gchar *leaf = NULL;
  gchar **parts = NULL;
  JsonNode *leaf_node;

  g_return_val_if_fail(value != NULL, FALSE);

  if (!stcs_json_writer_resolve_leaf(root, path, &parent, &leaf, &parts, error))
    return FALSE;

  leaf_node = json_node_new(JSON_NODE_VALUE);
  json_node_set_string(leaf_node, value);
  json_object_set_member(parent, leaf, leaf_node);
  g_strfreev(parts);
  return TRUE;
}

static gboolean
stcs_json_writer_flush(STCSJSONWriter *self, GError **error)
{
  JsonGenerator *generator = NULL;
  gchar *text = NULL;
  gsize len = 0;
  GBytes *bytes = NULL;
  gboolean r = FALSE;

  if (self->document == NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_NOT_INITIALIZED,
		  "json-writer has no template document");
      return FALSE;
    }

  generator = json_generator_new();
  json_generator_set_root(generator, self->document);
  json_generator_set_pretty(generator, FALSE);
  text = json_generator_to_data(generator, &len);
  if (text == NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_FAILED,
		  "failed to serialize JSON document");
      goto out;
    }

  bytes = g_bytes_new_take(text, len);
  text = NULL;

  r = stcs_writer_write_bytes(STCS_WRITER(self), bytes, error);

out:
  if (bytes != NULL)
    g_bytes_unref(bytes);
  g_free(text);
  if (generator != NULL)
    g_object_unref(generator);
  return r;
}

static gboolean
stcs_json_writer_configure_map(STCSJSONWriter *self,
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
  STCSValueToText *converter = NULL;
  xmlNodePtr child;
  gboolean r = FALSE;

  datapoint_attr = xmlGetProp(node, (const xmlChar *) "datapoint");
  path_attr = xmlGetProp(node, (const xmlChar *) "path");

  if (datapoint_attr == NULL || *datapoint_attr == '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "json-writer <map> requires a non-empty datapoint attribute");
      goto out;
    }

  if (path_attr == NULL || *path_attr == '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "json-writer <map> requires a non-empty path attribute");
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
		  "datapoint `%s' not found (json-writer path=%s)",
		  datapoint_id, path);
      goto out;
    }

  for (child = node->children; child != NULL; child = child->next)
    {
      if (child->type != XML_ELEMENT_NODE)
	continue;

      if (!xmlStrEqual(child->name, (const xmlChar *) "value-to-text"))
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "unexpected element <%s> inside json-writer <map>, expected <value-to-text>",
		      (const char *) child->name);
	  goto out;
	}

      if (converter != NULL)
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "json-writer <map path=\"%s\"> allows at most one <value-to-text>",
		      path);
	  goto out;
	}

      converter = stcs_value_to_text_new_from_node(child, error);
      if (converter == NULL)
	goto out;
    }

  stcs_writer_add_mapping(STCS_WRITER(self), path, datapoint);

  if (converter != NULL)
    {
      if (g_hash_table_contains(self->value_to_texts, path))
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "json-writer path `%s' already has a <value-to-text>; "
		      "shared paths must use one converter",
		      path);
	  goto out;
	}

      g_hash_table_insert(self->value_to_texts, g_strdup(path), converter);
      converter = NULL;
      LOGD("map path=`%s' → datapoint=`%s' (with value-to-text)", path, datapoint_id);
    }
  else
    {
      LOGD("map path=`%s' → datapoint=`%s'", path, datapoint_id);
    }

  r = TRUE;

out:
  if (converter != NULL)
    stcs_value_to_text_free(converter);
  if (datapoint_attr != NULL)
    xmlFree(datapoint_attr);
  if (path_attr != NULL)
    xmlFree(path_attr);
  return r;
}

static gboolean
stcs_json_writer_configure(STCSWriter *writer,
			   xmlNodePtr node,
			   STCSDatapointLookupFunc lookup_datapoint,
			   gpointer lookup_data,
			   GError **error)
{
  STCSJSONWriter *self = STCS_JSON_WRITER(writer);
  xmlNodePtr child;
  gchar *template_src = NULL;
  JsonParser *parser = NULL;
  gboolean r = FALSE;

  g_return_val_if_fail(lookup_datapoint != NULL, FALSE);

  g_hash_table_remove_all(self->value_to_texts);

  for (child = node->children; child != NULL; child = child->next)
    {
      if (child->type != XML_ELEMENT_NODE)
	continue;

      if (xmlStrEqual(child->name, (const xmlChar *) "write-attempts"))
	continue;

      if (xmlStrEqual(child->name, (const xmlChar *) "map"))
	{
	  if (!stcs_json_writer_configure_map(self, child, lookup_datapoint, lookup_data, error))
	    goto out;
	}
      else if (xmlStrEqual(child->name, (const xmlChar *) "template"))
	{
	  if (!stcs_json_writer_parse_string(child, &template_src, error))
	    goto out;
	}
      else
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "unexpected element <%s> inside json-writer config",
		      (const char *) child->name);
	  goto out;
	}
    }

  if (template_src == NULL || *template_src == '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "json-writer requires a non-empty <template> element");
      goto out;
    }

  parser = json_parser_new();
  if (!json_parser_load_from_data(parser, template_src, -1, error))
    goto out;

  if (json_parser_get_root(parser) == NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "JSON template has no root node");
      goto out;
    }

  g_clear_pointer(&self->document, json_node_unref);
  self->document = json_node_copy(json_parser_get_root(parser));

  LOGI("json-writer template loaded (%zu bytes)", strlen(template_src));
  r = TRUE;

out:
  g_free(template_src);
  if (parser != NULL)
    g_object_unref(parser);
  return r;
}

static gboolean
stcs_json_writer_write_value(STCSWriter *writer,
			     const gchar *key,
			     gdouble value,
			     GError **error)
{
  STCSJSONWriter *self = STCS_JSON_WRITER(writer);
  STCSValueToText *converter;
  gchar *text = NULL;
  gboolean r = FALSE;

  if (self->document == NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_NOT_INITIALIZED,
		  "json-writer is not configured");
      return FALSE;
    }

  converter = g_hash_table_lookup(self->value_to_texts, key);
  if (converter != NULL)
    {
      if (!stcs_value_to_text_apply(converter, value, &text, error))
	goto out;

      if (!stcs_json_writer_set_path_string(self->document, key, text, error))
	goto out;

      LOGD("updated path=`%s' text=`%s' — flushing", key, text);
    }
  else
    {
      if (!stcs_json_writer_set_path_double(self->document, key, value, error))
	goto out;

      LOGD("updated path=`%s' value=%g — flushing", key, value);
    }

  r = stcs_json_writer_flush(self, error);

out:
  g_free(text);
  return r;
}

static void
stcs_json_writer_dispose(GObject *object)
{
  STCSJSONWriter *self = STCS_JSON_WRITER(object);

  g_clear_pointer(&self->document, json_node_unref);
  g_clear_pointer(&self->value_to_texts, g_hash_table_unref);

  G_OBJECT_CLASS(stcs_json_writer_parent_class)->dispose(object);
}

static void
stcs_json_writer_class_init(STCSJSONWriterClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  STCSWriterClass *writer_class = STCS_WRITER_CLASS(klass);

  object_class->dispose = stcs_json_writer_dispose;

  writer_class->configure = stcs_json_writer_configure;
  writer_class->write_value = stcs_json_writer_write_value;
}

static void
stcs_json_writer_init(STCSJSONWriter *self)
{
  self->document = NULL;
  self->value_to_texts = g_hash_table_new_full(g_str_hash,
						g_str_equal,
						g_free,
						(GDestroyNotify) stcs_value_to_text_free);
}

G_MODULE_EXPORT STCSPluginInfo *
stcs_plugin_get_info(void)
{
  return stcs_plugin_info_new("json-writer", STCS_PLUGIN_CATEGORY_WRITER, "1.0.0");
}

G_MODULE_EXPORT GObject *
stcs_plugin_create(void)
{
  return g_object_new(STCS_TYPE_JSON_WRITER, NULL);
}
