/*
 * libstarter-configurable-service - transport.c
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

#include "starter/configurable-service/transport.h"

#define LOGD(msg...) g_log("transport", G_LOG_LEVEL_DEBUG, msg)
#define LOGW(msg...) g_log("transport", G_LOG_LEVEL_WARNING, msg)

enum {
  SIGNAL_DATA_READY,
  SIGNAL_DATA_ERROR,
  SIGNAL_WRITE_COMPLETE,
  SIGNAL_WRITE_ERROR,
  N_SIGNALS
};

typedef struct _STCSTransportPrivate {
  guint64 next_op_id;

  guint64 inflight_op_id; /* 0 = none */
  GBytes *inflight_data;

  guint64 pending_op_id; /* 0 = none; single-slot queue */
  GBytes *pending_data;

  gboolean write_failed;
  gboolean disposed;
} STCSTransportPrivate;

static guint transport_signals[N_SIGNALS];

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE(STCSTransport, stcs_transport, G_TYPE_OBJECT)

static void stcs_transport_kick_write(STCSTransport *self);

static gboolean
stcs_transport_real_configure(STCSTransport *self, xmlNodePtr node, GError **error)
{
  (void) self;
  (void) node;

  g_set_error(error,
	      G_IO_ERROR,
	      G_IO_ERROR_NOT_SUPPORTED,
	      "configure() not implemented for this transport");
  return FALSE;
}

static void
stcs_transport_real_start_write(STCSTransport *self, guint64 op_id, GBytes *data)
{
  GError *error = NULL;

  (void) data;

  g_set_error(&error,
	      G_IO_ERROR,
	      G_IO_ERROR_NOT_SUPPORTED,
	      "start_write() not implemented for this transport");
  stcs_transport_finish_write_error(self, op_id, error);
  g_clear_error(&error);
}

static void
stcs_transport_dispose(GObject *object)
{
  STCSTransport *self = STCS_TRANSPORT(object);
  STCSTransportPrivate *priv = stcs_transport_get_instance_private(self);

  priv->disposed = TRUE;

  if (priv->pending_op_id != 0)
    {
      GError *error = NULL;
      guint64 op_id = priv->pending_op_id;

      g_clear_pointer(&priv->pending_data, g_bytes_unref);
      priv->pending_op_id = 0;

      g_set_error(&error,
		  G_IO_ERROR,
		  G_IO_ERROR_CANCELLED,
		  "transport disposed");
      g_signal_emit(self, transport_signals[SIGNAL_WRITE_ERROR], 0, op_id, error);
      g_clear_error(&error);
    }

  /* In-flight ops may still complete; finish_* will no-op if disposed mismatch. */
  g_clear_pointer(&priv->inflight_data, g_bytes_unref);

  G_OBJECT_CLASS(stcs_transport_parent_class)->dispose(object);
}

static void
stcs_transport_class_init(STCSTransportClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->dispose = stcs_transport_dispose;

  klass->configure = stcs_transport_real_configure;
  klass->start_write = stcs_transport_real_start_write;

  /**
   * STCSTransport::data-ready:
   * @self: the transport
   * @data: (transfer none): payload bytes
   *
   * Emitted when a new data buffer is available for readers.
   */
  transport_signals[SIGNAL_DATA_READY] =
    g_signal_new("data-ready",
		 G_TYPE_FROM_CLASS(klass),
		 G_SIGNAL_RUN_LAST,
		 0,
		 NULL, NULL, NULL,
		 G_TYPE_NONE, 1,
		 G_TYPE_BYTES);

  /**
   * STCSTransport::data-error:
   * @self: the transport
   * @error: (transfer none): failure details
   *
   * Emitted when fetching/producing data fails (read path).
   */
  transport_signals[SIGNAL_DATA_ERROR] =
    g_signal_new("data-error",
		 G_TYPE_FROM_CLASS(klass),
		 G_SIGNAL_RUN_LAST,
		 0,
		 NULL, NULL, NULL,
		 G_TYPE_NONE, 1,
		 G_TYPE_ERROR);

  /**
   * STCSTransport::write-complete:
   * @self: the transport
   * @op_id: operation id returned by stcs_transport_write()
   * @nbytes: number of bytes written
   */
  transport_signals[SIGNAL_WRITE_COMPLETE] =
    g_signal_new("write-complete",
		 G_TYPE_FROM_CLASS(klass),
		 G_SIGNAL_RUN_LAST,
		 0,
		 NULL, NULL, NULL,
		 G_TYPE_NONE, 2,
		 G_TYPE_UINT64,
		 G_TYPE_UINT64);

  /**
   * STCSTransport::write-error:
   * @self: the transport
   * @op_id: operation id returned by stcs_transport_write()
   * @error: (transfer none): failure details
   */
  transport_signals[SIGNAL_WRITE_ERROR] =
    g_signal_new("write-error",
		 G_TYPE_FROM_CLASS(klass),
		 G_SIGNAL_RUN_LAST,
		 0,
		 NULL, NULL, NULL,
		 G_TYPE_NONE, 2,
		 G_TYPE_UINT64,
		 G_TYPE_ERROR);
}

static void
stcs_transport_init(STCSTransport *self)
{
  STCSTransportPrivate *priv = stcs_transport_get_instance_private(self);

  priv->next_op_id = 1;
  priv->inflight_op_id = 0;
  priv->inflight_data = NULL;
  priv->pending_op_id = 0;
  priv->pending_data = NULL;
  priv->write_failed = FALSE;
  priv->disposed = FALSE;

  LOGD("STCSTransport base instance initialized");
}

gboolean
stcs_transport_configure(STCSTransport *self, xmlNodePtr node, GError **error)
{
  STCSTransportClass *klass;

  g_return_val_if_fail(STCS_IS_TRANSPORT(self), FALSE);
  g_return_val_if_fail(node != NULL, FALSE);

  klass = STCS_TRANSPORT_GET_CLASS(self);
  g_return_val_if_fail(klass->configure != NULL, FALSE);

  return klass->configure(self, node, error);
}

gboolean
stcs_transport_is_syncing(STCSTransport *self)
{
  STCSTransportPrivate *priv;

  g_return_val_if_fail(STCS_IS_TRANSPORT(self), FALSE);

  priv = stcs_transport_get_instance_private(self);
  return priv->inflight_op_id != 0 || priv->pending_op_id != 0;
}

gboolean
stcs_transport_get_write_failed(STCSTransport *self)
{
  STCSTransportPrivate *priv;

  g_return_val_if_fail(STCS_IS_TRANSPORT(self), FALSE);

  priv = stcs_transport_get_instance_private(self);
  return priv->write_failed;
}

static void
stcs_transport_kick_write(STCSTransport *self)
{
  STCSTransportPrivate *priv = stcs_transport_get_instance_private(self);
  STCSTransportClass *klass;
  guint64 op_id;
  GBytes *data;

  if (priv->disposed)
    return;

  if (priv->inflight_op_id != 0)
    return;

  if (priv->pending_op_id == 0)
    return;

  op_id = priv->pending_op_id;
  data = priv->pending_data;
  priv->pending_op_id = 0;
  priv->pending_data = NULL;

  priv->inflight_op_id = op_id;
  priv->inflight_data = data; /* take ownership */

  klass = STCS_TRANSPORT_GET_CLASS(self);
  if (klass->start_write == NULL)
    {
      GError *error = NULL;
      g_set_error(&error,
		  G_IO_ERROR,
		  G_IO_ERROR_NOT_SUPPORTED,
		  "start_write() not implemented for this transport");
      stcs_transport_finish_write_error(self, op_id, error);
      g_clear_error(&error);
      return;
    }

  LOGD("start_write op_id=%" G_GUINT64_FORMAT " bytes=%" G_GSIZE_FORMAT,
       op_id, g_bytes_get_size(data));
  klass->start_write(self, op_id, data);
}

guint64
stcs_transport_write(STCSTransport *self, GBytes *data, GError **error)
{
  STCSTransportPrivate *priv;
  guint64 op_id;

  g_return_val_if_fail(STCS_IS_TRANSPORT(self), 0);
  g_return_val_if_fail(data != NULL, 0);

  priv = stcs_transport_get_instance_private(self);

  if (priv->disposed)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_CLOSED,
		  "transport is disposed");
      return 0;
    }

  op_id = priv->next_op_id++;
  if (op_id == 0)
    op_id = priv->next_op_id++; /* skip 0 */

  if (priv->pending_op_id != 0)
    {
      GError *replaced = NULL;
      guint64 old_id = priv->pending_op_id;

      g_clear_pointer(&priv->pending_data, g_bytes_unref);
      priv->pending_op_id = 0;

      g_set_error(&replaced,
		  G_IO_ERROR,
		  G_IO_ERROR_CANCELLED,
		  "write replaced by newer request");
      LOGD("write op_id=%" G_GUINT64_FORMAT " replaced by %" G_GUINT64_FORMAT,
	   old_id, op_id);
      g_signal_emit(self, transport_signals[SIGNAL_WRITE_ERROR], 0, old_id, replaced);
      g_clear_error(&replaced);
    }

  priv->pending_op_id = op_id;
  priv->pending_data = g_bytes_ref(data);

  LOGD("enqueue write op_id=%" G_GUINT64_FORMAT " bytes=%" G_GSIZE_FORMAT
       " inflight=%" G_GUINT64_FORMAT,
       op_id, g_bytes_get_size(data), priv->inflight_op_id);

  stcs_transport_kick_write(self);
  return op_id;
}

static void
stcs_transport_clear_inflight(STCSTransportPrivate *priv)
{
  priv->inflight_op_id = 0;
  g_clear_pointer(&priv->inflight_data, g_bytes_unref);
}

void
stcs_transport_finish_write(STCSTransport *self, guint64 op_id, gsize nbytes)
{
  STCSTransportPrivate *priv;

  g_return_if_fail(STCS_IS_TRANSPORT(self));

  priv = stcs_transport_get_instance_private(self);

  if (priv->inflight_op_id != op_id)
    {
      LOGW("finish_write ignored (expected op_id=%" G_GUINT64_FORMAT
	   " got %" G_GUINT64_FORMAT ")",
	   priv->inflight_op_id, op_id);
      return;
    }

  stcs_transport_clear_inflight(priv);

  if (priv->pending_op_id == 0)
    priv->write_failed = FALSE;

  LOGD("write-complete op_id=%" G_GUINT64_FORMAT " nbytes=%" G_GSIZE_FORMAT,
       op_id, nbytes);

  if (!priv->disposed)
    g_signal_emit(self, transport_signals[SIGNAL_WRITE_COMPLETE], 0,
		  op_id, (guint64) nbytes);

  stcs_transport_kick_write(self);
}

void
stcs_transport_finish_write_error(STCSTransport *self, guint64 op_id, GError *error)
{
  STCSTransportPrivate *priv;

  g_return_if_fail(STCS_IS_TRANSPORT(self));
  g_return_if_fail(error != NULL);

  priv = stcs_transport_get_instance_private(self);

  if (priv->inflight_op_id != op_id)
    {
      LOGW("finish_write_error ignored (expected op_id=%" G_GUINT64_FORMAT
	   " got %" G_GUINT64_FORMAT ")",
	   priv->inflight_op_id, op_id);
      return;
    }

  stcs_transport_clear_inflight(priv);
  priv->write_failed = TRUE;

  LOGW("write-error op_id=%" G_GUINT64_FORMAT ": %s",
       op_id, error->message);

  if (!priv->disposed)
    g_signal_emit(self, transport_signals[SIGNAL_WRITE_ERROR], 0, op_id, error);

  stcs_transport_kick_write(self);
}

void
stcs_transport_emit_data_ready(STCSTransport *self, GBytes *data)
{
  g_return_if_fail(STCS_IS_TRANSPORT(self));
  g_return_if_fail(data != NULL);

  g_signal_emit(self, transport_signals[SIGNAL_DATA_READY], 0, data);
}

void
stcs_transport_emit_data_error(STCSTransport *self, GError *error)
{
  g_return_if_fail(STCS_IS_TRANSPORT(self));
  g_return_if_fail(error != NULL);

  g_signal_emit(self, transport_signals[SIGNAL_DATA_ERROR], 0, error);
}
