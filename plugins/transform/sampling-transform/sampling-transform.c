/*
 * starter-configurable-service - sampling-transform.c
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
#include <math.h>
#include <time.h>

#include <glib-object.h>
#include <gmodule.h>
#include <gio/gio.h>

#include "plugin.h"
#include "starter/configurable-service/transform.h"

#define LOGD(msg...) g_log("sampling-transform", G_LOG_LEVEL_DEBUG, msg)

#define STCS_TYPE_SAMPLING_TRANSFORM (stcs_sampling_transform_get_type())
G_DECLARE_FINAL_TYPE(STCSSamplingTransform, stcs_sampling_transform, STCS, SAMPLING_TRANSFORM, STCSTransform)

typedef gboolean (*STCSSamplingReduceFunc)(gdouble *samples,
					   guint count,
					   gdouble *out,
					   GError **error);

typedef enum {
  STCS_SAMPLING_MODE_SLIDING = 0,
  STCS_SAMPLING_MODE_HOLD
} STCSSamplingMode;

typedef enum {
  STCS_SAMPLING_WINDOW_SAMPLES = 0,
  STCS_SAMPLING_WINDOW_DURATION
} STCSSamplingWindowKind;

struct _STCSSamplingTransform {
  STCSTransform parent_instance;

  STCSSamplingMode mode;
  STCSSamplingWindowKind window_kind;

  /* samples window: fixed capacity. duration hold: grows as needed. */
  guint capacity;
  guint64 period_sec; /* duration window only */

  gdouble *buf;
  guint count;
  guint head; /* sliding samples: index of oldest */

  gdouble *scratch;
  STCSSamplingReduceFunc reduce;
  gchar *reduce_name;

  /* hold (samples or duration) */
  gboolean hold_armed;
  gdouble last_out;

  /* hold + duration: clock-aligned bucket index (floor(unix/period)). */
  gboolean bucket_valid;
  gint64 bucket;
};

G_DEFINE_TYPE(STCSSamplingTransform, stcs_sampling_transform, STCS_TYPE_TRANSFORM)

static int
stcs_sampling_gdouble_cmp(const void *a, const void *b)
{
  gdouble da = *(const gdouble *) a;
  gdouble db = *(const gdouble *) b;

  if (da < db)
    return -1;
  if (da > db)
    return 1;
  return 0;
}

static gboolean
stcs_sampling_reduce_avg(gdouble *samples, guint count, gdouble *out, GError **error)
{
  gdouble sum = 0.0;
  guint i;

  (void) error;
  g_return_val_if_fail(count > 0, FALSE);

  for (i = 0; i < count; i++)
    sum += samples[i];

  *out = sum / (gdouble) count;
  return TRUE;
}

static gboolean
stcs_sampling_reduce_min(gdouble *samples, guint count, gdouble *out, GError **error)
{
  gdouble m;
  guint i;

  (void) error;
  g_return_val_if_fail(count > 0, FALSE);

  m = samples[0];
  for (i = 1; i < count; i++)
    {
      if (samples[i] < m)
	m = samples[i];
    }

  *out = m;
  return TRUE;
}

static gboolean
stcs_sampling_reduce_max(gdouble *samples, guint count, gdouble *out, GError **error)
{
  gdouble m;
  guint i;

  (void) error;
  g_return_val_if_fail(count > 0, FALSE);

  m = samples[0];
  for (i = 1; i < count; i++)
    {
      if (samples[i] > m)
	m = samples[i];
    }

  *out = m;
  return TRUE;
}

static gboolean
stcs_sampling_reduce_median(gdouble *samples, guint count, gdouble *out, GError **error)
{
  guint mid;

  (void) error;
  g_return_val_if_fail(count > 0, FALSE);

  qsort(samples, count, sizeof(gdouble), stcs_sampling_gdouble_cmp);

  mid = count / 2;
  if ((count % 2) == 0)
    *out = (samples[mid - 1] + samples[mid]) / 2.0;
  else
    *out = samples[mid];

  return TRUE;
}

typedef struct {
  const gchar *name;
  STCSSamplingReduceFunc func;
} STCSSamplingReduceEntry;

static const STCSSamplingReduceEntry stcs_sampling_reduce_table[] = {
  { "avg", stcs_sampling_reduce_avg },
  { "min", stcs_sampling_reduce_min },
  { "max", stcs_sampling_reduce_max },
  { "median", stcs_sampling_reduce_median },
};

static STCSSamplingReduceFunc
stcs_sampling_lookup_reduce(const gchar *name)
{
  guint i;

  for (i = 0; i < G_N_ELEMENTS(stcs_sampling_reduce_table); i++)
    {
      if (g_strcmp0(name, stcs_sampling_reduce_table[i].name) == 0)
	return stcs_sampling_reduce_table[i].func;
    }

  return NULL;
}

static gboolean
stcs_sampling_parse_uint(const char *text, guint *out, GError **error)
{
  gchar *end = NULL;
  guint64 v;

  if (text == NULL || *text == '\0')
    {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "empty unsigned integer");
      return FALSE;
    }

  v = g_ascii_strtoull(text, &end, 10);
  if (end == text || *end != '\0' || v == 0 || v > G_MAXUINT)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "invalid positive integer `%s'",
		  text);
      return FALSE;
    }

  *out = (guint) v;
  return TRUE;
}

/* Parses "300", "300s", "5m", "1h" into seconds. */
static gboolean
stcs_sampling_parse_duration(const char *text, guint64 *out_sec, GError **error)
{
  gchar *end = NULL;
  guint64 v;
  guint64 mult = 1;

  if (text == NULL || *text == '\0')
    {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "empty duration");
      return FALSE;
    }

  v = g_ascii_strtoull(text, &end, 10);
  if (end == text || v == 0)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "invalid duration `%s'",
		  text);
      return FALSE;
    }

  if (*end != '\0')
    {
      if (end[1] != '\0')
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "invalid duration suffix in `%s' (use s, m, or h)",
		      text);
	  return FALSE;
	}

      switch (*end)
	{
	case 's':
	case 'S':
	  mult = 1;
	  break;
	case 'm':
	case 'M':
	  mult = 60;
	  break;
	case 'h':
	case 'H':
	  mult = 3600;
	  break;
	default:
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "invalid duration suffix in `%s' (use s, m, or h)",
		      text);
	  return FALSE;
	}
    }

  if (v > G_MAXUINT64 / mult)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "duration `%s' is too large",
		  text);
      return FALSE;
    }

  *out_sec = v * mult;
  return TRUE;
}

static gboolean
stcs_sampling_parse_string_content(xmlNodePtr node, gchar **out, GError **error)
{
  xmlChar *content;
  gchar *trimmed;

  content = xmlNodeGetContent(node);
  if (content == NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "element <%s> has empty content",
		  (const char *) node->name);
      return FALSE;
    }

  trimmed = g_strstrip(g_strdup((const char *) content));
  xmlFree(content);

  if (*trimmed == '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "element <%s> has empty content",
		  (const char *) node->name);
      g_free(trimmed);
      return FALSE;
    }

  g_free(*out);
  *out = trimmed;
  return TRUE;
}

static void
stcs_sampling_clear_buffer(STCSSamplingTransform *self)
{
  g_clear_pointer(&self->buf, g_free);
  g_clear_pointer(&self->scratch, g_free);
  self->capacity = 0;
  self->count = 0;
  self->head = 0;
  self->hold_armed = FALSE;
  self->last_out = 0.0;
  self->bucket_valid = FALSE;
  self->bucket = 0;
  self->period_sec = 0;
}

static gboolean
stcs_sampling_ensure_capacity(STCSSamplingTransform *self, guint need, GError **error)
{
  guint new_cap;

  if (need <= self->capacity)
    return TRUE;

  new_cap = self->capacity == 0 ? 16 : self->capacity;
  while (new_cap < need)
    {
      if (new_cap > G_MAXUINT / 2)
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_NO_SPACE,
		      "sampling-transform buffer capacity overflow");
	  return FALSE;
	}
      new_cap *= 2;
    }

  self->buf = g_realloc(self->buf, new_cap * sizeof(gdouble));
  self->scratch = g_realloc(self->scratch, new_cap * sizeof(gdouble));
  self->capacity = new_cap;
  return TRUE;
}

static void
stcs_sampling_copy_window(STCSSamplingTransform *self, gdouble *dst)
{
  guint i;

  if (self->mode == STCS_SAMPLING_MODE_HOLD ||
      self->window_kind == STCS_SAMPLING_WINDOW_DURATION)
    {
      memcpy(dst, self->buf, self->count * sizeof(gdouble));
      return;
    }

  for (i = 0; i < self->count; i++)
    dst[i] = self->buf[(self->head + i) % self->capacity];
}

static void
stcs_sampling_push_sliding(STCSSamplingTransform *self, gdouble value)
{
  if (self->count < self->capacity)
    {
      self->buf[(self->head + self->count) % self->capacity] = value;
      self->count++;
      return;
    }

  self->buf[self->head] = value;
  self->head = (self->head + 1) % self->capacity;
}

static gboolean
stcs_sampling_push_hold(STCSSamplingTransform *self, gdouble value, GError **error)
{
  if (self->window_kind == STCS_SAMPLING_WINDOW_SAMPLES)
    {
      g_return_val_if_fail(self->count < self->capacity, FALSE);
      self->buf[self->count] = value;
      self->count++;
      return TRUE;
    }

  if (!stcs_sampling_ensure_capacity(self, self->count + 1, error))
    return FALSE;

  self->buf[self->count] = value;
  self->count++;
  return TRUE;
}

static void
stcs_sampling_reset_batch(STCSSamplingTransform *self)
{
  self->count = 0;
  self->head = 0;
}

static gboolean
stcs_sampling_apply_reduce(STCSSamplingTransform *self, gdouble *out, GError **error)
{
  gdouble result = 0.0;

  stcs_sampling_copy_window(self, self->scratch);

  if (!self->reduce(self->scratch, self->count, &result, error))
    return FALSE;

  if (isnan(result) || isinf(result))
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "sampling-transform reduce `%s' produced a non-finite result",
		  self->reduce_name ? self->reduce_name : "?");
      return FALSE;
    }

  *out = result;
  return TRUE;
}

static gint64
stcs_sampling_current_bucket(const STCSSamplingTransform *self)
{
  time_t now;

  now = time(NULL);
  if (now < 0)
    now = 0;

  return (gint64) ((guint64) now / self->period_sec);
}

static gboolean
stcs_sampling_parse_mode(const gchar *text, STCSSamplingMode *out, GError **error)
{
  if (g_strcmp0(text, "sliding") == 0)
    {
      *out = STCS_SAMPLING_MODE_SLIDING;
      return TRUE;
    }

  if (g_strcmp0(text, "hold") == 0)
    {
      *out = STCS_SAMPLING_MODE_HOLD;
      return TRUE;
    }

  g_set_error(error,
	      G_IO_ERROR,
	      G_IO_ERROR_INVALID_DATA,
	      "unknown sampling-transform mode `%s' (expected sliding|hold)",
	      text);
  return FALSE;
}

static gboolean
stcs_sampling_transform_configure(STCSTransform *transform, xmlNodePtr node, GError **error)
{
  STCSSamplingTransform *self = STCS_SAMPLING_TRANSFORM(transform);
  xmlNodePtr child;
  guint samples = 0;
  guint64 period_sec = 0;
  gboolean have_samples = FALSE;
  gboolean have_duration = FALSE;
  gchar *reduce_name = NULL;
  gchar *mode_name = NULL;
  STCSSamplingReduceFunc reduce_fn = NULL;
  STCSSamplingMode mode = STCS_SAMPLING_MODE_SLIDING;
  STCSSamplingWindowKind window_kind = STCS_SAMPLING_WINDOW_SAMPLES;
  gboolean r = FALSE;

  g_return_val_if_fail(node != NULL, FALSE);

  for (child = node->children; child != NULL; child = child->next)
    {
      if (child->type != XML_ELEMENT_NODE)
	continue;

      if (xmlStrEqual(child->name, (const xmlChar *) "window"))
	{
	  xmlChar *samples_attr;
	  xmlChar *duration_attr;

	  samples_attr = xmlGetProp(child, (const xmlChar *) "samples");
	  duration_attr = xmlGetProp(child, (const xmlChar *) "duration");

	  if ((samples_attr == NULL) == (duration_attr == NULL))
	    {
	      g_set_error(error,
			  G_IO_ERROR,
			  G_IO_ERROR_INVALID_DATA,
			  "sampling-transform <window> requires exactly one of "
			  "samples or duration");
	      xmlFree(samples_attr);
	      xmlFree(duration_attr);
	      goto out;
	    }

	  if (samples_attr != NULL)
	    {
	      if (!stcs_sampling_parse_uint((const char *) samples_attr, &samples, error))
		{
		  xmlFree(samples_attr);
		  goto out;
		}
	      xmlFree(samples_attr);
	      have_samples = TRUE;
	      window_kind = STCS_SAMPLING_WINDOW_SAMPLES;
	    }
	  else
	    {
	      if (!stcs_sampling_parse_duration((const char *) duration_attr, &period_sec, error))
		{
		  xmlFree(duration_attr);
		  goto out;
		}
	      xmlFree(duration_attr);
	      have_duration = TRUE;
	      window_kind = STCS_SAMPLING_WINDOW_DURATION;
	    }
	}
      else if (xmlStrEqual(child->name, (const xmlChar *) "reduce"))
	{
	  if (!stcs_sampling_parse_string_content(child, &reduce_name, error))
	    goto out;
	}
      else if (xmlStrEqual(child->name, (const xmlChar *) "mode"))
	{
	  if (!stcs_sampling_parse_string_content(child, &mode_name, error))
	    goto out;
	}
      else
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "unexpected element <%s> inside sampling-transform config",
		      (const char *) child->name);
	  goto out;
	}
    }

  if (!have_samples && !have_duration)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "sampling-transform requires <window samples=\"N\"> or <window duration=\"...\">");
      goto out;
    }

  if (reduce_name == NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "sampling-transform requires <reduce> (avg|min|max|median)");
      goto out;
    }

  reduce_fn = stcs_sampling_lookup_reduce(reduce_name);
  if (reduce_fn == NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "unknown sampling-transform reduce `%s' (expected avg|min|max|median)",
		  reduce_name);
      goto out;
    }

  if (mode_name != NULL && !stcs_sampling_parse_mode(mode_name, &mode, error))
    goto out;

  if (window_kind == STCS_SAMPLING_WINDOW_DURATION && mode != STCS_SAMPLING_MODE_HOLD)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "sampling-transform <window duration> requires <mode>hold</mode>");
      goto out;
    }

  stcs_sampling_clear_buffer(self);
  self->mode = mode;
  self->window_kind = window_kind;
  self->reduce = reduce_fn;
  g_free(self->reduce_name);
  self->reduce_name = reduce_name;
  reduce_name = NULL;

  if (window_kind == STCS_SAMPLING_WINDOW_SAMPLES)
    {
      self->capacity = samples;
      self->buf = g_new0(gdouble, samples);
      self->scratch = g_new0(gdouble, samples);
      LOGD("configured window samples=%u reduce=%s mode=%s",
	   self->capacity,
	   self->reduce_name,
	   self->mode == STCS_SAMPLING_MODE_HOLD ? "hold" : "sliding");
    }
  else
    {
      self->period_sec = period_sec;
      if (!stcs_sampling_ensure_capacity(self, 16, error))
	goto out;
      LOGD("configured window duration=%" G_GUINT64_FORMAT
	   "s (clock-aligned) reduce=%s mode=hold",
	   self->period_sec,
	   self->reduce_name);
    }

  r = TRUE;

out:
  g_free(reduce_name);
  g_free(mode_name);
  return r;
}

static gboolean
stcs_sampling_process_hold_samples(STCSSamplingTransform *self,
				   gdouble in,
				   gdouble *out,
				   GError **error)
{
  gdouble result = 0.0;

  if (!stcs_sampling_push_hold(self, in, error))
    return FALSE;

  if (!self->hold_armed)
    {
      if (!stcs_sampling_apply_reduce(self, &result, error))
	return FALSE;

      self->last_out = result;
      *out = result;

      if (self->count >= self->capacity)
	{
	  self->hold_armed = TRUE;
	  stcs_sampling_reset_batch(self);
	  LOGD("hold/samples warm-up complete in=%g reduce=%s out=%g — armed, next %u samples will hold",
	       in,
	       self->reduce_name ? self->reduce_name : "?",
	       result,
	       self->capacity);
	}
      else
	{
	  LOGD("hold/samples warm-up in=%g batch=%u/%u reduce=%s out=%g",
	       in,
	       self->count,
	       self->capacity,
	       self->reduce_name ? self->reduce_name : "?",
	       result);
	}

      return TRUE;
    }

  if (self->count < self->capacity)
    {
      *out = self->last_out;
      LOGD("hold/samples keeping in=%g batch=%u/%u out=%g (last reduce)",
	   in,
	   self->count,
	   self->capacity,
	   self->last_out);
      return TRUE;
    }

  if (!stcs_sampling_apply_reduce(self, &result, error))
    return FALSE;

  LOGD("hold/samples update in=%g batch=%u/%u reduce=%s out=%g (was %g)",
       in,
       self->capacity,
       self->capacity,
       self->reduce_name ? self->reduce_name : "?",
       result,
       self->last_out);

  self->last_out = result;
  *out = result;
  stcs_sampling_reset_batch(self);
  return TRUE;
}

static gboolean
stcs_sampling_process_hold_duration(STCSSamplingTransform *self,
				    gdouble in,
				    gdouble *out,
				    GError **error)
{
  gint64 bucket;
  gdouble result = 0.0;

  bucket = stcs_sampling_current_bucket(self);

  if (!self->bucket_valid)
    {
      self->bucket = bucket;
      self->bucket_valid = TRUE;
      if (!stcs_sampling_push_hold(self, in, error))
	return FALSE;

      if (!stcs_sampling_apply_reduce(self, &result, error))
	return FALSE;

      self->last_out = result;
      *out = result;
      LOGD("hold/duration warm-up start in=%g bucket=%" G_GINT64_FORMAT
	   " period=%" G_GUINT64_FORMAT "s n=%u reduce=%s out=%g",
	   in, bucket, self->period_sec, self->count,
	   self->reduce_name ? self->reduce_name : "?",
	   result);
      return TRUE;
    }

  if (bucket != self->bucket)
    {
      if (self->count > 0)
	{
	  if (!stcs_sampling_apply_reduce(self, &result, error))
	    return FALSE;

	  LOGD("hold/duration boundary bucket %" G_GINT64_FORMAT "→%" G_GINT64_FORMAT
	       " n=%u reduce=%s out=%g (was %g)",
	       self->bucket, bucket, self->count,
	       self->reduce_name ? self->reduce_name : "?",
	       result, self->last_out);

	  self->last_out = result;
	  self->hold_armed = TRUE;
	}
      else
	{
	  LOGD("hold/duration boundary bucket %" G_GINT64_FORMAT "→%" G_GINT64_FORMAT
	       " with no samples — keep out=%g",
	       self->bucket, bucket, self->last_out);
	}

      stcs_sampling_reset_batch(self);
      self->bucket = bucket;

      if (!stcs_sampling_push_hold(self, in, error))
	return FALSE;

      *out = self->last_out;
      return TRUE;
    }

  if (!stcs_sampling_push_hold(self, in, error))
    return FALSE;

  if (!self->hold_armed)
    {
      if (!stcs_sampling_apply_reduce(self, &result, error))
	return FALSE;

      self->last_out = result;
      *out = result;
      LOGD("hold/duration warm-up in=%g bucket=%" G_GINT64_FORMAT
	   " n=%u reduce=%s out=%g",
	   in, bucket, self->count,
	   self->reduce_name ? self->reduce_name : "?",
	   result);
      return TRUE;
    }

  *out = self->last_out;
  LOGD("hold/duration keeping in=%g bucket=%" G_GINT64_FORMAT
       " n=%u out=%g (last reduce)",
       in, bucket, self->count, self->last_out);
  return TRUE;
}

static gboolean
stcs_sampling_transform_process(STCSTransform *transform, gdouble in, gdouble *out, GError **error)
{
  STCSSamplingTransform *self = STCS_SAMPLING_TRANSFORM(transform);

  if (self->reduce == NULL ||
      (self->window_kind == STCS_SAMPLING_WINDOW_SAMPLES &&
       (self->buf == NULL || self->capacity == 0)) ||
      (self->window_kind == STCS_SAMPLING_WINDOW_DURATION &&
       (self->period_sec == 0 || self->buf == NULL)))
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_NOT_INITIALIZED,
		  "sampling-transform is not configured");
      return FALSE;
    }

  if (self->mode == STCS_SAMPLING_MODE_SLIDING)
    {
      stcs_sampling_push_sliding(self, in);
      if (!stcs_sampling_apply_reduce(self, out, error))
	return FALSE;

      LOGD("sliding in=%g window=%u/%u reduce=%s out=%g",
	   in,
	   self->count,
	   self->capacity,
	   self->reduce_name ? self->reduce_name : "?",
	   *out);
      return TRUE;
    }

  if (self->window_kind == STCS_SAMPLING_WINDOW_DURATION)
    return stcs_sampling_process_hold_duration(self, in, out, error);

  return stcs_sampling_process_hold_samples(self, in, out, error);
}

static void
stcs_sampling_transform_finalize(GObject *object)
{
  STCSSamplingTransform *self = STCS_SAMPLING_TRANSFORM(object);

  stcs_sampling_clear_buffer(self);
  g_clear_pointer(&self->reduce_name, g_free);
  self->reduce = NULL;

  G_OBJECT_CLASS(stcs_sampling_transform_parent_class)->finalize(object);
}

static void
stcs_sampling_transform_class_init(STCSSamplingTransformClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  STCSTransformClass *transform_class = STCS_TRANSFORM_CLASS(klass);

  object_class->finalize = stcs_sampling_transform_finalize;

  transform_class->configure = stcs_sampling_transform_configure;
  transform_class->process = stcs_sampling_transform_process;
}

static void
stcs_sampling_transform_init(STCSSamplingTransform *self)
{
  self->mode = STCS_SAMPLING_MODE_SLIDING;
  self->window_kind = STCS_SAMPLING_WINDOW_SAMPLES;
  self->capacity = 0;
  self->period_sec = 0;
  self->buf = NULL;
  self->count = 0;
  self->head = 0;
  self->scratch = NULL;
  self->reduce = NULL;
  self->reduce_name = NULL;
  self->hold_armed = FALSE;
  self->last_out = 0.0;
  self->bucket_valid = FALSE;
  self->bucket = 0;
}

G_MODULE_EXPORT STCSPluginInfo *
stcs_plugin_get_info(void)
{
  return stcs_plugin_info_new("sampling-transform", STCS_PLUGIN_CATEGORY_TRANSFORM, "1.2.0");
}

G_MODULE_EXPORT GObject *
stcs_plugin_create(void)
{
  return g_object_new(STCS_TYPE_SAMPLING_TRANSFORM, NULL);
}
