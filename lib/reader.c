/*
 * libstarter-configurable-service - reader.c
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

#include "starter/configurable-service/reader.h"

#define LOGD(msg...) g_log("reader", G_LOG_LEVEL_DEBUG, msg)
#define LOGW(msg...) g_log("reader", G_LOG_LEVEL_WARNING, msg)

typedef struct _STCSReaderPrivate {
  STCSTransport *transport;
  gulong data_ready_handler;
  gulong data_error_handler;
  GHashTable *mappings; /* key → GPtrArray of STCSDatapoint* */
  STCSReaderSyncHook sync_hook;
  gpointer sync_hook_data;
} STCSReaderPrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE(STCSReader, stcs_reader, G_TYPE_OBJECT)

static void
stcs_reader_disconnect_transport(STCSReader *self)
{
  STCSReaderPrivate *priv = stcs_reader_get_instance_private(self);

  if (priv->transport == NULL)
    return;

  if (priv->data_ready_handler != 0)
    {
      g_signal_handler_disconnect(priv->transport, priv->data_ready_handler);
      priv->data_ready_handler = 0;
    }

  if (priv->data_error_handler != 0)
    {
      g_signal_handler_disconnect(priv->transport, priv->data_error_handler);
      priv->data_error_handler = 0;
    }

  g_clear_object(&priv->transport);
}

static void
stcs_reader_on_data_ready(STCSTransport *transport, GBytes *data, gpointer user_data)
{
  STCSReader *self = STCS_READER(user_data);
  STCSReaderPrivate *priv = stcs_reader_get_instance_private(self);
  STCSReaderClass *klass;
  GError *error = NULL;

  (void) transport;

  klass = STCS_READER_GET_CLASS(self);
  if (klass->parse == NULL)
    {
      LOGW("reader has no parse() implementation");
      return;
    }

  if (priv->sync_hook != NULL)
    priv->sync_hook(TRUE, priv->sync_hook_data);

  if (!klass->parse(self, data, &error))
    {
      LOGW("reader parse failed: %s", error ? error->message : "unknown error");
      g_clear_error(&error);
    }

  if (priv->sync_hook != NULL)
    priv->sync_hook(FALSE, priv->sync_hook_data);
}

static void
stcs_reader_on_data_error(STCSTransport *transport, GError *error, gpointer user_data)
{
  STCSReader *self = STCS_READER(user_data);

  (void) self;
  (void) transport;

  LOGW("transport data-error: %s", error ? error->message : "unknown error");
}

static gboolean
stcs_reader_real_configure(STCSReader *self,
			   xmlNodePtr node,
			   STCSDatapointLookupFunc lookup_datapoint,
			   gpointer lookup_data,
			   GError **error)
{
  (void) self;
  (void) node;
  (void) lookup_datapoint;
  (void) lookup_data;

  g_set_error(error,
	      G_IO_ERROR,
	      G_IO_ERROR_NOT_SUPPORTED,
	      "configure() not implemented for this reader");
  return FALSE;
}

static gboolean
stcs_reader_real_parse(STCSReader *self, GBytes *data, GError **error)
{
  (void) self;
  (void) data;

  g_set_error(error,
	      G_IO_ERROR,
	      G_IO_ERROR_NOT_SUPPORTED,
	      "parse() not implemented for this reader");
  return FALSE;
}

static void
stcs_reader_dispose(GObject *object)
{
  STCSReader *self = STCS_READER(object);
  STCSReaderPrivate *priv = stcs_reader_get_instance_private(self);

  stcs_reader_disconnect_transport(self);
  g_clear_pointer(&priv->mappings, g_hash_table_unref);

  G_OBJECT_CLASS(stcs_reader_parent_class)->dispose(object);
}

static void
stcs_reader_class_init(STCSReaderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->dispose = stcs_reader_dispose;

  klass->configure = stcs_reader_real_configure;
  klass->parse = stcs_reader_real_parse;
}

static void
stcs_reader_init(STCSReader *self)
{
  STCSReaderPrivate *priv = stcs_reader_get_instance_private(self);

  priv->transport = NULL;
  priv->data_ready_handler = 0;
  priv->data_error_handler = 0;
  priv->mappings = g_hash_table_new_full(g_str_hash,
					 g_str_equal,
					 g_free,
					 (GDestroyNotify) g_ptr_array_unref);
  priv->sync_hook = NULL;
  priv->sync_hook_data = NULL;
}

void
stcs_reader_set_sync_hook(STCSReader *self,
			  STCSReaderSyncHook hook,
			  gpointer user_data)
{
  STCSReaderPrivate *priv;

  g_return_if_fail(STCS_IS_READER(self));

  priv = stcs_reader_get_instance_private(self);
  priv->sync_hook = hook;
  priv->sync_hook_data = user_data;
}

gboolean
stcs_reader_configure(STCSReader *self,
		      xmlNodePtr node,
		      STCSDatapointLookupFunc lookup_datapoint,
		      gpointer lookup_data,
		      GError **error)
{
  STCSReaderClass *klass;

  g_return_val_if_fail(STCS_IS_READER(self), FALSE);
  g_return_val_if_fail(node != NULL, FALSE);
  g_return_val_if_fail(lookup_datapoint != NULL, FALSE);

  klass = STCS_READER_GET_CLASS(self);
  g_return_val_if_fail(klass->configure != NULL, FALSE);

  return klass->configure(self, node, lookup_datapoint, lookup_data, error);
}

gboolean
stcs_reader_set_transport(STCSReader *self, STCSTransport *transport, GError **error)
{
  STCSReaderPrivate *priv;

  g_return_val_if_fail(STCS_IS_READER(self), FALSE);
  g_return_val_if_fail(STCS_IS_TRANSPORT(transport), FALSE);

  priv = stcs_reader_get_instance_private(self);

  stcs_reader_disconnect_transport(self);

  priv->transport = g_object_ref(transport);
  priv->data_ready_handler =
    g_signal_connect(priv->transport, "data-ready",
		     G_CALLBACK(stcs_reader_on_data_ready), self);
  priv->data_error_handler =
    g_signal_connect(priv->transport, "data-error",
		     G_CALLBACK(stcs_reader_on_data_error), self);

  LOGD("reader bound to transport %p", (void *) transport);
  (void) error;
  return TRUE;
}

STCSTransport *
stcs_reader_get_transport(STCSReader *self)
{
  STCSReaderPrivate *priv;

  g_return_val_if_fail(STCS_IS_READER(self), NULL);

  priv = stcs_reader_get_instance_private(self);
  return priv->transport;
}

void
stcs_reader_add_mapping(STCSReader *self, const gchar *key, STCSDatapoint *datapoint)
{
  STCSReaderPrivate *priv;
  GPtrArray *list;
  guint i;

  g_return_if_fail(STCS_IS_READER(self));
  g_return_if_fail(key != NULL && *key != '\0');
  g_return_if_fail(STCS_IS_DATAPOINT(datapoint));

  priv = stcs_reader_get_instance_private(self);
  list = g_hash_table_lookup(priv->mappings, key);
  if (list == NULL)
    {
      list = g_ptr_array_new_with_free_func(g_object_unref);
      g_hash_table_insert(priv->mappings, g_strdup(key), list);
    }

  for (i = 0; i < list->len; i++)
    {
      if (g_ptr_array_index(list, i) == datapoint)
	{
	  LOGD("reader map key=`%s' already has datapoint=`%s' — skip duplicate",
	       key, stcs_datapoint_get_id(datapoint));
	  return;
	}
    }

  g_ptr_array_add(list, g_object_ref(datapoint));
  LOGD("reader map key=`%s' → datapoint=`%s' (now %u target(s))",
       key,
       stcs_datapoint_get_id(datapoint),
       list->len);
}

STCSDatapoint *
stcs_reader_lookup_mapping(STCSReader *self, const gchar *key)
{
  const GPtrArray *list;

  list = stcs_reader_lookup_mappings(self, key);
  if (list == NULL || list->len == 0)
    return NULL;

  return STCS_DATAPOINT(g_ptr_array_index(list, 0));
}

const GPtrArray *
stcs_reader_lookup_mappings(STCSReader *self, const gchar *key)
{
  STCSReaderPrivate *priv;

  g_return_val_if_fail(STCS_IS_READER(self), NULL);
  g_return_val_if_fail(key != NULL, NULL);

  priv = stcs_reader_get_instance_private(self);
  return g_hash_table_lookup(priv->mappings, key);
}

typedef struct {
  STCSReaderMappingFunc func;
  gpointer user_data;
} STCSReaderForeachData;

static void
stcs_reader_foreach_mapping_cb(gpointer key, gpointer value, gpointer user_data)
{
  STCSReaderForeachData *data = user_data;
  GPtrArray *list = value;
  guint i;

  for (i = 0; i < list->len; i++)
    data->func((const gchar *) key, STCS_DATAPOINT(g_ptr_array_index(list, i)), data->user_data);
}

void
stcs_reader_foreach_mapping(STCSReader *self,
			    STCSReaderMappingFunc func,
			    gpointer user_data)
{
  STCSReaderPrivate *priv;
  STCSReaderForeachData data;

  g_return_if_fail(STCS_IS_READER(self));
  g_return_if_fail(func != NULL);

  priv = stcs_reader_get_instance_private(self);
  data.func = func;
  data.user_data = user_data;
  g_hash_table_foreach(priv->mappings, stcs_reader_foreach_mapping_cb, &data);
}

typedef struct {
  STCSReaderKeyFunc func;
  gpointer user_data;
} STCSReaderForeachKeyData;

static void
stcs_reader_foreach_key_cb(gpointer key, gpointer value, gpointer user_data)
{
  STCSReaderForeachKeyData *data = user_data;

  data->func((const gchar *) key, (const GPtrArray *) value, data->user_data);
}

void
stcs_reader_foreach_key(STCSReader *self,
			STCSReaderKeyFunc func,
			gpointer user_data)
{
  STCSReaderPrivate *priv;
  STCSReaderForeachKeyData data;

  g_return_if_fail(STCS_IS_READER(self));
  g_return_if_fail(func != NULL);

  priv = stcs_reader_get_instance_private(self);
  data.func = func;
  data.user_data = user_data;
  g_hash_table_foreach(priv->mappings, stcs_reader_foreach_key_cb, &data);
}

gboolean
stcs_reader_push_value(STCSReader *self, const gchar *key, gdouble value, GError **error)
{
  const GPtrArray *list;
  guint i;

  g_return_val_if_fail(STCS_IS_READER(self), FALSE);
  g_return_val_if_fail(key != NULL, FALSE);

  list = stcs_reader_lookup_mappings(self, key);
  if (list == NULL || list->len == 0)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_NOT_FOUND,
		  "no datapoint mapping for key `%s'", key);
      return FALSE;
    }

  for (i = 0; i < list->len; i++)
    {
      STCSDatapoint *datapoint = STCS_DATAPOINT(g_ptr_array_index(list, i));

      if (!stcs_datapoint_push(datapoint, value, error))
	{
	  LOGW("push key=`%s' datapoint=`%s' failed",
	       key, stcs_datapoint_get_id(datapoint));
	  return FALSE;
	}
    }

  return TRUE;
}
