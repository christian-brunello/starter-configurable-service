/*
 * starter-configurable-service - config.c
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

#include <libxml/parser.h>
#include <libxml/tree.h>

#include "internals.h"
#include "plugin.h"
#include "plugin-entry.h"
#include "glib-compat.h"
#include "starter/configurable-service/transport.h"
#include "starter/configurable-service/transform.h"
#include "starter/configurable-service/datapoint.h"
#include "starter/configurable-service/reader.h"
#include "starter/configurable-service/writer.h"

#include <starter/input.h>
#include <starter/output.h>
#include <starter/defs.h>

#define LOGD(msg...) g_log("config", G_LOG_LEVEL_DEBUG, msg)
#define LOGI(msg...) g_log("config", G_LOG_LEVEL_INFO, msg)
#define LOGW(msg...) g_log("config", G_LOG_LEVEL_WARNING, msg)
#define LOGE(msg...) g_log("config", G_LOG_LEVEL_ERROR, msg)

static gboolean parse_global_node(STCSContext *context, xmlNodePtr node, GError **error);
static gboolean parse_transports_node(STCSContext *context, xmlNodePtr node, GError **error);
static gboolean parse_transport_node(STCSContext *context, xmlNodePtr node, GError **error);
static gboolean parse_datapoints_node(STCSContext *context, xmlNodePtr node, GError **error);

static STCSDatapoint *
config_lookup_datapoint(const gchar *id, gpointer user_data)
{
  STCSContext *context = user_data;

  g_return_val_if_fail(context != NULL, NULL);
  g_return_val_if_fail(id != NULL, NULL);

  return g_hash_table_lookup(context->datapoints, id);
}

static gboolean parse_datapoint_node(STCSContext *context, xmlNodePtr node, GError **error);
static gboolean parse_readers_node(STCSContext *context, xmlNodePtr node, GError **error);
static gboolean parse_reader_node(STCSContext *context, xmlNodePtr node, GError **error);
static gboolean parse_writers_node(STCSContext *context, xmlNodePtr node, GError **error);
static gboolean parse_writer_node(STCSContext *context, xmlNodePtr node, GError **error);
static gboolean parse_inputs_node(STCSContext *context, xmlNodePtr node, GError **error);
static gboolean parse_input_node(STCSContext *context, xmlNodePtr node, GError **error);
static gboolean parse_outputs_node(STCSContext *context, xmlNodePtr node, GError **error);
static gboolean parse_output_node(STCSContext *context, xmlNodePtr node, GError **error);
static gboolean parse_bindings_node(STCSContext *context, xmlNodePtr node, GError **error);
static gboolean parse_bind_node(STCSContext *context, xmlNodePtr node, GError **error);

/*
 * A transport may be bound to at most one reader or one writer.
 * Returns TRUE and fills @out_kind/@out_id if already owned.
 */
static gboolean
config_find_transport_owner(STCSContext *context,
			    STCSTransport *transport,
			    const gchar **out_kind,
			    const gchar **out_id)
{
  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init(&iter, context->readers);
  while (g_hash_table_iter_next(&iter, &key, &value))
    {
      if (stcs_reader_get_transport(STCS_READER(value)) == transport)
	{
	  if (out_kind != NULL)
	    *out_kind = "reader";
	  if (out_id != NULL)
	    *out_id = (const gchar *) key;
	  return TRUE;
	}
    }

  g_hash_table_iter_init(&iter, context->writers);
  while (g_hash_table_iter_next(&iter, &key, &value))
    {
      if (stcs_writer_get_transport(STCS_WRITER(value)) == transport)
	{
	  if (out_kind != NULL)
	    *out_kind = "writer";
	  if (out_id != NULL)
	    *out_id = (const gchar *) key;
	  return TRUE;
	}
    }

  return FALSE;
}

static gboolean
config_claim_transport(STCSContext *context,
		       STCSTransport *transport,
		       const gchar *transport_id,
		       const gchar *consumer_kind,
		       const gchar *consumer_id,
		       GError **error)
{
  const gchar *owner_kind = NULL;
  const gchar *owner_id = NULL;

  if (!config_find_transport_owner(context, transport, &owner_kind, &owner_id))
    return TRUE;

  g_set_error(error,
	      G_IO_ERROR,
	      G_IO_ERROR_EXISTS,
	      "transport `%s' is already bound to %s `%s' "
	      "(a transport may bind to only one reader or one writer; refused by %s `%s')",
	      transport_id,
	      owner_kind,
	      owner_id,
	      consumer_kind,
	      consumer_id);
  return FALSE;
}

static void
config_warn_unbound_transports(STCSContext *context)
{
  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init(&iter, context->transports);
  while (g_hash_table_iter_next(&iter, &key, &value))
    {
      const gchar *transport_id = key;
      STCSTransport *transport = value;

      if (!config_find_transport_owner(context, transport, NULL, NULL))
	LOGW("transport `%s' is not bound to any reader or writer",
	     transport_id);
    }
}

static gboolean
parse_children_of(STCSContext *context,
		  xmlNodePtr parent,
		  const char *expected_child,
		  gboolean (*parse_child)(STCSContext *, xmlNodePtr, GError **),
		  GError **error)
{
  xmlNodePtr child;

  for (child = parent->children; child != NULL; child = child->next)
    {
      if (child->type != XML_ELEMENT_NODE)
	continue;

      if (!xmlStrEqual(child->name, (const xmlChar *) expected_child))
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "unexpected element <%s> inside <%s>, expected <%s>",
		      (const char *) child->name,
		      (const char *) parent->name,
		      expected_child);
	  return FALSE;
	}

      if (!parse_child(context, child, error))
	return FALSE;
    }

  return TRUE;
}

static gboolean
parse_string_content(xmlNodePtr node, gchar **out, GError **error)
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

static gboolean
parse_uint64_content(xmlNodePtr node, guint64 *out, GError **error)
{
  xmlChar *content;
  gchar *end = NULL;
  unsigned long long value;

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

  value = g_ascii_strtoull((const char *) content, &end, 10);
  if (end == (const char *) content || *end != '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "element <%s> has invalid integer content: %s",
		  (const char *) node->name, (const char *) content);
      xmlFree(content);
      return FALSE;
    }

  *out = (guint64) value;
  xmlFree(content);
  return TRUE;
}

static gboolean
parse_st_version_content(xmlNodePtr node, STVersion *out, GError **error)
{
  xmlChar *content;
  gchar *end = NULL;
  const gchar *p;
  unsigned long major, minor, micro;

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

  p = (const gchar *) content;
  major = g_ascii_strtoull(p, &end, 10);
  if (end == p || *end != '.')
    goto invalid;
  p = end + 1;

  minor = g_ascii_strtoull(p, &end, 10);
  if (end == p || *end != '.')
    goto invalid;
  p = end + 1;

  micro = g_ascii_strtoull(p, &end, 10);
  if (end == p || *end != '\0')
    goto invalid;

  if (major > G_MAXUINT8 || minor > G_MAXUINT8 || micro > G_MAXUINT8)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "element <%s> version components must be 0..255 (got %s)",
		  (const char *) node->name,
		  (const char *) content);
      xmlFree(content);
      return FALSE;
    }

  out->major = (guint8) major;
  out->minor = (guint8) minor;
  out->micro = (guint8) micro;
  xmlFree(content);
  return TRUE;

invalid:
  g_set_error(error,
	      G_IO_ERROR,
	      G_IO_ERROR_INVALID_DATA,
	      "element <%s> expects version as major.minor.micro (got `%s')",
	      (const char *) node->name,
	      (const char *) content);
  xmlFree(content);
  return FALSE;
}

static gboolean parse_global_node(STCSContext *context, xmlNodePtr node, GError **error)
{
  gboolean r = FALSE;
  xmlNodePtr child;

  LOGD("parse <global>");

  for (child = node->children; child != NULL; child = child->next)
    {
      if (child->type != XML_ELEMENT_NODE)
	continue;

      if (xmlStrEqual(child->name, (const xmlChar *) "service-name"))
	{
	  if (!parse_string_content(child, &context->options.service_name, error))
	    goto out;
	}
      else if (xmlStrEqual(child->name, (const xmlChar *) "service-version"))
	{
	  if (!parse_st_version_content(child, &context->options.service_version, error))
	    goto out;
	}
      else
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "unexpected element <%s> inside <global>",
		      (const char *) child->name);
	  goto out;
	}
    }

  r = TRUE;

  LOGI("global service-name=%s service-version=%u.%u.%u",
       context->options.service_name ? context->options.service_name : "(unset)",
       context->options.service_version.major,
       context->options.service_version.minor,
       context->options.service_version.micro);

out:
  return r;
}

static gboolean parse_transport_node(STCSContext *context, xmlNodePtr node, GError **error)
{
  gboolean r = FALSE;
  xmlChar *id_attr = NULL;
  xmlChar *plugin_attr = NULL;
  const gchar *id;
  const gchar *plugin_name;
  STCSPluginEntry *entry;
  STCSPluginInfo *info;
  GObject *object = NULL;
  STCSTransport *transport;

  LOGD("parse <transport>");

  id_attr = xmlGetProp(node, (const xmlChar *) "id");
  plugin_attr = xmlGetProp(node, (const xmlChar *) "plugin");

  if (id_attr == NULL || *id_attr == '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "<transport> requires a non-empty id attribute");
      goto out;
    }

  if (plugin_attr == NULL || *plugin_attr == '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "<transport id=\"%s\"> requires a non-empty plugin attribute",
		  (const char *) id_attr);
      goto out;
    }

  id = (const gchar *) id_attr;
  plugin_name = (const gchar *) plugin_attr;

  if (g_hash_table_contains(context->transports, id))
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_EXISTS,
		  "duplicate transport id `%s'", id);
      goto out;
    }

  entry = stcs_plugin_entry_lookup(context->plugins, plugin_name);
  if (entry == NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_NOT_FOUND,
		  "transport plugin `%s' not found (id=%s)",
		  plugin_name, id);
      goto out;
    }

  info = stcs_plugin_entry_get_info(entry);
  if (stcs_plugin_info_get_category(info) != STCS_PLUGIN_CATEGORY_TRANSPORT)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "plugin `%s' is not a transport (id=%s)",
		  plugin_name, id);
      goto out;
    }

  object = stcs_plugin_entry_create(entry, error);
  if (object == NULL)
    goto out;

  if (!STCS_IS_TRANSPORT(object))
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "plugin `%s' create() did not return an STCSTransport (id=%s)",
		  plugin_name, id);
      goto out;
    }

  transport = STCS_TRANSPORT(object);
  if (!stcs_transport_configure(transport, node, error))
    goto out;

  g_hash_table_insert(context->transports, g_strdup(id), g_object_ref(transport));
  LOGI("registered transport id=%s plugin=%s", id, plugin_name);
  r = TRUE;

out:
  if (object != NULL)
    g_object_unref(object);
  if (id_attr != NULL)
    xmlFree(id_attr);
  if (plugin_attr != NULL)
    xmlFree(plugin_attr);

  return r;
}

static gboolean parse_transports_node(STCSContext *context, xmlNodePtr node, GError **error)
{
  gboolean r = FALSE;

  LOGD("parse <transports>");

  r = parse_children_of(context, node, "transport", parse_transport_node, error);
  return r;
}

static gboolean
parse_datapoint_transform_node(STCSContext *context,
			       STCSDatapoint *datapoint,
			       xmlNodePtr node,
			       GError **error)
{
  gboolean r = FALSE;
  xmlChar *plugin_attr = NULL;
  const gchar *plugin_name;
  STCSPluginEntry *entry;
  STCSPluginInfo *info;
  GObject *object = NULL;
  STCSTransform *transform;

  plugin_attr = xmlGetProp(node, (const xmlChar *) "plugin");
  if (plugin_attr == NULL || *plugin_attr == '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "<transform> inside datapoint `%s' requires a non-empty plugin attribute",
		  stcs_datapoint_get_id(datapoint));
      goto out;
    }

  plugin_name = (const gchar *) plugin_attr;
  entry = stcs_plugin_entry_lookup(context->plugins, plugin_name);
  if (entry == NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_NOT_FOUND,
		  "transform plugin `%s' not found (datapoint=%s)",
		  plugin_name, stcs_datapoint_get_id(datapoint));
      goto out;
    }

  info = stcs_plugin_entry_get_info(entry);
  if (stcs_plugin_info_get_category(info) != STCS_PLUGIN_CATEGORY_TRANSFORM)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "plugin `%s' is not a transform (datapoint=%s)",
		  plugin_name, stcs_datapoint_get_id(datapoint));
      goto out;
    }

  object = stcs_plugin_entry_create(entry, error);
  if (object == NULL)
    goto out;

  if (!STCS_IS_TRANSFORM(object))
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "plugin `%s' create() did not return an STCSTransform (datapoint=%s)",
		  plugin_name, stcs_datapoint_get_id(datapoint));
      goto out;
    }

  transform = STCS_TRANSFORM(object);
  if (!stcs_transform_configure(transform, node, error))
    goto out;

  stcs_datapoint_add_transform(datapoint, transform);
  LOGI("datapoint `%s': added transform plugin=%s",
       stcs_datapoint_get_id(datapoint), plugin_name);
  r = TRUE;

out:
  if (object != NULL)
    g_object_unref(object);
  if (plugin_attr != NULL)
    xmlFree(plugin_attr);

  return r;
}

static gboolean parse_datapoint_node(STCSContext *context, xmlNodePtr node, GError **error)
{
  gboolean r = FALSE;
  xmlChar *id_attr = NULL;
  const gchar *id;
  STCSDatapoint *datapoint = NULL;
  xmlNodePtr child;

  LOGD("parse <datapoint>");

  id_attr = xmlGetProp(node, (const xmlChar *) "id");
  if (id_attr == NULL || *id_attr == '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "<datapoint> requires a non-empty id attribute");
      goto out;
    }

  id = (const gchar *) id_attr;
  if (g_hash_table_contains(context->datapoints, id))
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_EXISTS,
		  "duplicate datapoint id `%s'", id);
      goto out;
    }

  datapoint = stcs_datapoint_new(id);
  if (datapoint == NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_FAILED,
		  "unable to create datapoint `%s'", id);
      goto out;
    }

  for (child = node->children; child != NULL; child = child->next)
    {
      if (child->type != XML_ELEMENT_NODE)
	continue;

      if (!xmlStrEqual(child->name, (const xmlChar *) "transform"))
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "unexpected element <%s> inside <datapoint id=\"%s\">, expected <transform>",
		      (const char *) child->name, id);
	  goto out;
	}

      if (!parse_datapoint_transform_node(context, datapoint, child, error))
	goto out;
    }

  g_hash_table_insert(context->datapoints, g_strdup(id), g_object_ref(datapoint));
  LOGI("registered datapoint id=%s", id);
  r = TRUE;

out:
  if (datapoint != NULL)
    g_object_unref(datapoint);
  if (id_attr != NULL)
    xmlFree(id_attr);

  return r;
}

static gboolean parse_datapoints_node(STCSContext *context, xmlNodePtr node, GError **error)
{
  gboolean r = FALSE;

  LOGD("parse <datapoints>");

  r = parse_children_of(context, node, "datapoint", parse_datapoint_node, error);
  return r;
}

static gboolean parse_reader_node(STCSContext *context, xmlNodePtr node, GError **error)
{
  gboolean r = FALSE;
  xmlChar *id_attr = NULL;
  xmlChar *plugin_attr = NULL;
  xmlChar *transport_attr = NULL;
  const gchar *id;
  const gchar *plugin_name;
  const gchar *transport_id;
  STCSPluginEntry *entry;
  STCSPluginInfo *info;
  GObject *object = NULL;
  STCSReader *reader;
  STCSTransport *transport;

  LOGD("parse <reader>");

  id_attr = xmlGetProp(node, (const xmlChar *) "id");
  plugin_attr = xmlGetProp(node, (const xmlChar *) "plugin");
  transport_attr = xmlGetProp(node, (const xmlChar *) "transport");

  if (id_attr == NULL || *id_attr == '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "<reader> requires a non-empty id attribute");
      goto out;
    }

  if (plugin_attr == NULL || *plugin_attr == '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "<reader id=\"%s\"> requires a non-empty plugin attribute",
		  (const char *) id_attr);
      goto out;
    }

  if (transport_attr == NULL || *transport_attr == '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "<reader id=\"%s\"> requires a non-empty transport attribute",
		  (const char *) id_attr);
      goto out;
    }

  id = (const gchar *) id_attr;
  plugin_name = (const gchar *) plugin_attr;
  transport_id = (const gchar *) transport_attr;

  if (g_hash_table_contains(context->readers, id))
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_EXISTS,
		  "duplicate reader id `%s'", id);
      goto out;
    }

  entry = stcs_plugin_entry_lookup(context->plugins, plugin_name);
  if (entry == NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_NOT_FOUND,
		  "reader plugin `%s' not found (id=%s)",
		  plugin_name, id);
      goto out;
    }

  info = stcs_plugin_entry_get_info(entry);
  if (stcs_plugin_info_get_category(info) != STCS_PLUGIN_CATEGORY_READER)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "plugin `%s' is not a reader (id=%s)",
		  plugin_name, id);
      goto out;
    }

  transport = g_hash_table_lookup(context->transports, transport_id);
  if (transport == NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_NOT_FOUND,
		  "transport `%s' not found (reader=%s)",
		  transport_id, id);
      goto out;
    }

  if (!config_claim_transport(context, transport, transport_id, "reader", id, error))
    goto out;

  object = stcs_plugin_entry_create(entry, error);
  if (object == NULL)
    goto out;

  if (!STCS_IS_READER(object))
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "plugin `%s' create() did not return an STCSReader (id=%s)",
		  plugin_name, id);
      goto out;
    }

  reader = STCS_READER(object);

  if (!stcs_reader_set_transport(reader, transport, error))
    goto out;

  if (!stcs_reader_configure(reader, node, config_lookup_datapoint, context, error))
    goto out;

  g_hash_table_insert(context->readers, g_strdup(id), g_object_ref(reader));
  LOGI("registered reader id=%s plugin=%s transport=%s", id, plugin_name, transport_id);
  r = TRUE;

out:
  if (object != NULL)
    g_object_unref(object);
  if (id_attr != NULL)
    xmlFree(id_attr);
  if (plugin_attr != NULL)
    xmlFree(plugin_attr);
  if (transport_attr != NULL)
    xmlFree(transport_attr);

  return r;
}

static gboolean parse_readers_node(STCSContext *context, xmlNodePtr node, GError **error)
{
  gboolean r = FALSE;

  LOGD("parse <readers>");

  r = parse_children_of(context, node, "reader", parse_reader_node, error);
  return r;
}

static gboolean parse_writer_node(STCSContext *context, xmlNodePtr node, GError **error)
{
  gboolean r = FALSE;
  xmlChar *id_attr = NULL;
  xmlChar *plugin_attr = NULL;
  xmlChar *transport_attr = NULL;
  const gchar *id;
  const gchar *plugin_name;
  const gchar *transport_id;
  STCSPluginEntry *entry;
  STCSPluginInfo *info;
  GObject *object = NULL;
  STCSWriter *writer;
  STCSTransport *transport;
  xmlNodePtr child;

  LOGD("parse <writer>");

  id_attr = xmlGetProp(node, (const xmlChar *) "id");
  plugin_attr = xmlGetProp(node, (const xmlChar *) "plugin");
  transport_attr = xmlGetProp(node, (const xmlChar *) "transport");

  if (id_attr == NULL || *id_attr == '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "<writer> requires a non-empty id attribute");
      goto out;
    }

  if (plugin_attr == NULL || *plugin_attr == '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "<writer id=\"%s\"> requires a non-empty plugin attribute",
		  (const char *) id_attr);
      goto out;
    }

  if (transport_attr == NULL || *transport_attr == '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "<writer id=\"%s\"> requires a non-empty transport attribute",
		  (const char *) id_attr);
      goto out;
    }

  id = (const gchar *) id_attr;
  plugin_name = (const gchar *) plugin_attr;
  transport_id = (const gchar *) transport_attr;

  if (g_hash_table_contains(context->writers, id))
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_EXISTS,
		  "duplicate writer id `%s'", id);
      goto out;
    }

  entry = stcs_plugin_entry_lookup(context->plugins, plugin_name);
  if (entry == NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_NOT_FOUND,
		  "writer plugin `%s' not found (id=%s)",
		  plugin_name, id);
      goto out;
    }

  info = stcs_plugin_entry_get_info(entry);
  if (stcs_plugin_info_get_category(info) != STCS_PLUGIN_CATEGORY_WRITER)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "plugin `%s' is not a writer (id=%s)",
		  plugin_name, id);
      goto out;
    }

  transport = g_hash_table_lookup(context->transports, transport_id);
  if (transport == NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_NOT_FOUND,
		  "transport `%s' not found (writer=%s)",
		  transport_id, id);
      goto out;
    }

  if (!config_claim_transport(context, transport, transport_id, "writer", id, error))
    goto out;

  object = stcs_plugin_entry_create(entry, error);
  if (object == NULL)
    goto out;

  if (!STCS_IS_WRITER(object))
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "plugin `%s' create() did not return an STCSWriter (id=%s)",
		  plugin_name, id);
      goto out;
    }

  writer = STCS_WRITER(object);

  if (!stcs_writer_set_transport(writer, transport, error))
    goto out;

  for (child = node->children; child != NULL; child = child->next)
    {
      if (child->type != XML_ELEMENT_NODE)
	continue;

      if (xmlStrEqual(child->name, (const xmlChar *) "write-attempts"))
	{
	  guint64 attempts = 0;

	  if (!parse_uint64_content(child, &attempts, error))
	    goto out;

	  if (attempts == 0 || attempts > G_MAXUINT)
	    {
	      g_set_error(error,
			  G_IO_ERROR,
			  G_IO_ERROR_INVALID_DATA,
			  "<write-attempts> for writer `%s' must be in 1..%u",
			  id, G_MAXUINT);
	      goto out;
	    }

	  stcs_writer_set_write_attempts(writer, (guint) attempts);
	}
    }

  if (!stcs_writer_configure(writer, node, config_lookup_datapoint, context, error))
    goto out;

  g_hash_table_insert(context->writers, g_strdup(id), g_object_ref(writer));
  LOGI("registered writer id=%s plugin=%s transport=%s write-attempts=%u",
       id, plugin_name, transport_id, stcs_writer_get_write_attempts(writer));
  r = TRUE;

out:
  if (object != NULL)
    g_object_unref(object);
  if (id_attr != NULL)
    xmlFree(id_attr);
  if (plugin_attr != NULL)
    xmlFree(plugin_attr);
  if (transport_attr != NULL)
    xmlFree(transport_attr);

  return r;
}

static gboolean parse_writers_node(STCSContext *context, xmlNodePtr node, GError **error)
{
  gboolean r = FALSE;

  LOGD("parse <writers>");

  r = parse_children_of(context, node, "writer", parse_writer_node, error);
  return r;
}

static gboolean
parse_double_attr(xmlNodePtr node,
		  const char *attr_name,
		  gboolean required,
		  gdouble default_value,
		  gdouble *out,
		  GError **error)
{
  xmlChar *attr;
  gchar *end = NULL;
  gdouble value;

  attr = xmlGetProp(node, (const xmlChar *) attr_name);
  if (attr == NULL || *attr == '\0')
    {
      if (attr != NULL)
	xmlFree(attr);

      if (required)
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "<%s> requires a non-empty %s attribute",
		      (const char *) node->name, attr_name);
	  return FALSE;
	}

      *out = default_value;
      return TRUE;
    }

  value = g_ascii_strtod((const char *) attr, &end);
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

  *out = value;
  xmlFree(attr);
  return TRUE;
}

static gboolean
parse_st_unit_attr(xmlNodePtr node, STUnit *out, GError **error)
{
  xmlChar *attr;
  gchar *key;
  gboolean r = FALSE;

  attr = xmlGetProp(node, (const xmlChar *) "unit");
  if (attr == NULL || *attr == '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "<%s> requires a non-empty unit attribute",
		  (const char *) node->name);
      if (attr != NULL)
	xmlFree(attr);
      return FALSE;
    }

  key = g_ascii_strdown((const char *) attr, -1);

  if (g_strcmp0(key, "null") == 0)
    *out = ST_UNIT_NULL;
  else if (g_strcmp0(key, "celsius") == 0)
    *out = ST_UNIT_CELSIUS;
  else if (g_strcmp0(key, "percent") == 0)
    *out = ST_UNIT_PERCENT;
  else if (g_strcmp0(key, "bar") == 0)
    *out = ST_UNIT_BAR;
  else if (g_strcmp0(key, "onoff") == 0)
    *out = ST_UNIT_ONOFF;
  else if (g_strcmp0(key, "enum") == 0)
    *out = ST_UNIT_ENUM;
  else if (g_strcmp0(key, "v") == 0)
    *out = ST_UNIT_V;
  else if (g_strcmp0(key, "w") == 0)
    *out = ST_UNIT_W;
  else if (g_strcmp0(key, "var") == 0)
    *out = ST_UNIT_VAR;
  else if (g_strcmp0(key, "kw") == 0)
    *out = ST_UNIT_KW;
  else if (g_strcmp0(key, "wh") == 0)
    *out = ST_UNIT_WH;
  else if (g_strcmp0(key, "kwh") == 0)
    *out = ST_UNIT_KWH;
  else if (g_strcmp0(key, "seconds") == 0)
    *out = ST_UNIT_SECONDS;
  else if (g_strcmp0(key, "minutes") == 0)
    *out = ST_UNIT_MINUTES;
  else if (g_strcmp0(key, "hours") == 0)
    *out = ST_UNIT_HOURS;
  else
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "<%s> has unknown unit `%s'",
		  (const char *) node->name, (const char *) attr);
      goto out;
    }

  r = TRUE;

out:
  g_free(key);
  xmlFree(attr);
  return r;
}

static gboolean
parse_flags_attr(xmlNodePtr node, guint64 *out, GError **error)
{
  xmlChar *attr;
  gchar **tokens;
  guint64 flags = ST_IO_FLAG_NULL;
  guint i;

  attr = xmlGetProp(node, (const xmlChar *) "flags");
  if (attr == NULL || *attr == '\0')
    {
      if (attr != NULL)
	xmlFree(attr);
      *out = ST_IO_FLAG_NULL;
      return TRUE;
    }

  tokens = g_strsplit((const char *) attr, ",", -1);
  for (i = 0; tokens[i] != NULL; i++)
    {
      gchar *token;

      g_strstrip(tokens[i]);
      if (*tokens[i] == '\0')
	continue;

      token = g_ascii_strdown(tokens[i], -1);
      if (g_strcmp0(token, "no-history") == 0)
	flags |= ST_IO_FLAG_NO_HISTORY;
      else
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "<%s> has unknown flags token `%s'",
		      (const char *) node->name, tokens[i]);
	  g_free(token);
	  g_strfreev(tokens);
	  xmlFree(attr);
	  return FALSE;
	}
      g_free(token);
    }

  g_strfreev(tokens);
  xmlFree(attr);
  *out = flags;
  return TRUE;
}

static gboolean
stcs_context_has_input_name(STCSContext *context, const gchar *name)
{
  guint i;

  for (i = 0; i < context->inputs->len; i++)
    {
      STInput *input = g_ptr_array_index(context->inputs, i);

      if (g_strcmp0(st_input_get_name(input), name) == 0)
	return TRUE;
    }

  return FALSE;
}

static gboolean parse_input_node(STCSContext *context, xmlNodePtr node, GError **error)
{
  gboolean r = FALSE;
  xmlChar *name_attr = NULL;
  xmlChar *description_attr = NULL;
  const gchar *name;
  const gchar *description;
  STUnit unit = ST_UNIT_NULL;
  gdouble min = 0.0;
  gdouble max = 0.0;
  gdouble step = 0.0;
  gdouble val = 0.0;
  guint64 flags = ST_IO_FLAG_NULL;
  STInput *input = NULL;

  LOGD("parse <input>");

  name_attr = xmlGetProp(node, (const xmlChar *) "name");
  if (name_attr == NULL || *name_attr == '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "<input> requires a non-empty name attribute");
      goto out;
    }

  name = (const gchar *) name_attr;

  if (stcs_context_has_input_name(context, name))
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_EXISTS,
		  "duplicate input name `%s'", name);
      goto out;
    }

  description_attr = xmlGetProp(node, (const xmlChar *) "description");
  description = (description_attr != NULL) ? (const gchar *) description_attr : "";

  if (!parse_st_unit_attr(node, &unit, error))
    goto out;
  if (!parse_double_attr(node, "min", TRUE, 0.0, &min, error))
    goto out;
  if (!parse_double_attr(node, "max", TRUE, 0.0, &max, error))
    goto out;
  if (!parse_double_attr(node, "step", TRUE, 0.0, &step, error))
    goto out;
  if (!parse_double_attr(node, "val", TRUE, 0.0, &val, error))
    goto out;
  if (!parse_flags_attr(node, &flags, error))
    goto out;

  /* Binding fields (reader, period_ms, id, datapoint) intentionally ignored for now. */

  input = st_input_new(name, description, unit, min, max, step, val, flags);
  if (input == NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_FAILED,
		  "st_input_new failed for input `%s'", name);
      goto out;
    }

  g_ptr_array_add(context->inputs, input);
  input = NULL;

  LOGI("registered input name=%s unit=%d min=%g max=%g step=%g val=%g",
       name, (int) unit, min, max, step, val);
  r = TRUE;

out:
  if (input != NULL)
    g_object_unref(input);
  if (name_attr != NULL)
    xmlFree(name_attr);
  if (description_attr != NULL)
    xmlFree(description_attr);

  return r;
}

static gboolean parse_inputs_node(STCSContext *context, xmlNodePtr node, GError **error)
{
  gboolean r = FALSE;

  LOGD("parse <inputs>");

  r = parse_children_of(context, node, "input", parse_input_node, error);
  return r;
}

static STInput *
stcs_context_find_input_by_name(STCSContext *context, const gchar *name)
{
  guint i;

  for (i = 0; i < context->inputs->len; i++)
    {
      STInput *input = g_ptr_array_index(context->inputs, i);

      if (g_strcmp0(st_input_get_name(input), name) == 0)
	return input;
    }

  return NULL;
}

static STOutput *
stcs_context_find_output_by_name(STCSContext *context, const gchar *name)
{
  guint i;

  for (i = 0; i < context->outputs->len; i++)
    {
      STOutput *output = g_ptr_array_index(context->outputs, i);

      if (g_strcmp0(st_output_get_name(output), name) == 0)
	return output;
    }

  return NULL;
}

static gboolean
stcs_binding_datapoint_bound(STCSContext *context, const gchar *datapoint_id)
{
  guint i;

  for (i = 0; i < context->bindings->len; i++)
    {
      STCSBinding *b = g_ptr_array_index(context->bindings, i);

      if (g_strcmp0(b->datapoint_id, datapoint_id) == 0)
	return TRUE;
    }

  return FALSE;
}

static gboolean
stcs_binding_output_has_role(STCSContext *context,
			     const gchar *output_name,
			     STCSBindingRole role)
{
  guint i;

  for (i = 0; i < context->bindings->len; i++)
    {
      STCSBinding *b = g_ptr_array_index(context->bindings, i);

      if (b->kind == STCS_BINDING_KIND_OUTPUT
	  && b->role == role
	  && g_strcmp0(b->peer_name, output_name) == 0)
	return TRUE;
    }

  return FALSE;
}

static gboolean
parse_duration_text(const char *text,
		    guint64 *out_ms,
		    gboolean allow_zero,
		    const char *what,
		    GError **error)
{
  gchar *end = NULL;
  guint64 v;
  guint64 mult = 1;

  v = g_ascii_strtoull(text, &end, 10);
  if (end == text || (!allow_zero && v == 0))
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "invalid %s `%s'",
		  what,
		  text);
      return FALSE;
    }

  if (*end == '\0')
    {
      /* bare integer = milliseconds */
      *out_ms = v;
      return TRUE;
    }

  if (g_strcmp0(end, "ms") == 0)
    mult = 1;
  else if (g_strcmp0(end, "s") == 0)
    mult = 1000;
  else if (g_strcmp0(end, "m") == 0)
    mult = 60 * 1000;
  else if (g_strcmp0(end, "h") == 0)
    mult = 3600 * 1000;
  else
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "invalid %s suffix in `%s' (use ms, s, m, h)",
		  what,
		  text);
      return FALSE;
    }

  if (v > G_MAXUINT64 / mult)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "%s `%s' is too large",
		  what,
		  text);
      return FALSE;
    }

  *out_ms = v * mult;
  return TRUE;
}

static gboolean
parse_override_timeout_content(xmlNodePtr node, guint64 *out_ms, GError **error)
{
  xmlChar *content;
  const char *text;
  gboolean ok;

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

  text = g_strstrip((char *) content);
  ok = parse_duration_text(text, out_ms, FALSE, "<override-timeout>", error);
  xmlFree(content);
  return ok;
}

#define STCS_DEFAULT_SETTLE_DELAY_MS ((guint64) 5000)

static gboolean
stcs_binding_datapoint_has_kind(STCSContext *context,
				const gchar *datapoint_id,
				STCSBindingKind kind)
{
  guint i;

  for (i = 0; i < context->bindings->len; i++)
    {
      STCSBinding *b = g_ptr_array_index(context->bindings, i);

      if (b->kind == kind && g_strcmp0(b->datapoint_id, datapoint_id) == 0)
	return TRUE;
    }

  return FALSE;
}

static gboolean
stcs_binding_exists(STCSContext *context,
		    STCSBindingKind kind,
		    const gchar *datapoint_id,
		    const gchar *peer_name)
{
  guint i;

  for (i = 0; i < context->bindings->len; i++)
    {
      STCSBinding *b = g_ptr_array_index(context->bindings, i);

      if (b->kind == kind
	  && g_strcmp0(b->datapoint_id, datapoint_id) == 0
	  && g_strcmp0(b->peer_name, peer_name) == 0)
	return TRUE;
    }

  return FALSE;
}

static gboolean
stcs_binding_peer_in_use(STCSContext *context,
			 STCSBindingKind kind,
			 const gchar *peer_name)
{
  guint i;

  for (i = 0; i < context->bindings->len; i++)
    {
      STCSBinding *b = g_ptr_array_index(context->bindings, i);

      if (b->kind == kind && g_strcmp0(b->peer_name, peer_name) == 0)
	return TRUE;
    }

  return FALSE;
}

static gboolean parse_bind_node(STCSContext *context, xmlNodePtr node, GError **error)
{
  gboolean r = FALSE;
  xmlChar *datapoint_attr = NULL;
  xmlChar *input_attr = NULL;
  xmlChar *output_attr = NULL;
  xmlChar *role_attr = NULL;
  xmlChar *settle_attr = NULL;
  const gchar *datapoint_id;
  STCSBindingKind kind;
  STCSBindingRole role = STCS_BINDING_ROLE_COMMAND;
  const gchar *peer_name;
  STCSBinding *binding = NULL;
  guint64 settle_delay_ms = 0;
  gboolean have_settle_delay = FALSE;

  LOGD("parse <bind>");

  datapoint_attr = xmlGetProp(node, (const xmlChar *) "datapoint");
  input_attr = xmlGetProp(node, (const xmlChar *) "input");
  output_attr = xmlGetProp(node, (const xmlChar *) "output");
  role_attr = xmlGetProp(node, (const xmlChar *) "role");
  settle_attr = xmlGetProp(node, (const xmlChar *) "settle-delay");

  if (datapoint_attr == NULL || *datapoint_attr == '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "<bind> requires a non-empty datapoint attribute");
      goto out;
    }

  datapoint_id = (const gchar *) datapoint_attr;

  if (!g_hash_table_contains(context->datapoints, datapoint_id))
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_NOT_FOUND,
		  "<bind> references unknown datapoint `%s'",
		  datapoint_id);
      goto out;
    }

  if (role_attr != NULL && *role_attr != '\0')
    {
      if (g_strcmp0((const char *) role_attr, "command") == 0)
	role = STCS_BINDING_ROLE_COMMAND;
      else if (g_strcmp0((const char *) role_attr, "feedback") == 0)
	role = STCS_BINDING_ROLE_FEEDBACK;
      else
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "<bind> unknown role `%s' (expected command|feedback)",
		      (const char *) role_attr);
	  goto out;
	}
    }

  if (settle_attr != NULL)
    {
      gchar *stripped;

      if (role != STCS_BINDING_ROLE_FEEDBACK)
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "<bind datapoint=\"%s\"> settle-delay is only valid with role=feedback",
		      datapoint_id);
	  goto out;
	}

      stripped = g_strdup((const char *) settle_attr);
      g_strstrip(stripped);
      if (*stripped == '\0')
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "<bind datapoint=\"%s\"> settle-delay is empty",
		      datapoint_id);
	  g_free(stripped);
	  goto out;
	}

      if (!parse_duration_text(stripped,
			       &settle_delay_ms,
			       TRUE,
			       "settle-delay",
			       error))
	{
	  g_free(stripped);
	  goto out;
	}
      g_free(stripped);
      have_settle_delay = TRUE;
    }

  {
    gboolean has_input = (input_attr != NULL && *input_attr != '\0');
    gboolean has_output = (output_attr != NULL && *output_attr != '\0');

    if (has_input == has_output)
      {
	g_set_error(error,
		    G_IO_ERROR,
		    G_IO_ERROR_INVALID_DATA,
		    "<bind datapoint=\"%s\"> requires exactly one of input= or output=",
		    datapoint_id);
	goto out;
      }

    if (has_input)
      {
	kind = STCS_BINDING_KIND_INPUT;
	peer_name = (const gchar *) input_attr;
	if (role == STCS_BINDING_ROLE_FEEDBACK)
	  {
	    g_set_error(error,
			G_IO_ERROR,
			G_IO_ERROR_INVALID_DATA,
			"<bind datapoint=\"%s\"> role=feedback requires output=",
			datapoint_id);
	    goto out;
	  }
      }
    else
      {
	kind = STCS_BINDING_KIND_OUTPUT;
	peer_name = (const gchar *) output_attr;
      }
  }

  if (stcs_binding_datapoint_bound(context, datapoint_id))
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_EXISTS,
		  "datapoint `%s' is already bound",
		  datapoint_id);
      goto out;
    }

  if (kind == STCS_BINDING_KIND_INPUT)
    {
      if (stcs_context_find_input_by_name(context, peer_name) == NULL)
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_NOT_FOUND,
		      "<bind> references unknown input `%s'",
		      peer_name);
	  goto out;
	}

      if (stcs_binding_peer_in_use(context, STCS_BINDING_KIND_INPUT, peer_name))
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_EXISTS,
		      "input `%s' is already bound to a datapoint",
		      peer_name);
	  goto out;
	}
    }
  else
    {
      if (stcs_context_find_output_by_name(context, peer_name) == NULL)
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_NOT_FOUND,
		      "<bind> references unknown output `%s'",
		      peer_name);
	  goto out;
	}

      if (stcs_binding_output_has_role(context, peer_name, role))
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_EXISTS,
		      "output `%s' already has a %s binding",
		      peer_name,
		      role == STCS_BINDING_ROLE_FEEDBACK ? "feedback" : "command");
	  goto out;
	}
    }

  binding = g_new0(STCSBinding, 1);
  binding->kind = kind;
  binding->role = role;
  binding->datapoint_id = g_strdup(datapoint_id);
  binding->peer_name = g_strdup(peer_name);
  binding->handler_id = 0;
  if (role == STCS_BINDING_ROLE_FEEDBACK)
    binding->settle_delay_ms =
      have_settle_delay ? settle_delay_ms : STCS_DEFAULT_SETTLE_DELAY_MS;
  else
    binding->settle_delay_ms = 0;

  g_ptr_array_add(context->bindings, binding);
  binding = NULL;

  if (kind == STCS_BINDING_KIND_INPUT)
    LOGI("registered bind datapoint=%s input=%s", datapoint_id, peer_name);
  else if (role == STCS_BINDING_ROLE_FEEDBACK)
    LOGI("registered bind datapoint=%s output=%s role=feedback settle_delay_ms=%" G_GUINT64_FORMAT,
	 datapoint_id,
	 peer_name,
	 have_settle_delay ? settle_delay_ms : STCS_DEFAULT_SETTLE_DELAY_MS);
  else
    LOGI("registered bind datapoint=%s output=%s role=command",
	 datapoint_id,
	 peer_name);
  r = TRUE;

out:
  stcs_binding_free(binding);
  if (datapoint_attr != NULL)
    xmlFree(datapoint_attr);
  if (input_attr != NULL)
    xmlFree(input_attr);
  if (output_attr != NULL)
    xmlFree(output_attr);
  if (role_attr != NULL)
    xmlFree(role_attr);
  if (settle_attr != NULL)
    xmlFree(settle_attr);

  return r;
}

static gboolean parse_bindings_node(STCSContext *context, xmlNodePtr node, GError **error)
{
  gboolean r = FALSE;

  LOGD("parse <bindings>");

  r = parse_children_of(context, node, "bind", parse_bind_node, error);
  return r;
}

static gboolean parse_output_node(STCSContext *context, xmlNodePtr node, GError **error)
{
  gboolean r = FALSE;
  xmlChar *name_attr = NULL;
  xmlChar *description_attr = NULL;
  const gchar *name;
  const gchar *description;
  STUnit unit = ST_UNIT_NULL;
  gdouble min = 0.0;
  gdouble max = 0.0;
  gdouble step = 0.0;
  gdouble val = 0.0;
  guint64 flags = ST_IO_FLAG_NULL;
  guint64 override_timeout_ms = 0;
  gboolean have_override_timeout = FALSE;
  STOutput *output = NULL;
  STCSOutputRuntime *runtime = NULL;
  xmlNodePtr child;
  guint i;

  LOGD("parse <output>");

  name_attr = xmlGetProp(node, (const xmlChar *) "name");
  if (name_attr == NULL || *name_attr == '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "<output> requires a non-empty name attribute");
      goto out;
    }

  name = (const gchar *) name_attr;

  for (i = 0; i < context->outputs->len; i++)
    {
      STOutput *existing = g_ptr_array_index(context->outputs, i);

      if (g_strcmp0(st_output_get_name(existing), name) == 0)
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_EXISTS,
		      "duplicate output name `%s'", name);
	  goto out;
	}
    }

  description_attr = xmlGetProp(node, (const xmlChar *) "description");
  description = (description_attr != NULL) ? (const gchar *) description_attr : "";

  if (!parse_st_unit_attr(node, &unit, error))
    goto out;
  if (!parse_double_attr(node, "min", TRUE, 0.0, &min, error))
    goto out;
  if (!parse_double_attr(node, "max", TRUE, 0.0, &max, error))
    goto out;
  if (!parse_double_attr(node, "step", TRUE, 0.0, &step, error))
    goto out;
  if (!parse_double_attr(node, "val", TRUE, 0.0, &val, error))
    goto out;
  if (!parse_flags_attr(node, &flags, error))
    goto out;

  for (child = node->children; child != NULL; child = child->next)
    {
      if (child->type != XML_ELEMENT_NODE)
	continue;

      if (xmlStrEqual(child->name, (const xmlChar *) "override-timeout"))
	{
	  if (have_override_timeout)
	    {
	      g_set_error(error,
			  G_IO_ERROR,
			  G_IO_ERROR_INVALID_DATA,
			  "<output name=\"%s\"> has duplicate <override-timeout>",
			  name);
	      goto out;
	    }

	  if (!parse_override_timeout_content(child, &override_timeout_ms, error))
	    goto out;
	  have_override_timeout = TRUE;
	}
      else
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "unexpected element <%s> inside <output name=\"%s\">",
		      (const char *) child->name, name);
	  goto out;
	}
    }

  output = st_output_new(name, description, unit, min, max, step, val, flags);
  if (output == NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_FAILED,
		  "st_output_new failed for output `%s'", name);
      goto out;
    }

  runtime = stcs_output_runtime_new(context, output, override_timeout_ms);
  g_hash_table_insert(context->output_runtimes, g_strdup(name), runtime);
  runtime = NULL;

  g_ptr_array_add(context->outputs, output);
  output = NULL;

  LOGI("registered output name=%s unit=%d min=%g max=%g step=%g val=%g override_timeout_ms=%" G_GUINT64_FORMAT,
       name, (int) unit, min, max, step, val, override_timeout_ms);
  r = TRUE;

out:
  if (output != NULL)
    g_object_unref(output);
  if (name_attr != NULL)
    xmlFree(name_attr);
  if (description_attr != NULL)
    xmlFree(description_attr);

  return r;
}

static gboolean parse_outputs_node(STCSContext *context, xmlNodePtr node, GError **error)
{
  gboolean r = FALSE;

  LOGD("parse <outputs>");

  r = parse_children_of(context, node, "output", parse_output_node, error);
  return r;
}

static gboolean parse_stcs_child(STCSContext *context, xmlNodePtr node, GError **error)
{
  const char *name = (const char *) node->name;

  if (g_strcmp0(name, "global") == 0)
    return parse_global_node(context, node, error);
  if (g_strcmp0(name, "transports") == 0)
    return parse_transports_node(context, node, error);
  if (g_strcmp0(name, "datapoints") == 0)
    return parse_datapoints_node(context, node, error);
  if (g_strcmp0(name, "readers") == 0)
    return parse_readers_node(context, node, error);
  if (g_strcmp0(name, "writers") == 0)
    return parse_writers_node(context, node, error);
  if (g_strcmp0(name, "inputs") == 0)
    return parse_inputs_node(context, node, error);
  if (g_strcmp0(name, "outputs") == 0)
    return parse_outputs_node(context, node, error);
  if (g_strcmp0(name, "bindings") == 0)
    return parse_bindings_node(context, node, error);

  g_set_error(error,
	      G_IO_ERROR,
	      G_IO_ERROR_INVALID_DATA,
	      "unexpected element <%s> inside <stcs>",
	      name);
  return FALSE;
}

/*
 * Run config preprocessor (same idea as starter-core rules-preprocessor).
 * Template may contain {F} replaced with @path. On success returns stdout
 * (transfer full); on failure sets @error and returns NULL.
 */
static gchar *
stcs_config_run_preprocessor(const gchar *preprocessor,
			     const gchar *path,
			     GError **error)
{
  GString *cmd;
  gint spawn_argc = 0;
  gchar **spawn_argv = NULL;
  gchar *stdout_buf = NULL;
  gchar *stderr_buf = NULL;
  gint exit_status = 0;
  GError *local_error = NULL;
  gchar *result = NULL;

  cmd = g_string_new(preprocessor);
  g_string_replace(cmd, "{F}", path, 0);

  LOGD("preprocessor command: `%s'", cmd->str);

  if (!g_shell_parse_argv(cmd->str, &spawn_argc, &spawn_argv, &local_error))
    {
      g_propagate_error(error, local_error);
      g_string_free(cmd, TRUE);
      return NULL;
    }

  if (!g_spawn_sync(NULL,
		    spawn_argv,
		    NULL,
		    G_SPAWN_SEARCH_PATH,
		    NULL, NULL,
		    &stdout_buf, &stderr_buf, &exit_status, &local_error))
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_FAILED,
		  "failed to run config preprocessor: %s",
		  local_error ? local_error->message : "unknown error");
      g_clear_error(&local_error);
      goto out;
    }

  if (exit_status != 0)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_FAILED,
		  "config preprocessor exited with status %d%s%s",
		  exit_status,
		  (stderr_buf && *stderr_buf) ? ": " : "",
		  (stderr_buf && *stderr_buf) ? stderr_buf : "");
      g_free(stdout_buf);
      stdout_buf = NULL;
      goto out;
    }

  if (stdout_buf == NULL || *stdout_buf == '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_FAILED,
		  "config preprocessor produced empty output");
      g_free(stdout_buf);
      stdout_buf = NULL;
      goto out;
    }

  result = stdout_buf;
  stdout_buf = NULL;

out:
  g_free(stdout_buf);
  g_free(stderr_buf);
  g_strfreev(spawn_argv);
  g_string_free(cmd, TRUE);
  return result;
}

gboolean stcs_internals_config_parse(STCSContext *context, const gchar *path, GError **error)
{
  gboolean r = FALSE;
  xmlDocPtr doc = NULL;
  xmlNodePtr root;
  xmlNodePtr child;
  const gchar *preprocessor;
  gchar *preprocessed = NULL;

  g_return_val_if_fail(context != NULL, FALSE);
  g_return_val_if_fail(path != NULL, FALSE);

  preprocessor = context->options.config_preprocessor;
  LOGD("parsing config file: %s (preprocessor: %s)",
       path,
       (preprocessor && *preprocessor) ? preprocessor : "(none)");

  if (preprocessor != NULL && *preprocessor != '\0')
    {
      preprocessed = stcs_config_run_preprocessor(preprocessor, path, error);
      if (preprocessed == NULL)
	goto out;

      doc = xmlReadMemory(preprocessed, (int) strlen(preprocessed), path, NULL, 0);
      if (doc == NULL)
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_FAILED,
		      "failed to parse preprocessed XML config from: %s",
		      path);
	  goto out;
	}
    }
  else
    {
      doc = xmlReadFile(path, NULL, 0);
      if (doc == NULL)
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_FAILED,
		      "failed to parse XML config file: %s",
		      path);
	  goto out;
	}
    }

  root = xmlDocGetRootElement(doc);
  if (root == NULL || !xmlStrEqual(root->name, (const xmlChar *) "stcs"))
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "config root element must be <stcs>");
      goto out;
    }

  for (child = root->children; child != NULL; child = child->next)
    {
      if (child->type != XML_ELEMENT_NODE)
	continue;

      if (!parse_stcs_child(context, child, error))
	goto out;
    }

  config_warn_unbound_transports(context);

  r = TRUE;

out:
  g_free(preprocessed);
  if (doc)
    xmlFreeDoc(doc);

  return r;
}
