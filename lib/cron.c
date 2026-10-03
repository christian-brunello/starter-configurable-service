/*
 * libstarter-configurable-service - cron.c
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

#include <string.h>
#include <time.h>

#include <gio/gio.h>

#include "starter/configurable-service/cron.h"

#define LOGD(msg...) g_log("cron", G_LOG_LEVEL_DEBUG, msg)
#define LOGW(msg...) g_log("cron", G_LOG_LEVEL_WARNING, msg)

#define STCS_CRON_MS_MAX     1000
#define STCS_CRON_SEC_MAX    60
#define STCS_CRON_MIN_MAX    60
#define STCS_CRON_HOUR_MAX   24
#define STCS_CRON_DOM_MAX    32  /* 1..31 used */
#define STCS_CRON_MONTH_MAX  13  /* 1..12 used */
#define STCS_CRON_DOW_MAX    7   /* 0..6 */

#define STCS_CRON_SEARCH_DAYS (366 * 5)

struct _STCSCron {
  GObject parent_instance;

  gchar *expression;
  gboolean parsed;

  guint8 ms[STCS_CRON_MS_MAX];
  guint8 sec[STCS_CRON_SEC_MAX];
  guint8 min[STCS_CRON_MIN_MAX];
  guint8 hour[STCS_CRON_HOUR_MAX];
  guint8 dom[STCS_CRON_DOM_MAX];
  guint8 month[STCS_CRON_MONTH_MAX];
  guint8 dow[STCS_CRON_DOW_MAX];

  gboolean dom_star;
  gboolean dow_star;
};

G_DEFINE_TYPE(STCSCron, stcs_cron, G_TYPE_OBJECT)

static void
stcs_cron_clear_schedule(STCSCron *self)
{
  memset(self->ms, 0, sizeof(self->ms));
  memset(self->sec, 0, sizeof(self->sec));
  memset(self->min, 0, sizeof(self->min));
  memset(self->hour, 0, sizeof(self->hour));
  memset(self->dom, 0, sizeof(self->dom));
  memset(self->month, 0, sizeof(self->month));
  memset(self->dow, 0, sizeof(self->dow));
  self->dom_star = FALSE;
  self->dow_star = FALSE;
}

static void
stcs_cron_finalize(GObject *object)
{
  STCSCron *self = STCS_CRON(object);

  g_free(self->expression);

  G_OBJECT_CLASS(stcs_cron_parent_class)->finalize(object);
}

static void
stcs_cron_class_init(STCSCronClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->finalize = stcs_cron_finalize;
}

static void
stcs_cron_init(STCSCron *self)
{
  self->expression = NULL;
  self->parsed = FALSE;
  stcs_cron_clear_schedule(self);
}

STCSCron *
stcs_cron_new(void)
{
  return g_object_new(STCS_TYPE_CRON, NULL);
}

static gboolean
stcs_cron_parse_int(const gchar *text, gint *out, GError **error)
{
  gchar *end = NULL;
  gint64 value;

  if (text == NULL || *text == '\0')
    {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		  "expected number in cron field");
      return FALSE;
    }

  value = g_ascii_strtoll(text, &end, 10);
  if (end == text || *end != '\0')
    {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		  "invalid number `%s' in cron field", text);
      return FALSE;
    }

  if (value < G_MININT || value > G_MAXINT)
    {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		  "cron number out of range");
      return FALSE;
    }

  *out = (gint) value;
  return TRUE;
}

static void
stcs_cron_set_bit(guint8 *bits, gint index, gboolean is_dow)
{
  if (is_dow)
    {
      if (index == 7)
	index = 0;
      if (index >= 0 && index < STCS_CRON_DOW_MAX)
	bits[index] = 1;
      return;
    }

  bits[index] = 1;
}

static gboolean
stcs_cron_parse_field(const gchar *field,
		      guint8 *bits,
		      gint min_v,
		      gint max_v,
		      gboolean is_dow,
		      gboolean *out_star,
		      GError **error)
{
  gchar **parts;
  guint i;
  gboolean any = FALSE;
  gint effective_max = max_v;

  if (out_star)
    *out_star = FALSE;

  if (field == NULL || *field == '\0')
    {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		  "empty cron field");
      return FALSE;
    }

  /* Dow accepts 0-7 (7 == Sunday). */
  if (is_dow)
    effective_max = 7;

  parts = g_strsplit(field, ",", 0);
  for (i = 0; parts[i] != NULL; i++)
    {
      gchar *token = parts[i];
      gchar **step_parts;
      gchar *range_tok;
      gint step = 1;
      gint start;
      gint end;
      gint v;

      if (*token == '\0')
	{
	  g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		      "empty cron list item");
	  g_strfreev(parts);
	  return FALSE;
	}

      step_parts = g_strsplit(token, "/", 2);
      range_tok = step_parts[0];
      if (step_parts[1] != NULL)
	{
	  if (step_parts[1][0] == '\0' ||
	      !stcs_cron_parse_int(step_parts[1], &step, error) ||
	      step <= 0)
	    {
	      if (error && *error == NULL)
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
			    "invalid cron step in `%s'", token);
	      g_strfreev(step_parts);
	      g_strfreev(parts);
	      return FALSE;
	    }
	}

      if (g_strcmp0(range_tok, "*") == 0)
	{
	  start = min_v;
	  end = effective_max;
	  if (out_star && step == 1)
	    *out_star = TRUE;
	}
      else
	{
	  gchar **range_parts = g_strsplit(range_tok, "-", 2);

	  if (!stcs_cron_parse_int(range_parts[0], &start, error))
	    {
	      g_strfreev(range_parts);
	      g_strfreev(step_parts);
	      g_strfreev(parts);
	      return FALSE;
	    }

	  if (range_parts[1] != NULL)
	    {
	      if (!stcs_cron_parse_int(range_parts[1], &end, error))
		{
		  g_strfreev(range_parts);
		  g_strfreev(step_parts);
		  g_strfreev(parts);
		  return FALSE;
		}
	    }
	  else
	    end = start;

	  g_strfreev(range_parts);
	}

      if (start < min_v || end > effective_max || start > end)
	{
	  g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		      "invalid cron range %d-%d in `%s' (allowed %d-%d)",
		      start, end, token, min_v, effective_max);
	  g_strfreev(step_parts);
	  g_strfreev(parts);
	  return FALSE;
	}

      for (v = start; v <= end; v += step)
	{
	  stcs_cron_set_bit(bits, v, is_dow);
	  any = TRUE;
	}

      g_strfreev(step_parts);
    }

  g_strfreev(parts);

  if (!any)
    {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		  "cron field matched no values");
      return FALSE;
    }

  return TRUE;
}

static gchar **
stcs_cron_split_fields(const gchar *expr)
{
  gchar *stripped;
  gchar **raw;
  GPtrArray *clean;
  guint i;

  stripped = g_strstrip(g_strdup(expr));
  raw = g_strsplit_set(stripped, " \t", 0);
  g_free(stripped);

  clean = g_ptr_array_new();
  for (i = 0; raw[i] != NULL; i++)
    {
      if (raw[i][0] != '\0')
	g_ptr_array_add(clean, g_strdup(raw[i]));
    }
  g_ptr_array_add(clean, NULL);
  g_strfreev(raw);

  return (gchar **) g_ptr_array_free(clean, FALSE);
}

gboolean
stcs_cron_parse(STCSCron *self, const gchar *expr, GError **error)
{
  gchar **fields = NULL;
  guint nfields;
  STCSCron tmp;
  gboolean ok = FALSE;

  g_return_val_if_fail(STCS_IS_CRON(self), FALSE);
  g_return_val_if_fail(expr != NULL, FALSE);

  fields = stcs_cron_split_fields(expr);
  nfields = g_strv_length(fields);

  if (nfields != 6 && nfields != 7)
    {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		  "cron expression must have 6 or 7 fields (got %u): `%s'",
		  nfields, expr);
      goto out;
    }

  memset(&tmp, 0, sizeof(tmp));
  stcs_cron_clear_schedule(&tmp);

  if (!stcs_cron_parse_field(fields[0], tmp.ms, 0, 999, FALSE, NULL, error) ||
      !stcs_cron_parse_field(fields[1], tmp.sec, 0, 59, FALSE, NULL, error) ||
      !stcs_cron_parse_field(fields[2], tmp.min, 0, 59, FALSE, NULL, error) ||
      !stcs_cron_parse_field(fields[3], tmp.hour, 0, 23, FALSE, NULL, error) ||
      !stcs_cron_parse_field(fields[4], tmp.dom, 1, 31, FALSE, &tmp.dom_star, error) ||
      !stcs_cron_parse_field(fields[5], tmp.month, 1, 12, FALSE, NULL, error))
    goto out;

  if (nfields == 7)
    {
      if (!stcs_cron_parse_field(fields[6], tmp.dow, 0, 7, TRUE, &tmp.dow_star, error))
	goto out;
    }
  else
    {
      gint d;
      for (d = 0; d < STCS_CRON_DOW_MAX; d++)
	tmp.dow[d] = 1;
      tmp.dow_star = TRUE;
    }

  stcs_cron_clear_schedule(self);
  memcpy(self->ms, tmp.ms, sizeof(self->ms));
  memcpy(self->sec, tmp.sec, sizeof(self->sec));
  memcpy(self->min, tmp.min, sizeof(self->min));
  memcpy(self->hour, tmp.hour, sizeof(self->hour));
  memcpy(self->dom, tmp.dom, sizeof(self->dom));
  memcpy(self->month, tmp.month, sizeof(self->month));
  memcpy(self->dow, tmp.dow, sizeof(self->dow));
  self->dom_star = tmp.dom_star;
  self->dow_star = tmp.dow_star;

  g_free(self->expression);
  self->expression = g_strdup(expr);
  self->parsed = TRUE;
  ok = TRUE;

  LOGD("parsed cron `%s'", expr);

out:
  g_strfreev(fields);
  return ok;
}

gboolean
stcs_cron_is_parsed(STCSCron *self)
{
  g_return_val_if_fail(STCS_IS_CRON(self), FALSE);
  return self->parsed;
}

const gchar *
stcs_cron_get_expression(STCSCron *self)
{
  g_return_val_if_fail(STCS_IS_CRON(self), NULL);
  return self->expression;
}

static gboolean
stcs_cron_date_matches(const STCSCron *self, const struct tm *tm)
{
  gint month = tm->tm_mon + 1;
  gint dom = tm->tm_mday;
  gint dow = tm->tm_wday;

  if (!self->month[month])
    return FALSE;

  /* Vixie: if both dom and dow are restricted, OR them. */
  if (!self->dom_star && !self->dow_star)
    return self->dom[dom] || self->dow[dow];

  if (!self->dom_star && !self->dom[dom])
    return FALSE;

  if (!self->dow_star && !self->dow[dow])
    return FALSE;

  return TRUE;
}

static gint
stcs_cron_next_set(const guint8 *bits, gint from, gint max_exclusive)
{
  gint i;

  for (i = from; i < max_exclusive; i++)
    {
      if (bits[i])
	return i;
    }

  return -1;
}

gint64
stcs_cron_ms_until_next(STCSCron *self)
{
  gint64 now_us;
  gint64 now_ms;
  gint64 after_ms;
  time_t after_sec;
  gint after_millis;
  struct tm cursor;
  gint day;

  g_return_val_if_fail(STCS_IS_CRON(self), -1);

  if (!self->parsed)
    return -1;

  now_us = g_get_real_time();
  now_ms = now_us / 1000;
  after_ms = now_ms + 1;
  after_sec = (time_t) (after_ms / 1000);
  after_millis = (gint) (after_ms % 1000);

  if (localtime_r(&after_sec, &cursor) == NULL)
    return -1;

  for (day = 0; day < STCS_CRON_SEARCH_DAYS; day++)
    {
      struct tm day_tm = cursor;
      gint hour_from;
      gint hour;

      if (day > 0)
	{
	  day_tm.tm_mday = cursor.tm_mday + day;
	  day_tm.tm_hour = 0;
	  day_tm.tm_min = 0;
	  day_tm.tm_sec = 0;
	  day_tm.tm_isdst = -1;
	  if (mktime(&day_tm) == (time_t) -1)
	    continue;
	}

      if (!stcs_cron_date_matches(self, &day_tm))
	continue;

      hour_from = (day == 0) ? cursor.tm_hour : 0;

      for (hour = stcs_cron_next_set(self->hour, hour_from, STCS_CRON_HOUR_MAX);
	   hour >= 0;
	   hour = stcs_cron_next_set(self->hour, hour + 1, STCS_CRON_HOUR_MAX))
	{
	  gint min_from = (day == 0 && hour == cursor.tm_hour) ? cursor.tm_min : 0;
	  gint min;

	  for (min = stcs_cron_next_set(self->min, min_from, STCS_CRON_MIN_MAX);
	       min >= 0;
	       min = stcs_cron_next_set(self->min, min + 1, STCS_CRON_MIN_MAX))
	    {
	      gint sec_from =
		(day == 0 && hour == cursor.tm_hour && min == cursor.tm_min)
		? cursor.tm_sec
		: 0;
	      gint sec;

	      for (sec = stcs_cron_next_set(self->sec, sec_from, STCS_CRON_SEC_MAX);
		   sec >= 0;
		   sec = stcs_cron_next_set(self->sec, sec + 1, STCS_CRON_SEC_MAX))
		{
		  gint ms_from =
		    (day == 0 && hour == cursor.tm_hour &&
		     min == cursor.tm_min && sec == cursor.tm_sec)
		    ? after_millis
		    : 0;
		  gint ms;
		  struct tm cand_tm;
		  time_t cand_sec;
		  gint64 cand_ms;
		  gint64 delay;

		  ms = stcs_cron_next_set(self->ms, ms_from, STCS_CRON_MS_MAX);
		  if (ms < 0)
		    continue;

		  cand_tm = day_tm;
		  cand_tm.tm_hour = hour;
		  cand_tm.tm_min = min;
		  cand_tm.tm_sec = sec;
		  cand_tm.tm_isdst = -1;
		  cand_sec = mktime(&cand_tm);
		  if (cand_sec == (time_t) -1)
		    continue;

		  /* Reject DST gaps / mismatches where wall clock differs. */
		  if (cand_tm.tm_hour != hour ||
		      cand_tm.tm_min != min ||
		      cand_tm.tm_sec != sec)
		    continue;

		  cand_ms = (gint64) cand_sec * 1000 + ms;
		  delay = cand_ms - now_ms;
		  if (delay < 1)
		    continue;

		  LOGD("next fire in %" G_GINT64_FORMAT " ms (cron `%s')",
		       delay, self->expression ? self->expression : "");
		  return delay;
		}
	    }
	}
    }

  LOGW("no matching cron fire within search window for `%s'",
       self->expression ? self->expression : "");
  return -1;
}

guint
stcs_cron_timeout_add(STCSCron *self, GSourceFunc func, gpointer data)
{
  gint64 delay;

  g_return_val_if_fail(STCS_IS_CRON(self), 0);
  g_return_val_if_fail(func != NULL, 0);

  delay = stcs_cron_ms_until_next(self);
  if (delay < 1)
    {
      LOGW("stcs_cron_timeout_add: cannot schedule (delay=%" G_GINT64_FORMAT ")",
	   delay);
      return 0;
    }

  if (delay > G_MAXUINT)
    {
      LOGW("stcs_cron_timeout_add: delay exceeds G_MAXUINT, clamping");
      delay = G_MAXUINT;
    }

  return g_timeout_add((guint) delay, func, data);
}
