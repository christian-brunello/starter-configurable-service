/*
 * libstarter-configurable-service - writer.c
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

#include "starter/configurable-service/writer.h"

#define LOGD(msg...) g_log("writer", G_LOG_LEVEL_DEBUG, msg)
#define LOGI(msg...) g_log("writer", G_LOG_LEVEL_INFO, msg)
#define LOGW(msg...) g_log("writer", G_LOG_LEVEL_WARNING, msg)

typedef struct {
  STCSWriter *writer; /* not owned */
  gchar *key;
  STCSDatapoint *datapoint;
  gulong handler_id;
} STCSWriterMapping;

typedef struct _STCSWriterPrivate {
  STCSTransport *transport;
  GHashTable *mappings; /* key → GPtrArray of STCSWriterMapping* */
  guint write_attempts; /* total attempts per payload; default 1 */
  guint attempt;        /* 1-based attempt for last_bytes */
  guint64 last_op_id;
  GBytes *last_bytes;
  gchar *last_ack_datapoint_id;
  gulong write_complete_id;
  gulong write_error_id;
  STCSWriterAckHook ack_hook;
  gpointer ack_hook_data;
} STCSWriterPrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE(STCSWriter, stcs_writer, G_TYPE_OBJECT)

static void
stcs_writer_mapping_free(gpointer data)
{
  STCSWriterMapping *mapping = data;

  if (mapping == NULL)
    return;

  if (mapping->datapoint != NULL && mapping->handler_id != 0)
    g_signal_handler_disconnect(mapping->datapoint, mapping->handler_id);

  g_clear_object(&mapping->datapoint);
  g_free(mapping->key);
  g_free(mapping);
}

static void
stcs_writer_clear_write_state(STCSWriterPrivate *priv)
{
  priv->last_op_id = 0;
  priv->attempt = 0;
  g_clear_pointer(&priv->last_bytes, g_bytes_unref);
  g_clear_pointer(&priv->last_ack_datapoint_id, g_free);
}

static void
stcs_writer_emit_ack(STCSWriterPrivate *priv, gboolean success)
{
  gchar *datapoint_id;

  if (priv->ack_hook == NULL || priv->last_ack_datapoint_id == NULL)
    return;

  datapoint_id = g_strdup(priv->last_ack_datapoint_id);
  priv->ack_hook(datapoint_id, success, priv->ack_hook_data);
  g_free(datapoint_id);
}

static void
stcs_writer_disconnect_transport_signals(STCSWriterPrivate *priv)
{
  if (priv->transport == NULL)
    return;

  if (priv->write_complete_id != 0)
    {
      g_signal_handler_disconnect(priv->transport, priv->write_complete_id);
      priv->write_complete_id = 0;
    }

  if (priv->write_error_id != 0)
    {
      g_signal_handler_disconnect(priv->transport, priv->write_error_id);
      priv->write_error_id = 0;
    }
}

static void
stcs_writer_on_datapoint_value_changed(STCSDatapoint *datapoint,
				       gdouble value,
				       gpointer user_data)
{
  STCSWriterMapping *mapping = user_data;
  STCSWriterClass *klass;
  GError *error = NULL;

  (void) datapoint;

  klass = STCS_WRITER_GET_CLASS(mapping->writer);
  if (klass->write_value == NULL)
    {
      LOGW("writer has no write_value() implementation");
      return;
    }

  LOGD("datapoint value-changed key=`%s' value=%g", mapping->key, value);

  if (!klass->write_value(mapping->writer, mapping->key, value, &error))
    {
      STCSWriterPrivate *priv = stcs_writer_get_instance_private(mapping->writer);

      LOGW("writer write_value failed for key `%s': %s",
	   mapping->key,
	   error ? error->message : "unknown error");
      g_clear_error(&error);

      g_free(priv->last_ack_datapoint_id);
      priv->last_ack_datapoint_id = g_strdup(stcs_datapoint_get_id(mapping->datapoint));
      stcs_writer_emit_ack(priv, FALSE);
      g_clear_pointer(&priv->last_ack_datapoint_id, g_free);
      return;
    }

  /* Remember which datapoint triggered the in-flight/last write for ack. */
  {
    STCSWriterPrivate *priv = stcs_writer_get_instance_private(mapping->writer);

    g_free(priv->last_ack_datapoint_id);
    priv->last_ack_datapoint_id = g_strdup(stcs_datapoint_get_id(mapping->datapoint));
  }
}

static void
stcs_writer_on_write_complete(STCSTransport *transport,
			      guint64 op_id,
			      guint64 nbytes,
			      gpointer user_data)
{
  STCSWriter *self = user_data;
  STCSWriterPrivate *priv = stcs_writer_get_instance_private(self);

  (void) transport;
  (void) nbytes;

  if (op_id == 0 || op_id != priv->last_op_id)
    return;

  LOGD("write-complete op_id=%" G_GUINT64_FORMAT " after attempt %u/%u",
       op_id, priv->attempt, priv->write_attempts);
  stcs_writer_emit_ack(priv, TRUE);
  stcs_writer_clear_write_state(priv);
}

static void
stcs_writer_on_write_error(STCSTransport *transport,
			   guint64 op_id,
			   GError *error,
			   gpointer user_data)
{
  STCSWriter *self = user_data;
  STCSWriterPrivate *priv = stcs_writer_get_instance_private(self);
  GError *retry_error = NULL;
  guint64 new_op_id;

  (void) transport;

  if (op_id == 0 || op_id != priv->last_op_id)
    return;

  if (error != NULL &&
      error->domain == G_IO_ERROR &&
      error->code == G_IO_ERROR_CANCELLED)
    {
      LOGD("write-error op_id=%" G_GUINT64_FORMAT " cancelled/replaced — no retry",
	   op_id);
      stcs_writer_clear_write_state(priv);
      return;
    }

  if (priv->last_bytes == NULL)
    {
      LOGW("write-error op_id=%" G_GUINT64_FORMAT " with no retained payload",
	   op_id);
      stcs_writer_clear_write_state(priv);
      return;
    }

  if (priv->attempt >= priv->write_attempts)
    {
      LOGW("write failed after %u attempt(s) op_id=%" G_GUINT64_FORMAT ": %s",
	   priv->attempt,
	   op_id,
	   error ? error->message : "unknown error");
      stcs_writer_emit_ack(priv, FALSE);
      stcs_writer_clear_write_state(priv);
      return;
    }

  LOGI("write-error op_id=%" G_GUINT64_FORMAT " attempt %u/%u — retrying: %s",
       op_id,
       priv->attempt,
       priv->write_attempts,
       error ? error->message : "unknown error");

  new_op_id = stcs_transport_write(priv->transport, priv->last_bytes, &retry_error);
  if (new_op_id == 0)
    {
      LOGW("write retry enqueue failed: %s",
	   retry_error ? retry_error->message : "unknown error");
      g_clear_error(&retry_error);
      stcs_writer_clear_write_state(priv);
      return;
    }

  priv->last_op_id = new_op_id;
  priv->attempt++;
}

static gboolean
stcs_writer_real_configure(STCSWriter *self,
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
	      "configure() not implemented for this writer");
  return FALSE;
}

static gboolean
stcs_writer_real_write_value(STCSWriter *self,
			     const gchar *key,
			     gdouble value,
			     GError **error)
{
  (void) self;
  (void) key;
  (void) value;

  g_set_error(error,
	      G_IO_ERROR,
	      G_IO_ERROR_NOT_SUPPORTED,
	      "write_value() not implemented for this writer");
  return FALSE;
}

static void
stcs_writer_dispose(GObject *object)
{
  STCSWriter *self = STCS_WRITER(object);
  STCSWriterPrivate *priv = stcs_writer_get_instance_private(self);

  stcs_writer_disconnect_transport_signals(priv);
  stcs_writer_clear_write_state(priv);
  g_clear_pointer(&priv->mappings, g_hash_table_unref);
  g_clear_object(&priv->transport);

  G_OBJECT_CLASS(stcs_writer_parent_class)->dispose(object);
}

static void
stcs_writer_class_init(STCSWriterClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->dispose = stcs_writer_dispose;

  klass->configure = stcs_writer_real_configure;
  klass->write_value = stcs_writer_real_write_value;
}

static void
stcs_writer_init(STCSWriter *self)
{
  STCSWriterPrivate *priv = stcs_writer_get_instance_private(self);

  priv->transport = NULL;
  priv->write_attempts = 1;
  priv->last_ack_datapoint_id = NULL;
  priv->ack_hook = NULL;
  priv->ack_hook_data = NULL;
  priv->mappings = g_hash_table_new_full(g_str_hash,
					 g_str_equal,
					 g_free,
					 (GDestroyNotify) g_ptr_array_unref);
}

void
stcs_writer_set_ack_hook(STCSWriter *self,
			 STCSWriterAckHook hook,
			 gpointer user_data)
{
  STCSWriterPrivate *priv;

  g_return_if_fail(STCS_IS_WRITER(self));

  priv = stcs_writer_get_instance_private(self);
  priv->ack_hook = hook;
  priv->ack_hook_data = user_data;
}

gboolean
stcs_writer_configure(STCSWriter *self,
		      xmlNodePtr node,
		      STCSDatapointLookupFunc lookup_datapoint,
		      gpointer lookup_data,
		      GError **error)
{
  STCSWriterClass *klass;

  g_return_val_if_fail(STCS_IS_WRITER(self), FALSE);
  g_return_val_if_fail(node != NULL, FALSE);
  g_return_val_if_fail(lookup_datapoint != NULL, FALSE);

  klass = STCS_WRITER_GET_CLASS(self);
  g_return_val_if_fail(klass->configure != NULL, FALSE);

  return klass->configure(self, node, lookup_datapoint, lookup_data, error);
}

gboolean
stcs_writer_set_transport(STCSWriter *self, STCSTransport *transport, GError **error)
{
  STCSWriterPrivate *priv;

  g_return_val_if_fail(STCS_IS_WRITER(self), FALSE);
  g_return_val_if_fail(STCS_IS_TRANSPORT(transport), FALSE);

  priv = stcs_writer_get_instance_private(self);

  stcs_writer_disconnect_transport_signals(priv);
  stcs_writer_clear_write_state(priv);
  g_clear_object(&priv->transport);

  priv->transport = g_object_ref(transport);
  priv->write_complete_id =
    g_signal_connect(transport, "write-complete",
		     G_CALLBACK(stcs_writer_on_write_complete),
		     self);
  priv->write_error_id =
    g_signal_connect(transport, "write-error",
		     G_CALLBACK(stcs_writer_on_write_error),
		     self);

  LOGD("writer bound to transport %p (write-complete=%lu write-error=%lu)",
       (void *) transport,
       (unsigned long) priv->write_complete_id,
       (unsigned long) priv->write_error_id);
  (void) error;
  return TRUE;
}

STCSTransport *
stcs_writer_get_transport(STCSWriter *self)
{
  STCSWriterPrivate *priv;

  g_return_val_if_fail(STCS_IS_WRITER(self), NULL);

  priv = stcs_writer_get_instance_private(self);
  return priv->transport;
}

void
stcs_writer_set_write_attempts(STCSWriter *self, guint attempts)
{
  STCSWriterPrivate *priv;

  g_return_if_fail(STCS_IS_WRITER(self));
  g_return_if_fail(attempts >= 1);

  priv = stcs_writer_get_instance_private(self);
  priv->write_attempts = attempts;
}

guint
stcs_writer_get_write_attempts(STCSWriter *self)
{
  STCSWriterPrivate *priv;

  g_return_val_if_fail(STCS_IS_WRITER(self), 1);

  priv = stcs_writer_get_instance_private(self);
  return priv->write_attempts;
}

void
stcs_writer_add_mapping(STCSWriter *self, const gchar *key, STCSDatapoint *datapoint)
{
  STCSWriterPrivate *priv;
  STCSWriterMapping *mapping;
  GPtrArray *list;
  guint i;

  g_return_if_fail(STCS_IS_WRITER(self));
  g_return_if_fail(key != NULL && *key != '\0');
  g_return_if_fail(STCS_IS_DATAPOINT(datapoint));

  priv = stcs_writer_get_instance_private(self);

  list = g_hash_table_lookup(priv->mappings, key);
  if (list == NULL)
    {
      list = g_ptr_array_new_with_free_func(stcs_writer_mapping_free);
      g_hash_table_insert(priv->mappings, g_strdup(key), list);
    }

  for (i = 0; i < list->len; i++)
    {
      STCSWriterMapping *existing = g_ptr_array_index(list, i);

      if (existing->datapoint == datapoint)
	{
	  LOGD("writer map key=`%s' already has datapoint=`%s' — skip duplicate",
	       key, stcs_datapoint_get_id(datapoint));
	  return;
	}
    }

  mapping = g_new0(STCSWriterMapping, 1);
  mapping->writer = self;
  mapping->key = g_strdup(key);
  mapping->datapoint = g_object_ref(datapoint);
  mapping->handler_id =
    g_signal_connect(datapoint, "value-changed",
		     G_CALLBACK(stcs_writer_on_datapoint_value_changed),
		     mapping);

  g_ptr_array_add(list, mapping);

  LOGD("writer map key=`%s' → datapoint=`%s' handler=%lu (now %u source(s))",
       key,
       stcs_datapoint_get_id(datapoint),
       (unsigned long) mapping->handler_id,
       list->len);
}

STCSDatapoint *
stcs_writer_lookup_mapping(STCSWriter *self, const gchar *key)
{
  STCSWriterPrivate *priv;
  GPtrArray *list;
  STCSWriterMapping *mapping;

  g_return_val_if_fail(STCS_IS_WRITER(self), NULL);
  g_return_val_if_fail(key != NULL, NULL);

  priv = stcs_writer_get_instance_private(self);
  list = g_hash_table_lookup(priv->mappings, key);
  if (list == NULL || list->len == 0)
    return NULL;

  mapping = g_ptr_array_index(list, 0);
  return mapping->datapoint;
}

typedef struct {
  STCSWriterMappingFunc func;
  gpointer user_data;
} STCSWriterForeachData;

static void
stcs_writer_foreach_mapping_cb(gpointer key, gpointer value, gpointer user_data)
{
  STCSWriterForeachData *data = user_data;
  GPtrArray *list = value;
  guint i;

  for (i = 0; i < list->len; i++)
    {
      STCSWriterMapping *mapping = g_ptr_array_index(list, i);

      data->func((const gchar *) key, mapping->datapoint, data->user_data);
    }
}

void
stcs_writer_foreach_mapping(STCSWriter *self,
			    STCSWriterMappingFunc func,
			    gpointer user_data)
{
  STCSWriterPrivate *priv;
  STCSWriterForeachData data;

  g_return_if_fail(STCS_IS_WRITER(self));
  g_return_if_fail(func != NULL);

  priv = stcs_writer_get_instance_private(self);
  data.func = func;
  data.user_data = user_data;
  g_hash_table_foreach(priv->mappings, stcs_writer_foreach_mapping_cb, &data);
}

gboolean
stcs_writer_write_bytes(STCSWriter *self, GBytes *data, GError **error)
{
  STCSWriterPrivate *priv;
  guint64 op_id;

  g_return_val_if_fail(STCS_IS_WRITER(self), FALSE);
  g_return_val_if_fail(data != NULL, FALSE);

  priv = stcs_writer_get_instance_private(self);
  if (priv->transport == NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_NOT_CONNECTED,
		  "writer has no transport bound");
      return FALSE;
    }

  op_id = stcs_transport_write(priv->transport, data, error);
  if (op_id == 0)
    return FALSE;

  /* New command replaces any in-progress retry tracking. */
  g_clear_pointer(&priv->last_bytes, g_bytes_unref);
  priv->last_bytes = g_bytes_ref(data);
  priv->last_op_id = op_id;
  priv->attempt = 1;

  LOGD("enqueued %" G_GSIZE_FORMAT " bytes to transport (op_id=%" G_GUINT64_FORMAT
       " attempt 1/%u)",
       g_bytes_get_size(data), op_id, priv->write_attempts);
  return TRUE;
}
