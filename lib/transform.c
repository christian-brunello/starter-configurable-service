/*
 * libstarter-configurable-service - transform.c
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

#include "starter/configurable-service/transform.h"

#define LOGD(msg...) g_log("transform", G_LOG_LEVEL_DEBUG, msg)

G_DEFINE_ABSTRACT_TYPE(STCSTransform, stcs_transform, G_TYPE_OBJECT)

static gboolean
stcs_transform_real_configure(STCSTransform *self, xmlNodePtr node, GError **error)
{
  (void) self;
  (void) node;

  g_set_error(error,
	      G_IO_ERROR,
	      G_IO_ERROR_NOT_SUPPORTED,
	      "configure() not implemented for this transform");
  return FALSE;
}

static gboolean
stcs_transform_real_process(STCSTransform *self, gdouble in, gdouble *out, GError **error)
{
  (void) self;
  (void) in;
  (void) out;

  g_set_error(error,
	      G_IO_ERROR,
	      G_IO_ERROR_NOT_SUPPORTED,
	      "process() not implemented for this transform");
  return FALSE;
}

static void
stcs_transform_class_init(STCSTransformClass *klass)
{
  klass->configure = stcs_transform_real_configure;
  klass->process = stcs_transform_real_process;
}

static void
stcs_transform_init(STCSTransform *self)
{
  (void) self;
  LOGD("STCSTransform base instance initialized");
}

gboolean
stcs_transform_configure(STCSTransform *self, xmlNodePtr node, GError **error)
{
  STCSTransformClass *klass;

  g_return_val_if_fail(STCS_IS_TRANSFORM(self), FALSE);
  g_return_val_if_fail(node != NULL, FALSE);

  klass = STCS_TRANSFORM_GET_CLASS(self);
  g_return_val_if_fail(klass->configure != NULL, FALSE);

  return klass->configure(self, node, error);
}

gboolean
stcs_transform_process(STCSTransform *self, gdouble in, gdouble *out, GError **error)
{
  STCSTransformClass *klass;

  g_return_val_if_fail(STCS_IS_TRANSFORM(self), FALSE);
  g_return_val_if_fail(out != NULL, FALSE);

  klass = STCS_TRANSFORM_GET_CLASS(self);
  g_return_val_if_fail(klass->process != NULL, FALSE);

  return klass->process(self, in, out, error);
}
