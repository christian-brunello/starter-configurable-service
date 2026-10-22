/*
 * starter-configurable-service - adapter-transport.c
 *
 * Long-lived GDBus peer client for starter-configurable-adapter
 * (org.starter.Adapter).
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
#include <stdlib.h>

#include <glib-object.h>
#include <gmodule.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include "plugin.h"
#include "starter/configurable-service/transport.h"

#define LOGD(msg...) g_log("adapter-transport", G_LOG_LEVEL_DEBUG, msg)
#define LOGI(msg...) g_log("adapter-transport", G_LOG_LEVEL_INFO, msg)
#define LOGW(msg...) g_log("adapter-transport", G_LOG_LEVEL_WARNING, msg)

#define STCA_DBUS_OBJECT_PATH "/org/starter/adapter"
#define STCA_DBUS_INTERFACE "org.starter.Adapter"

#define STCS_ADAPTER_RECONNECT_MIN_MS 500
#define STCS_ADAPTER_RECONNECT_MAX_MS 30000

#define STCS_TYPE_ADAPTER_TRANSPORT (stcs_adapter_transport_get_type())
G_DECLARE_FINAL_TYPE(STCSAdapterTransport, stcs_adapter_transport,
		     STCS, ADAPTER_TRANSPORT, STCSTransport)

typedef enum {
  STCS_ADAPTER_MODE_READ = 0,
  STCS_ADAPTER_MODE_WRITE
} STCSAdapterMode;

struct _STCSAdapterTransport {
  STCSTransport parent_instance;

  gchar *address;
  guint port;
  STCSAdapterMode mode;

  GDBusConnection *connection;
  guint signal_sub_id;
  guint reconnect_source_id;
  guint reconnect_delay_ms;
  gboolean shutting_down;
  gboolean connect_in_progress;
};

G_DEFINE_TYPE(STCSAdapterTransport, stcs_adapter_transport, STCS_TYPE_TRANSPORT)

static void stcs_adapter_transport_schedule_reconnect(STCSAdapterTransport *self);
static void stcs_adapter_transport_try_connect(STCSAdapterTransport *self);
static void stcs_adapter_transport_on_connected(STCSAdapterTransport *self);

static gboolean
stcs_adapter_transport_parse_string(xmlNodePtr node, gchar **out, GError **error)
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

static gchar *
variant_data_to_json(GVariant *map)
{
  JsonBuilder *builder;
  JsonGenerator *gen;
  JsonNode *root;
  GVariantIter iter;
  const gchar *key;
  gdouble value;
  const gchar *access;
  gchar *text;

  builder = json_builder_new();
  json_builder_begin_object(builder);

  g_variant_iter_init(&iter, map);
  while (g_variant_iter_next(&iter, "{s(ds)}", &key, &value, &access))
    {
      json_builder_set_member_name(builder, key);
      json_builder_begin_object(builder);
      json_builder_set_member_name(builder, "value");
      json_builder_add_double_value(builder, value);
      json_builder_set_member_name(builder, "access");
      json_builder_add_string_value(builder, access != NULL ? access : "");
      json_builder_end_object(builder);
    }

  json_builder_end_object(builder);
  root = json_builder_get_root(builder);
  gen = json_generator_new();
  json_generator_set_root(gen, root);
  text = json_generator_to_data(gen, NULL);

  json_node_unref(root);
  g_object_unref(gen);
  g_object_unref(builder);
  return text;
}

static void
stcs_adapter_transport_emit_json(STCSAdapterTransport *self, GVariant *map)
{
  gchar *json;
  GBytes *bytes;

  json = variant_data_to_json(map);
  bytes = g_bytes_new_take(json, strlen(json));
  stcs_transport_emit_data_ready(STCS_TRANSPORT(self), bytes);
  g_bytes_unref(bytes);
}

static void
stcs_adapter_transport_on_data_changed(GDBusConnection *connection,
				       const gchar *sender_name,
				       const gchar *object_path,
				       const gchar *interface_name,
				       const gchar *signal_name,
				       GVariant *parameters,
				       gpointer user_data)
{
  STCSAdapterTransport *self = user_data;
  GVariant *map = NULL;

  (void) connection;
  (void) sender_name;
  (void) object_path;
  (void) interface_name;
  (void) signal_name;

  g_variant_get(parameters, "(@a{s(ds)})", &map);
  if (map != NULL)
    {
      LOGD("DataChanged (%" G_GSIZE_FORMAT " bytes variant)",
	   (gsize) g_variant_get_size(map));
      stcs_adapter_transport_emit_json(self, map);
      g_variant_unref(map);
    }
}

static void
stcs_adapter_transport_disconnect(STCSAdapterTransport *self)
{
  if (self->signal_sub_id != 0 && self->connection != NULL)
    {
      g_dbus_connection_signal_unsubscribe(self->connection, self->signal_sub_id);
      self->signal_sub_id = 0;
    }

  g_clear_object(&self->connection);
}

static void
stcs_adapter_transport_on_closed(GDBusConnection *connection,
				 gboolean remote_peer_vanished,
				 GError *error,
				 gpointer user_data)
{
  STCSAdapterTransport *self = user_data;

  (void) connection;
  (void) remote_peer_vanished;

  LOGW("connection closed%s%s",
       error ? ": " : "",
       error ? error->message : "");

  stcs_adapter_transport_disconnect(self);
  if (!self->shutting_down)
    stcs_adapter_transport_schedule_reconnect(self);
}

static void
stcs_adapter_transport_fetch_all(STCSAdapterTransport *self)
{
  GVariant *reply = NULL;
  GVariant *map = NULL;
  GError *error = NULL;

  if (self->connection == NULL || self->mode != STCS_ADAPTER_MODE_READ)
    return;

  reply = g_dbus_connection_call_sync(self->connection,
				      NULL,
				      STCA_DBUS_OBJECT_PATH,
				      STCA_DBUS_INTERFACE,
				      "GetData",
				      NULL,
				      G_VARIANT_TYPE("(a{s(ds)})"),
				      G_DBUS_CALL_FLAGS_NONE,
				      -1,
				      NULL,
				      &error);
  if (reply == NULL)
    {
      LOGW("GetData failed: %s", error ? error->message : "unknown");
      g_clear_error(&error);
      return;
    }

  g_variant_get(reply, "(@a{s(ds)})", &map);
  g_variant_unref(reply);

  if (map != NULL)
    {
      LOGI("GetData snapshot received");
      stcs_adapter_transport_emit_json(self, map);
      g_variant_unref(map);
    }
}

static void
stcs_adapter_transport_on_connected(STCSAdapterTransport *self)
{
  self->reconnect_delay_ms = STCS_ADAPTER_RECONNECT_MIN_MS;

  g_signal_connect(self->connection, "closed",
		   G_CALLBACK(stcs_adapter_transport_on_closed), self);

  if (self->mode == STCS_ADAPTER_MODE_READ)
    {
      self->signal_sub_id =
	g_dbus_connection_signal_subscribe(self->connection,
					   NULL,
					   STCA_DBUS_INTERFACE,
					   "DataChanged",
					   STCA_DBUS_OBJECT_PATH,
					   NULL,
					   G_DBUS_SIGNAL_FLAGS_NONE,
					   stcs_adapter_transport_on_data_changed,
					   self,
					   NULL);
      stcs_adapter_transport_fetch_all(self);
    }

  LOGI("connected to %s:%u mode=%s",
       self->address,
       self->port,
       self->mode == STCS_ADAPTER_MODE_READ ? "read" : "write");
}

static void
stcs_adapter_transport_connect_ready(GObject *source,
				     GAsyncResult *result,
				     gpointer user_data)
{
  STCSAdapterTransport *self = user_data;
  GError *error = NULL;
  GDBusConnection *conn;

  self->connect_in_progress = FALSE;

  conn = g_dbus_connection_new_for_address_finish(result, &error);
  if (self->shutting_down)
    {
      g_clear_object(&conn);
      g_clear_error(&error);
      return;
    }

  if (conn == NULL)
    {
      LOGW("connect failed: %s", error ? error->message : "unknown");
      g_clear_error(&error);
      stcs_adapter_transport_schedule_reconnect(self);
      return;
    }

  self->connection = conn;
  stcs_adapter_transport_on_connected(self);
  (void) source;
}

static void
stcs_adapter_transport_try_connect(STCSAdapterTransport *self)
{
  gchar *addr;

  if (self->shutting_down || self->connect_in_progress || self->connection != NULL)
    return;

  self->connect_in_progress = TRUE;
  addr = g_strdup_printf("tcp:host=%s,port=%u", self->address, self->port);
  LOGD("connecting to %s", addr);

  g_dbus_connection_new_for_address(addr,
				    G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT,
				    NULL,
				    NULL,
				    stcs_adapter_transport_connect_ready,
				    self);
  g_free(addr);
}

static gboolean
stcs_adapter_transport_reconnect_cb(gpointer user_data)
{
  STCSAdapterTransport *self = user_data;

  self->reconnect_source_id = 0;
  stcs_adapter_transport_try_connect(self);
  return G_SOURCE_REMOVE;
}

static void
stcs_adapter_transport_schedule_reconnect(STCSAdapterTransport *self)
{
  if (self->shutting_down || self->reconnect_source_id != 0)
    return;

  LOGD("reconnect in %u ms", self->reconnect_delay_ms);
  self->reconnect_source_id =
    g_timeout_add(self->reconnect_delay_ms,
		  stcs_adapter_transport_reconnect_cb,
		  self);

  self->reconnect_delay_ms = MIN(self->reconnect_delay_ms * 2,
				 STCS_ADAPTER_RECONNECT_MAX_MS);
}

static gboolean
stcs_adapter_transport_configure(STCSTransport *transport,
				 xmlNodePtr node,
				 GError **error)
{
  STCSAdapterTransport *self = STCS_ADAPTER_TRANSPORT(transport);
  xmlNodePtr child;
  gchar *mode_str = NULL;
  gchar *port_str = NULL;
  gboolean have_mode = FALSE;

  for (child = node->children; child != NULL; child = child->next)
    {
      if (child->type != XML_ELEMENT_NODE)
	continue;

      if (xmlStrEqual(child->name, (const xmlChar *) "address"))
	{
	  if (!stcs_adapter_transport_parse_string(child, &self->address, error))
	    goto fail;
	}
      else if (xmlStrEqual(child->name, (const xmlChar *) "port"))
	{
	  if (!stcs_adapter_transport_parse_string(child, &port_str, error))
	    goto fail;
	}
      else if (xmlStrEqual(child->name, (const xmlChar *) "mode"))
	{
	  if (!stcs_adapter_transport_parse_string(child, &mode_str, error))
	    goto fail;
	  have_mode = TRUE;
	}
      else
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "unexpected element <%s> in adapter-transport",
		      (const char *) child->name);
	  goto fail;
	}
    }

  if (self->address == NULL || *self->address == '\0')
    {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		  "adapter-transport requires <address>");
      goto fail;
    }

  if (port_str == NULL)
    {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		  "adapter-transport requires <port>");
      goto fail;
    }

  {
    char *end = NULL;
    long port = strtol(port_str, &end, 10);

    if (end == port_str || *end != '\0' || port <= 0 || port > 65535)
      {
	g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		    "invalid adapter-transport port `%s'", port_str);
	goto fail;
      }
    self->port = (guint) port;
  }

  if (!have_mode || mode_str == NULL)
    {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		  "adapter-transport requires <mode>read|write</mode>");
      goto fail;
    }

  if (g_strcmp0(mode_str, "read") == 0)
    self->mode = STCS_ADAPTER_MODE_READ;
  else if (g_strcmp0(mode_str, "write") == 0)
    self->mode = STCS_ADAPTER_MODE_WRITE;
  else
    {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		  "adapter-transport mode must be `read' or `write' (got `%s')",
		  mode_str);
      goto fail;
    }

  g_free(mode_str);
  g_free(port_str);

  self->reconnect_delay_ms = STCS_ADAPTER_RECONNECT_MIN_MS;
  stcs_adapter_transport_try_connect(self);
  return TRUE;

fail:
  g_free(mode_str);
  g_free(port_str);
  return FALSE;
}

static gboolean
parse_json_object_numbers(const gchar *body,
			  gsize len,
			  GHashTable **out,
			  GError **error)
{
  JsonParser *parser;
  JsonNode *root;
  JsonObject *object;
  GHashTable *table;
  GList *members;
  GList *l;

  *out = NULL;
  parser = json_parser_new();
  if (!json_parser_load_from_data(parser, body, (gssize) len, error))
    {
      g_object_unref(parser);
      return FALSE;
    }

  root = json_parser_get_root(parser);
  if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root))
    {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		  "write body must be a JSON object of numbers");
      g_object_unref(parser);
      return FALSE;
    }

  object = json_node_get_object(root);
  table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  members = json_object_get_members(object);
  for (l = members; l != NULL; l = l->next)
    {
      const gchar *name = l->data;
      JsonNode *node = json_object_get_member(object, name);
      GType vtype;
      gdouble *boxed;

      if (node == NULL || !JSON_NODE_HOLDS_VALUE(node))
	{
	  g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		      "member `%s' is not a JSON number", name);
	  g_list_free(members);
	  g_hash_table_unref(table);
	  g_object_unref(parser);
	  return FALSE;
	}

      vtype = json_node_get_value_type(node);
      if (vtype != G_TYPE_INT64 && vtype != G_TYPE_DOUBLE && vtype != G_TYPE_INT)
	{
	  g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		      "member `%s' is not a JSON number", name);
	  g_list_free(members);
	  g_hash_table_unref(table);
	  g_object_unref(parser);
	  return FALSE;
	}

      boxed = g_new(gdouble, 1);
      *boxed = json_node_get_double(node);
      g_hash_table_insert(table, g_strdup(name), boxed);
    }
  g_list_free(members);
  g_object_unref(parser);

  if (g_hash_table_size(table) == 0)
    {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		  "JSON object has no numeric members");
      g_hash_table_unref(table);
      return FALSE;
    }

  *out = table;
  return TRUE;
}

static void
stcs_adapter_transport_start_write(STCSTransport *transport,
				   guint64 op_id,
				   GBytes *data)
{
  STCSAdapterTransport *self = STCS_ADAPTER_TRANSPORT(transport);
  const gchar *body;
  gsize len = 0;
  GHashTable *numbers = NULL;
  GError *error = NULL;
  GVariantBuilder builder;
  GHashTableIter iter;
  gpointer key;
  gpointer value;
  GVariant *reply = NULL;
  gboolean ok = FALSE;

  if (self->mode != STCS_ADAPTER_MODE_WRITE)
    {
      g_set_error(&error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
		  "adapter-transport mode=read cannot write");
      stcs_transport_finish_write_error(transport, op_id, error);
      return;
    }

  if (self->connection == NULL)
    {
      g_set_error(&error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
		  "adapter-transport is not connected");
      stcs_transport_finish_write_error(transport, op_id, error);
      return;
    }

  body = g_bytes_get_data(data, &len);
  if (!parse_json_object_numbers(body ? body : "", len, &numbers, &error))
    {
      stcs_transport_finish_write_error(transport, op_id, error);
      return;
    }

  g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sd}"));
  g_hash_table_iter_init(&iter, numbers);
  while (g_hash_table_iter_next(&iter, &key, &value))
    g_variant_builder_add(&builder, "{sd}",
			  (const gchar *) key,
			  *(const gdouble *) value);

  reply = g_dbus_connection_call_sync(self->connection,
				      NULL,
				      STCA_DBUS_OBJECT_PATH,
				      STCA_DBUS_INTERFACE,
				      "SetData",
				      g_variant_new("(a{sd})", &builder),
				      G_VARIANT_TYPE("(b)"),
				      G_DBUS_CALL_FLAGS_NONE,
				      -1,
				      NULL,
				      &error);
  g_hash_table_unref(numbers);

  if (reply == NULL)
    {
      stcs_transport_finish_write_error(transport, op_id, error);
      return;
    }

  g_variant_get(reply, "(b)", &ok);
  g_variant_unref(reply);

  if (!ok)
    {
      g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED,
		  "SetData returned false");
      stcs_transport_finish_write_error(transport, op_id, error);
      return;
    }

  stcs_transport_finish_write(transport, op_id, len);
}

static void
stcs_adapter_transport_dispose(GObject *object)
{
  STCSAdapterTransport *self = STCS_ADAPTER_TRANSPORT(object);

  self->shutting_down = TRUE;

  if (self->reconnect_source_id != 0)
    {
      g_source_remove(self->reconnect_source_id);
      self->reconnect_source_id = 0;
    }

  stcs_adapter_transport_disconnect(self);

  g_clear_pointer(&self->address, g_free);

  G_OBJECT_CLASS(stcs_adapter_transport_parent_class)->dispose(object);
}

static void
stcs_adapter_transport_class_init(STCSAdapterTransportClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  STCSTransportClass *transport_class = STCS_TRANSPORT_CLASS(klass);

  object_class->dispose = stcs_adapter_transport_dispose;
  transport_class->configure = stcs_adapter_transport_configure;
  transport_class->start_write = stcs_adapter_transport_start_write;
}

static void
stcs_adapter_transport_init(STCSAdapterTransport *self)
{
  self->reconnect_delay_ms = STCS_ADAPTER_RECONNECT_MIN_MS;
}

G_MODULE_EXPORT STCSPluginInfo *
stcs_plugin_get_info(void)
{
  return stcs_plugin_info_new("adapter-transport",
			      STCS_PLUGIN_CATEGORY_TRANSPORT,
			      "1.0.0");
}

G_MODULE_EXPORT GObject *
stcs_plugin_create(void)
{
  return g_object_new(STCS_TYPE_ADAPTER_TRANSPORT, NULL);
}
