/*
 * libstarter-configurable-service - datapoint.c
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

#include "starter/configurable-service/datapoint.h"

#define LOGD(msg...) g_log("datapoint", G_LOG_LEVEL_DEBUG, msg)
#define LOGW(msg...) g_log("datapoint", G_LOG_LEVEL_WARNING, msg)

enum {
  SIGNAL_VALUE_CHANGED,
  N_SIGNALS
};

static guint datapoint_signals[N_SIGNALS];

struct _STCSDatapoint {
  GObject parent_instance;
  gchar *id;
  GPtrArray *transforms; /* STCSTransform* */
  gboolean has_value;
  gdouble value;
};

G_DEFINE_TYPE(STCSDatapoint, stcs_datapoint, G_TYPE_OBJECT)

static void
stcs_datapoint_dispose(GObject *object)
{
  STCSDatapoint *self = STCS_DATAPOINT(object);

  g_clear_pointer(&self->transforms, g_ptr_array_unref);

  G_OBJECT_CLASS(stcs_datapoint_parent_class)->dispose(object);
}

static void
stcs_datapoint_finalize(GObject *object)
{
  STCSDatapoint *self = STCS_DATAPOINT(object);

  g_free(self->id);

  G_OBJECT_CLASS(stcs_datapoint_parent_class)->finalize(object);
}

static void
stcs_datapoint_class_init(STCSDatapointClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->dispose = stcs_datapoint_dispose;
  object_class->finalize = stcs_datapoint_finalize;

  /**
   * STCSDatapoint::value-changed:
   * @self: the datapoint
   * @value: the new processed value
   *
   * Emitted only when the processed value changes.
   */
  datapoint_signals[SIGNAL_VALUE_CHANGED] =
    g_signal_new("value-changed",
		 G_TYPE_FROM_CLASS(klass),
		 G_SIGNAL_RUN_LAST,
		 0,
		 NULL, NULL, NULL,
		 G_TYPE_NONE, 1,
		 G_TYPE_DOUBLE);
}

static void
stcs_datapoint_init(STCSDatapoint *self)
{
  self->id = NULL;
  self->transforms = g_ptr_array_new_with_free_func(g_object_unref);
  self->has_value = FALSE;
  self->value = 0.0;
}

STCSDatapoint *
stcs_datapoint_new(const gchar *id)
{
  STCSDatapoint *self;

  g_return_val_if_fail(id != NULL && *id != '\0', NULL);

  self = g_object_new(STCS_TYPE_DATAPOINT, NULL);
  self->id = g_strdup(id);
  return self;
}

const gchar *
stcs_datapoint_get_id(STCSDatapoint *self)
{
  g_return_val_if_fail(STCS_IS_DATAPOINT(self), NULL);

  return self->id;
}

gboolean
stcs_datapoint_get_value(STCSDatapoint *self, gdouble *out)
{
  g_return_val_if_fail(STCS_IS_DATAPOINT(self), FALSE);
  g_return_val_if_fail(out != NULL, FALSE);

  if (!self->has_value)
    return FALSE;

  *out = self->value;
  return TRUE;
}

void
stcs_datapoint_add_transform(STCSDatapoint *self, STCSTransform *transform)
{
  g_return_if_fail(STCS_IS_DATAPOINT(self));
  g_return_if_fail(STCS_IS_TRANSFORM(transform));

  g_ptr_array_add(self->transforms, g_object_ref(transform));
}

gboolean
stcs_datapoint_push(STCSDatapoint *self, gdouble raw, GError **error)
{
  gdouble current = raw;
  guint i;
  gboolean had_value;
  gdouble previous = 0.0;
  gboolean will_emit;

  g_return_val_if_fail(STCS_IS_DATAPOINT(self), FALSE);

  LOGD("datapoint `%s' push begin raw=%g transforms=%u has_value=%s",
       self->id,
       raw,
       self->transforms->len,
       self->has_value ? "true" : "false");

  for (i = 0; i < self->transforms->len; i++)
    {
      STCSTransform *transform = g_ptr_array_index(self->transforms, i);
      gdouble next = 0.0;

      if (!stcs_transform_process(transform, current, &next, error))
	{
	  LOGW("datapoint `%s' transform[%u] failed", self->id, i);
	  return FALSE;
	}

      LOGD("datapoint `%s' transform[%u] %g -> %g", self->id, i, current, next);
      current = next;
    }

  had_value = self->has_value;
  previous = self->value;
  will_emit = !had_value || previous != current;

  LOGD("datapoint `%s' push raw=%g processed=%g previous=%g had_value=%s will_emit=%s handlers=%s",
       self->id,
       raw,
       current,
       previous,
       had_value ? "true" : "false",
       will_emit ? "true" : "false",
       g_signal_has_handler_pending(self,
				    datapoint_signals[SIGNAL_VALUE_CHANGED],
				    0, FALSE) ? "yes" : "no");

  if (will_emit)
    {
      self->has_value = TRUE;
      self->value = current;
      LOGD("datapoint `%s' emitting value-changed value=%g", self->id, current);
      g_signal_emit(self, datapoint_signals[SIGNAL_VALUE_CHANGED], 0, current);
      LOGD("datapoint `%s' value-changed emitted -> %g", self->id, current);
    }
  else
    {
      LOGD("datapoint `%s' skip value-changed (unchanged %g)", self->id, current);
    }

  return TRUE;
}
